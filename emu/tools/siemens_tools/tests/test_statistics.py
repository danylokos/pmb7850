from __future__ import annotations

import copy
import struct
import unittest

from tools.siemens_tools.fullflash.normalization import (
    normalize_partition,
    restore_normalization,
)
from tools.siemens_tools.fullflash.statistics import (
    PAGE_SIZE,
    inspect_statistics,
    normalize_statistics,
    restore_statistics,
)
from tools.siemens_tools.layout import FirmwareError, load_layout


class StatisticsTests(unittest.TestCase):
    @staticmethod
    def page(*, template: bool = False, trampoline: bytes = b"\xFA\x07\xF0\xFF") -> bytes:
        page = bytearray(b"\xFF" * PAGE_SIZE)
        struct.pack_into("<HH", page, 0x10, 0x8774, 0x11F8)
        if not template:
            values = {
                0x18: 0x1720,
                0x1A: 3,
                0x1C: 0,
                0x1E: 0,
                0x20: 2,
                0x24: 0x10,
                0x26: 0x20,
                0x28: 0x17,
                0x2E: 0,
                0x30: 3,
                0x3A: 0,
                0x3C: 0,
            }
            for offset, value in values.items():
                struct.pack_into("<H", page, offset, value)
            page[0x40:0x50] = b"03.06.0314:16:36"
            page[0x60:0x70] = b"02.06.0312:11:26"
        struct.pack_into("<II", page, 0x80, 0x00020000, 0x0007FE00)
        page[0x88:0x8C] = b"\xFF" * 4
        page[0x100:0x1FC] = bytes(range(0xFC))
        page[0x1FC:0x200] = trampoline
        return bytes(page)

    def test_installed_field_round_trip(self) -> None:
        page = self.page()
        parsed = inspect_statistics(page)
        self.assertEqual(parsed["status"], "parsed")
        self.assertEqual(parsed["record_kind"], "installed")
        self.assertEqual(parsed["fields"]["xor16"]["value"], 0x8774)
        self.assertEqual(parsed["fields"]["generation"]["value"], 3)
        self.assertEqual(parsed["fields"]["unknown_1"]["raw"], "0000")
        self.assertEqual(
            parsed["fields"]["descriptor_0_unknown_1"]["raw"], "ffff"
        )
        self.assertEqual(
            parsed["fields"]["current_timestamp"]["text"],
            "03.06.0314:16:36",
        )
        self.assertEqual(parsed["checksum_ranges"], [{
            "start": {"offset": 0x80, "length": 4, "value": 0x20000},
            "end": {"offset": 0x84, "length": 4, "value": 0x7FE00},
        }])
        self.assertEqual(parsed["trampoline"]["state"], "jmps")
        self.assertEqual(parsed["trampoline"]["target"], 0x07FFF0)

        payload = b"prefix" + page + b"suffix"
        normalized = normalize_statistics(
            payload,
            page_offset=6,
            layout_offset=0x7FE00,
            full_boundary_present=True,
        )
        self.assertIsNotNone(normalized)
        assert normalized is not None
        self.assertEqual(normalized.payload[6:6 + 0x100], b"\xFF" * 0x100)
        self.assertEqual(
            normalized.payload[6 + 0x100:6 + 0x1FC],
            page[0x100:0x1FC],
        )
        self.assertEqual(
            normalized.payload[6 + 0x1FC:6 + 0x200], b"\xFF" * 4
        )
        self.assertNotIn("statistics_page", normalized.metadata)
        self.assertEqual(
            restore_statistics(normalized.payload, normalized.metadata), payload
        )

    def test_package_template_and_raw_field_forms(self) -> None:
        page = self.page(template=True, trampoline=b"\xFF" * 4)
        parsed = inspect_statistics(page)
        self.assertEqual(parsed["status"], "parsed")
        self.assertEqual(parsed["record_kind"], "package-template")
        self.assertEqual(
            parsed["fields"]["compact_flash_id"],
            {"offset": 0x18, "length": 2, "raw": "ffff"},
        )
        self.assertEqual(
            parsed["fields"]["current_timestamp"]["raw"], "ff" * 16
        )
        self.assertEqual(parsed["trampoline"]["state"], "erased")
        normalized = normalize_statistics(
            page,
            page_offset=0,
            layout_offset=0x7FE00,
            full_boundary_present=True,
        )
        self.assertIsNotNone(normalized)
        assert normalized is not None
        self.assertEqual(
            restore_statistics(normalized.payload, normalized.metadata), page
        )

    def test_malformed_ranges_timestamps_and_trampolines(self) -> None:
        malformed_ranges = bytearray(self.page())
        malformed_ranges[0x80:0x100] = b"\0" * 0x80
        parsed = inspect_statistics(bytes(malformed_ranges))
        self.assertEqual(parsed["status"], "malformed")
        self.assertIn("invalid checksum range", parsed["reason"])
        self.assertIsNone(normalize_statistics(
            bytes(malformed_ranges),
            page_offset=0,
            layout_offset=0x7FE00,
            full_boundary_present=True,
        ))

        malformed_time = bytearray(self.page())
        malformed_time[0x40:0x50] = b"99.99.9999:99:99"
        parsed = inspect_statistics(bytes(malformed_time))
        self.assertEqual(parsed["status"], "parsed")
        self.assertEqual(
            parsed["fields"]["current_timestamp"]["raw"],
            malformed_time[0x40:0x50].hex(),
        )
        self.assertEqual(
            inspect_statistics(self.page(trampoline=b"\x12\x34\x56\x78"))[
                "trampoline"
            ]["state"],
            "unrecognized",
        )
        self.assertEqual(
            inspect_statistics(self.page(trampoline=b"\xFF" * 4))[
                "trampoline"
            ]["state"],
            "erased",
        )

    def test_tampered_metadata_and_payload_are_rejected(self) -> None:
        normalized = normalize_statistics(
            self.page(),
            page_offset=0,
            layout_offset=0x7FE00,
            full_boundary_present=True,
        )
        assert normalized is not None
        mutations = []
        extra = copy.deepcopy(normalized.metadata)
        extra["aggregate"] = "forbidden"
        mutations.append(extra)
        field = copy.deepcopy(normalized.metadata)
        field["fields"]["generation"]["value"] += 1
        mutations.append(field)
        range_offset = copy.deepcopy(normalized.metadata)
        range_offset["checksum_ranges"][0]["start"]["offset"] += 4
        mutations.append(range_offset)
        trampoline = copy.deepcopy(normalized.metadata)
        trampoline["trampoline"]["state"] = "erased"
        mutations.append(trampoline)
        for metadata in mutations:
            with self.subTest(metadata=metadata), self.assertRaises(FirmwareError):
                restore_statistics(normalized.payload, metadata)

        payload = bytearray(normalized.payload)
        payload[0x120] ^= 1
        with self.assertRaisesRegex(FirmwareError, "hash metadata"):
            restore_statistics(bytes(payload), normalized.metadata)

    def test_dispatcher_requires_complete_structural_partition(self) -> None:
        layout = load_layout("C55").layout
        start, end = 0x20000, 0x80000
        payload = bytearray(b"\xA5" * (end - start))
        page_offset = layout.statistic_offset - start
        payload[page_offset:page_offset + PAGE_SIZE] = self.page()
        normalized, metadata = normalize_partition(
            bytes(payload),
            layout,
            role="UNKNOWN_1",
            partition_start=start,
            partition_end=end,
            full_boundary_present=True,
        )
        self.assertIsNotNone(metadata)
        self.assertEqual(
            restore_normalization(normalized, metadata), bytes(payload)
        )
        unchanged, absent = normalize_partition(
            bytes(payload[:-1]),
            layout,
            role="UNKNOWN_1",
            partition_start=start,
            partition_end=end,
            full_boundary_present=False,
        )
        self.assertEqual(unchanged, bytes(payload[:-1]))
        self.assertIsNone(absent)


if __name__ == "__main__":
    unittest.main()
