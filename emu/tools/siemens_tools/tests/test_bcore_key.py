#!/usr/bin/env python3
"""Joker BCORE BOOTKEY derivation and recovery tests."""

from __future__ import annotations

import contextlib
import io
import json
import unittest

from tools.siemens_tools.fullflash.bcore_key import (
    build_seed,
    derive_bcore_key,
    hash_bootkey,
    parse_bcore_hash,
    parse_fsn,
    parse_skey,
    recover_bcore_key,
)
from tools.siemens_tools.fullflash.cli import main


CUSTOM_FSN = 0xA35F2F28
CUSTOM_SKEY = 12345678
CUSTOM_SEED = "282f5fa34e61bc008b613e1f4eeadd3e"
CUSTOM_BOOTKEY = "d84e49ef877e777f5da37dc4fe5149a2"
CUSTOM_HASH = "7723abc20acf34174c33f4a34459fb4e"

STOCK_FSN = 0x3EBF952C
STOCK_SKEY = 55347257
STOCK_SEED = "2c95bf3e39884c0312ac37723a9ae034"
STOCK_BOOTKEY = "ba8b5bd416d417eabc687d1f283ab6ad"
STOCK_HASH = "d023c9776d088a18298205e5ae48acc7"


class BCoreKeyTests(unittest.TestCase):
    def test_ordered_xor_fold(self) -> None:
        self.assertEqual(
            build_seed(CUSTOM_FSN, CUSTOM_SKEY).hex(), CUSTOM_SEED
        )

    def test_c55_known_answers(self) -> None:
        for fsn, skey, seed, bootkey, bcore_hash in (
            (
                CUSTOM_FSN,
                CUSTOM_SKEY,
                CUSTOM_SEED,
                CUSTOM_BOOTKEY,
                CUSTOM_HASH,
            ),
            (
                STOCK_FSN,
                STOCK_SKEY,
                STOCK_SEED,
                STOCK_BOOTKEY,
                STOCK_HASH,
            ),
        ):
            with self.subTest(fsn=f"{fsn:08X}"):
                result = derive_bcore_key(fsn, skey)
                self.assertEqual(result.seed.hex(), seed)
                self.assertEqual(result.bootkey.hex(), bootkey)
                self.assertEqual(result.bcore_hash.hex(), bcore_hash)

    def test_freia_shared_bootkey_hash(self) -> None:
        self.assertEqual(
            hash_bootkey(b"nutzoisthebest\0\0").hex(),
            "d48621ac4874bf027c2c52043e1bb456",
        )

    def test_recover(self) -> None:
        expected = derive_bcore_key(0x12345678, 2)
        recovered = recover_bcore_key(
            expected.fsn, expected.bcore_hash, workers=1
        )
        self.assertEqual(recovered, expected)

    def test_validation_errors(self) -> None:
        for value in ("A35F2F2", "A35F2F280", "not-hex!"):
            with self.subTest(fsn=value), self.assertRaises(ValueError):
                parse_fsn(value)
        for value in ("1234567", "123456789", "1234A678"):
            with self.subTest(skey=value), self.assertRaises(ValueError):
                parse_skey(value)
        for value in ("00", "z" * 32):
            with self.subTest(bcore_hash=value), self.assertRaises(ValueError):
                parse_bcore_hash(value)
        with self.assertRaises(ValueError):
            recover_bcore_key(0, bytes(16), workers=0)

    def test_derive_cli_text_and_json(self) -> None:
        text_output = io.StringIO()
        with contextlib.redirect_stdout(text_output):
            status = main([
                "bcore-key", "derive",
                "--fsn", "A35F2F28",
                "--skey", "12345678",
            ])
        self.assertEqual(status, 0)
        self.assertIn(f"BOOTKEY:    {CUSTOM_BOOTKEY}", text_output.getvalue())
        self.assertIn(f"BCORE hash: {CUSTOM_HASH}", text_output.getvalue())

        json_output = io.StringIO()
        with contextlib.redirect_stdout(json_output):
            status = main([
                "bcore-key", "derive",
                "--fsn", "0x3EBF952C",
                "--skey", "55347257",
                "--json",
            ])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(json_output.getvalue()), {
            "fsn": "3EBF952C",
            "skey": "55347257",
            "seed": STOCK_SEED,
            "bootkey": STOCK_BOOTKEY,
            "bcore_hash": STOCK_HASH,
        })

    def test_recover_cli_json(self) -> None:
        expected = derive_bcore_key(0x12345678, 0)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = main([
                "bcore-key", "recover",
                "--fsn", "12345678",
                "--hash", expected.bcore_hash.hex(),
                "--workers", "1",
                "--json",
            ])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(output.getvalue()), expected.as_dict())


if __name__ == "__main__":
    unittest.main()
