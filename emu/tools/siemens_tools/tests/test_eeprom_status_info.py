from __future__ import annotations

import tempfile
import unittest
from datetime import date
from pathlib import Path

from tools.siemens_tools.eeprom.status_info import (
    decode_status_info,
    encode_status_info,
    load_status_info,
    replace_status_info_payload,
)


class EepromStatusInfoTests(unittest.TestCase):
    def test_c55_canonical_decoding(self) -> None:
        payload = bytearray(b"\x00" * 64)
        payload[0x0B:0x15] = bytes.fromhex("03a3001801880110914f")
        info = decode_status_info(bytes(payload))
        self.assertEqual(info.production_date, date(2003, 10, 3))
        self.assertEqual(info.standard_map_sw, "1/24")
        self.assertEqual(info.d_map_provider_text, "1/136")
        self.assertEqual(info.variant, "P914")

    def test_m55_canonical_decoding_uses_same_layout(self) -> None:
        payload = bytearray(b"\xff" * 64)
        payload[0x0B:0x15] = bytes.fromhex("0839005b01a73701915f")
        info = decode_status_info(bytes(payload))
        self.assertEqual(info.production_date, date(2009, 3, 8))
        self.assertEqual(info.standard_map_sw, "1/91")
        self.assertEqual(info.d_map_provider_text, "55/167")
        self.assertEqual(info.variant, "A915")

    def test_erased_placeholders_and_invalid_variant(self) -> None:
        info = decode_status_info(b"\xff" * 64)
        self.assertIsNone(info.production_date)
        self.assertEqual(info.standard_map_sw, "-/-")
        self.assertEqual(info.d_map_provider_text, "-/-")
        self.assertEqual(info.variant, "?FFF")

    def test_numbers_are_binary_but_suffix_is_nibbles(self) -> None:
        payload = bytearray(b"\x00" * 64)
        payload[0x0B:0x15] = bytes.fromhex("175400072a630802376a")
        info = decode_status_info(bytes(payload))
        self.assertEqual(info.production_date, date(2004, 5, 23))
        self.assertEqual(info.standard_map_sw, "42/7")
        self.assertEqual(info.d_map_provider_text, "8/99")
        self.assertEqual(info.variant, "B376")

    def test_sentinel_round_trip_preserves_opaque_and_ignored_bits(self) -> None:
        template = bytes(range(64))
        encoded = encode_status_info(
            template,
            production_date=date(2004, 5, 23),
            standard_sw=7,
            standard_map=42,
            d_map_provider=99,
            d_map=8,
            variant="B376",
        )
        self.assertEqual(decode_status_info(encoded).variant, "B376")
        changed = {i for i, (a, b) in enumerate(zip(template, encoded)) if a != b}
        self.assertLessEqual(changed, {0x0B, 0x0C, 0x0E, 0x0F, 0x10,
                                      0x11, 0x12, 0x13, 0x14})
        self.assertEqual(encoded[0x0D], template[0x0D])
        self.assertEqual(encoded[0x14] & 0x0F, template[0x14] & 0x0F)
        self.assertEqual(encoded[:0x0B], template[:0x0B])
        self.assertEqual(encoded[0x15:], template[0x15:])

    def test_wrong_length_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "length is 63"):
            decode_status_info(b"\0" * 63)

    def test_load_rejects_missing_record(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "empty.map"
            source.write_text("[MapFileInfo]\n", encoding="ascii")
            with self.assertRaisesRegex(ValueError, "5005 is missing"):
                load_status_info(source)

    def test_source_replacement_requires_binary_dump(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "one.map"
            output = Path(tmp) / "out.bin"
            source.write_text(
                "[MapFileInfo]\n[5005]\nDataSize = 64\nData {\n" +
                " ".join("0xFF" for _ in range(64)) + "\n}\n",
                encoding="ascii",
            )
            with self.assertRaisesRegex(ValueError, "binary dump"):
                replace_status_info_payload(source, output, b"\xff" * 64)


if __name__ == "__main__":
    unittest.main()
