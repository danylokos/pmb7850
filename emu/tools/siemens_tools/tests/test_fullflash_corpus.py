#!/usr/bin/env python3
"""Fullflash corpus discovery, validation, and grouping tests."""

from __future__ import annotations


import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from tools.siemens_tools import fullflash as ff
from tools.siemens_tools import layout as lt
from tools.siemens_tools.fullflash.cli import main as fullflash_main
from tools.siemens_tools.fullflash.corpus_scope import (
    embedded_t9_version,
    payload_paths,
    resolve_scope_references,
    scoped_payload_plan,
    select_payload_path,
)
from tools.siemens_tools.fullflash.corpus import (
    _eeprom_snr_label,
    _langpack_lg,
    _official_attribution,
    _short_hashes,
    _software_version,
    build_corpus,
    discover_sources,
    render_report,
)
from tools.siemens_tools.fullflash.catalog_info import catalog_info
from tools.siemens_tools.fullflash.reconstruct import reconstruct_recipe
from tools.siemens_tools.fullflash.official_corpus import build_official_corpus
from tools.siemens_tools.tests.firmware_fixtures import build_xbi


def _write_bcore(
    data: bytearray,
    software: int = 0x24,
    field: bytes = bytes(range(16)),
    model: bytes = b"C55",
) -> None:
    data[:4] = bytes.fromhex("FA 80 34 12")
    data[0x1234] = 0
    data[0x300:0x30C] = bytes.fromhex(
        "00 01 4C 53 01 00 00 01 70 01 80 00"
    )
    data[0x30C:0x31C] = model + b"\0" * (16 - len(model))
    data[0x31C:0x324] = b"SIEMENS\0"
    data[0x32C] = software
    data[0x330:0x340] = field


def _write_metadata(
    data: bytearray, software: int = 0x24, langpack: bytes = b"lg1",
    model: bytes = b"C55",
) -> None:
    offset = 0x7FF50
    data[offset:offset + 16] = bytes((
        software, 0xFF, 0x0A, 0x50, 0x14, 0x14, 0x01, 0x00,
        0xD5, 0x21, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    ))
    data[offset + 0x10:offset + 0x20] = (
        langpack + b"\0" * (16 - len(langpack))
    )
    data[offset + 0x20:offset + 0x30] = (
        model + b"\0" * (16 - len(model))
    )
    data[offset + 0x30:offset + 0x38] = b"SIEMENS\0"
    data[0x7FE26:0x7FE2A] = bytes.fromhex("20 00 17 00")


def _write_langpack(
    data: bytearray, start: int, langpack: bytes = b"lg91",
) -> None:
    data[start:start + 2] = b"\xBB\xBB"
    field = b"@" + langpack + b"\0"
    data[start + 0x15:start + 0x20] = field + b"\xFF" * (11 - len(field))


def _synthetic_sources(root: Path) -> tuple[Path, bytes]:
    layout = lt.load_layout("C55").layout
    image = bytearray(b"\xFF") * layout.length
    _write_bcore(image, software=0x18)
    _write_metadata(image)
    t9_header = bytes.fromhex(
        "54 39 85 90 98 62 03 00 0B 02 53 39 01 01 00 00 "
        "00 00 00 00 05 01 00 00 00 00 00 00 06 01 00 00 "
        "00 00 00 00 07 01 00 00 00 00 00 00 08 01 00 00 "
        "00 00 00 00 09 01 00 00 BC 00 00 00 0A 01 00 00"
    )
    t9_start = layout.region("T9").offset
    image[t9_start:t9_start + len(t9_header)] = t9_header
    eeprom, _manifest = ff.pack_c55_eeprom(
        {1: b"corpus"}, {1: 2}, {1: 1}
    )
    eeprom_region = layout.region("EEPROM")
    image[eeprom_region.offset:eeprom_region.end] = eeprom

    source_root = root / "Fullflash" / "C55"
    _write_langpack(image, layout.region("LangPack").offset)
    source_root.mkdir(parents=True)
    fullflash = source_root / "complete.bin"
    fullflash.write_bytes(image)
    (source_root / "bcore.bin").write_bytes(
        image[layout.region("BCORE").offset:layout.region("BCORE").end]
    )
    (source_root / "t9.bin").write_bytes(
        image[layout.region("T9").offset:layout.region("T9").end]
    )
    (source_root / "eeprom.bin").write_bytes(eeprom)
    (source_root / "t9-small.bin").write_bytes(b"T9" + b"\xFF" * 0x1FFFE)
    generated = source_root / "old.split"
    generated.mkdir()
    (generated / "ignored.bin").write_bytes(bytes(image))
    (root / "FULLFLASH_COLLECTION.json").write_text(json.dumps({
        "artifacts": [{
            "canonical_path": "C55/complete.bin",
            "role": "fullflash",
            "software_version": 24,
            "provenance": [{
                "source_archive": "fixture.zip",
                "member_name": "complete.bin",
            }],
        }],
    }))
    return source_root, bytes(image)


def _materialize_official_catalog(
    root: Path, inputs: list[Path], loaded: object,
) -> Path:
    document = build_official_corpus(
        inputs, loaded, materialize=True, corpus_root=root
    )
    path = root / "catalog.json"
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
    return path


def _empty_official_catalog(root: Path, loaded: object) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    path = root / "catalog.json"
    path.write_text(json.dumps({
        "schema": "siemens-official-corpus",
        "schema_version": 9,
        "layout": {
            "name": loaded.layout.name,
            "base": loaded.layout.base,
            "length": loaded.layout.length,
            "catalog_sha256": hashlib.sha256(loaded.catalog_bytes).hexdigest(),
        },
        "summary": {},
        "lg_evidence": [],
        "packages": [],
        "regions": [],
    }, indent=2, sort_keys=True) + "\n")
    return path


class FullflashCorpusTests(unittest.TestCase):
    @staticmethod
    def run_main(argv: list[str]) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            status = fullflash_main(argv)
        return status, stdout.getvalue(), stderr.getvalue()

    def test_discovery_grouping_canonical_store_and_recipe_reuse(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            generated = source_root / "corpus" / "C55"
            generated.mkdir(parents=True)
            (generated / "generated.bin").write_bytes(image)

            discovered = discover_sources([source_root])
            self.assertEqual(len(discovered), 5)
            self.assertFalse(any("old.split" in str(path) for path in discovered))

            loaded = lt.load_layout("C55")
            corpus_root = root / "corpus" / "C55"
            first = build_corpus(
                [source_root], loaded, write_splits=True,
                corpus_root=corpus_root,
            )
            self.assertEqual(first["schema"], "siemens-community-fullflash-corpus")
            self.assertEqual(first["schema_version"], 14)
            self.assertEqual(first["summary"]["source_artifacts"], 4)
            self.assertEqual(first["summary"]["complete_fullflashes"], 1)
            self.assertEqual(first["summary"]["standalone_regions"], 3)
            self.assertEqual(first["summary"]["region_occurrences"], 13)
            self.assertEqual(first["summary"]["role_scoped_variants"], 10)
            self.assertEqual(first["summary"]["regular_payload_files"], 5)
            self.assertEqual(first["summary"]["payload_symlinks"], 0)
            self.assertEqual(first["summary"]["payload_paths"], 5)
            self.assertEqual(
                first["summary"]["regular_payload_bytes"], 0x1a0000
            )
            self.assertEqual(first["summary"]["fullflash_recipes"], 1)
            self.assertEqual(
                {item["role"] for item in first["artifacts"]
                 if item["kind"] == "standalone-region"},
                {"BCORE", "T9", "EEPROM"},
            )
            self.assertEqual(first["excluded"][0]["reason"],
                             "placement-unresolved")
            complete = next(
                item for item in first["artifacts"]
                if item["kind"] == "complete-fullflash"
            )
            self.assertEqual(
                complete["collection"]["provenance"][0]["source_archive"],
                "fixture.zip",
            )
            self.assertEqual(
                complete["recipe"]["reset_ownership"],
                {
                    "storage": "low-alias",
                    "layout_offset": 0,
                    "owning_role": "BCORE",
                    "owning_payload_path": next(
                        region["variants"][0]["payload_path"]
                        for region in first["regions"]
                        if region["role"] == "BCORE"
                    ),
                    "duplicates_payload_bytes": False,
                },
            )

            recipe_path = corpus_root / complete["recipe"]["path"]
            recipe_document = json.loads(recipe_path.read_text())
            self.assertEqual(recipe_document["schema_version"], 4)
            erased_variants = [
                variant
                for region in first["regions"]
                for variant in region["variants"]
                if variant["erased"]
            ]
            self.assertEqual(len(erased_variants), 5)
            self.assertTrue(all(
                "payload_path" not in variant
                and "symlink_paths" not in variant
                for variant in erased_variants
            ))
            erased_slices = [
                item for item in recipe_document["slices"]
                if item.get("erased") is True
            ]
            self.assertEqual(len(erased_slices), 5)
            self.assertTrue(all(
                "payload_path" not in item and "normalization" not in item
                for item in erased_slices
            ))
            self.assertEqual(
                first["summary"]["implicit_erased_role_variants"], 5
            )
            self.assertEqual(
                first["summary"]["implicit_erased_occurrences"], 5
            )
            bcore_recipe = recipe_document["slices"][0]
            self.assertEqual(
                bcore_recipe["normalization"]["stored_value"],
                bytes(range(16)).hex(),
            )
            self.assertEqual(
                (corpus_root / bcore_recipe["payload_path"]).read_bytes()[
                    0x330:0x340
                ],
                b"\xFF" * 16,
            )
            reconstructed = reconstruct_recipe(recipe_path)
            self.assertEqual(reconstructed, image)
            self.assertEqual(
                hashlib.sha256(reconstructed).hexdigest(),
                complete["sha256"],
            )
            stored_paths = {
                item["payload_path"]
                for item in first["occurrences"]
                if not item["erased"]
            }
            self.assertEqual(len(stored_paths), 5)
            self.assertTrue(
                any(path.startswith("18/bcore/") for path in stored_paths)
            )
            self.assertTrue(
                any(path.startswith("24/lg91/11/t9/") for path in stored_paths)
            )
            self.assertTrue(
                any(path.startswith("24/lg1/11/eeprom/")
                    for path in stored_paths)
            )
            for occurrence in first["occurrences"]:
                if occurrence["role"] == "EEPROM":
                    self.assertRegex(
                        Path(occurrence["payload_path"]).name,
                        r"^[0-9a-f]{12,64}\.bin$",
                    )
            for relative in stored_paths:
                self.assertTrue((corpus_root / relative).is_file())
                self.assertGreaterEqual(len(Path(relative).parts), 3)
                self.assertFalse(Path(relative).is_absolute())

            second = build_corpus(
                [source_root], loaded, write_splits=True,
                corpus_root=corpus_root,
            )
            self.assertEqual(first, second)

            payload = corpus_root / next(iter(stored_paths))
            payload.write_bytes(b"stale")
            with self.assertRaisesRegex(lt.FirmwareError, "already exists"):
                build_corpus(
                    [source_root], loaded, write_splits=True,
                    corpus_root=corpus_root,
                )
            repaired = build_corpus(
                [source_root], loaded, write_splits=True,
                corpus_root=corpus_root, force=True,
            )
            self.assertEqual(first, repaired)
            self.assertEqual(reconstruct_recipe(recipe_path), image)

    def test_eeprom_snr_label_uses_six_digit_imei_field(self) -> None:
        metadata = {"eeprom": {"imeis": {
            "76": "35159000001234",
            "5009": "35159000001234",
        }}}
        self.assertEqual(_eeprom_snr_label(metadata), "001234")
        self.assertEqual(
            _eeprom_snr_label({
                "eeprom": {"imeis": {"5009": "35101137339709"}}
            }),
            "339709",
        )
        self.assertIsNone(_eeprom_snr_label({"eeprom": {"imeis": {}}}))
        self.assertIsNone(_eeprom_snr_label({
            "eeprom": {"imeis": {"5009": "not-an-imei"}}
        }))
        self.assertIsNone(_eeprom_snr_label({
            "eeprom": {"imeis": {
                "76": "35101137339709",
                "5009": "35101151975996",
            }}
        }))
        self.assertIsNone(_eeprom_snr_label({
            "eeprom": {"imeis": {
                "76": "35101137339709",
                "5009": "broken",
            }}
        }))

    def test_t9_header_padding_and_strict_selection(self) -> None:
        payload = bytearray(b"\xFF" * 0x20)
        payload[:14] = bytes.fromhex(
            "54 39 85 90 98 62 03 00 09 02 53 39 01 01"
        )
        payload[14:22] = b"\0" * 8
        self.assertEqual(embedded_t9_version(bytes(payload)), 9)
        self.assertIsNone(embedded_t9_version(b"X9" + bytes(payload[2:])))
        payload[8] = 0xFF
        self.assertIsNone(embedded_t9_version(bytes(payload)))
        payload[8] = 9
        payload[10:14] = b"bad!"
        self.assertIsNone(embedded_t9_version(bytes(payload)))
        payload[10:14] = b"S9\x19\x01"
        self.assertEqual(embedded_t9_version(bytes(payload)), 9)

        storage, paths_by_scope = scoped_payload_plan(
            "abc123",
            [
                {"software_version": 24, "langpack": "lg1",
                 "t9_version": 1, "evidence_source": "first"},
                {"software_version": 24, "langpack": "lg1",
                 "t9_version": 9, "evidence_source": "second"},
                {"software_version": 24, "langpack": "lg92",
                 "t9_version": 9, "evidence_source": "third"},
            ],
            "t9",
        )
        self.assertEqual(
            payload_paths(storage),
            [
                "24/lg1/01/t9/abc123.bin",
                "24/lg1/09/t9/abc123.bin",
                "24/lg92/09/t9/abc123.bin",
            ],
        )
        self.assertEqual(
            select_payload_path(paths_by_scope, 24, "lg1", 9, "t9"),
            "24/lg1/09/t9/abc123.bin",
        )
        with self.assertRaisesRegex(lt.FirmwareError, "no unique"):
            select_payload_path(paths_by_scope, 24, "lg1", None, "t9")

    def test_short_hashes_extend_only_colliding_prefixes(self) -> None:
        first = "a" * 12 + "0" + "b" * 51
        second = "a" * 12 + "1" + "c" * 51
        third = "d" * 64
        names = _short_hashes([second, third, first])
        self.assertEqual(names[first], "a" * 12 + "0")
        self.assertEqual(names[second], "a" * 12 + "1")
        self.assertEqual(names[third], "d" * 12)

    def test_bcore_deduplicates_restores_and_uses_lowest_version_owner(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            second = bytearray(image)
            second[0x330:0x340] = b"\xA5" * 16
            _write_metadata(second, software=0x14)
            (source_root / "second.bin").write_bytes(second)
            corpus_root = root / "corpus" / "C55"
            corpus = build_corpus(
                [source_root],
                lt.load_layout("C55"),
                write_splits=True,
                corpus_root=corpus_root,
            )
            bcore = next(
                region for region in corpus["regions"]
                if region["role"] == "BCORE"
            )
            self.assertEqual(bcore["variant_count"], 1)
            variant = bcore["variants"][0]
            self.assertEqual(len(variant["evidence"]["source_sha256"]), 2)
            self.assertEqual(
                variant["evidence"]["bcore_field_330_stored_values"],
                [bytes(range(16)).hex(), (b"\xA5" * 16).hex()],
            )
            self.assertEqual(
                payload_paths(variant),
                [f"18/bcore/{variant['sha256'][:12]}.bin"],
            )
            self.assertFalse(variant["lg_evidence"]["lg_independent"])
            payload = (corpus_root / variant["payload_path"]).read_bytes()
            self.assertEqual(payload[0x330:0x340], b"\xFF" * 16)

            complete = [
                item for item in corpus["artifacts"]
                if item["kind"] == "complete-fullflash"
            ]
            self.assertEqual(len(complete), 2)
            for artifact in complete:
                rebuilt = reconstruct_recipe(
                    corpus_root / artifact["recipe"]["path"]
                )
                self.assertEqual(
                    hashlib.sha256(rebuilt).hexdigest(), artifact["sha256"]
                )

    def test_langpack_lg_requires_the_fixed_header_field(self) -> None:
        payload = bytearray(b"\xFF" * 0x40)
        _write_langpack(payload, 0, b"lg91")
        self.assertEqual(_langpack_lg(payload), "lg91")

        for malformed in (
            bytes(payload[2:]),
            bytes(payload[:0x15] + b"lg91\0" + payload[0x1A:]),
            bytes(payload[:0x15] + b"@LG91\0" + payload[0x1B:]),
            bytes(payload[:0x15] + b"@lg91X" + payload[0x1B:]),
        ):
            with self.subTest(payload=malformed[:0x20].hex()):
                self.assertIsNone(_langpack_lg(malformed))

    def test_embedded_lg_and_t9_pairing_override_parent_metadata(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source_root, _image = _synthetic_sources(Path(directory))
            corpus = build_corpus([source_root], lt.load_layout("C55"))
            complete = next(
                item for item in corpus["artifacts"]
                if item["kind"] == "complete-fullflash"
            )
            self.assertEqual(complete["metadata"]["firmware"]["langpack"], "lg1")

            langpack = next(
                item for item in corpus["occurrences"]
                if item["source_id"] == complete["id"]
                and item["role"] == "LangPack"
            )
            t9 = next(
                item for item in corpus["occurrences"]
                if item["source_id"] == complete["id"]
                and item["role"] == "T9"
            )
            unknown = next(
                item for item in corpus["occurrences"]
                if item["source_id"] == complete["id"]
                and item["role"] == "UNKNOWN_1"
            )
            self.assertEqual(langpack["embedded_langpack"], "lg91")
            self.assertEqual(t9["paired_langpack"], "lg91")
            self.assertEqual(unknown["scope"]["langpack"], "lg1")
            self.assertEqual(
                unknown["scope"]["langpack_source"], "fullflash-metadata"
            )
            self.assertTrue(langpack["payload_path"].startswith(
                "24/lg91/11/langpack/"
            ))
            self.assertTrue(t9["payload_path"].startswith(
                "24/lg91/11/t9/"
            ))

    def test_malformed_language_headers_fall_back_to_primary_lg(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            malformed = bytearray(image)
            layout = lt.load_layout("C55")
            langpack = layout.layout.region("LangPack")
            t9 = layout.layout.region("T9")
            malformed[langpack.offset + 0x15:langpack.offset + 0x1B] = (
                b"@LG91\0"
            )
            malformed[t9.offset + 0x08] = 0xFF
            (source_root / "complete.bin").write_bytes(malformed)

            corpus = build_corpus([source_root], layout)
            complete = next(
                item for item in corpus["artifacts"]
                if item["kind"] == "complete-fullflash"
            )
            occurrences = {
                item["role"]: item
                for item in corpus["occurrences"]
                if item["source_id"] == complete["id"]
            }
            for role in ("LangPack", "T9", "UNKNOWN_1"):
                with self.subTest(role=role):
                    self.assertEqual(occurrences[role]["scope"]["langpack"], "lg1")
                    self.assertEqual(
                        occurrences[role]["scope"]["langpack_source"],
                        "fullflash-metadata",
                    )
                    self.assertIsNone(
                        occurrences[role]["scope"]["t9_version"]
                    )
            self.assertTrue(occurrences["LangPack"]["payload_path"].startswith(
                "24/lg1/unknown/langpack/"
            ))
            self.assertTrue(occurrences["T9"]["payload_path"].startswith(
                "24/lg1/unknown/t9/"
            ))

    def test_t9_is_duplicated_for_each_exact_fullflash_lg_pair(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            second = bytearray(image)
            layout = lt.load_layout("C55")
            _write_langpack(
                second, layout.layout.region("LangPack").offset, b"lg4"
            )
            (source_root / "second.bin").write_bytes(second)
            corpus_root = root / "corpus" / "C55"
            corpus = build_corpus(
                [source_root], layout, write_splits=True,
                corpus_root=corpus_root,
            )
            t9_digest = hashlib.sha256(
                image[
                    layout.layout.region("T9").offset:
                    layout.layout.region("T9").end
                ]
            ).hexdigest()
            t9 = next(
                variant
                for region in corpus["regions"] if region["role"] == "T9"
                for variant in region["variants"]
                if variant["sha256"] == t9_digest
            )
            self.assertEqual(
                payload_paths(t9),
                [
                    f"24/lg4/11/t9/{t9_digest[:12]}.bin",
                    f"24/lg91/11/t9/{t9_digest[:12]}.bin",
                ],
            )
            standalone_t9 = next(
                occurrence for occurrence in corpus["occurrences"]
                if occurrence["source_kind"] == "standalone-region"
                and occurrence["role"] == "T9"
                and occurrence["sha256"] == t9_digest
            )
            self.assertEqual(standalone_t9["scope"]["langpack"], None)
            self.assertEqual(
                standalone_t9["scope"]["langpack_source"], "unknown"
            )
            self.assertEqual(standalone_t9["scope"]["t9_version"], 11)
            self.assertEqual(
                standalone_t9["payload_path"],
                f"24/lg4/11/t9/{t9_digest[:12]}.bin",
            )
            self.assertFalse((corpus_root / t9["payload_path"]).is_symlink())
            for relative in t9["symlink_paths"]:
                link = corpus_root / relative
                self.assertTrue(link.is_symlink())
                self.assertFalse(Path(link.readlink()).is_absolute())
            for artifact in corpus["artifacts"]:
                if artifact["kind"] != "complete-fullflash":
                    continue
                rebuilt = reconstruct_recipe(
                    corpus_root / artifact["recipe"]["path"]
                )
                self.assertEqual(
                    hashlib.sha256(rebuilt).hexdigest(), artifact["sha256"]
                )

    def test_scope_reference_ranking_prefers_artifact_overlap(self) -> None:
        t9_hash = "a" * 64
        companion_hash = "b" * 64
        complete = {
            "software_version": 24,
            "langpack": "lg4",
            "t9_version": 11,
        }
        other = {
            "software_version": 24,
            "langpack": "lg91",
            "t9_version": 11,
        }
        unknown = {
            "software_version": 24,
            "langpack": None,
            "t9_version": 11,
            "langpack_source": "unknown",
        }
        artifacts = [
            {"id": "low", "kind": "complete-fullflash"},
            {"id": "high", "kind": "complete-fullflash"},
            {"id": "donor", "kind": "standalone-region"},
        ]
        occurrences = [
            {"id": "low-t9", "source_id": "low", "role": "T9",
             "sha256": t9_hash, "scope": complete},
            {"id": "low-companion", "source_id": "low",
             "role": "UNKNOWN_1", "sha256": companion_hash,
             "scope": complete},
            {"id": "high-t9", "source_id": "high", "role": "T9",
             "sha256": t9_hash, "scope": other},
            {"id": "donor-t9", "source_id": "donor", "role": "T9",
             "sha256": t9_hash, "scope": unknown},
            {"id": "donor-companion", "source_id": "donor",
             "role": "UNKNOWN_1", "sha256": companion_hash,
             "scope": unknown},
        ]
        references, resolutions = resolve_scope_references(
            occurrences, artifacts
        )
        self.assertEqual(references["donor-t9"]["langpack"], "lg4")
        self.assertEqual(references["donor-t9"]["langpack_source"], "unknown")
        resolution = next(
            item for item in resolutions
            if item["occurrence_id"] == "donor-t9"
        )
        self.assertEqual(resolution["candidate_count"], 2)
        self.assertEqual(resolution["artifact_overlap"], 2)
        self.assertEqual(occurrences[3]["scope"], unknown)


    def test_official_t9_uses_catalog_scope(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            loaded = lt.load_layout("C55")
            t9 = loaded.layout.region("T9")
            langpack = loaded.layout.region("LangPack")
            package_langpack = bytearray(b"\xFF" * langpack.length)
            _write_langpack(package_langpack, 0, b"lg4")
            official = root / "official-packages"
            official.mkdir()
            (official / "C55_199111.xbz").write_bytes(build_xbi(
                flash_size=loaded.layout.length,
                model=b"C55",
                svn=19,
                langpack=b"lg1",
                writes=[
                    (t9.offset, image[t9.offset:t9.offset + 0x40]),
                    (langpack.offset, bytes(package_langpack[:0x20])),
                ],
                erase_regions=[
                    (t9.offset, t9.end - 1),
                    (langpack.offset, langpack.end - 1),
                ],
            ))
            official_catalog = _materialize_official_catalog(
                root / "official-catalog", [official], loaded
            )
            corpus = build_corpus(
                [source_root], loaded, official_catalog=official_catalog
            )
            t9_digest = hashlib.sha256(image[t9.offset:t9.end]).hexdigest()
            variant = next(
                variant
                for region in corpus["regions"] if region["role"] == "T9"
                for variant in region["variants"]
                if variant["sha256"] == t9_digest
            )
            self.assertEqual(
                payload_paths(variant),
                [f"24/lg91/11/t9/{t9_digest[:12]}.bin"],
            )
            match = variant["official_package_matches"][0]
            self.assertEqual(match["scope"]["langpack"], "lg4")
            self.assertEqual(match["scope"]["t9_version"], 11)

    def test_all_roles_use_strict_scope_and_official_lg_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            loaded = lt.load_layout("C55")
            first = build_corpus([source_root], loaded)
            paths = {
                region["role"]: [
                    path
                    for variant in region["variants"]
                    for path in payload_paths(variant)
                ]
                for region in first["regions"]
            }
            ee_fs = next(
                item for item in first["occurrences"]
                if item["source_kind"] == "complete-fullflash"
                and item["role"] == "EE_FS"
            )
            self.assertEqual(ee_fs["scope"]["software_version"], 24)
            self.assertEqual(ee_fs["scope"]["langpack"], "lg1")
            self.assertEqual(ee_fs["scope"]["t9_version"], 11)
            self.assertTrue(any("/lg1/11/eeprom/" in path
                                for path in paths["EEPROM"]))
            self.assertTrue(any(path.startswith("18/bcore/")
                                for path in paths["BCORE"]))

            t9 = loaded.layout.region("T9")
            t9_digest = hashlib.sha256(image[t9.offset:t9.end]).hexdigest()
            official = root / "official-lg4"
            official.mkdir()
            (official / "C55_240411.xbz").write_bytes(build_xbi(
                version=32,
                flash_size=loaded.layout.length,
                model=b"C55",
                svn=24,
                langpack=b"lg4",
                writes=[
                    (t9.offset + offset, image[
                        t9.offset + offset:t9.offset + offset + 0xFFFF
                    ])
                    for offset in range(0, t9.length, 0xFFFF)
                ],
                erase_regions=[(t9.offset, t9.end - 1)],
            ))
            official_catalog = _materialize_official_catalog(
                root / "official-catalog", [official], loaded
            )
            collapsed = build_corpus(
                [source_root], loaded, official_catalog=official_catalog
            )
            variant = next(
                variant
                for region in collapsed["regions"] if region["role"] == "T9"
                for variant in region["variants"]
                if variant["sha256"] == t9_digest
            )
            self.assertTrue(variant["lg_evidence"]["lg_independent"])
            self.assertEqual(
                payload_paths(variant),
                [f"24/lg91/11/t9/{t9_digest[:12]}.bin"],
            )
            self.assertEqual(_software_version(19.0), 19)
            self.assertIsNone(_software_version(19.5))

    def test_recipe_rejects_unsafe_and_corrupt_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            corpus_root = root / "corpus" / "C55"
            corpus = build_corpus(
                [source_root], lt.load_layout("C55"),
                write_splits=True, corpus_root=corpus_root,
            )
            complete = next(
                item for item in corpus["artifacts"]
                if item["kind"] == "complete-fullflash"
            )
            recipe_path = corpus_root / complete["recipe"]["path"]
            original_recipe = json.loads(recipe_path.read_text())
            erased_index = next(
                index for index, item in enumerate(original_recipe["slices"])
                if item.get("erased") is True
            )
            cases = (
                ("both", {"payload_path": "missing.bin"}, "exactly one"),
                ("neither", None, "exactly one"),
                ("false", {"erased": False}, "must be true"),
                ("normalization", {"normalization": {}},
                 "unexpected normalization"),
                ("hash", {"sha256": "0" * 64}, "erased recipe slice hash"),
            )
            for name, replacement, message in cases:
                with self.subTest(erased_contract=name):
                    candidate = json.loads(json.dumps(original_recipe))
                    item = candidate["slices"][erased_index]
                    if name == "neither":
                        item.pop("erased")
                    else:
                        item.update(replacement)
                    recipe_path.write_text(json.dumps(candidate))
                    with self.assertRaisesRegex(lt.FirmwareError, message):
                        reconstruct_recipe(recipe_path)

            recipe_path.write_text(json.dumps(original_recipe))
            recipe = json.loads(recipe_path.read_text())
            recipe["slices"][0]["payload_path"] = "../outside.bin"
            recipe_path.write_text(json.dumps(recipe))
            with self.assertRaisesRegex(lt.FirmwareError, "unsafe"):
                reconstruct_recipe(recipe_path)

            build_corpus(
                [source_root], lt.load_layout("C55"),
                write_splits=True, corpus_root=corpus_root, force=True,
            )
            recipe = json.loads(recipe_path.read_text())
            payload = corpus_root / recipe["slices"][0]["payload_path"]
            payload.write_bytes(b"\0" * recipe["slices"][0]["size"])
            with self.assertRaisesRegex(lt.FirmwareError, "hash mismatch"):
                reconstruct_recipe(recipe_path)

            build_corpus(
                [source_root], lt.load_layout("C55"),
                write_splits=True, corpus_root=corpus_root, force=True,
            )
            recipe = json.loads(recipe_path.read_text())
            recipe["slices"][0]["normalization"]["stored_value"] = "00" * 16
            recipe_path.write_text(json.dumps(recipe))
            with self.assertRaisesRegex(
                lt.FirmwareError, "restored hash mismatch"
            ):
                reconstruct_recipe(recipe_path)
            self.assertEqual(len(image), 0x800000)

    def test_reconstruct_cli_rejects_existing_output_unless_forced(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            corpus_root = root / "corpus" / "C55"
            corpus = build_corpus(
                [source_root], lt.load_layout("C55"),
                write_splits=True, corpus_root=corpus_root,
            )
            complete = next(
                item for item in corpus["artifacts"]
                if item["kind"] == "complete-fullflash"
            )
            recipe = corpus_root / complete["recipe"]["path"]
            output = root / "rebuilt.bin"

            status, stdout, stderr = self.run_main([
                "catalog", "reconstruct", str(recipe), "-o", str(output),
            ])
            self.assertEqual(status, 0, stderr)
            self.assertIn("written:", stdout)
            self.assertEqual(output.read_bytes(), image)

            status, _stdout, stderr = self.run_main([
                "catalog", "reconstruct", str(recipe), "-o", str(output),
            ])
            self.assertEqual(status, 1)
            self.assertIn("output already exists", stderr)

            output.write_bytes(b"stale")
            status, _stdout, stderr = self.run_main([
                "catalog", "reconstruct", str(recipe), "-o", str(output),
                "--force",
            ])
            self.assertEqual(status, 0, stderr)
            self.assertEqual(output.read_bytes(), image)

    def test_older_recipe_schemas_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = b"legacy recipe payload"
            payload_path = root / "common" / "unknown" / "payload.bin"
            payload_path.parent.mkdir(parents=True)
            payload_path.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            recipe_path = root / "fullflashes" / "legacy.json"
            recipe_path.parent.mkdir()
            for schema_version in (1, 2, 3):
                recipe_path.write_text(json.dumps({
                    "schema": "siemens-fullflash-recipe",
                    "schema_version": schema_version,
                    "fullflash": {"size": len(payload), "sha256": digest},
                    "slices": [{
                        "order": 0,
                        "role": "UNKNOWN_1",
                        "payload_path": "common/unknown/payload.bin",
                        "sha256": digest,
                        "size": len(payload),
                        "source_range": {
                            "from": 0,
                            "to_exclusive": len(payload),
                            "length": len(payload),
                        },
                    }],
                }))
                with self.assertRaisesRegex(
                    lt.FirmwareError, "unsupported fullflash recipe schema"
                ):
                    reconstruct_recipe(recipe_path)


    def test_obsolete_schema_is_rejected_even_with_force(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, _image = _synthetic_sources(root)
            corpus_root = root / "corpus" / "C55"
            generated = corpus_root / "24" / "t9"
            generated.mkdir(parents=True)
            payload = generated / "old.bin"
            recipe = corpus_root / "fullflashes" / "old.json"
            recipe.parent.mkdir(parents=True)
            payload.write_bytes(b"old")
            recipe.write_text("{}")
            unrelated = corpus_root / "notes.txt"
            unrelated.write_text("keep")
            catalog = corpus_root / "catalog.json"
            catalog.write_text(json.dumps({
                "schema": "siemens-community-fullflash-corpus",
                "schema_version": 7,
                "regions": [{
                    "variants": [{
                        "materializations": [{
                            "payload_path": "24/t9/old.bin"
                        }]
                    }]
                }],
                "artifacts": [{
                    "recipe": {"path": "fullflashes/old.json"}
                }],
            }))

            command = [
                "catalog", "build", str(source_root), "--layout", "C55",
                "--official-catalog", str(root / "official"),
                "--materialize", "--catalog", str(corpus_root),
            ]
            status, _stdout, stderr = self.run_main(command)
            self.assertEqual(status, 1)
            self.assertIn("rebuild into an empty catalog root", stderr)
            self.assertTrue(payload.exists())

            status, _stdout, stderr = self.run_main(command + ["--force"])
            self.assertEqual(status, 1)
            self.assertIn("rebuild into an empty catalog root", stderr)
            self.assertTrue(generated.exists())
            self.assertTrue(recipe.exists())
            self.assertEqual(unrelated.read_text(), "keep")
            self.assertEqual(
                json.loads(catalog.read_text())["schema_version"], 7
            )


    def test_schema_fourteen_force_regeneration_prunes_owned_paths(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            official_root = root / "official"
            _empty_official_catalog(
                official_root, lt.load_layout("C55")
            )
            source_root, _image = _synthetic_sources(root)
            corpus_root = root / "corpus" / "C55"
            old_payload = corpus_root / "legacy" / "old.bin"
            old_payload.parent.mkdir(parents=True)
            old_payload.write_bytes(b"old")
            old_recipe = corpus_root / "fullflashes" / "old.json"
            old_recipe.parent.mkdir(parents=True)
            old_recipe.write_text("{}")
            unrelated = corpus_root / "notes.txt"
            unrelated.write_text("keep")
            catalog = corpus_root / "catalog.json"
            catalog.write_text(json.dumps({
                "schema": "siemens-community-fullflash-corpus",
                "schema_version": 14,
                "summary": {
                    "official_backed_variants": 0,
                    "official_backed_paths": 0,
                    "official_backed_bytes": 0,
                },
                "regions": [{
                    "role": "T9",
                    "variants": [{
                        "erased": False,
                        "size": 3,
                        "sha256": hashlib.sha256(b"old").hexdigest(),
                        "payload_path": "legacy/old.bin",
                        "symlink_paths": [],
                        "official_backing": {
                            "catalog": "../../old-official/catalog.json",
                            "catalog_sha256": "0" * 64,
                            "paths": [{
                                "path": "legacy/old.bin",
                                "target": "../../old-official/old.bin",
                            }],
                        },
                    }],
                }],
                "artifacts": [{
                    "recipe": {"path": "fullflashes/old.json"},
                }],
            }))

            command = [
                "catalog", "build", str(source_root), "--layout", "C55",
                "--official-catalog", str(official_root),
                "--materialize", "--catalog", str(corpus_root), "--force",
            ]
            status, _stdout, stderr = self.run_main(command)
            self.assertEqual(status, 0, stderr)
            self.assertFalse(old_payload.exists())
            self.assertFalse(old_recipe.exists())
            self.assertEqual(unrelated.read_text(), "keep")
            self.assertEqual(
                json.loads(catalog.read_text())["schema_version"], 14
            )

    def test_materialized_build_minimizes_against_official_catalog(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            loaded = lt.load_layout("C55")
            t9 = loaded.layout.region("T9")
            t9_payload = image[t9.offset:t9.end]

            packages = root / "official-packages"
            packages.mkdir()
            (packages / "C55_240111.xbz").write_bytes(build_xbi(
                version=32,
                flash_size=loaded.layout.length,
                model=b"C55",
                svn=24,
                langpack=b"lg1",
                t9=11,
                writes=[
                    (t9.offset + offset, t9_payload[offset:offset + 0xFFFF])
                    for offset in range(0, len(t9_payload), 0xFFFF)
                ],
                erase_regions=[(t9.offset, t9.end - 1)],
            ))
            official_root = root / "official"
            _materialize_official_catalog(
                official_root, [packages], loaded
            )

            community_root = root / "community"
            status, _stdout, stderr = self.run_main([
                "catalog", "build", str(source_root), "--layout", "C55",
                "--official-catalog", str(official_root),
                "--materialize", "--catalog", str(community_root),
            ])
            self.assertEqual(status, 0, stderr)
            catalog_path = community_root / "catalog.json"
            catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
            digest = hashlib.sha256(t9_payload).hexdigest()
            variant = next(
                variant
                for region in catalog["regions"] if region["role"] == "T9"
                for variant in region["variants"]
                if variant["sha256"] == digest
            )
            self.assertIn("official_backing", variant)
            self.assertTrue(
                (community_root / variant["payload_path"]).is_symlink()
            )
    def test_equal_size_inputs_require_role_structure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, _image = _synthetic_sources(root)
            malformed = source_root / "fake-eeprom.bin"
            malformed.write_bytes(b"\xFF" * 0x60000)
            corpus = build_corpus([source_root], lt.load_layout("C55"))
            rejection = next(
                item for item in corpus["excluded"]
                if item["path"].endswith("fake-eeprom.bin")
            )
            self.assertIn(
                rejection["reason"],
                ("placement-unresolved", "eeprom-parser-validation-failed"),
            )

    def test_reset_failure_quarantines_only_owning_region(self) -> None:
        cases = {
            "invalid-opcode": bytes.fromhex("00 80 34 12"),
            "target-erased": bytes.fromhex("FA 80 00 20"),
        }
        for expected_reason, reset in cases.items():
            with self.subTest(reset=expected_reason), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source_root, image = _synthetic_sources(root)
                damaged = bytearray(image)
                damaged[:4] = reset
                path = source_root / f"damaged-{expected_reason}.bin"
                path.write_bytes(damaged)
                corpus_root = root / "corpus"
                corpus = build_corpus(
                    [source_root], lt.load_layout("C55"),
                    write_splits=True, corpus_root=corpus_root,
                )
                artifact = next(
                    item for item in corpus["artifacts"]
                    if item["path"].endswith(path.name)
                )
                self.assertEqual(artifact["kind"], "partial-fullflash")
                self.assertNotIn("recipe", artifact)
                self.assertEqual(
                    artifact["quarantined_regions"][0]["role"], "BCORE"
                )
                self.assertEqual(
                    artifact["quarantined_regions"][0]["reset_reason"],
                    expected_reason,
                )
                self.assertEqual(len(artifact["harvested_ranges"]), 9)
                donor_occurrences = [
                    item for item in corpus["occurrences"]
                    if item["source_id"] == artifact["id"]
                ]
                self.assertEqual(len(donor_occurrences), 9)
                self.assertNotIn(
                    "BCORE", {item["role"] for item in donor_occurrences}
                )

                (corpus_root / "catalog.json").write_text(json.dumps(corpus))
                source_info = catalog_info(corpus_root, artifact["sha256"][:12])
                self.assertEqual(source_info["matches"][0]["kind"], "partial-fullflash")
                self.assertEqual(source_info["matches"][0]["recipe_status"], "absent")
                payload = next(
                    item["payload_path"] for item in donor_occurrences
                    if not item["erased"]
                )
                payload_info = catalog_info(corpus_root, payload)
                self.assertEqual(payload_info["selected_payload"], payload)
                self.assertTrue(any(
                    item["kind"] == "partial-fullflash"
                    for item in payload_info["matches"]
                ))

    def test_partial_boundaries_evidence_and_metadata_scope(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, image = _synthetic_sources(root)
            layout = lt.load_layout("C55")
            t9 = layout.layout.region("T9")
            langpack = layout.layout.region("LangPack")
            captures = {
                "prefix.bin": image[:langpack.end],
                "suffix.bin": image[0x60000:],
                "interior.bin": image[t9.offset:langpack.end],
                "weak.bin": image[t9.offset:langpack.offset],
                "clipped-langpack.bin": image[langpack.offset:langpack.offset + 0x20000],
            }
            for name, payload in captures.items():
                (source_root / name).write_bytes(payload)

            corpus = build_corpus([source_root], layout)
            partial_by_name = {
                Path(item["path"]).name: item
                for item in corpus["artifacts"]
                if item["kind"] == "partial-fullflash"
            }
            self.assertEqual(
                set(partial_by_name), {"prefix.bin", "suffix.bin", "interior.bin"}
            )
            for artifact in partial_by_name.values():
                self.assertTrue(artifact["harvested_ranges"])
                self.assertTrue(all(
                    item["layout_range"]["length"] > 0
                    for item in artifact["harvested_ranges"]
                ))
            interior = partial_by_name["interior.bin"]
            interior_occurrences = [
                item for item in corpus["occurrences"]
                if item["source_id"] == interior["id"]
            ]
            self.assertTrue(interior_occurrences)
            self.assertTrue(all(
                item["scope"]["software_version"] is None
                and item["scope"]["langpack"] is None
                for item in interior_occurrences
            ))
            suffix = partial_by_name["suffix.bin"]
            suffix_occurrences = [
                item for item in corpus["occurrences"]
                if item["source_id"] == suffix["id"]
            ]
            self.assertTrue(all(
                item["scope"]["software_version"] == 24
                for item in suffix_occurrences
            ))
            reasons = {
                Path(item["path"]).name: item["reason"]
                for item in corpus["excluded"]
            }
            self.assertEqual(
                reasons["weak.bin"], "partial-placement-insufficient-evidence"
            )
            self.assertEqual(
                reasons["clipped-langpack.bin"],
                "partial-capture-has-no-complete-region",
            )

    def test_non_reset_owning_standalone_bcore_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            layout = lt.load_layout("M55")
            image = bytearray(b"\xFF") * layout.layout.length
            image[:4] = bytes.fromhex("FA 80 34 12")
            primary = bytearray(image[0x800000:])
            _write_bcore(primary, software=0x11, model=b"M55")
            _write_metadata(primary, software=0x11, model=b"M55")
            image[0x800000:] = primary
            image[0x803412] = 0
            (root / "complete.bin").write_bytes(image)
            bcore = bytes(primary[:0x10000])
            (root / "bcore.bin").write_bytes(bcore)

            corpus = build_corpus([root], layout)
            artifact = next(
                item for item in corpus["artifacts"]
                if item["path"].endswith("bcore.bin")
            )
            self.assertEqual(artifact["kind"], "standalone-region")
            self.assertEqual(artifact["role"], "BCORE")
            occurrence = next(
                item for item in corpus["occurrences"]
                if item["source_id"] == artifact["id"]
            )
            self.assertEqual(occurrence["scope"]["software_version"], 11)

    def test_chip_orders_reconstruct_original_source_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            loaded = lt.load_layout("S55")
            secondary = bytes([0xA5]) * 0x400000
            primary = bytearray(b"\xFF" * 0x800000)
            _write_bcore(primary, software=0x12, model=b"S55")
            _write_metadata(primary, software=0x12, model=b"S55")
            logical = secondary + bytes(primary)
            reversed_order = bytes(primary) + secondary
            (root / "secondary-first.bin").write_bytes(logical)
            (root / "primary-first.bin").write_bytes(reversed_order)
            corpus_root = root / "corpus"
            corpus = build_corpus(
                [root], loaded, write_splits=True, corpus_root=corpus_root,
            )
            complete = [
                item for item in corpus["artifacts"]
                if item["kind"] == "complete-fullflash"
            ]
            self.assertEqual(len(complete), 2)
            originals = {
                hashlib.sha256(logical).hexdigest(): logical,
                hashlib.sha256(reversed_order).hexdigest(): reversed_order,
            }
            for artifact in complete:
                rebuilt = reconstruct_recipe(
                    corpus_root / artifact["recipe"]["path"]
                )
                self.assertEqual(rebuilt, originals[artifact["sha256"]])

    def test_sparse_official_hole_is_not_an_exact_match(self) -> None:
        loaded = lt.load_layout("C55")
        bcore = bytearray(b"\xFF" * loaded.layout.region("BCORE").length)
        _write_bcore(bcore)
        source_digest = hashlib.sha256(bcore).hexdigest()
        normalized = bytearray(bcore)
        normalized[0x330:0x340] = b"\xFF" * 16
        digest = hashlib.sha256(normalized).hexdigest()
        writes = []
        cursor = 0
        while cursor < len(bcore):
            if bcore[cursor] == 0xFF:
                cursor += 1
                continue
            end = cursor + 1
            while (
                end < len(bcore)
                and bcore[end] != 0xFF
                and end - cursor < 0xFF
            ):
                end += 1
            writes.append((cursor, bytes(bcore[cursor:end])))
            cursor = end
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "sparse.xbi").write_bytes(build_xbi(
                flash_size=loaded.layout.length,
                model=b"C55",
                writes=writes,
                erase_regions=[(0, len(bcore) - 2)],
            ))
            matches, summary = _official_attribution([root], loaded)
            self.assertEqual(summary["unique_payloads_parsed"], 1)
            self.assertNotIn(("BCORE", digest), matches)

            (root / "exact.xbi").write_bytes(build_xbi(
                flash_size=loaded.layout.length,
                model=b"C55",
                writes=writes,
                erase_regions=[(0, len(bcore) - 1)],
            ))
            matches, summary = _official_attribution([root], loaded)
            self.assertEqual(summary["unique_payloads_parsed"], 2)
            self.assertEqual(len(matches[("BCORE", digest)]), 1)
            self.assertNotIn(("BCORE", source_digest), matches)
            self.assertEqual(
                matches[("BCORE", digest)][0]["source_region_sha256"],
                source_digest,
            )

    def test_report_renderer_uses_corpus_model_and_generic_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_root, _image = _synthetic_sources(root)
            corpus = build_corpus(
                [source_root],
                lt.load_layout("C55"),
                corpus_root=Path("fw/corpus/A52/community"),
            )
            report = render_report(corpus)
            self.assertTrue(report.startswith("# A52 Community Fullflash Corpus\n"))
            self.assertIn("Standalone EEPROM matching:", report)
            self.assertIn("Excluded inputs retain their measured size", report)
            self.assertNotIn("C55_SW24_flash-range", report)



class BundledCommunityCorpusTests(unittest.TestCase):
    def test_catalogs_and_recipes_cover_every_bundled_image(self) -> None:
        from tools.bundled_firmware import manifest, entries, ROOT
        from .bundled_fixtures import catalogs
        for model in manifest()["devices"]:
            with self.subTest(model=model):
                root = catalogs(model)
                catalog = json.loads((root / "community/catalog.json").read_text())
                selected = entries(model)
                self.assertEqual(catalog["summary"]["complete_fullflashes"], len(selected))
                self.assertEqual(catalog["summary"]["excluded_candidates"], 0)
                self.assertEqual(catalog["summary"]["fullflash_recipes"], len(selected))
                artifacts = catalog["artifacts"]
                self.assertEqual({x["sha256"] for x in artifacts},
                                 {x["sha256"] for x in selected})
                for artifact in artifacts:
                    recipe = artifact["recipe"]
                    self.assertEqual(recipe["status"], "materialized")
                    result = reconstruct_recipe(root / "community" / recipe["path"])
                    source = next(x for x in selected if x["sha256"] == artifact["sha256"])
                    self.assertEqual(result, (ROOT / source["image"]).read_bytes())
                for region in catalog["regions"]:
                    for variant in region["variants"]:
                        for path in payload_paths(variant):
                            payload = (root / "community" / path).read_bytes()
                            self.assertEqual(len(payload), variant["size"])
                            self.assertEqual(hashlib.sha256(payload).hexdigest(), variant["sha256"])
                report = render_report(catalog)
                self.assertIn(model.upper(), report)


if __name__ == "__main__":
    unittest.main()
