#!/usr/bin/env python3
"""Fullflash split planning, manifest, and CLI tests."""

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
from tools.siemens_tools.fullflash.split import (
    MANIFEST_NAME,
    plan_split,
    write_split,
)
from tools.siemens_tools.layout.render import render_layout_reference


class FullflashSplitTests(unittest.TestCase):
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

    @staticmethod
    def _write_bcore(
            data: bytearray, model: bytes = b"C55", base: int = 0,
            field: bytes = b"\xFF" * 16) -> None:
        data[base:base + 4] = bytes.fromhex("FA 80 34 12")
        data[base + 0x1234] = 0
        data[base + 0x300:base + 0x30C] = bytes.fromhex(
            "00 01 4C 53 01 00 00 01 70 01 80 00"
        )
        data[base + 0x30C:base + 0x31C] = (
            model + b"\0" * (16 - len(model))
        )
        data[base + 0x31C:base + 0x324] = b"SIEMENS\0"
        data[base + 0x32C] = 0x24
        data[base + 0x330:base + 0x340] = field

    @staticmethod
    def _write_metadata(
            data: bytearray, base: int, model: bytes = b"SL55") -> None:
        offset = base + 0x7FF50
        data[offset:offset + 16] = bytes.fromhex(
            "24 FF 0A 50 14 14 01 00 D5 21 00 00 FF FF FF FF"
        )
        data[offset + 0x10:offset + 0x40] = b"\0" * 0x30
        data[offset + 0x10:offset + 0x14] = b"lg4\0"
        data[offset + 0x20:offset + 0x20 + len(model)] = model
        data[offset + 0x30:offset + 0x38] = b"SIEMENS\0"
        data[base + 0x7FE26:base + 0x7FE2A] = bytes.fromhex(
            "89 00 54 88"
        )

    def test_complete_layout_partition_and_c55_filenames(self) -> None:
        layout = lt.load_layout("C55").layout
        partitions = lt.partition_layout(layout)
        plan = plan_split(layout.length, layout, 0)

        self.assertEqual(
            [item.label for item in partitions],
            [
                "BCORE", "UNKNOWN_1", "T9", "UNKNOWN_2", "LangPack",
                "EE_FS", "UNKNOWN_3", "FFS(A)", "UNKNOWN_4", "EEPROM",
            ],
        )
        self.assertEqual(
            [item.filename for item in plan.slices],
            [
                "80-81_FF-bcore.bin",
                "82-87_FF-unknown1.bin",
                "88-8B_FF-t9.bin",
                "8C-DF_FF-unknown2.bin",
                "E0-E7_FF-langpack.bin",
                "E8-ED_FF-ee-fs.bin",
                "EE-EF_FF-unknown3.bin",
                "F0-F7_FF-ffs-a.bin",
                "F8-F9_FF-unknown4.bin",
                "FA-FF_FF-eeprom.bin",
            ],
        )
        self.assertTrue(all(item.full_boundary_present for item in plan.slices))

        edge_layout = lt.FlashLayout(
            "EDGE", 0, 0x50000,
            (
                lt.LayoutRegion("FIRST", 0x10000, 0x10000),
                lt.LayoutRegion("LAST", 0x30000, 0x10000),
            ),
        )
        self.assertEqual(
            [(item.label, item.start, item.end)
             for item in lt.partition_layout(edge_layout)],
            [
                ("UNKNOWN_1", 0, 0x10000),
                ("FIRST", 0x10000, 0x20000),
                ("UNKNOWN_2", 0x20000, 0x30000),
                ("LAST", 0x30000, 0x40000),
                ("UNKNOWN_3", 0x40000, 0x50000),
            ],
        )

    def test_partial_crossing_and_explicit_file_offset_plans(self) -> None:
        layout = lt.load_layout("C55").layout
        cases = (
            (0x30000, 0, ["bcore.bin", "unknown1.bin"]),
            (0x200000, 0x600000, ["langpack.bin", "ee-fs.bin",
                                  "unknown3.bin", "ffs-a.bin",
                                  "unknown4.bin", "eeprom.bin"]),
            (0x20000, 0, ["bcore.bin"]),
            (0x10000, 0x30000, ["unknown1.bin"]),
            (0x80000, 0x10000, ["bcore.bin", "unknown1.bin", "t9.bin"]),
        )
        for size, shift, suffixes in cases:
            with self.subTest(size=size, shift=shift):
                plan = plan_split(size, layout, shift)
                self.assertEqual(
                    [item.filename.split("_FF-")[1] for item in plan.slices],
                    suffixes,
                )
                self.assertEqual(
                    sum(item.size for item in plan.slices), size
                )

        unknown = plan_split(0x10000, layout, 0x30000).slices[0]
        self.assertEqual(unknown.filename, "82-87_FF-unknown1.bin")
        self.assertFalse(unknown.full_boundary_present)
        self.assertEqual((unknown.layout_start, unknown.layout_end),
                         (0x30000, 0x40000))

        # Source + 0x2FF00 = layout, so source offset 0x100 owns 0x830000.
        explicit = plan_split(0x300, layout, 0x2FF00)
        self.assertEqual(
            (explicit.slices[0].layout_start, explicit.slices[0].layout_end),
            (0x2FF00, 0x30200),
        )

    def test_granular_boundaries_trimming_and_grouped_aliases(self) -> None:
        a52 = lt.load_layout("A52").layout
        self.assertEqual(a52, lt.load_layout("A51").layout)
        self.assertEqual(a52.region("LangPack").end, 0x340000)
        self.assertEqual(
            [(item.label, item.start, item.end)
             for item in lt.partition_layout(a52)[-5:]],
            [
                ("LangPack", 0x300000, 0x340000),
                ("UNKNOWN_4", 0x340000, 0x360000),
                ("UNKNOWN_5", 0x360000, 0x380000),
                ("UNKNOWN_6", 0x380000, 0x3A0000),
                ("EEPROM", 0x3A0000, 0x400000),
            ],
        )
        mc60 = lt.load_layout("MC60").layout
        self.assertEqual(mc60.region("LangPack").end, 0x7C0000)
        self.assertEqual(
            [(item.label, item.start, item.end)
             for item in lt.partition_layout(mc60)[4:7]],
            [
                ("LangPack", 0x760000, 0x7C0000),
                ("UNKNOWN_5", 0x7C0000, 0x7E0000),
                ("UNKNOWN_6", 0x7E0000, 0x800000),
            ],
        )
        self.assertEqual(
            lt.load_layout("M55").layout.partition_boundaries,
            lt.load_layout("M56").layout.partition_boundaries,
        )
        statistic_offsets = {
            "A52": 0x07FE00, "A55": 0x07FE00, "C55": 0x07FE00,
            "A60": 0x07FE00, "M55": 0x87FE00, "S55": 0x47FE00,
            "SL55": 0x47FE00, "A65": 0x87FE00, "C60": 0x87FE00,
            "MC60": 0x87FE00, "CF62": 0x87FE00,
        }
        entry_transfer_offsets = {
            model: (0x47FFFC if model in {"S55", "SL55"} else 0x07FFFC)
            for model in statistic_offsets
        }
        for model, offset in statistic_offsets.items():
            with self.subTest(statistics_model=model):
                model_layout = lt.load_layout(model).layout
                self.assertEqual(model_layout.statistic_offset, offset)
                self.assertEqual(
                    model_layout.entry_transfer_offset,
                    entry_transfer_offsets[model],
                )
                owner = next(
                    item for item in lt.partition_layout(model_layout)
                    if item.start <= offset < item.end
                )
                self.assertEqual(owner.end, offset + 0x200)

        invalid = lt.FlashLayout(
            "INVALID", 0, 0x40000,
            (lt.LayoutRegion("NAMED", 0x10000, 0x20000),),
            partition_boundaries=(0x20000,),
        )
        with self.assertRaisesRegex(lt.FirmwareError, "named regions"):
            lt.partition_layout(invalid)

        with tempfile.TemporaryDirectory() as directory:
            catalog = Path(directory) / "layouts.yaml"
            catalog.write_text("""
phones:
  - name: BAD
    base: 0
    length: 0x40000
    partition_boundaries: [0x18000]
    regions: []
""")
            with self.assertRaisesRegex(
                    lt.FirmwareError, "partition_boundaries"):
                lt.load_flash_layout(catalog, "BAD")

            catalog.write_text("""
phones:
  - name: BAD
    base: 0
    length: 0x40000
    partition_boundaries: [0x20000]
    regions:
      - {name: NAMED, offset: 0x10000, length: 0x20000}
""")
            with self.assertRaisesRegex(
                    lt.FirmwareError, "inside named regions: 0x20000"):
                lt.load_flash_layout(catalog, "BAD")

        rendered = render_layout_reference()
        self.assertIn("| UNKNOWN_4 | `B40000` | `B5FFFF`", rendered)
        self.assertIn("| UNKNOWN_5 | `B60000` | `B7FFFF`", rendered)
        self.assertIn("| UNKNOWN_6 | `B80000` | `B9FFFF`", rendered)

    def test_embedded_prefix_suffix_and_duplicate_prevention(self) -> None:
        layout = lt.load_layout("C55").layout
        plan = plan_split(layout.length + 12, layout, -5)
        self.assertEqual(plan.slices[0].filename,
                         "SOURCE-00000000-00000004_FF-outside-layout.bin")
        self.assertEqual(
            plan.slices[-1].filename,
            "SOURCE-00800005-0080000B_FF-outside-layout.bin",
        )
        self.assertEqual(plan.outside_layout_bytes, 12)
        self.assertEqual(plan.mapped_bytes, layout.length)

        duplicate = lt.FlashLayout(
            "DUP", 0, 0x10000,
            (
                lt.LayoutRegion("FFS_A", 0, 0x8000),
                lt.LayoutRegion("FFS-A", 0x8000, 0x8000),
            ),
        )
        with self.assertRaisesRegex(lt.FirmwareError, "duplicate"):
            plan_split(duplicate.length, duplicate, 0)

    def test_cli_explicit_manifest_and_round_trip(self) -> None:
        data = bytes(range(256)) * 0x301
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "chunk.raw"
            output = root / "pieces"
            source.write_bytes(data)
            status, stdout, stderr = self.run_main([
                "split", str(source), "-o", str(output),
                "--layout", "C55", "--flash-address", "0x87FF00",
                "--file-offset", "0x100",
            ])
            self.assertEqual((status, stderr), (0, ""))
            manifest_path = output / MANIFEST_NAME
            document = json.loads(manifest_path.read_text())
            payload = b"".join(
                (output / item["filename"]).read_bytes()
                for item in document["slices"]
            )

            self.assertEqual(document["schema"], "siemens-fullflash-split")
            self.assertEqual(document["schema_version"], 6)
            self.assertEqual(
                document["placement"]["selection_method"], "explicit-address"
            )
            self.assertEqual(
                document["placement"]["source_to_layout_shift"], 0x7FE00
            )
            self.assertEqual(payload, data)
            self.assertEqual(
                document["output"]["round_trip_sha256"],
                hashlib.sha256(data).hexdigest(),
            )
            self.assertEqual(
                stdout.splitlines()[-1], str(manifest_path)
            )
            self.assertEqual(
                [item["order"] for item in document["slices"]],
                list(range(len(document["slices"]))),
            )
            self.assertTrue(all(
                item["size"] == item["source_range"]["length"]
                for item in document["slices"]
            ))
            self.assertEqual(document["reset"]["storage"], "low-alias")
            self.assertFalse(document["reset"]["duplicates_slice_bytes"])
            self.assertNotIn("reset-vector", " ".join(
                item["filename"] for item in document["slices"]
            ))

    def test_erased_mapped_slices_are_implicit_and_cleanup_current_schema(
        self,
    ) -> None:
        loaded = lt.load_layout("C55")
        audit = {
            "content_type": "bin",
            "layout_resolution": {
                "selected": {"selection": "test"},
            },
            "bcore": {"selected": None},
        }
        data = b"\xFF" * 0x200
        plan = plan_split(len(data), loaded.layout, -0x100)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "split"
            output.mkdir()
            old_payload = output / "old-erased.bin"
            old_payload.write_bytes(b"\xFF" * 0x100)
            unrelated = output / "notes.txt"
            unrelated.write_text("keep")
            (output / MANIFEST_NAME).write_text(json.dumps({
                "schema": "siemens-fullflash-split",
                "schema_version": 6,
                "slices": [{"filename": old_payload.name}],
            }))

            manifest_path = write_split(
                data, output, loaded, audit, plan, force=True
            )
            document = json.loads(manifest_path.read_text())
            outside, mapped = document["slices"]
            self.assertIn("filename", outside)
            self.assertNotIn("erased", outside)
            self.assertEqual((output / outside["filename"]).read_bytes(),
                             b"\xFF" * 0x100)
            self.assertTrue(mapped["erased"])
            self.assertNotIn("filename", mapped)
            self.assertNotIn("normalization", mapped)
            self.assertEqual(document["output"]["slice_count"], 2)
            self.assertEqual(document["output"]["materialized_file_count"], 1)
            self.assertEqual(
                document["output"]["round_trip_sha256"],
                hashlib.sha256(data).hexdigest(),
            )
            self.assertFalse(old_payload.exists())
            self.assertEqual(unrelated.read_text(), "keep")

    def test_older_split_schemas_are_rejected(self) -> None:
        loaded = lt.load_layout("C55")
        audit = {
            "content_type": "bin",
            "layout_resolution": {"selected": {"selection": "test"}},
            "bcore": {"selected": None},
        }
        data = b"X" * 0x100
        plan = plan_split(len(data), loaded.layout, 0)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            for version in range(1, 6):
                (output / MANIFEST_NAME).write_text(json.dumps({
                    "schema": "siemens-fullflash-split",
                    "schema_version": version,
                    "slices": [{"filename": "old.bin"}],
                }))
                with self.subTest(version=version), self.assertRaisesRegex(
                    lt.FirmwareError, "unsupported schema"
                ):
                    write_split(
                        data, output, loaded, audit, plan, force=True
                    )

    def test_dual_chip_orders_have_identical_canonical_split_trees(self) -> None:
        secondary = bytearray(b"\xA5" * 0x400000)
        primary = bytearray(b"\x5A" * 0x800000)
        self._write_bcore(primary, b"SL55", field=bytes(range(16)))
        self._write_metadata(primary, 0)
        images = {
            "primary": bytes(primary + secondary),
            "secondary": bytes(secondary + primary),
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifests = {}
            trees = {}
            orders = {}
            for name, image in images.items():
                source = root / f"{name}.bin"
                output = root / name
                source.write_bytes(image)
                status, _, error = self.run_main([
                    "split", str(source), "-o", str(output)
                ])
                self.assertEqual((status, error), (0, ""))
                document = json.loads(
                    (output / MANIFEST_NAME).read_text()
                )
                manifests[name] = (output / MANIFEST_NAME).read_bytes()
                trees[name] = [
                    (item["filename"], (output / item["filename"]).read_bytes())
                    for item in document["slices"]
                ]
                audit = ff.audit_fullflash(image)
                orders[name] = audit["representation"]["chip_order"]

            self.assertEqual(orders, {
                "primary": "primary-first",
                "secondary": "secondary-first",
            })
            self.assertEqual(trees["primary"], trees["secondary"])
            self.assertEqual(manifests["primary"], manifests["secondary"])
            manifest = json.loads(manifests["primary"])
            self.assertEqual(
                manifest["placement"]["normalization"],
                "logical-address-order",
            )
            self.assertNotIn("chip_order", json.dumps(manifest))
            stored = b"".join(
                payload for _filename, payload in trees["primary"]
            )
            self.assertEqual(
                hashlib.sha256(stored).hexdigest(),
                manifest["output"]["materialized_payloads_sha256"],
            )
            self.assertEqual(
                manifest["output"]["round_trip_sha256"],
                hashlib.sha256(secondary + primary).hexdigest(),
            )
            self.assertTrue(all(
                item["sha256"] == hashlib.sha256(
                    (output / item["filename"]).read_bytes()
                ).hexdigest()
                for item in document["slices"]
            ))
            self.assertTrue(all(
                item["actual_layout_range"] is not None
                and item["canonical_native_range"] is not None
                for item in document["slices"]
            ))

    def test_bcore_normalization_scope_and_manifest(self) -> None:
        field = bytes(range(16))
        cases = (
            ("C55", b"C55", 0x20000, True),
            ("C55", b"A55", 0x20000, True),
            ("C55", b"C55", 0x10000, False),
            ("A50", b"A50", 0x10000, False),
            ("A70", b"A70", 0x10000, False),
            ("AX72", b"AX72", 0x20000, False),
            ("M55", b"M56", 0x10000, False),
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, (layout_name, model, size, expected) in enumerate(cases):
                with self.subTest(layout=layout_name, size=size):
                    layout = lt.load_layout(layout_name).layout
                    image = bytearray(b"\xFF" * size)
                    self._write_bcore(image, model, field=field)
                    source = root / f"source-{index}.bin"
                    output = root / f"split-{index}"
                    source.write_bytes(image)
                    bcore = layout.region("BCORE")
                    status, _stdout, stderr = self.run_main([
                        "split", str(source), "-o", str(output),
                        "--layout", layout_name,
                        "--flash-address", hex(layout.base + bcore.offset),
                    ])
                    self.assertEqual(status, 0, stderr)
                    manifest = json.loads(
                        (output / MANIFEST_NAME).read_text()
                    )
                    item = manifest["slices"][0]
                    payload = (output / item["filename"]).read_bytes()
                    self.assertEqual(
                        payload[0x330:0x340],
                        b"\xFF" * 16 if expected else field,
                    )
                    self.assertEqual(
                        item["normalization"] is not None, expected
                    )
                    if expected:
                        self.assertEqual(
                            item["normalization"]["stored_value"], field.hex()
                        )
                        self.assertEqual(
                            item["normalization"]["source_sha256"],
                            hashlib.sha256(image).hexdigest(),
                        )
                        self.assertEqual(
                            manifest["output"]["round_trip_sha256"],
                            hashlib.sha256(image).hexdigest(),
                        )
    def test_auto_placement_errors_and_container_rejection(self) -> None:
        layout = lt.load_layout("C55").layout
        image = bytearray(b"\xFF") * layout.length
        self._write_bcore(image)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "c55.bin"
            output = root / "auto"
            source.write_bytes(image)
            status, _, error = self.run_main([
                "split", str(source), "-o", str(output)
            ])
            self.assertEqual((status, error), (0, ""))
            document = json.loads((output / MANIFEST_NAME).read_text())
            self.assertEqual(document["layout"]["name"], "C55/C56/CT56")
            self.assertEqual(document["output"]["slice_count"], 10)

            empty = root / "empty.bin"
            empty.write_bytes(b"")
            empty_status, _, empty_error = self.run_main([
                "split", str(empty), "-o", str(root / "empty-output")
            ])
            self.assertEqual(empty_status, 1)
            self.assertIn("input is empty", empty_error)
            self.assertFalse((root / "empty-output").exists())

            unresolved = root / "unresolved.bin"
            unresolved.write_bytes(b"\xFF" * 0x1000)
            unresolved_status, _, unresolved_error = self.run_main([
                "split", str(unresolved), "-o", str(root / "unresolved-output")
            ])
            self.assertEqual(unresolved_status, 1)
            self.assertIn("unresolved or ambiguous", unresolved_error)
            self.assertFalse((root / "unresolved-output").exists())

            container = root / "container.zip"
            container.write_bytes(b"PK\x03\x04" + b"\0" * 100)
            container_status, _, container_error = self.run_main([
                "split", str(container), "-o", str(root / "container-output"),
                "--layout", "C55", "--flash-address", "0x800000",
            ])
            self.assertEqual(container_status, 1)
            self.assertIn("not a raw firmware capture", container_error)
            self.assertFalse((root / "container-output").exists())

    def test_existing_output_and_force_preserve_unrelated(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            output = root / "split"
            source.write_bytes(b"A" * 0x30000)
            argv = [
                "split", str(source), "-o", str(output), "--layout", "C55",
                "--flash-address", "0x800000",
            ]
            first, _, first_error = self.run_main(argv)
            blocked, _, blocked_error = self.run_main(argv)
            self.assertEqual((first, first_error), (0, ""))
            self.assertEqual(blocked, 1)
            self.assertIn("already exists", blocked_error)
            stale = output / "82-87_FF-unknown1.bin"
            self.assertTrue(stale.exists())
            unrelated = output / "notes.txt"
            unrelated.write_text("keep")

            source.write_bytes(b"B" * 0x10000)
            forced, _, forced_error = self.run_main(argv + ["--force"])
            self.assertEqual((forced, forced_error), (0, ""))
            self.assertFalse(stale.exists())
            self.assertEqual(unrelated.read_text(), "keep")
            document = json.loads((output / MANIFEST_NAME).read_text())
            self.assertEqual(len(document["slices"]), 1)
            self.assertEqual(
                (output / document["slices"][0]["filename"]).read_bytes(),
                b"B" * 0x10000,
            )

    def test_default_output_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "capture.bin"
            source.write_bytes(b"x" * 0x100)
            status, stdout, stderr = self.run_main([
                "split", str(source), "--layout", "C55",
                "--flash-address", "0x800000",
            ])
            output = Path(directory) / "capture.split"
            self.assertEqual((status, stderr), (0, ""))
            self.assertTrue((output / MANIFEST_NAME).is_file())
            self.assertEqual(
                stdout.splitlines()[-1], str(output / MANIFEST_NAME)
            )

    def test_write_preflights_non_file_output(self) -> None:
        layout = lt.load_layout("C55")
        data = b"x" * 0x100
        audit = ff.audit_fullflash(
            data, layout.layout, flash_address=0x800000
        )
        plan = plan_split(len(data), layout.layout, 0)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "split"
            output.mkdir()
            (output / plan.slices[0].filename).mkdir()
            with self.assertRaisesRegex(lt.FirmwareError, "not a file"):
                write_split(
                    data, output, layout, audit, plan, True
                )


if __name__ == "__main__":
    unittest.main()
