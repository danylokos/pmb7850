#!/usr/bin/env python3
"""Unified fullflash region-mining and placement tests."""

from __future__ import annotations

from tools.bundled_firmware import image as bundled_image

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from tools.siemens_tools import eeprom
from tools.siemens_tools import fullflash as ff
from tools.siemens_tools import layout as lt
from tools.siemens_tools.fullflash.cli import main as fullflash_main


class FullflashMiningTests(unittest.TestCase):
    @staticmethod
    def layout() -> lt.FlashLayout:
        return lt.FlashLayout(
            "TST", 0x800000, 0x100000,
            (
                lt.LayoutRegion("BCORE", 0x00000, 0x10000),
                lt.LayoutRegion("T9", 0x20000, 0x10000),
                lt.LayoutRegion("LangPack", 0x40000, 0x10000),
                lt.LayoutRegion("EEPROM", 0xA0000, 0x30000),
            ),
            reset_offset=0,
        )

    @staticmethod
    def _write_bcore(data: bytearray, base: int = 0,
                     model: bytes = b"TST", software: int = 0x24) -> None:
        data[base:base + 4] = bytes.fromhex("FA 80 34 12")
        data[base + 0x1234] = 0
        data[base + 0x300:base + 0x30C] = bytes.fromhex(
            "00 01 4C 53 01 00 00 01 70 01 80 00"
        )
        data[base + 0x30C:base + 0x31C] = (
            model + b"\0" * (16 - len(model))
        )
        data[base + 0x31C:base + 0x324] = b"SIEMENS\0"
        data[base + 0x32C] = software
        data[base + 0x104:base + 0x109] = b"test\0"
        data[base + 0x114:base + 0x120] = b"240101120000"

    @staticmethod
    def _write_metadata(data: bytearray, offset: int = 0x7FF50,
                        model: bytes = b"TST") -> None:
        data[offset:offset + 16] = bytes.fromhex(
            "24 FF 0A 50 14 14 01 00 D5 21 00 00 FF FF FF FF"
        )
        data[offset + 0x10:offset + 0x40] = b"\0" * 0x30
        data[offset + 0x10:offset + 0x14] = b"lg1\0"
        data[offset + 0x20:offset + 0x20 + len(model)] = model
        data[offset + 0x20 + len(model)] = 0
        data[offset + 0x30:offset + 0x38] = b"SIEMENS\0"

    @classmethod
    def image(cls) -> bytes:
        data = bytearray(b"\xFF") * cls.layout().length
        cls._write_bcore(data)
        data[0x20000:0x20010] = bytes.fromhex(
            "54 39 00 00 A0 E8 03 00 04 02 53 39 01 01 00 00"
        )
        data[0x20010:0x20020] = b"\0" * 16
        langpack = (
            b"\xBB\xBB\x00\x00\x53\xD4\x10\x00"
            b"\x5A\x00\x8A\x00\x82\x00\x07\x00"
            b"\0\0\xEC\0\0@lg1\0\xFF\xFF\xFF"
        )
        data[0x40000:0x40000 + len(langpack)] = langpack
        cls._write_metadata(data)
        identity = eeprom.generate_identity_bundle(
            "11223344556677", 0x150D0442
        )
        blocks = {3: b"test"}
        classes = {3: 2}
        versions = {3: 1}
        for text_id, payload in identity["blocks"].items():
            block_id = int(text_id)
            blocks[block_id] = bytes.fromhex(payload)
            classes[block_id] = 2 if block_id == 76 else 8
            versions[block_id] = 0
        packed, _description = ff.pack_m55_eeprom(
            blocks, classes, versions
        )
        data[0xA0000:0xD0000] = packed
        return bytes(data)

    def test_full_prefix_suffix_interior_and_standalone_regions(self) -> None:
        data = self.image()
        layout = self.layout()
        cases = (
            ("exact-fullflash", data, None),
            ("prefix-capture", data[:0x50000], None),
            ("suffix-capture", data[0x20000:], None),
            ("interior-slice", data[0x20000:0x50000], None),
            ("standalone-region", data[:0x10000], None),
            ("standalone-region", data[0xA0000:0xD0000], None),
        )
        for expected, capture, address in cases:
            with self.subTest(expected=expected, size=len(capture)):
                result = ff.audit_fullflash(capture, layout, address)
                self.assertEqual(result["layout_status"], "mapped")
                self.assertEqual(result["representation"]["kind"], expected)

        full = ff.audit_fullflash(data, layout)
        self.assertEqual(full["schema_version"], 3)
        self.assertEqual(full["reset"]["status"], "valid")
        self.assertEqual(full["cold_boot"]["status"], "feasible")
        self.assertEqual(full["bcore"]["selected"]["model"], "TST")
        self.assertEqual(full["eeprom"]["selected_block_count"], 5)
        self.assertEqual(
            full["eeprom"]["imeis"],
            {"76": "11223344556677", "5009": "11223344556677"},
        )
        self.assertEqual(len(full["eeprom"]["blocks"]), 5)

    def test_embedded_repeated_conflicting_and_explicit_mapping(self) -> None:
        data = self.image()
        layout = self.layout()
        embedded = ff.audit_fullflash(b"container" + data + b"tail", layout)
        repeated = ff.audit_fullflash(data + data, layout)

        conflict = bytearray(b"\xFF") * 0x90000
        self._write_bcore(conflict)
        self._write_metadata(conflict, 0x8FF50)
        conflicting = ff.audit_fullflash(bytes(conflict), layout)

        arbitrary = bytes(range(0x80))
        explicit = ff.audit_fullflash(
            arbitrary, layout, flash_address=0x830000, file_offset=0x20
        )

        self.assertEqual(embedded["representation"]["kind"], "embedded-view")
        self.assertEqual(
            embedded["bcore"]["selected"]["header_file_offset"], "0x309"
        )
        self.assertEqual(
            embedded["representation"]["source_range"]["from"], len(b"container")
        )
        self.assertEqual(embedded["reset"]["status"], "valid")
        self.assertEqual(embedded["eeprom"]["selected_block_count"], 5)
        self.assertEqual(repeated["layout_status"], "unresolved")
        self.assertEqual(
            repeated["representation"]["kind"], "repeated-container"
        )
        self.assertGreaterEqual(
            len(repeated["layout_resolution"]["candidates"]), 2
        )
        self.assertEqual(conflicting["layout_status"], "unresolved")
        self.assertEqual(
            explicit["layout_resolution"]["selected"]["selection"],
            "explicit-address",
        )
        self.assertEqual(
            explicit["representation"]["layout_range"]["from"], 0x2FFE0
        )

    def test_false_positive_rejection(self) -> None:
        stray = bytearray(b"\xFF") * 0x50000
        stray[0x100:0x107] = b"SIEMENS"
        stray[0x1000:0x1006] = b"EELITE"
        stray[0x2000:0x2002] = b"T9"
        self._write_bcore(stray, 0x10000, model=b"AAAAAA")
        stray[0x10000] = 0xFF
        stray[0x1032C] = 0xFA
        self._write_metadata(stray, 0x2FF50)
        stray[0x2FF50] = 0xFA
        self._write_metadata(stray, 0x3FF50, model=b"lower")

        malformed_eeprom = bytearray(b"\xFF") * 0x30000
        malformed_eeprom[0x10:0x18] = b"\xFE\xFEEELITE"
        malformed_eeprom[0x10010:0x10018] = b"\xFE\xFEEFULL"
        malformed_eeprom[0x20010:0x20018] = b"\xFE\xFEEFULL"

        result = ff.audit_fullflash(bytes(stray), self.layout())
        malformed = ff.audit_fullflash(bytes(malformed_eeprom), self.layout())
        kinds = {finding["kind"] for finding in result["findings"]}
        malformed_kinds = {
            finding["kind"] for finding in malformed["findings"]
        }
        self.assertNotIn("bcore", kinds)
        self.assertNotIn("eeprom", kinds)
        self.assertNotIn("eeprom", malformed_kinds)
        self.assertNotIn("firmware-metadata", kinds)
        self.assertNotIn("region-marker", kinds)
        self.assertEqual(result["layout_status"], "unresolved")

    def test_json_always_has_blocks_and_text_blocks_flag_expands(self) -> None:
        eeprom_data = self.image()[0xA0000:0xD0000]
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "eeprom.bin"
            source.write_bytes(eeprom_data)
            json_out = io.StringIO()
            text_out = io.StringIO()
            with contextlib.redirect_stdout(json_out):
                json_status = fullflash_main([
                    "info", str(source), "--layout", "M55", "--json"
                ])
            with contextlib.redirect_stdout(text_out):
                text_status = fullflash_main([
                    "info", str(source), "--layout", "M55", "--blocks"
                ])
        document = json.loads(json_out.getvalue())
        self.assertEqual((json_status, text_status), (0, 0))
        self.assertEqual(len(document["eeprom"]["blocks"]), 5)
        self.assertIn("len=", text_out.getvalue())

        standalone = ff.audit_fullflash(eeprom_data)
        self.assertEqual(standalone["layout_status"], "unresolved")
        self.assertEqual(standalone["eeprom"]["selected_block_count"], 5)
        prefixed = ff.audit_fullflash(b"prefix" + eeprom_data)
        self.assertEqual(prefixed["layout_status"], "unresolved")
        self.assertEqual(prefixed["eeprom"]["region_file_base"], "0x6")

    def test_text_prints_reset_vector_separately_from_bcore(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "fullflash.bin"
            source.write_bytes(self.image())
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = fullflash_main([
                    "info", str(source), "--layout", "C55",
                    "--flash-address", "0x800000",
                ])

        self.assertEqual(status, 0)
        lines = output.getvalue().splitlines()
        self.assertIn("reset vector: fa 80 34 12", lines)
        bcore_line = next(line for line in lines if line.startswith("bcore"))
        self.assertNotIn("reset", bcore_line)
        self.assertIn("header-file-offset=0x300", bcore_line)

    def test_real_m55_suffix_and_standalone_bcore(self) -> None:
        data = bundled_image("m55").read_bytes()
        region = lt.load_layout("M55").layout.region("BCORE")
        suffix_result = ff.audit_fullflash(data[0x200000:])
        bcore_result = ff.audit_fullflash(data[region.offset:region.end])
        selected = suffix_result["layout_resolution"]["selected"]
        self.assertEqual(selected["layout"], "M55/M56")
        self.assertEqual(selected["shift"], 0x200000)
        self.assertEqual(
            suffix_result["representation"]["native_range"],
            {"from": 0x200000, "to": 0x1000000},
        )
        self.assertEqual(
            suffix_result["missing_ranges"],
            [{"from": "0x0", "to_exclusive": "0x200000",
              "length": 0x200000}],
        )
        self.assertEqual(suffix_result["reset"]["status"], "not-covered")
        self.assertEqual(
            bcore_result["representation"]["kind"], "standalone-region"
        )
        self.assertEqual(bcore_result["bcore"]["selected"]["model"], "M55")

    def test_real_complete_c55_and_c60_suffix(self) -> None:
        complete = ff.audit_fullflash(bundled_image("c55").read_bytes())
        suffix = ff.audit_fullflash(bundled_image("c60").read_bytes()[0x200000:])
        self.assertEqual(
            (complete["layout"]["name"], complete["representation"]["kind"]),
            ("C55/C56/CT56", "exact-fullflash"),
        )
        self.assertEqual(complete["cold_boot"]["status"], "feasible")
        self.assertEqual(
            (suffix["layout"]["name"],
             suffix["layout_resolution"]["selected"]["shift"]),
            ("A65/C60", 0x200000),
        )
        self.assertEqual(suffix["reset"]["status"], "not-covered")

    def test_real_eeprom_capture_geometries(self) -> None:
        for model, size in (("S55", 0x20000), ("M55", 0x30000), ("C55", 0x60000)):
            region = lt.load_layout(model).layout.region("EEPROM")
            data = bundled_image(model).read_bytes()[region.offset:region.end]
            with self.subTest(model=model):
                result = ff.audit_fullflash(
                    data, lt.load_layout(model).layout
                )
                self.assertEqual(result["size"], size)
                self.assertEqual(result["eeprom"]["status"], "parsed")
                self.assertGreater(result["eeprom"]["selected_block_count"], 0)
                self.assertEqual(
                    result["eeprom"]["imei"]["consistent"], True
                )


if __name__ == "__main__":
    unittest.main()
