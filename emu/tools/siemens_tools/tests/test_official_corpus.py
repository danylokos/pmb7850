from __future__ import annotations

import contextlib
import hashlib
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.siemens_tools import layout as lt
from tools.siemens_tools.firmware.compression import materialize_xbi
from tools.siemens_tools.firmware.cli import main as firmware_main
from tools.siemens_tools.firmware.xbi import parse_xbi
from tools.siemens_tools.fullflash.compose import compose_manifest
from tools.siemens_tools.fullflash.corpus_scope import payload_paths
from tools.siemens_tools.fullflash.official_corpus import (
    build_official_corpus,
    discover_official_packages,
    render_official_report,
)
from tools.siemens_tools.tests.firmware_fixtures import (
    build_xbi,
    service_v1,
    update_exe,
)


def _write_langpack_header(langpack: bytes) -> bytes:
    data = bytearray(b"\xFF" * 0x20)
    data[:2] = b"\xBB\xBB"
    field = b"@" + langpack + b"\0"
    data[0x15:0x20] = field + b"\xFF" * (11 - len(field))
    return bytes(data)


def _firmware_metadata(langpack: bytes) -> bytes:
    data = bytearray(b"\xFF" * 0x38)
    data[:16] = bytes((
        0x24, 0xFF, 0x0A, 0x50, 0x14, 0x14, 0x01, 0x00,
        0xD5, 0x21, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    ))
    data[0x10:0x20] = langpack + b"\0" * (16 - len(langpack))
    data[0x20:0x30] = b"C55\0" + b"\0" * 12
    data[0x30:0x38] = b"SIEMENS\0"
    return bytes(data)


def _write_bcore(data: bytearray, software: int = 0xFF) -> None:
    data[:4] = bytes.fromhex("FA 80 34 12")
    data[0x1234] = 0
    data[0x300:0x30C] = bytes.fromhex(
        "00 01 4C 53 01 00 00 01 70 01 80 00"
    )
    data[0x30C:0x31C] = b"C55" + b"\0" * 13
    data[0x31C:0x324] = b"SIEMENS\0"
    data[0x32C] = software


class OfficialCorpusTests(unittest.TestCase):
    @staticmethod
    def run_main(argv: list[str]) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with (
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            status = firmware_main(argv)
        return status, stdout.getvalue(), stderr.getvalue()

    def test_raw_executable_dedup_metadata_actions_and_replay(self) -> None:
        loaded = lt.load_layout("C55")
        unknown = next(
            part for part in lt.partition_layout(loaded.layout)
            if part.label == "UNKNOWN_1"
        )
        t9 = loaded.layout.region("T9")
        langpack = loaded.layout.region("LangPack")
        package = build_xbi(
            flash_size=loaded.layout.length,
            model=b"C55",
            svn=19,
            langpack=b"lg1",
            erase_regions=[
                (unknown.start, unknown.end - 1),
                (t9.offset, t9.offset + 7),
                (langpack.offset, langpack.end - 1),
            ],
            writes=[
                (unknown.start, b"\xFF"),
                (t9.offset + 4, b"\xFFX"),
                (langpack.offset, _write_langpack_header(b"lg4")),
            ],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            (source / "package.xt").write_bytes(package)
            (source / "duplicate.exe").write_bytes(service_v1(package))
            (source / "ignored.map").write_bytes(b"not a package")

            discovered, excluded = discover_official_packages([source])
            self.assertEqual(len(discovered), 1)
            self.assertEqual(len(discovered[0]["sources"]), 2)
            self.assertEqual(excluded, [])

            unknown_digest = hashlib.sha256(
                b"\xFF" * (unknown.end - unknown.start)
            ).hexdigest()
            corpus_root = root / "official"
            corpus = build_official_corpus(
                [source],
                loaded,
                materialize=True,
                corpus_root=corpus_root,
            )
            self.assertEqual(corpus["summary"]["package_references"], 2)
            self.assertEqual(corpus["summary"]["unique_packages"], 1)
            package_row = corpus["packages"][0]
            self.assertEqual(package_row["scope"], {
                "software_version": 19,
                "langpack": "lg1",
                "t9_version": 1,
                "langpack_source": "package-metadata",
                "t9_version_source": "declared-package",
            })
            self.assertEqual(
                package_row["metadata"]["fields"]["model"], "C55"
            )
            self.assertEqual(package_row["metadata"]["write_frame_count"], 3)
            unknown_variant = next(
                variant
                for region in corpus["regions"]
                if region["role"] == "UNKNOWN_1"
                for variant in region["variants"]
            )
            self.assertFalse(unknown_variant["erased"])
            self.assertFalse(
                unknown_variant["lg_evidence"]["lg_independent"]
            )
            self.assertEqual(
                unknown_variant["payload_path"],
                f"19/lg1/01/unknown_1/{unknown_digest[:12]}.bin",
            )
            self.assertTrue(
                (corpus_root / unknown_variant["payload_path"]).is_file()
            )
            langpack_variant = next(
                variant
                for region in corpus["regions"]
                if region["role"] == "LangPack"
                for variant in region["variants"]
            )
            self.assertTrue(
                langpack_variant["payload_path"].startswith(
                    "19/lg4/01/langpack/"
                )
            )
            self.assertEqual(
                langpack_variant["occurrences"][0]["scope"]["langpack_source"],
                "embedded-langpack",
            )
            report_corpus = json.loads(json.dumps(corpus))
            for item in report_corpus["packages"]:
                item["metadata"]["fields"]["model"] = "A52"
            report = render_official_report(report_corpus)
            self.assertTrue(report.startswith("# A52 Official Package Corpus\n"))
            self.assertNotIn("BOOTL55 XBB trailer fragments", report)

            recipe = corpus_root / package_row["recipe"]["path"]
            recipe_document = json.loads(recipe.read_text())
            t9_operations = [
                item for item in recipe_document["operations"]
                if item["role"] == "T9"
            ]
            self.assertEqual(
                [item["action"] for item in t9_operations],
                ["erase", "write"],
            )
            write = t9_operations[1]
            self.assertTrue(
                write["payload_path"].startswith(
                    "19/lg1/01/t9/fragments/"
                )
            )
            self.assertEqual(
                (corpus_root / write["payload_path"]).read_bytes(),
                b"\xFFX",
            )

            baseline = bytes((index * 17 + 3) & 0xFF
                             for index in range(loaded.layout.length))
            baseline_path = root / "baseline.bin"
            baseline_path.write_bytes(baseline)
            manifest = root / "compose.json"
            manifest.write_text(json.dumps({
                "schema": "siemens-fullflash-composition",
                "schema_version": 1,
                "layout": {"name": "C55"},
                "operations": [
                    {
                        "kind": "custom",
                        "path": "baseline.bin",
                        "range": {
                            "from": 0,
                            "to_exclusive": loaded.layout.length,
                            "length": loaded.layout.length,
                        },
                        "sha256": hashlib.sha256(baseline).hexdigest(),
                    },
                    {
                        "kind": "official-package",
                        "recipe": recipe.relative_to(root).as_posix(),
                    },
                ],
            }))
            result = compose_manifest(manifest)
            direct = bytearray(baseline)
            expanded = materialize_xbi(package, parse_xbi(package))
            for index, erased in enumerate(expanded.erase_mask):
                if erased:
                    direct[index] = 0xFF
            for index, written in enumerate(expanded.write_mask):
                if written:
                    direct[index] = expanded.flash[index]
            self.assertEqual(result.image, bytes(direct))
            self.assertGreater(len(result.overlaps), 0)

            fragment_path = corpus_root / write["payload_path"]
            original_fragment = fragment_path.read_bytes()
            fragment_path.write_bytes(b"corrupt")
            with self.assertRaisesRegex(
                lt.FirmwareError, "size or hash mismatch"
            ):
                compose_manifest(manifest)
            fragment_path.write_bytes(original_fragment)

    def test_materialized_metadata_is_primary_but_language_roles_pair(self) -> None:
        loaded = lt.load_layout("C55")
        unknown = next(
            part for part in lt.partition_layout(loaded.layout)
            if part.label == "UNKNOWN_1"
        )
        t9 = loaded.layout.region("T9")
        langpack = loaded.layout.region("LangPack")
        t9_header = bytearray(b"\xFF" * 0x20)
        t9_header[:14] = bytes.fromhex(
            "54 39 85 90 98 62 03 00 0B 02 53 39 01 01"
        )
        t9_header[14:22] = b"\0" * 8
        package = build_xbi(
            flash_size=loaded.layout.length,
            model=b"C55",
            svn=19,
            langpack=b"lg4",
            t9=1,
            erase_regions=[
                (unknown.start, unknown.end - 1),
                (t9.offset, t9.end - 1),
                (langpack.offset, langpack.end - 1),
            ],
            writes=[
                (0x7FF50, _firmware_metadata(b"lg1")),
                (unknown.start, b"\0"),
                (t9.offset, bytes(t9_header)),
                (langpack.offset, _write_langpack_header(b"lg91")),
            ],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "mixed.xbi").write_bytes(package)
            corpus = build_official_corpus([root], loaded)

        self.assertEqual(corpus["packages"][0]["scope"], {
            "software_version": 19,
            "langpack": "lg1",
            "t9_version": 11,
            "langpack_source": "fullflash-metadata",
            "t9_version_source": "embedded-t9",
        })
        variants = {
            region["role"]: region["variants"][0]
            for region in corpus["regions"] if region["variants"]
        }
        self.assertTrue(variants["UNKNOWN_1"]["payload_path"].startswith(
            "19/lg1/11/unknown_1/"
        ))
        self.assertTrue(variants["LangPack"]["payload_path"].startswith(
            "19/lg91/11/langpack/"
        ))
        self.assertTrue(variants["T9"]["payload_path"].startswith(
            "19/lg91/11/t9/"
        ))
        self.assertEqual(
            variants["UNKNOWN_1"]["occurrences"][0]["scope"][
                "langpack_source"
            ],
            "fullflash-metadata",
        )
        self.assertEqual(
            variants["LangPack"]["occurrences"][0]["scope"][
                "langpack_source"
            ],
            "embedded-langpack",
        )
        self.assertEqual(
            variants["T9"]["occurrences"][0]["scope"]["langpack_source"],
            "paired-langpack",
        )

    def test_ffsinit_references_are_deduplicated_with_raw_xfs(self) -> None:
        loaded = lt.load_layout("C55")
        ffs = loaded.layout.region("FFS(A)")
        xfs = build_xbi(
            flash_size=loaded.layout.length,
            model=b"C55",
            svn=24,
            update_type=7,
            erase_regions=[(ffs.offset, ffs.end - 1)],
            writes=[(ffs.offset, b"FFS")],
        )
        archive_io = io.BytesIO()
        with zipfile.ZipFile(archive_io, "w") as archive:
            archive.writestr("Bitmap/Test.bmp", b"payload")
        executable = update_exe(archive_io.getvalue(), False)
        unmatched_io = io.BytesIO()
        with zipfile.ZipFile(unmatched_io, "w") as archive:
            archive.writestr("Bitmap/New.bmp", b"new")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "C55_2_Test_24_0001.xfs").write_bytes(xfs)
            (root / "FFSInit_C55_2_Test_24_0001.exe").write_bytes(executable)
            (root / "FFSInit_C55_2_Copy_24_0001.exe").write_bytes(executable)
            (root / "FFSInit_C55_2_New_24_0001.exe").write_bytes(
                update_exe(unmatched_io.getvalue(), False)
            )
            discovered, excluded = discover_official_packages([root])
            corpus = build_official_corpus([root], loaded)

        self.assertEqual(len(discovered), 1)
        self.assertEqual(len(discovered[0]["sources"]), 3)
        transformed = [
            source for source in discovered[0]["sources"]
            if source["container_type"] == "ffsinit-reference"
        ]
        self.assertEqual(len(transformed), 2)
        self.assertTrue(all(
            source["transform"]["reference_sha256"]
            == hashlib.sha256(xfs).hexdigest()
            for source in transformed
        ))
        self.assertEqual(
            [item["reason"] for item in excluded],
            ["ffsinit-conversion-failed"],
        )
        self.assertEqual(corpus["summary"]["ffsinit_references"], 2)
        self.assertEqual(corpus["summary"]["unique_packages"], 1)


    def test_obsolete_schema_rejected_before_current_schema_cleanup(self) -> None:
        loaded = lt.load_layout("C55")
        unknown = next(
            part for part in lt.partition_layout(loaded.layout)
            if part.label == "UNKNOWN_1"
        )
        package = build_xbi(
            flash_size=loaded.layout.length, model=b"C55", svn=24,
            erase_regions=[(unknown.start, unknown.end - 1)],
            writes=[(unknown.start, b"payload")],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.xbi"
            source.write_bytes(package)
            output = root / "official" / "catalog.json"
            old_payload = output.parent / "24" / "unknown_1" / "old.bin"
            old_fragment = output.parent / "24" / "fragments" / "old.bin"
            old_recipe = output.parent / "packages" / "old.json"
            for path in (old_payload, old_fragment, old_recipe):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"old")
            unrelated = output.parent / "notes.txt"
            unrelated.write_text("keep")
            output.write_text(json.dumps({
                "schema": "siemens-official-corpus",
                "schema_version": 1,
                "regions": [{"variants": [{
                    "payload_path": "24/unknown_1/old.bin",
                    "symlink_paths": [],
                }]}],
                "packages": [{"recipe": {"path": "packages/old.json"}}],
            }))
            old_recipe.write_text(json.dumps({"operations": [{
                "action": "write",
                "payload_path": "24/fragments/old.bin",
            }]}))
            command = [
                "catalog", "build", str(source), "--layout", "C55",
                "--materialize", "--catalog", str(output.parent), "--force",
            ]
            status, _stdout, stderr = self.run_main(command)
            self.assertEqual(status, 1)
            self.assertIn("rebuild into an empty catalog root", stderr)
            self.assertTrue(old_payload.exists())
            self.assertTrue(old_fragment.exists())
            self.assertTrue(old_recipe.exists())

            document = json.loads(output.read_text())
            document["schema_version"] = 9
            output.write_text(json.dumps(document))
            status, _stdout, stderr = self.run_main(command)
            self.assertEqual(status, 0, stderr)
            self.assertEqual(json.loads(output.read_text())["schema_version"], 9)
            self.assertFalse(old_payload.exists())
            self.assertFalse(old_fragment.exists())
            self.assertFalse(old_recipe.exists())
            self.assertEqual(unrelated.read_text(), "keep")

    def test_embedded_t9_overrides_declared_metadata(self) -> None:
        loaded = lt.load_layout("C55")
        t9 = loaded.layout.region("T9")
        header = bytearray(b"\xFF" * 0x20)
        header[:14] = bytes.fromhex(
            "54 39 85 90 98 62 03 00 09 02 53 39 01 01"
        )
        header[14:22] = b"\0" * 8
        package = build_xbi(
            flash_size=loaded.layout.length,
            model=b"C55", svn=24, langpack=b"lg1", t9=1,
            erase_regions=[(t9.offset, t9.end - 1)],
            writes=[(t9.offset, bytes(header))],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "C55_240101.xbi").write_bytes(package)
            corpus = build_official_corpus([root], loaded)
            self.assertEqual(corpus["packages"][0]["scope"]["t9_version"], 9)
            self.assertEqual(
                corpus["packages"][0]["scope"]["t9_version_source"],
                "embedded-t9",
            )
            paths = [
                path
                for region in corpus["regions"]
                for variant in region["variants"]
                for path in payload_paths(variant)
            ]
            self.assertTrue(all("/09/" in path for path in paths))

    def test_bcore_bundle_scope_is_independent_of_partition_order(self) -> None:
        loaded = lt.load_layout("A65")
        bcore_region = loaded.layout.region("BCORE")
        bcore = bytearray(b"\xFF" * bcore_region.length)
        _write_bcore(bcore)
        writes = [(0, b"AUX")]
        cursor = 0
        while cursor < len(bcore):
            while cursor < len(bcore) and bcore[cursor] == 0xFF:
                cursor += 1
            start = cursor
            while (
                cursor < len(bcore)
                and bcore[cursor] != 0xFF
                and cursor - start < 0xFF
            ):
                cursor += 1
            if start < cursor:
                writes.append((
                    bcore_region.offset + start, bytes(bcore[start:cursor])
                ))
        package = build_xbi(
            flash_size=loaded.layout.length,
            model=b"BOOTL55PM",
            svn=0,
            erase_regions=[(bcore_region.offset, bcore_region.end - 1)],
            writes=writes,
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "A65" / "17" / "bcore" / "A65_17.xbb"
            source.parent.mkdir(parents=True)
            source.write_bytes(package)
            corpus_root = root / "corpus"
            corpus = build_official_corpus(
                [source], loaded, materialize=True, corpus_root=corpus_root
            )

            row = corpus["packages"][0]
            self.assertEqual(row["bundle"], {
                "role": "BCORE",
                "software_version": 17,
                "software_version_source": "source-path",
            })
            aux = next(
                region for region in row["regions"]
                if region["role"] == "UNKNOWN_1"
            )
            self.assertEqual(aux["semantic_role"], "BCORE_AUX")
            recipe = json.loads(
                (corpus_root / row["recipe"]["path"]).read_text()
            )
            aux_write = next(
                operation for operation in recipe["operations"]
                if operation["role"] == "UNKNOWN_1"
                and operation["action"] == "write"
            )
            self.assertTrue(
                aux_write["payload_path"].startswith("17/bcore/fragments/")
            )

    def test_bcore_source_hint_and_ffs_label_aliases(self) -> None:
        loaded = lt.load_layout("C55")
        bcore_region = loaded.layout.region("BCORE")
        unknown = next(
            part for part in lt.partition_layout(loaded.layout)
            if part.label == "UNKNOWN_1"
        )
        bcore = bytearray(b"\xFF" * bcore_region.length)
        _write_bcore(bcore)
        bcore_writes = []
        cursor = 0
        while cursor < len(bcore):
            while cursor < len(bcore) and bcore[cursor] == 0xFF:
                cursor += 1
            start = cursor
            while (
                cursor < len(bcore)
                and bcore[cursor] != 0xFF
                and cursor - start < 0xFF
            ):
                cursor += 1
            if start < cursor:
                bcore_writes.append((start, bytes(bcore[start:cursor])))
        bcore_package = build_xbi(
            flash_size=loaded.layout.length,
            model=b"BOOTL55T",
            svn=2,
            erase_regions=[
                (bcore_region.offset, bcore_region.end - 1),
                (unknown.end - 0x20000, unknown.end - 1),
            ],
            writes=[*bcore_writes, (unknown.end - 8, b"BOOTL55")],
        )
        ffs = loaded.layout.region("FFS(A)")
        ffs_payload = b"brand" + b"\xFF" * (ffs.length - 5)
        ffs_package = build_xbi(
            flash_size=loaded.layout.length,
            model=b"C55",
            svn=14,
            langpack=b"lg1",
            t9=1,
            erase_regions=[(ffs.offset, ffs.end - 1)],
            writes=[(ffs.offset, ffs_payload[:5])],
        )
        ffs_package_lg92 = build_xbi(
            flash_size=loaded.layout.length,
            model=b"C55",
            svn=14,
            langpack=b"lg92",
            t9=9,
            erase_regions=[(ffs.offset, ffs.end - 1)],
            writes=[(ffs.offset, ffs_payload[:5])],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bcore_path = root / "C55" / "14" / "bcore" / "C55_14.xbb"
            bcore_path.parent.mkdir(parents=True)
            bcore_path.write_bytes(bcore_package)
            ffs_root = root / "C55" / "14" / "ffs"
            ffs_root.mkdir(parents=True)
            for label in ("BRD-Handel", "O2-Germany-postpaid"):
                (ffs_root / f"C55_2_{label}_14_0001.xfs").write_bytes(
                    ffs_package
                )
            (ffs_root / "C55_2_O2-Germany-postpaid_14_0002.xfs").write_bytes(
                ffs_package_lg92
            )
            corpus_root = root / "corpus"
            corpus = build_official_corpus(
                [root / "C55"], loaded, materialize=True,
                corpus_root=corpus_root,
            )
            bcore_variant = next(
                variant
                for region in corpus["regions"] if region["role"] == "BCORE"
                for variant in region["variants"]
            )
            self.assertTrue(bcore_variant["payload_path"].startswith(
                "14/bcore/"
            ))
            self.assertEqual(bcore_variant["symlink_paths"], [])
            self.assertEqual(
                bcore_variant["occurrences"][0]["scope"][
                    "software_version_source"
                ],
                "source-path",
            )
            bcore_row = next(
                item for item in corpus["packages"]
                if item["sha256"] == hashlib.sha256(bcore_package).hexdigest()
            )
            self.assertEqual(bcore_row["scope"]["software_version"], 2)
            self.assertEqual(bcore_row["bundle"], {
                "role": "BCORE",
                "software_version": 14,
                "software_version_source": "source-path",
            })
            aux_region = next(
                item for item in bcore_row["regions"]
                if item["role"] == "UNKNOWN_1"
            )
            self.assertEqual(aux_region["semantic_role"], "BCORE_AUX")
            bcore_recipe = json.loads(
                (corpus_root / bcore_row["recipe"]["path"]).read_text()
            )
            self.assertEqual(
                bcore_recipe["package"]["bundle"], bcore_row["bundle"]
            )
            aux_operations = [
                item for item in bcore_recipe["operations"]
                if item["role"] == "UNKNOWN_1"
            ]
            self.assertEqual(
                [item["action"] for item in aux_operations],
                ["erase", "write"],
            )
            self.assertTrue(all(
                item["semantic_role"] == "BCORE_AUX"
                for item in aux_operations
            ))
            self.assertTrue(
                aux_operations[1]["payload_path"].startswith(
                    "14/bcore/fragments/07fff8-07fffe/"
                )
            )
            ffs_variant = next(
                variant
                for region in corpus["regions"] if region["role"] == "FFS(A)"
                for variant in region["variants"]
            )
            names = payload_paths(ffs_variant)
            self.assertTrue(all(name.startswith("14/ffs_a/") for name in names))
            self.assertEqual(
                {
                    (item["scope"]["langpack"], item["scope"]["t9_version"])
                    for item in ffs_variant["occurrences"]
                },
                {("lg1", 1), ("lg92", 9)},
            )
            self.assertEqual(len(names), 1)
            self.assertTrue(Path(names[0]).name.startswith("BRD-Handel-"))
            self.assertFalse((corpus_root / names[0]).is_symlink())
            recipe = next(
                corpus_root / package["recipe"]["path"]
                for package in corpus["packages"]
                if package["sha256"] == hashlib.sha256(ffs_package).hexdigest()
            )
            operation = json.loads(recipe.read_text())["operations"][0]
            self.assertEqual(operation["payload_path"], names[0])

    def test_partial_t9_header_uses_declared_metadata(self) -> None:
        loaded = lt.load_layout("C55")
        t9 = loaded.layout.region("T9")
        header = bytearray(b"\xFF" * 0x20)
        header[:14] = bytes.fromhex(
            "54 39 85 90 98 62 03 00 09 02 53 39 01 01"
        )
        header[14:22] = b"\0" * 8
        package = build_xbi(
            flash_size=loaded.layout.length, model=b"C55", t9=1,
            erase_regions=[(0, 0)],
            writes=[
                (t9.offset, bytes(header[:15])),
                (t9.offset + 16, bytes(header[16:])),
            ],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "partial.xbi").write_bytes(package)
            scope = build_official_corpus([root], loaded)["packages"][0][
                "scope"
            ]
            self.assertEqual(scope["t9_version"], 1)
            self.assertEqual(scope["t9_version_source"], "declared-package")

    def test_declared_lg_fallback_and_unknown_are_strict(self) -> None:
        loaded = lt.load_layout("C55")
        t9 = loaded.layout.region("T9")
        known = build_xbi(
            flash_size=loaded.layout.length,
            model=b"C55",
            svn=21,
            langpack=b"LG5",
            t9=9,
            erase_regions=[(t9.offset, t9.end - 1)],
            writes=[(t9.offset, b"T9-known")],
        )
        unknown = build_xbi(
            flash_size=loaded.layout.length,
            model=b"C55",
            svn=21,
            langpack=b"English",
            t9=None,
            erase_regions=[(t9.offset, t9.end - 1)],
            writes=[(t9.offset, b"T9-unknown")],
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "known.xbi").write_bytes(known)
            (root / "unknown.xbi").write_bytes(unknown)
            corpus = build_official_corpus([root], loaded)
            scopes = {
                item["sha256"]: item["scope"] for item in corpus["packages"]
            }
            self.assertEqual(scopes[hashlib.sha256(known).hexdigest()], {
                "software_version": 21,
                "langpack": "lg5",
                "t9_version": 9,
                "langpack_source": "package-metadata",
                "t9_version_source": "declared-package",
            })
            self.assertEqual(scopes[hashlib.sha256(unknown).hexdigest()], {
                "software_version": 21,
                "langpack": None,
                "t9_version": None,
                "langpack_source": "unknown",
                "t9_version_source": "unknown",
            })
            paths = [
                path
                for region in corpus["regions"] if region["role"] == "T9"
                for variant in region["variants"]
                for path in payload_paths(variant)
            ]
            self.assertTrue(any(path.startswith("21/lg5/09/t9/") for path in paths))
            self.assertTrue(any(
                path.startswith("21/unknown/unknown/t9/") for path in paths
            ))

    def test_layout_without_t9_partition_uses_declared_package_scope(self) -> None:
        loaded = lt.load_layout("A60")
        langpack = loaded.layout.region("LangPack")
        package = build_xbi(
            flash_size=loaded.layout.length,
            model=b"A60",
            svn=27,
            langpack=b"LG91",
            t9=5,
            erase_regions=[(langpack.offset, langpack.end - 1)],
            writes=[],
        )
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "A60_270105.xbi"
            source.write_bytes(package)
            corpus = build_official_corpus([source], loaded)

        self.assertEqual(len(corpus["packages"]), 1)
        self.assertEqual(corpus["packages"][0]["scope"], {
            "software_version": 27,
            "langpack": "lg91",
            "t9_version": 5,
            "langpack_source": "package-metadata",
            "t9_version_source": "declared-package",
        })
        self.assertNotIn(
            "T9", {region["role"] for region in corpus["regions"]}
        )


if __name__ == "__main__":
    unittest.main()
