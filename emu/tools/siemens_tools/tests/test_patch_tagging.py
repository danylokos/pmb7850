from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.siemens_tools.firmware.xbi import FirmwareError
from tools.siemens_tools.fullflash.catalog_info import catalog_info
from tools.siemens_tools.fullflash.patch_tagging import (
    PatchDefinition,
    _operations,
    _print_plan,
    apply_patch_tag_plan,
    command_tag_patches,
    parse_vkp,
    patched_payload_path,
    plan_patch_tags,
)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


class SyntheticCorpus:
    def __init__(
        self,
        root: Path,
        *,
        dump_data: bytes = b"\xAA\x20\xFF\xFF",
        official_data: bytes = b"\x10\x20\xFF\xFF",
        t9_version: int | None = 1,
        create_collision: bool = False,
    ) -> None:
        self.root = root
        self.dump_root = root / "community"
        self.official_root = root / "official"
        self.dump_catalog = self.dump_root / "catalog.json"
        self.official_catalog = self.official_root / "catalog.json"
        self.archive = root / "patches.zip"
        self.definition = PatchDefinition(
            "T55",
            1,
            7,
            "T55v1/7-test.vkp",
            alternatives=((1, (0x20, 0x30)),),
        )
        self.dump_digest = _sha256(dump_data)
        self.official_digest = _sha256(official_data)
        self.fullflash_digest = _sha256(dump_data)
        self.package_digest = "1" * 64
        self.old_payload = f"1/lg1/01/fw/{self.dump_digest[:12]}.bin"
        self.new_payload = f"1/lg1/01/fw/patched-1-{self.dump_digest[:12]}.bin"
        dump_payload = self.dump_root / self.old_payload
        dump_payload.parent.mkdir(parents=True, exist_ok=True)
        dump_payload.write_bytes(dump_data)
        official_payload = (
            self.official_root
            / f"1/lg1/01/fw/{self.official_digest[:12]}.bin"
        )
        official_payload.parent.mkdir(parents=True, exist_ok=True)
        official_payload.write_bytes(official_data)
        if create_collision:
            collision = self.dump_root / self.new_payload
            collision.parent.mkdir(parents=True, exist_ok=True)
            collision.write_bytes(b"collision")

        layout = {
            "name": "T55",
            "base": 0,
            "length": len(dump_data),
            "catalog_path": "layout.yaml",
            "catalog_sha256": "2" * 64,
        }
        scope = {
            "software_version": 1,
            "software_version_source": "test",
            "langpack": "lg1",
            "t9_version": t9_version,
            "evidence_source": "test",
        }
        dump_recipe_path = Path("fullflashes") / f"{self.fullflash_digest[:12]}.json"
        dump_recipe = {
            "schema": "siemens-fullflash-recipe",
            "schema_version": 4,
            "layout": layout,
            "fullflash": {
                "sha256": self.fullflash_digest,
                "size": len(dump_data),
                "source_id": "source-1",
                "source_path": "fixture.bin",
            },
            "slices": [{
                "order": 0,
                "role": "FW",
                "size": len(dump_data),
                "sha256": self.dump_digest,
                "source_sha256": self.dump_digest,
                "payload_path": self.old_payload,
                "source_range": {
                    "from": 0,
                    "to_exclusive": len(dump_data),
                    "length": len(dump_data),
                },
            }],
        }
        _write_json(self.dump_root / dump_recipe_path, dump_recipe)
        dump_catalog = {
            "schema": "siemens-community-fullflash-corpus",
            "schema_version": 14,
            "layout": layout,
            "summary": {
                "official_backed_variants": 0,
                "official_backed_paths": 0,
                "official_backed_bytes": 0,
            },
            "artifacts": [{
                "id": "source-1",
                "kind": "complete-fullflash",
                "path": "fixture.bin",
                "size": len(dump_data),
                "sha256": self.fullflash_digest,
                "recipe": {
                    "path": dump_recipe_path.as_posix(),
                    "status": "materialized",
                },
            }],
            "occurrences": [{
                "id": "occurrence-1",
                "source_id": "source-1",
                "source_kind": "complete-fullflash",
                "role": "FW",
                "size": len(dump_data),
                "sha256": self.dump_digest,
                "source_sha256": self.dump_digest,
                "payload_path": self.old_payload,
                "scope": scope,
                "source_range": {
                    "from": 0,
                    "to_exclusive": len(dump_data),
                    "length": len(dump_data),
                },
            }],
            "regions": [{
                "role": "FW",
                "variants": [{
                    "size": len(dump_data),
                    "sha256": self.dump_digest,
                    "payload_path": self.old_payload,
                    "symlink_paths": [],
                }],
            }],
        }
        _write_json(self.dump_catalog, dump_catalog)

        official_payload_path = (
            f"1/lg1/01/fw/{self.official_digest[:12]}.bin"
        )
        official_recipe_path = (
            Path("packages") / f"{self.package_digest[:12]}.json"
        )
        official_recipe = {
            "schema": "siemens-official-package-recipe",
            "schema_version": 2,
            "package": {"sha256": self.package_digest},
            "operations": [{
                "order": 0,
                "action": "replace",
                "role": "FW",
                "size": len(official_data),
                "sha256": self.official_digest,
                "source_sha256": self.official_digest,
                "payload_path": official_payload_path,
                "range": {
                    "from": 0,
                    "to_exclusive": len(official_data),
                    "length": len(official_data),
                },
            }],
        }
        _write_json(self.official_root / official_recipe_path, official_recipe)
        official_layout = dict(layout)
        official_layout["catalog_path"] = "other/layout.yaml"
        official_catalog = {
            "schema": "siemens-official-corpus",
            "schema_version": 9,
            "layout": official_layout,
            "summary": {},
            "packages": [{
                "sha256": self.package_digest,
                "recipe": {
                    "path": official_recipe_path.as_posix(),
                    "status": "materialized",
                },
                "regions": [{
                    "role": "FW",
                    "sha256": self.official_digest,
                    "coverage": "full",
                }],
            }],
            "regions": [{
                "role": "FW",
                "variants": [{
                    "size": len(official_data),
                    "sha256": self.official_digest,
                    "payload_path": official_payload_path,
                    "symlink_paths": [],
                    "occurrences": [{
                        "package_sha256": self.package_digest,
                        "role": "FW",
                        "sha256": self.official_digest,
                        "scope": scope,
                    }],
                }],
            }],
        }
        _write_json(self.official_catalog, official_catalog)
        index = {
            "T55v1": {
                "7": {
                    "id": 7,
                    "model": "T55v1",
                    "file": "T55v1/7-test.vkp",
                    "title": {"en": "Test patch"},
                },
            },
        }
        with zipfile.ZipFile(self.archive, "w") as archive:
            archive.writestr("patches/index.json", json.dumps(index))
            archive.writestr(
                "patches/T55v1/7-test.vkp",
                b"; fixture\n000000: 10 AA\n000001: 20 ??\n",
            )

    def plan(self):
        return plan_patch_tags(
            model="T55",
            dump_catalog_path=self.dump_catalog,
            official_catalog_path=self.official_catalog,
            archive_path=self.archive,
            definitions=(self.definition,),
        )

    def snapshot(self) -> dict[str, tuple[str, bytes]]:
        result = {}
        for path in sorted(self.root.rglob("*")):
            if path.is_file():
                result[path.relative_to(self.root).as_posix()] = (
                    "symlink" if path.is_symlink() else "file",
                    path.read_bytes(),
                )
        return result


class VkpParsingTests(unittest.TestCase):
    def test_parses_active_records_and_placeholders(self) -> None:
        records = parse_vkp(
            b"; comment\n000010: AABB CCDD ; inline\n000020: 11 ??\n"
        )
        self.assertEqual(records[0].address, 0x10)
        self.assertEqual(records[0].old, bytes.fromhex("AABB"))
        self.assertEqual(records[0].new, (0xCC, 0xDD))
        self.assertEqual(records[1].new, (None,))

    def test_rejects_malformed_or_unequal_records(self) -> None:
        for value in (
            b"000010: AA B\n",
            b"000010: AABB CC\n",
            b"000010: AA G1\n",
        ):
            with self.subTest(value=value), self.assertRaises(FirmwareError):
                parse_vkp(value)

    def test_alternatives_are_bounded(self) -> None:
        definition = PatchDefinition(
            "T55", 1, 1, "T55v1/test.vkp",
            alternatives=((0x10, (0x20, 0x30)),),
        )
        operations = _operations(
            definition, parse_vkp(b"000010: 20 ??\n")
        )
        self.assertEqual(operations[0].new, (frozenset((0x20, 0x30)),))
        with self.assertRaises(FirmwareError):
            _operations(
                PatchDefinition("T55", 1, 1, "T55v1/test.vkp"),
                parse_vkp(b"000010: 20 XX\n"),
            )

    def test_c55_relocation_override_is_exact(self) -> None:
        body = (
            b"612092: "
            + b"FF" * 26
            + b" 46FC09002D0746FC0A002D068880F08CFA94E693FA84180DDB00\n"
        )
        records = parse_vkp(
            body + b"1493E2: 8880F08C FAE19220\n"
        )
        definition = PatchDefinition(
            "C55", 24, 2406, "C55v24/test.vkp",
            override="c55-black-list-relocation",
        )
        operations = _operations(definition, records)
        self.assertEqual(operations[0].old_address, 0x0B6EC0)
        self.assertEqual(operations[0].new_address, 0x0B6EC0)
        self.assertEqual(
            bytes(next(iter(value)) for value in operations[1].new),
            bytes.fromhex("FA8BC06E"),
        )
        changed = list(records)
        changed[0] = type(records[0])(
            records[0].line, records[0].address, b"\x00" * 26, records[0].new
        )
        with self.assertRaises(FirmwareError):
            _operations(definition, tuple(changed))


class NamingTests(unittest.TestCase):
    def test_name_uses_payload_patch_count_and_hash(self) -> None:
        self.assertEqual(
            patched_payload_path("1/fw/abc.bin", 2, "a" * 64),
            "1/fw/patched-2-aaaaaaaaaaaa.bin",
        )
        self.assertEqual(
            patched_payload_path(
                "1/fw/patched-2-aaaaaaaaaaaa.bin", 2, "a" * 64
            ),
            "1/fw/patched-2-aaaaaaaaaaaa.bin",
        )

    def test_rejects_invalid_count_hash_and_overlong_names(self) -> None:
        with self.assertRaises(FirmwareError):
            patched_payload_path("abc.bin", 0, "a" * 64)
        with self.assertRaises(FirmwareError):
            patched_payload_path("abc.bin", 1, "not-a-hash")


class PlanningTests(unittest.TestCase):
    def test_two_sided_match_and_reference_plan(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(Path(directory))
            plan = fixture.plan()
            self.assertEqual(len(plan.matches), 1)
            self.assertEqual(plan.rejections, [])
            self.assertEqual(plan.affected_fullflashes, [
                fixture.fullflash_digest
            ])
            self.assertEqual(
                [(move.old_path, move.new_path) for move in plan.payload_moves],
                [(fixture.old_payload, fixture.new_payload)],
            )
            pointers = {change.pointer for change in plan.reference_changes}
            self.assertIn("regions[0].variants[0].payload_path", pointers)
            self.assertIn("occurrences[0].payload_path", pointers)
            self.assertIn("slices[0].payload_path", pointers)
            self.assertEqual(len(plan.analyses), 1)
            analysis = plan.analyses[0]
            self.assertEqual(
                [
                    f"{item['archive_key']}/{item['patch_id']}"
                    for item in analysis["identified_patches"]
                ],
                ["T55v1/7"],
            )
            self.assertNotIn("tag", analysis["identified_patches"][0])
            self.assertEqual(analysis["unidentified_regions"], [])
            updated_artifact = plan.dump_catalog["artifacts"][0]
            self.assertEqual(
                updated_artifact["metadata"]["patch_analysis"][
                    "baseline"
                ]["package_sha256"],
                fixture.package_digest,
            )
            self.assertEqual(
                plan.dump_catalog["occurrences"][0]["patch_analysis"],
                {"identified_patch_ids": ["T55v1/7"],
                 "unidentified_region_ids": []},
            )

    def test_unknown_only_payload_is_indexed_but_not_renamed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(Path(directory))
            dump_extra = b"\x01\x02"
            official_extra = b"\xFF\xFF"
            dump_digest = _sha256(dump_extra)
            official_digest = _sha256(official_extra)
            dump_path = f"1/lg1/01/langpack/{dump_digest[:12]}.bin"
            official_path = f"1/lg1/01/langpack/{official_digest[:12]}.bin"
            (fixture.dump_root / dump_path).parent.mkdir(parents=True)
            (fixture.dump_root / dump_path).write_bytes(dump_extra)
            (fixture.official_root / official_path).parent.mkdir(parents=True)
            (fixture.official_root / official_path).write_bytes(official_extra)

            dump_catalog = json.loads(fixture.dump_catalog.read_text())
            scope = dict(dump_catalog["occurrences"][0]["scope"])
            dump_catalog["occurrences"].append({
                "id": "occurrence-2",
                "source_id": "source-1",
                "source_kind": "complete-fullflash",
                "role": "LangPack",
                "size": 2,
                "sha256": dump_digest,
                "source_sha256": dump_digest,
                "payload_path": dump_path,
                "scope": scope,
                "source_range": {"from": 4, "to_exclusive": 6, "length": 2},
            })
            dump_catalog["regions"].append({
                "role": "LangPack",
                "variants": [{
                    "size": 2,
                    "sha256": dump_digest,
                    "payload_path": dump_path,
                    "symlink_paths": [],
                }],
            })
            _write_json(fixture.dump_catalog, dump_catalog)
            dump_recipe_path = fixture.dump_root / dump_catalog["artifacts"][0][
                "recipe"
            ]["path"]
            dump_recipe = json.loads(dump_recipe_path.read_text())
            dump_recipe["slices"].append({
                "order": 1,
                "role": "LangPack",
                "size": 2,
                "sha256": dump_digest,
                "source_sha256": dump_digest,
                "payload_path": dump_path,
                "source_range": {"from": 4, "to_exclusive": 6, "length": 2},
            })
            _write_json(dump_recipe_path, dump_recipe)

            official_catalog = json.loads(fixture.official_catalog.read_text())
            official_catalog["regions"].append({
                "role": "LangPack",
                "variants": [{
                    "size": 2,
                    "sha256": official_digest,
                    "payload_path": official_path,
                    "symlink_paths": [],
                    "occurrences": [{
                        "package_sha256": fixture.package_digest,
                        "role": "LangPack",
                        "sha256": official_digest,
                        "scope": scope,
                    }],
                }],
            })
            _write_json(fixture.official_catalog, official_catalog)
            official_recipe_path = (
                fixture.official_root
                / official_catalog["packages"][0]["recipe"]["path"]
            )
            official_recipe = json.loads(official_recipe_path.read_text())
            official_recipe["operations"].append({
                "order": 1,
                "action": "replace",
                "role": "LangPack",
                "size": 2,
                "sha256": official_digest,
                "source_sha256": official_digest,
                "payload_path": official_path,
                "range": {"from": 4, "to_exclusive": 6, "length": 2},
            })
            _write_json(official_recipe_path, official_recipe)

            plan = fixture.plan()
            self.assertEqual(
                [move.old_path for move in plan.payload_moves],
                [fixture.old_payload],
            )
            self.assertEqual(
                plan.dump_catalog["occurrences"][1]["patch_analysis"],
                {
                    "identified_patch_ids": [],
                    "unidentified_region_ids": ["unknown-001"],
                },
            )
            self.assertEqual(
                plan.dump_catalog["regions"][1]["variants"][0]["payload_path"],
                dump_path,
            )

    def test_unidentified_regions_are_contiguous_and_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(
                Path(directory),
                dump_data=b"\xAA\x20" + b"\x00" * 70,
                official_data=b"\x10\x20" + b"\xFF" * 70,
            )
            unknown = fixture.plan().analyses[0]["unidentified_regions"]
            self.assertEqual(len(unknown), 1)
            region = unknown[0]
            self.assertEqual(region["payload_range"], {
                "from": 2, "to_exclusive": 72, "length": 70,
            })
            self.assertNotIn("old_bytes", region)
            self.assertNotIn("old_sha256", region)
            self.assertNotIn("new_sha256", region)
            self.assertEqual(len(region["old_prefix"]), 64)
            self.assertEqual(len(region["old_suffix"]), 64)

    def test_short_unidentified_region_keeps_exact_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(
                Path(directory), dump_data=b"\xAA\x20\x01\x02"
            )
            unknown = fixture.plan().analyses[0]["unidentified_regions"]
            self.assertEqual(len(unknown), 1)
            self.assertEqual(unknown[0]["old_bytes"], "ffff")
            self.assertEqual(unknown[0]["new_bytes"], "0102")
            self.assertNotIn("old_sha256", unknown[0])
            self.assertNotIn("new_sha256", unknown[0])

    def test_obsolete_catalog_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(Path(directory))
            catalog = json.loads(fixture.dump_catalog.read_text())
            catalog["schema_version"] = 12
            _write_json(fixture.dump_catalog, catalog)
            with self.assertRaisesRegex(FirmwareError, "schema-14"):
                fixture.plan()

    def test_requires_old_bytes_in_one_official_package(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(
                Path(directory), official_data=b"\x11\x20\xFF\xFF"
            )
            plan = fixture.plan()
            self.assertEqual(plan.matches, [])
            self.assertEqual(len(plan.rejections), 1)
            self.assertIn("official package", plan.rejections[0].reason)

    def test_rejects_unknown_scope(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(Path(directory), t9_version=None)
            plan = fixture.plan()
            self.assertEqual(plan.matches, [])
            self.assertEqual(len(plan.rejections), 1)
            self.assertIn("unknown community scope", plan.rejections[0].reason)

    def test_accepts_documented_configured_alternative(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(
                Path(directory), dump_data=b"\xAA\x30\xFF\xFF"
            )
            self.assertEqual(len(fixture.plan().matches), 1)

    def test_collision_fails_planning(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(
                Path(directory), create_collision=True
            )
            with self.assertRaises(FirmwareError):
                fixture.plan()

    def test_dry_run_is_immutable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(Path(directory))
            before = fixture.snapshot()
            args = argparse.Namespace(
                model="T55",
                catalog=fixture.dump_root,
                official_catalog=fixture.official_root,
                patch_archive=fixture.archive,
                dry_run=True,
            )
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                command_tag_patches(args)
                _print_plan(fixture.plan(), True)
            self.assertIn('patch=T55v1/7 title="Test patch"', output.getvalue())
            self.assertIn("T55v1/7: Test patch", output.getvalue())
            self.assertEqual(fixture.snapshot(), before)

    def test_application_uses_the_validated_plan(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(Path(directory))
            plan = fixture.plan()
            apply_patch_tag_plan(plan)
            self.assertFalse((fixture.dump_root / fixture.old_payload).exists())
            self.assertTrue((fixture.dump_root / fixture.new_payload).is_file())
            catalog = json.loads(fixture.dump_catalog.read_text())
            self.assertEqual(catalog["schema_version"], 14)
            self.assertEqual(
                catalog["regions"][0]["variants"][0]["payload_path"],
                fixture.new_payload,
            )
            recipe_path = next(iter(plan.recipes))
            recipe = json.loads(recipe_path.read_text())
            self.assertEqual(
                recipe["slices"][0]["payload_path"], fixture.new_payload
            )

    def test_retag_removes_stale_names_and_analysis(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(Path(directory))
            apply_patch_tag_plan(fixture.plan())
            with zipfile.ZipFile(fixture.archive, "w") as archive:
                archive.writestr("patches/index.json", "{}")

            plan = plan_patch_tags(
                model="T55",
                dump_catalog_path=fixture.dump_catalog,
                official_catalog_path=fixture.official_catalog,
                archive_path=fixture.archive,
                definitions=(),
            )
            apply_patch_tag_plan(plan)

            self.assertTrue(
                (fixture.dump_root / fixture.old_payload).is_file()
            )
            self.assertFalse(
                (fixture.dump_root / fixture.new_payload).exists()
            )
            catalog = json.loads(fixture.dump_catalog.read_text())
            self.assertEqual(
                catalog["regions"][0]["variants"][0]["payload_path"],
                fixture.old_payload,
            )
            self.assertNotIn(
                "patch_analysis", catalog["artifacts"][0]["metadata"]
            )
            self.assertNotIn("patch_analysis", catalog["occurrences"][0])
    def test_catalog_info_resolves_hash_recipe_and_payload(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            fixture = SyntheticCorpus(Path(directory))
            apply_patch_tag_plan(fixture.plan())
            for identifier in (
                fixture.fullflash_digest[:12],
                f"fullflashes/{fixture.fullflash_digest[:12]}.json",
                fixture.new_payload,
            ):
                with self.subTest(identifier=identifier):
                    info = catalog_info(fixture.dump_root, identifier)
                    self.assertEqual(
                        info["catalog"], fixture.dump_catalog.resolve().as_posix()
                    )
                    self.assertEqual(len(info["matches"]), 1)
                    self.assertEqual(
                        info["matches"][0]["patch_analysis"][
                            "identified_patches"
                        ][0]["title"], "Test patch"
                    )

            with self.assertRaisesRegex(FirmwareError, "not a directory"):
                catalog_info(fixture.dump_catalog, fixture.fullflash_digest[:12])

            missing_root = fixture.root / "missing"
            missing_root.mkdir()
            with self.assertRaisesRegex(FirmwareError, "catalog is missing"):
                catalog_info(missing_root, fixture.fullflash_digest[:12])

            catalog = json.loads(fixture.dump_catalog.read_text())
            catalog["schema_version"] = 10
            _write_json(fixture.dump_catalog, catalog)
            with self.assertRaisesRegex(FirmwareError, "not schema 14"):
                catalog_info(fixture.dump_root, fixture.fullflash_digest[:12])


if __name__ == "__main__":
    unittest.main()
