from __future__ import annotations

from tools.bundled_firmware import image as bundled_image

import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from . import REPO_ROOT
from tools.siemens_tools.eeprom import C55_EMULATOR_MINIMAL_PROFILE
from tools.siemens_tools.fullflash import (
    pack_c55_eeprom,
    pack_c60_eeprom,
    pack_m55_eeprom,
)
from tools.siemens_tools.fullflash.assembly import (
    _SUPPORTED_MODELS,
    _assemble_request,
    _baseline_artifacts,
    _baseline_flash_ids,
    _default_output,
    _imei_serial,
    _mobsw_selections,
    _resolve_explicit_regions,
    _resolve_mobsw,
    _resolve_region_candidate,
    _schema4_eeprom_replay_policy,
    _sha256,
    _tuple_key,
    _write_output_pair,
    Baseline,
    AssemblyRequest,
    parse_bootkey,
    parse_flash_id,
    region_candidates,
)
from tools.siemens_tools.fullflash.cli import main as fullflash_main
from tools.siemens_tools.fullflash.statistics import inspect_statistics
from tools.siemens_tools.firmware.xbi import FirmwareError

from .bundled_fixtures import catalogs, context as load_assembly_context, eeprom_map, ambiguous_t9
from tools.bundled_firmware import entry as bundled_entry


C55_BASELINE = bundled_entry("c55", langpack=91)["sha256"][:12]
C55_BASELINE_SHA = bundled_entry("c55", langpack=91)["sha256"]
C55_MOBSW = "724c44ebaa95"
C55_T9 = "06aca91c3667"
C55_DONOR = bundled_image("c55")
C55_MAP = eeprom_map("c55")
M55_BASELINE = bundled_image("m55")
M55_MAP = eeprom_map("m55")
A55_BASELINE = (
    bundled_image("a55")
)
A55_MAP = eeprom_map("a55")


class FullflashAssemblyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        from dataclasses import replace
        from tools.siemens_tools.eeprom.profiles import EEPROM_PROFILES
        # Bind test copies of profiles to the temporary map and bundled donor.
        # Production profiles retain their original evidence hashes.
        profiles = {}
        for model in ("c55", "a55", "m55"):
            name = f"{model}-emulator-minimal-v1"
            profiles[name] = replace(
                EEPROM_PROFILES[name],
                map_sha256=hashlib.sha256(eeprom_map(model).read_bytes()).hexdigest(),
                compatible_map_sha256s=(),
                donor_sha256=bundled_entry(model)["sha256"],
            )
        patch = mock.patch.dict(EEPROM_PROFILES, profiles)
        patch.start()
        cls.addClassCleanup(patch.stop)

    def run_cli(self, argv: list[str]) -> tuple[int, str, str]:
        if "--recipe" not in argv:
            model = argv[1]
            root = catalogs(model)
            argv = [*argv, "--community-catalog", str(root / "community"),
                    "--official-catalog", str(root / "official")]
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = fullflash_main(argv)
        return status, stdout.getvalue(), stderr.getvalue()

    def test_schema4_replay_recovers_superseded_zero_only_profile(self) -> None:
        old_records = tuple(
            record for record in C55_EMULATOR_MINIMAL_PROFILE.records
            if record.block_id in (1, 2, 55, 5121, 5122, 5123)
        )
        selection = {
            "profile": "c55-emulator-minimal-v1",
            "map": {"sha256": "1" * 64},
            "donor": {"sha256": "2" * 64, "blocks": [67, 5005, 5006]},
            "normalizations": [{"id": 5372, "length": 30, "version": 0}],
            "omitted_blocks": [],
            "records": [
                {
                    "id": record.block_id,
                    "length": len(record.payload),
                    "memory_class": record.memory_class,
                    "version": record.version,
                    "source": "profile",
                    "provenance": "historical zero-only profile",
                    "payload_kind": "synthesized-zero",
                    "observed_bytes": 0,
                    "payload_sha256": hashlib.sha256(record.payload).hexdigest(),
                }
                for record in old_records
            ],
        }
        override, normalizations = _schema4_eeprom_replay_policy(selection)
        self.assertIsNotNone(override)
        assert override is not None
        self.assertEqual(
            tuple(record.block_id for record in override.records),
            (1, 2, 55, 5121, 5122, 5123),
        )
        self.assertEqual(override.donor_blocks, (67, 5005, 5006))
        self.assertEqual(normalizations, frozenset({5372}))

        selection["records"][0]["payload_sha256"] = "0" * 64
        with self.assertRaisesRegex(FirmwareError, "cannot be replayed"):
            _schema4_eeprom_replay_policy(selection)

    def test_schema4_replay_reconstructs_versioned_battery_calibration(self) -> None:
        records = [
            record for record in C55_EMULATOR_MINIMAL_PROFILE.records
            if record.block_id != 75
        ]
        selection = {
            "profile": "c55-emulator-minimal-v1",
            "map": {"sha256": "1" * 64},
            "donor": {"sha256": "2" * 64, "blocks": [5005, 5006]},
            "normalizations": [],
            "omitted_blocks": [],
            "records": [
                {
                    "id": record.block_id,
                    "length": len(record.payload),
                    "memory_class": record.memory_class,
                    "version": record.version,
                    "source": "profile",
                    "provenance": record.provenance,
                    "payload_kind": record.payload_kind,
                    "observed_bytes": record.observed_bytes,
                    "payload_sha256": hashlib.sha256(record.payload).hexdigest(),
                }
                for record in records
            ],
        }
        override, _normalizations = _schema4_eeprom_replay_policy(selection)
        self.assertIsNotNone(override)
        assert override is not None
        battery = next(record for record in override.records if record.block_id == 67)
        self.assertEqual(
            hashlib.sha256(battery.payload).hexdigest(),
            "c4247e2de00dc5e625f29fe4653b513b1e576ecbc597f9672614c640ccc70d8f",
        )

        battery_manifest = next(
            record for record in selection["records"] if record["id"] == 67
        )
        battery_manifest["payload_sha256"] = "0" * 64
        with self.assertRaisesRegex(FirmwareError, "cannot be replayed"):
            _schema4_eeprom_replay_policy(selection)

    @staticmethod
    def request(**overrides: object) -> AssemblyRequest:
        corpus = catalogs("C55")
        values = {
            "model": "C55",
            "community_catalog": corpus / "community",
            "official_catalog": corpus / "official",
            "baseline": C55_BASELINE,
            "region_overrides": (),
            "mob_sw": None,
            "flash_ids": (),
            "bootkey_action": "preserve",
            "bootkey_hash": None,
            "bootkey_source": None,
            "entry_target": None,
            "interactive": False,
        }
        values.update(overrides)
        return AssemblyRequest(**values)  # type: ignore[arg-type]

    def test_all_supported_models_assemble_a_catalog_baseline(self) -> None:
        observed = {}
        no_t9 = {"A60", "A62", "A65", "C60", "MC60"}
        for model in _SUPPORTED_MODELS:
            with self.subTest(model=model):
                self._assert_model_assembly(model, observed, no_t9)
        self.assertEqual(set(observed), set(_SUPPORTED_MODELS))
        self.assertTrue(all(
            filename.startswith(f"{model.lower()}sw")
            for model, filename in observed.items()
        ))

    def _assert_model_assembly(
        self, model: str, observed: dict[str, str], no_t9: set[str],
    ) -> None:
        context = load_assembly_context(model)
        baselines = _baseline_artifacts(context)
        self.assertTrue(baselines, model)
        self.assertTrue(_mobsw_selections(context), model)
        baseline = baselines[0]
        result = _assemble_request(AssemblyRequest(
            model=model,
            community_catalog=context.community.path,
            official_catalog=context.official.path,
            baseline=baseline["sha256"],
            region_overrides=(),
            mob_sw=None,
            flash_ids=(),
            bootkey_action="preserve",
            bootkey_hash=None,
            bootkey_source=None,
            entry_target=None,
            interactive=False,
        ))
        observed[model] = _default_output(result).name
        self.assertEqual(len(result.image), context.layout.layout.length)
        if model in no_t9:
            self.assertEqual(result.scope["t9_version"], 0)

    def test_catalog_identifiers_accept_absolute_filesystem_paths(self) -> None:
        context = load_assembly_context("C55")
        baseline = next(
            item for item in _baseline_artifacts(context)
            if item["sha256"] == C55_BASELINE_SHA
        )
        baseline_path = (REPO_ROOT / baseline["path"]).resolve()
        t9 = next(
            item for item in region_candidates(context, "official", "T9")
            if item.sha256.startswith(C55_T9)
            and item.scope.get("software_version") == 24
        )
        t9_path = (context.official.root / t9.payload_path).resolve()
        mob_sw = next(
            item for item in _mobsw_selections(context)
            if item.sha256.startswith(C55_MOBSW)
        )
        mob_sw_path = next(
            (REPO_ROOT / alias).resolve()
            for alias in mob_sw.aliases
            if (REPO_ROOT / alias).is_file()
        )

        self.assertEqual(
            _resolve_region_candidate(
                context, "official", "T9", str(t9_path), {"software_version": 24},
            ).sha256,
            t9.sha256,
        )
        self.assertEqual(_resolve_mobsw(context, str(mob_sw_path)).sha256, mob_sw.sha256)
        self.assertEqual(
            _assemble_request(self.request(baseline=str(baseline_path))).baseline.sha256,
            C55_BASELINE_SHA,
        )

    def test_noninteractive_requires_baseline(self) -> None:
        request = self.request(baseline=None)
        with self.assertRaisesRegex(FirmwareError, "requires --baseline"):
            _assemble_request(request)

    def test_baseline_assembly_naming_and_idempotent_pair(self) -> None:
        result = _assemble_request(self.request())
        self.assertEqual(result.baseline.sha256, C55_BASELINE_SHA)
        self.assertEqual(
            result.scope,
            {"software_version": 24, "langpack": 91, "t9_version": 11},
        )
        self.assertEqual(result.serial, "556677")
        self.assertEqual(result.serial_source, "consistent-eeprom-imei")
        self.assertEqual(
            _default_output(result).name,
            f"c55sw249111-556677-{_sha256(result.image)[:12]}.bin",
        )

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "custom.bin"
            argv = [
                "assemble", "C55", "--baseline", C55_BASELINE,
                "--non-interactive", "-o", str(output),
            ]
            first, text, error = self.run_cli(argv)
            second, second_text, second_error = self.run_cli(argv)
            recipe = output.with_suffix(".json")
            document = json.loads(recipe.read_text(encoding="utf-8"))

        self.assertEqual((first, error), (0, ""))
        self.assertIn("written:", text)
        self.assertEqual((second, second_error), (0, ""))
        self.assertIn("reused:", second_text)
        self.assertEqual(document["schema"], "siemens-synthetic-fullflash-recipe")
        self.assertEqual(document["identity"]["filename_serial"], "556677")
        self.assertEqual(document["application_order"], [
            "community-baseline", "statistics-checksum",
        ])

    def test_region_personalization_and_recipe_replay(self) -> None:
        bootkey = bytes.fromhex("00112233445566778899aabbccddeeff")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "personalized.bin"
            replay = Path(directory) / "replayed.bin"
            status, _text, error = self.run_cli([
                "assemble", "C55", "--baseline", C55_BASELINE,
                "--region", f"T9=official:{C55_T9}",
                "--mobsw", C55_MOBSW,
                "--flash-id", "0x20:0x17",
                "--bootkey", bootkey.hex(),
                "--entry-target", "0x07FFF0",
                "--non-interactive", "-o", str(output),
            ])
            image = output.read_bytes()
            recipe_path = output.with_suffix(".json")
            recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
            replay_status, _replay_text, replay_error = self.run_cli([
                "assemble", "--recipe", str(recipe_path),
                "-o", str(replay),
            ])
            replayed = replay.read_bytes()

        self.assertEqual(status, 0)
        self.assertIn("scope mismatch", error)
        self.assertEqual(replay_status, 0)
        self.assertIn("scope mismatch", replay_error)
        self.assertEqual(replayed, image)
        self.assertEqual(image[0x330:0x340], hashlib.md5(bootkey).digest())
        self.assertEqual(image[0x7FFFC:0x80000], b"\xFA\x07\xF0\xFF")
        statistics = inspect_statistics(image[0x7FE00:0x80000])
        self.assertEqual(statistics["record_kind"], "installed")
        self.assertEqual(statistics["fields"]["generation"]["value"], 0)
        self.assertEqual(
            statistics["fields"]["descriptor_0_manufacturer_id"]["value"],
            0x20,
        )
        self.assertEqual(
            statistics["fields"]["descriptor_0_device_id"]["value"], 0x17,
        )
        self.assertEqual(recipe["configuration"]["regions"][0]["source"], "official")
        self.assertIn("scope mismatch", "\n".join(recipe["warnings"]))
        self.assertEqual(recipe["output"]["sha256"], _sha256(image))

    def test_logical_eeprom_composition_and_recipe_replay(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            map_path = root / "factory.map"
            map_path.write_bytes(C55_MAP.read_bytes())
            output = root / "factory-eeprom.bin"
            status, _text, error = self.run_cli([
                "assemble", "C55", "--baseline", C55_BASELINE,
                "--eeprom-map", str(map_path),
                "--eeprom-profile", "c55-emulator-minimal-v1",
                "--eeprom-donor", str(C55_DONOR),
                "--eeprom-imei", "35335000894548",
                "--eeprom-fsn", "A35F2F28",
                "--non-interactive", "-o", str(output),
            ])
            recipe_path = output.with_suffix(".json")
            recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
            replay = root / "replay.bin"
            replay_status, _text, replay_error = self.run_cli([
                "assemble", "--recipe", str(recipe_path), "-o", str(replay),
            ])
            map_path.write_text(
                map_path.read_text(encoding="latin-1") + "\n; changed\n",
                encoding="latin-1",
            )
            tampered_status, _text, tampered_error = self.run_cli([
                "assemble", "--recipe", str(recipe_path),
                "-o", str(root / "tampered.bin"),
            ])

            conflict_status, _text, conflict_error = self.run_cli([
                "assemble", "C55", "--baseline", C55_BASELINE,
                "--region", "EEPROM=erased",
                "--eeprom-map", str(map_path), "--non-interactive",
                "-o", str(root / "conflict.bin"),
            ])
            output_bytes = output.read_bytes()
            replay_bytes = replay.read_bytes()

        self.assertEqual(status, 0)
        self.assertIn("c55-emulator-minimal-v1", error)
        self.assertEqual((replay_status, replay_error), (0, error))
        self.assertEqual(replay_bytes, output_bytes)
        selection = recipe["configuration"]["eeprom"]
        self.assertEqual(selection["map"]["sha256"], recipe["operations"][-2]["map"]["sha256"])
        self.assertEqual(selection["profile"], "c55-emulator-minimal-v1")
        self.assertEqual(selection["donor"]["blocks"], [5005, 5006])
        self.assertEqual(selection["identity"]["imei"], "35335000894548")
        self.assertEqual(recipe["identity"]["filename_serial"], "894548")
        self.assertEqual(tampered_status, 1)
        self.assertIn("EEPROM map SHA-256", tampered_error)
        self.assertEqual(conflict_status, 1)
        self.assertIn("conflicts with an explicit EEPROM", conflict_error)

    def test_eeprom_options_require_map_and_paired_identity(self) -> None:
        cases = (
            (["--eeprom-profile", "c55-emulator-minimal-v1"], "require --eeprom-map"),
            (["--eeprom-imei", "35335000894548"], "require --eeprom-map"),
            (["--eeprom-donor", "donor.bin"], "require --eeprom-map"),
            (
                ["--eeprom-map", "factory.map", "--eeprom-fsn", "A35F2F28"],
                "must be supplied together",
            ),
            (
                ["--eeprom-map", "factory.map", "--eeprom-donor", "donor.bin"],
                "requires --eeprom-profile",
            ),
        )
        for options, message in cases:
            with self.subTest(options=options):
                status, _text, error = self.run_cli([
                    "assemble", "C55", "--baseline", C55_BASELINE,
                    "--non-interactive", *options,
                ])
                self.assertEqual(status, 1)
                self.assertIn(message, error)

    def test_m55_schema4_eeprom_replay_and_donor_validation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "m55.bin"
            status, _text, error = self.run_cli([
                "assemble", "M55", "--baseline", str(M55_BASELINE),
                "--eeprom-map", str(M55_MAP),
                "--eeprom-profile", "m55-emulator-minimal-v1",
                "--eeprom-donor", str(M55_BASELINE),
                "--eeprom-imei", "35202600729559",
                "--eeprom-fsn", "C8AAE55F", "--non-interactive",
                "-o", str(output),
            ])
            recipe_path = output.with_suffix(".json")
            recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
            replay = root / "replayed.bin"
            replay_status, _text, replay_error = self.run_cli([
                "assemble", "--recipe", str(recipe_path), "-o", str(replay),
            ])

            tampered_donor = root / "tampered-donor.bin"
            donor_bytes = bytearray(M55_BASELINE.read_bytes())
            donor_bytes[0] ^= 1
            tampered_donor.write_bytes(donor_bytes)
            donor_recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
            donor_recipe["configuration"]["eeprom"]["donor"]["path"] = str(
                tampered_donor
            )
            donor_recipe_path = root / "tampered-donor.json"
            donor_recipe_path.write_text(
                json.dumps(donor_recipe, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            donor_status, _text, donor_error = self.run_cli([
                "assemble", "--recipe", str(donor_recipe_path),
                "-o", str(root / "bad-donor.bin"),
            ])

            hash_recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
            hash_recipe["configuration"]["eeprom"]["sha256"] = "0" * 64
            hash_recipe_path = root / "tampered-eeprom-hash.json"
            hash_recipe_path.write_text(
                json.dumps(hash_recipe, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            hash_status, _text, hash_error = self.run_cli([
                "assemble", "--recipe", str(hash_recipe_path),
                "-o", str(root / "bad-hash.bin"),
            ])
            provenance_recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
            provenance_recipe["configuration"]["eeprom"]["records"][0][
                "provenance"
            ] = "tampered"
            provenance_recipe_path = root / "tampered-provenance.json"
            provenance_recipe_path.write_text(
                json.dumps(provenance_recipe, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            provenance_status, _text, provenance_error = self.run_cli([
                "assemble", "--recipe", str(provenance_recipe_path),
                "-o", str(root / "bad-provenance.bin"),
            ])
            output_bytes = output.read_bytes()
            replay_bytes = replay.read_bytes()

        self.assertEqual(status, 0)
        self.assertIn("m55-emulator-minimal-v1", error)
        self.assertEqual(replay_status, 0)
        self.assertEqual(replay_error, error)
        self.assertEqual(replay_bytes, output_bytes)
        self.assertEqual(recipe["schema_version"], 4)
        selection = recipe["configuration"]["eeprom"]
        self.assertEqual(selection["model"], "M55")
        self.assertEqual(selection["record_count"], 383)
        self.assertEqual(selection["donor"]["blocks"], [5005, 5006])
        self.assertEqual(selection["packing"]["bank_size"], 0x10000)
        self.assertEqual(selection["source_counts"]["profile"], 11)
        self.assertEqual(donor_status, 1)
        self.assertIn("EEPROM donor SHA-256", donor_error)
        self.assertEqual(hash_status, 1)
        self.assertIn("EEPROM SHA-256", hash_error)
        self.assertEqual(provenance_status, 1)
        self.assertIn("composition manifest", provenance_error)

    def test_a55_schema4_eeprom_replay_scope_and_region_confinement(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            control = root / "control.bin"
            control_status, _text, control_error = self.run_cli([
                "assemble", "A55", "--baseline", str(A55_BASELINE),
                "--non-interactive", "-o", str(control),
            ])
            output = root / "a55.bin"
            status, _text, error = self.run_cli([
                "assemble", "A55", "--baseline", str(A55_BASELINE),
                "--eeprom-map", str(A55_MAP),
                "--eeprom-profile", "a55-emulator-minimal-v1",
                "--eeprom-donor", str(A55_BASELINE),
                "--eeprom-imei", "35283800375615",
                "--eeprom-fsn", "AF4E6A0B", "--non-interactive",
                "-o", str(output),
            ])
            recipe = json.loads(
                output.with_suffix(".json").read_text(encoding="utf-8")
            )
            replay = root / "replayed.bin"
            replay_status, _text, replay_error = self.run_cli([
                "assemble", "--recipe", str(output.with_suffix(".json")),
                "-o", str(replay),
            ])

            tampered_map = root / "tampered.map"
            tampered_map.write_bytes(A55_MAP.read_bytes() + b"\n; tampered\n")
            map_status, _text, map_error = self.run_cli([
                "assemble", "A55", "--baseline", str(A55_BASELINE),
                "--eeprom-map", str(tampered_map),
                "--eeprom-profile", "a55-emulator-minimal-v1",
                "--eeprom-donor", str(A55_BASELINE),
                "--non-interactive", "-o", str(root / "bad-map.bin"),
            ])
            tampered_donor = root / "tampered-donor.bin"
            donor_bytes = bytearray(A55_BASELINE.read_bytes())
            donor_bytes[0] ^= 1
            tampered_donor.write_bytes(donor_bytes)
            donor_status, _text, donor_error = self.run_cli([
                "assemble", "A55", "--baseline", str(A55_BASELINE),
                "--eeprom-map", str(A55_MAP),
                "--eeprom-profile", "a55-emulator-minimal-v1",
                "--eeprom-donor", str(tampered_donor),
                "--non-interactive", "-o", str(root / "bad-donor.bin"),
            ])

            control_bytes = control.read_bytes()
            output_bytes = output.read_bytes()
            replay_bytes = replay.read_bytes()

        self.assertEqual((control_status, control_error), (0, ""))
        self.assertEqual(status, 0)
        self.assertIn("a55-emulator-minimal-v1", error)
        self.assertEqual(replay_status, 0)
        self.assertEqual(replay_error, error)
        self.assertEqual(replay_bytes, output_bytes)
        changed = [
            offset for offset, (before, after) in enumerate(
                zip(control_bytes, output_bytes)
            ) if before != after
        ]
        self.assertTrue(changed)
        self.assertGreaterEqual(min(changed), 0x7A0000)
        self.assertLess(max(changed), 0x800000)
        selection = recipe["configuration"]["eeprom"]
        self.assertEqual(recipe["schema_version"], 4)
        self.assertEqual(selection["model"], "A55")
        self.assertEqual(selection["record_count"], 295)
        self.assertEqual(selection["source_counts"], {
            "donor-fullflash": 2,
            "factory-map": 271,
            "generated-identity": 4,
            "profile": 18,
        })
        self.assertEqual(
            selection["donor"]["blocks"], [5005, 5006],
        )
        self.assertEqual(
            [output_bytes[0x7A001C], output_bytes[0x7C001C], output_bytes[0x7E001C]],
            [0xA0, 0xA3, 0xA3],
        )
        self.assertEqual(map_status, 1)
        self.assertIn("EEPROM map SHA-256", map_error)
        self.assertEqual(donor_status, 1)
        self.assertIn("EEPROM donor SHA-256", donor_error)

    def test_stable_tuple_keys_and_tuple_cli_resolution(self) -> None:
        context = load_assembly_context("C55")
        expected = {
            "UNKNOWN_1": "9db6f5d14bef",
            "T9": "60687cc2f261",
            "UNKNOWN_2": "21fb5c368131",
            "LangPack": "569ede8a6f53",
        }
        for role, digest in expected.items():
            with self.subTest(role=role):
                matches = [
                    item for item in region_candidates(context, "official", role)
                    if _tuple_key(context, item) == "249111"
                ]
                self.assertEqual({item.sha256[:12] for item in matches}, {digest})
        selected = _resolve_region_candidate(
            context, "official", "LangPack", "249111", {},
        )
        self.assertTrue(selected.sha256.startswith(expected["LangPack"]))
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "tuple.bin"
            status, _text, error = self.run_cli([
                "assemble", "C55", "--baseline", C55_BASELINE,
                "--region", "LangPack=official:249111",
                "--non-interactive", "-o", str(output),
            ])
            recipe = json.loads(
                output.with_suffix(".json").read_text(encoding="utf-8")
            )
        self.assertEqual((status, error), (0, ""))
        self.assertTrue(
            recipe["configuration"]["regions"][0]["identifier"].startswith(
                expected["LangPack"]
            )
        )

        no_t9 = load_assembly_context("C60")
        candidate = next(
            item for item in region_candidates(no_t9, "community", "UNKNOWN_1")
            if item.scope.get("software_version") is not None
            and item.scope.get("langpack") is not None
        )
        self.assertTrue(_tuple_key(no_t9, candidate).endswith("00"))

    def test_duplicate_tuple_requires_sha_and_uses_sha_submenu(self) -> None:
        context = load_assembly_context("C55")
        with mock.patch("tools.siemens_tools.fullflash.assembly.region_candidates",
                        return_value=ambiguous_t9(context)):
            with self.assertRaisesRegex(FirmwareError, "select a SHA prefix or path"):
                _resolve_region_candidate(
                    context, "community", "T9", "249111", {"software_version": 24},
                )
            selected = _resolve_region_candidate(
                context, "community", "T9", "60687cc2f261",
                {"software_version": 24},
            )
            self.assertTrue(selected.sha256.startswith("60687cc2f261"))

    def test_recipe_scope_hint_disambiguates_shared_official_payload(self) -> None:
        context = load_assembly_context("A65")
        baseline = Baseline({}, b"", {})
        selected = _resolve_explicit_regions(
            context,
            baseline,
            (("UNKNOWN_2", "official", "80ba042be21f"),),
            {"software_version": 17, "langpack": 1, "t9_version": 0},
        )
        self.assertEqual(selected[0].scope["software_version"], 17)
        self.assertEqual(selected[0].scope["langpack"], "lg1")


    def test_exact_erasure_and_baseline_scope_identity(self) -> None:
        baseline = _assemble_request(self.request())
        for role in ("EE_FS", "BCORE", "EEPROM", "UNKNOWN_1"):
            with self.subTest(role=role):
                result = _assemble_request(self.request(
                    region_overrides=((role, "erased", "all-ff"),),
                ))
                erased = result.regions[0]
                self.assertEqual(
                    result.image[erased.start:erased.end],
                    b"\xFF" * (erased.end - erased.start),
                )
                self.assertEqual(result.scope, baseline.scope)
                self.assertEqual(erased.source, "erased")
                self.assertEqual(erased.sha256, _sha256(
                    b"\xFF" * (erased.end - erased.start)
                ))
                if role == "EEPROM":
                    self.assertEqual(result.serial_source, "output-hash-fallback")
                if role == "UNKNOWN_1":
                    kinds = [item["kind"] for item in result.operations]
                    self.assertIn("statistics-checksum-skipped", kinds)
                    self.assertNotIn("statistics-checksum", kinds)
                    entry = result.context.layout.layout.entry_transfer_offset
                    self.assertEqual(result.image[entry:entry + 4], b"\xFF" * 4)
                    self.assertIn("statistics checksum skipped", "\n".join(result.warnings))

        result = _assemble_request(self.request(
            region_overrides=(("BCORE", "erased", "all-ff"),),
            bootkey_action="erase",
            bootkey_source={"kind": "erased"},
        ))
        bcore = result.regions[0]
        self.assertEqual(result.image[bcore.start:bcore.end], b"\xFF" * (bcore.end - bcore.start))

    def test_erased_roles_reject_conflicting_personalization(self) -> None:
        cases = (
            (
                "mobsw",
                self.request(
                    region_overrides=(("UNKNOWN_1", "erased", "all-ff"),),
                    mob_sw=C55_MOBSW,
                ),
            ),
            (
                "flash-id",
                self.request(
                    region_overrides=(("UNKNOWN_1", "erased", "all-ff"),),
                    flash_ids=((0x20, 0x17),),
                ),
            ),
            (
                "entry-target",
                self.request(
                    region_overrides=(("UNKNOWN_1", "erased", "all-ff"),),
                    entry_target=0x07FFF0,
                ),
            ),
            (
                "BOOTKEY",
                self.request(
                    region_overrides=(("BCORE", "erased", "all-ff"),),
                    bootkey_action="write-hash",
                    bootkey_hash=b"\x00" * 16,
                ),
            ),
        )
        for message, request in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(FirmwareError, message):
                    _assemble_request(request)

    def test_erased_recipe_v4_replay_and_schema_v1_v2_v3_compatibility(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            erased_output = root / "erased.bin"
            status, _text, error = self.run_cli([
                "assemble", "C55", "--baseline", C55_BASELINE,
                "--region", "EE_FS=erased", "--non-interactive",
                "-o", str(erased_output),
            ])
            recipe_path = erased_output.with_suffix(".json")
            recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
            replay = root / "erased-replay.bin"
            replay_status, _text, replay_error = self.run_cli([
                "assemble", "--recipe", str(recipe_path), "-o", str(replay),
            ])

            legacy_output = root / "legacy.bin"
            legacy_status, _text, legacy_error = self.run_cli([
                "assemble", "C55", "--baseline", C55_BASELINE,
                "--non-interactive", "-o", str(legacy_output),
            ])
            legacy = json.loads(
                legacy_output.with_suffix(".json").read_text(encoding="utf-8")
            )
            legacy["schema_version"] = 1
            legacy_recipe = root / "legacy-v1.json"
            legacy_recipe.write_text(
                json.dumps(legacy, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            legacy_replay = root / "legacy-replay.bin"
            legacy_replay_status, _text, legacy_replay_error = self.run_cli([
                "assemble", "--recipe", str(legacy_recipe),
                "-o", str(legacy_replay),
            ])
            version_two = dict(legacy)
            version_two["schema_version"] = 2
            version_two_recipe = root / "legacy-v2.json"
            version_two_recipe.write_text(
                json.dumps(version_two, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            version_two_replay = root / "legacy-v2-replay.bin"
            version_two_status, _text, version_two_error = self.run_cli([
                "assemble", "--recipe", str(version_two_recipe),
                "-o", str(version_two_replay),
            ])
            version_three = dict(legacy)
            version_three["schema_version"] = 3
            version_three_recipe = root / "legacy-v3.json"
            version_three_recipe.write_text(
                json.dumps(version_three, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            version_three_replay = root / "legacy-v3-replay.bin"
            version_three_status, _text, version_three_error = self.run_cli([
                "assemble", "--recipe", str(version_three_recipe),
                "-o", str(version_three_replay),
            ])
            erased_image = erased_output.read_bytes()
            replayed_erased_image = replay.read_bytes()
            legacy_image = legacy_output.read_bytes()
            replayed_legacy_image = legacy_replay.read_bytes()
            replayed_version_two_image = version_two_replay.read_bytes()
            replayed_version_three_image = version_three_replay.read_bytes()

        self.assertEqual((status, error), (0, ""))
        self.assertEqual(recipe["schema_version"], 4)
        selection = recipe["configuration"]["regions"][0]
        self.assertEqual(selection["source"], "erased")
        self.assertEqual(selection["identifier"], "all-ff")
        self.assertIn("range", selection)
        self.assertEqual(len(selection["sha256"]), 64)
        self.assertEqual((replay_status, replay_error), (0, ""))
        self.assertEqual(replayed_erased_image, erased_image)
        self.assertEqual((legacy_status, legacy_error), (0, ""))
        self.assertEqual((legacy_replay_status, legacy_replay_error), (0, ""))
        self.assertEqual(replayed_legacy_image, legacy_image)
        self.assertEqual((version_two_status, version_two_error), (0, ""))
        self.assertEqual(replayed_version_two_image, legacy_image)
        self.assertEqual((version_three_status, version_three_error), (0, ""))
        self.assertEqual(replayed_version_three_image, legacy_image)

    def test_recipe_rejects_fresh_assembly_options(self) -> None:
        conflicts = (
            ["C55"],
            ["--baseline", C55_BASELINE],
            ["--region", f"T9=official:{C55_T9}"],
            ["--mobsw", C55_MOBSW],
            ["--flash-id", "0x20:0x17"],
            ["--entry-target", "0x07FFF0"],
            ["--eeprom-map", "defaults.map"],
            ["--eeprom-profile", "c55-emulator-minimal-v1"],
            ["--eeprom-imei", "35335000894548"],
            ["--eeprom-fsn", "A35F2F28"],
            ["--eeprom-donor", "donor.bin"],
            ["--bootkey", "00" * 16],
            ["--fsn", "A35F2F28"],
            ["--skey", "12345678"],
            ["--erase-bootkey"],
            ["--community-catalog", "community"],
            ["--official-catalog", "official"],
            ["--non-interactive"],
        )
        for conflict in conflicts:
            with self.subTest(conflict=conflict[0]):
                status, _text, error = self.run_cli([
                    "assemble", "--recipe", "unused.json", *conflict,
                ])
                self.assertEqual(status, 1)
                self.assertIn("--recipe cannot be combined with", error)

    def test_binary_output_rejects_json_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "synthetic.json"
            with self.assertRaisesRegex(FirmwareError, "must not use the .json suffix"):
                _write_output_pair(output, b"image", {}, False)
            self.assertFalse(output.exists())

    def test_flash_ids_fall_back_to_catalog_for_malformed_statistics(self) -> None:
        baseline = Baseline({
            "metadata": {
                "firmware": {
                    "flash": {
                        "manufacturer_id": "0x0020",
                        "device_id": "0x8810",
                    },
                },
            },
        }, b"", {})
        self.assertEqual(
            _baseline_flash_ids(baseline, b"\xFF" * 0x200),
            ((0x0020, 0x8810),),
        )

    def test_output_hash_identity_fallback(self) -> None:
        self.assertEqual(_imei_serial(b"\xFF" * 0x40000), (
            None, "output-hash-fallback",
        ))

    def test_assembly_parsers(self) -> None:
        self.assertEqual(parse_flash_id("0x20:0x17"), (0x20, 0x17))
        self.assertEqual(parse_bootkey("00" * 16), b"\x00" * 16)
        with self.assertRaises(ValueError):
            parse_flash_id("bad")
        with self.assertRaises(ValueError):
            parse_bootkey("00")

    def test_eeprom_packing_remains_deterministic(self) -> None:
        blocks = {1: b"lite", 5001: b"full"}
        classes = {1: 2, 5001: 8}
        versions = {1: 0, 5001: 2}
        first, description = pack_c55_eeprom(blocks, classes, versions)
        second, _ = pack_c55_eeprom(blocks, classes, versions)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 0x60000)
        self.assertEqual(
            [bank["records"] for bank in description["banks"]], [1, 1, 0]
        )

        m55, m55_description = pack_m55_eeprom(blocks, classes, versions)
        self.assertEqual(
            [m55[0x8C], m55[0x1008C], m55[0x2008C]],
            [0xA1, 0xAA, 0xA9],
        )
        self.assertEqual(m55_description["format"], "M55 EELITE/EEFULL")
        c60, c60_description = pack_c60_eeprom(blocks, classes, versions)
        self.assertEqual(
            [c60[0x8C], c60[0x1008C], c60[0x2008C]],
            [0xA0, 0xA2, 0xA2],
        )
        self.assertEqual(c60_description["format"], "C60 EELITE/EEFULL")

        with self.assertRaises(FirmwareError):
            pack_c55_eeprom(blocks, classes, {})
        with self.assertRaises(FirmwareError):
            pack_c55_eeprom(blocks, classes, {1: 0, 5001: 256})

if __name__ == "__main__":
    unittest.main()
