#!/usr/bin/env python3
"""EEPROM battery-calibration tooling tests."""

from __future__ import annotations

from tools.bundled_firmware import image as bundled_image

import hashlib
import json
import struct
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path

from tools.siemens_tools.eeprom import (
    CEMU_DEFAULT_BATTERY_CALIBRATION,
    AffineCorrection,
    BatteryCalibration,
    decode_battery_calibration,
    encode_battery_calibration,
    load_battery_calibration,
    synthesize_battery_calibration,
)
from tools.siemens_tools.eeprom.cli import main


STOCK = bytes.fromhex("93115210a4f9e9036200a9fe620027ff64000000")
ALTERNATE = bytes.fromhex("271351102afce603ffffffffffffffffffffffff")
A52_FLASH = bundled_image("a52")
A55_FLASH = (
    bundled_image("a55")
)
QUALIFIED_DONORS = (
    A52_FLASH,
    A55_FLASH,
    bundled_image("a60"),
    bundled_image("a62"),
    bundled_image("a65"),
    bundled_image("c55"),
    bundled_image("c60"),
    bundled_image("cf62"),
    bundled_image("m55"),
    bundled_image("mc60"),
    bundled_image("s55"),
    bundled_image("sl55"),
)


def blockheader(
    start: int, length: int, linear: int, block_id: int, end: int
) -> bytes:
    return struct.pack(
        "<HHHHHH",
        start,
        length,
        linear & 0xFFFF,
        linear >> 16,
        block_id,
        end,
    )


def battery_dump(payload: bytes = STOCK) -> bytes:
    image = bytearray(b"\xFF" * 0x60000)
    image[0x10:0x12] = b"\xFE\xFE"
    image[0x12:0x18] = b"EELITE"
    image[0x20012:0x20018] = b"EEFULL"
    image[0x40012:0x40018] = b"EEFULL"
    records = (
        blockheader(0x00FC, 4, 0xFA0400, 1, 0xFC00)
        + blockheader(0x02FC, len(payload), 0xFF0000, 67, 0xFC00)
    )
    image[0x100:0x100 + len(records)] = records
    image[0x50000:0x50000 + len(payload)] = payload
    return bytes(image)


class BatteryCalibrationTests(unittest.TestCase):
    def test_default_calibration_encodes_known_payload(self) -> None:
        self.assertEqual(
            encode_battery_calibration(CEMU_DEFAULT_BATTERY_CALIBRATION),
            STOCK,
        )
        self.assertEqual(
            decode_battery_calibration(STOCK),
            CEMU_DEFAULT_BATTERY_CALIBRATION,
        )
        self.assertEqual(
            hashlib.sha256(STOCK).hexdigest(),
            "c4247e2de00dc5e625f29fe4653b513b1e576ecbc597f9672614c640ccc70d8f",
        )

    def test_qualified_donors_share_block67_structure(self) -> None:
        for path in QUALIFIED_DONORS:
            with self.subTest(path=path):
                _kind, block, calibration = load_battery_calibration(path)
                self.assertEqual(len(block.payload), 20)
                self.assertEqual(block.memory_class, 2)
                self.assertEqual(block.version, 2)
                self.assertEqual(
                    decode_battery_calibration(block.payload), calibration,
                )

    def test_affine_correction_truncates_toward_zero(self) -> None:
        correction = AffineCorrection(scale_percent=98, offset=-217)
        self.assertEqual(correction.apply(0), 217)
        self.assertEqual(correction.apply(101), 315)
        self.assertEqual(correction.apply(-101), 119)

    def test_known_vectors(self) -> None:
        stock = decode_battery_calibration(STOCK)
        self.assertEqual(
            (stock.low_raw, stock.low_mv, stock.high_raw, stock.high_mv),
            (4499, 3177, -1628, 4178),
        )
        alternate = decode_battery_calibration(ALTERNATE)
        self.assertEqual(
            (alternate.low_raw, alternate.low_mv,
             alternate.high_raw, alternate.high_mv),
            (4903, 3179, -982, 4177),
        )

    def test_a52_and_a55_known_vectors(self) -> None:
        _, _, a52 = load_battery_calibration(A52_FLASH)
        self.assertEqual(
            (a52.low_raw, a52.low_mv, a52.high_raw, a52.high_mv),
            (4499, 3177, -1628, 4178),
        )
        _, _, a55 = load_battery_calibration(A55_FLASH)
        self.assertEqual(
            (a55.low_raw, a55.low_mv, a55.high_raw, a55.high_mv),
            (4499, 3177, -1628, 4178),
        )

    def test_invalid_length_and_endpoints(self) -> None:
        with self.assertRaisesRegex(ValueError, "expected 20"):
            decode_battery_calibration(STOCK[:-1])
        with self.assertRaisesRegex(ValueError, "not monotonic"):
            decode_battery_calibration(b"\xFF" * 20)

    def test_inspect_human_and_json(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.bin"
            source.write_bytes(battery_dump())
            human = StringIO()
            with redirect_stdout(human):
                self.assertEqual(main(["battery", "inspect", str(source)]), 0)
            self.assertIn("low endpoint : raw 4499, 3177 mV", human.getvalue())
            self.assertIn("TBAT adjust  : 98% scale, offset -343",
                          human.getvalue())
            self.assertIn("TENV adjust  : 98% scale, offset -217",
                          human.getvalue())
            self.assertIn("reserved     : 100% scale, offset 0",
                          human.getvalue())

            encoded = StringIO()
            with redirect_stdout(encoded):
                self.assertEqual(
                    main(["battery", "inspect", str(source), "--json"]), 0
                )
            report = json.loads(encoded.getvalue())
            self.assertEqual(report["block"]["file_offset"], 0x50000)
            self.assertEqual(report["calibration"]["high_raw"], -1628)
            self.assertEqual(report["calibration"]["span_mv"], 1001)
            self.assertEqual(
                report["calibration"]["tenv"],
                {"scale_percent": 98, "offset": -217},
            )

    def test_synthesis_changes_only_first_eight_payload_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.bin"
            output = Path(directory) / "output.bin"
            before = battery_dump()
            source.write_bytes(before)
            block, result = synthesize_battery_calibration(
                source,
                output,
                low_raw=4903,
                low_mv=3179,
                high_raw=-982,
                high_mv=4177,
            )
            after = output.read_bytes()
            self.assertEqual(block.file_off, 0x50000)
            self.assertEqual(after[0x50000:0x50008], ALTERNATE[:8])
            self.assertEqual(after[0x50008:0x50014], STOCK[8:])
            changed = [
                offset for offset, pair in enumerate(zip(before, after))
                if pair[0] != pair[1]
            ]
            self.assertTrue(changed)
            self.assertTrue(all(0x50000 <= offset < 0x50008
                                for offset in changed))
            self.assertEqual(encode_battery_calibration(result)[8:], STOCK[8:])

            stdout = StringIO()
            stderr = StringIO()
            with redirect_stdout(stdout), redirect_stderr(stderr):
                self.assertEqual(main([
                    "battery", "synthesize", str(source),
                    "--low-raw", "4903", "--low-mv", "3179",
                    "--high-raw", "-982", "--high-mv", "4177",
                    "--output", str(output),
                ]), 0)
            self.assertIn("physical handset calibration", stderr.getvalue())

    def test_encoder_accepts_custom_named_values(self) -> None:
        custom = BatteryCalibration(
            low_raw=4903,
            low_mv=3179,
            high_raw=-982,
            high_mv=4177,
            tbat=AffineCorrection(scale_percent=97, offset=-300),
            tenv=AffineCorrection(scale_percent=99, offset=-200),
            reserved=AffineCorrection(scale_percent=100, offset=0),
        )
        self.assertEqual(decode_battery_calibration(
            encode_battery_calibration(custom)
        ), custom)

    def test_encoder_rejects_out_of_range_named_values(self) -> None:
        valid = CEMU_DEFAULT_BATTERY_CALIBRATION
        invalid = (
            BatteryCalibration(
                32768, valid.low_mv, valid.high_raw, valid.high_mv,
                valid.tbat, valid.tenv, valid.reserved,
            ),
            BatteryCalibration(
                valid.low_raw, valid.low_mv, valid.high_raw, valid.high_mv,
                AffineCorrection(-32769, 0), valid.tenv, valid.reserved,
            ),
            BatteryCalibration(
                valid.low_raw, valid.low_mv, valid.high_raw, valid.high_mv,
                valid.tbat, AffineCorrection(100, 32768), valid.reserved,
            ),
            BatteryCalibration(
                valid.low_raw, 0, valid.high_raw, valid.high_mv,
                valid.tbat, valid.tenv, valid.reserved,
            ),
            BatteryCalibration(
                valid.low_raw, valid.low_mv, valid.high_raw, 65536,
                valid.tbat, valid.tenv, valid.reserved,
            ),
        )
        for calibration in invalid:
            with self.subTest(calibration=calibration), self.assertRaises(ValueError):
                encode_battery_calibration(calibration)

    def test_synthesis_rejects_map_and_invalid_template(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            map_file = root / "battery.map"
            map_file.write_text(
                "[MapFileInfo]\nProduct=C55\n"
                "[67]\nDataSize = 20\nData { "
                + " ".join(f"0x{byte:02X}" for byte in STOCK)
                + " }\n",
                encoding="ascii",
            )
            with self.assertRaisesRegex(ValueError, "binary dump template"):
                synthesize_battery_calibration(
                    map_file, root / "out.bin",
                    low_raw=4903, low_mv=3179,
                    high_raw=-982, high_mv=4177,
                )

            malformed = root / "malformed.bin"
            malformed.write_bytes(battery_dump(b"\xFF" * 20))
            with self.assertRaisesRegex(ValueError, "not monotonic"):
                load_battery_calibration(malformed)


if __name__ == "__main__":
    unittest.main()
