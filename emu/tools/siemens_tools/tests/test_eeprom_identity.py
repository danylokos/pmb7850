#!/usr/bin/env python3
"""EEPROM identity and security tests."""

from tools.bundled_firmware import image as bundled_image

from . import REPO_ROOT
from .eeprom_fixtures import *  # noqa: F401,F403
from tools.siemens_tools.eeprom import (
    AffineCorrection,
    BatteryCalibration,
    encode_battery_calibration,
)


class BcdTests(unittest.TestCase):
    def test_convert_to_bcd_matches_firmware_template(self) -> None:
        # Known-answer packed BCD bytes for the generated IMEI.
        self.assertEqual(convert_to_bcd(CUST_IMEI).hex(), "1a21324354657607")

    def test_convert_to_bcd_rejects_short_imei(self) -> None:
        with self.assertRaises(ValueError):
            convert_to_bcd("12345")


class CrcTests(unittest.TestCase):
    def test_crc_check_bytes_sum_then_xor(self) -> None:
        buf = bytes([0x01, 0x02, 0x04, 0x08])
        self.assertEqual(crc_check_bytes(buf, 4), (0x0F, 0x0F))
        self.assertEqual(crc_check_bytes(bytes([0xFF, 0x01]), 2), (0x00, 0xFE))


class ImeiTests(unittest.TestCase):
    def test_bcd_imei_to_normal_empty_block(self) -> None:
        self.assertEqual(bcd_imei_to_normal(bytes([0xFF] * B5009_LEN)), ("", True))

    def test_bcd_imei_to_normal_low_nibble_first(self) -> None:
        # Digit pairs are (low nibble, high nibble) per byte.
        block = bytes([0x21, 0x43, 0x65, 0x87, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00])
        imei, empty = bcd_imei_to_normal(block)
        self.assertFalse(empty)
        self.assertEqual(imei, "12345678900000")

    def test_recreate_imei_matches_known_c55_5009_blocks(self) -> None:
        # 5009 blocks recorded this session; decode without any FSN.
        cust = bytes.fromhex("284d24635566e4c77b05")
        stock = bytes.fromhex("239a687f5c66e57aace9")
        self.assertEqual(recreate_imei(cust), ("35335000894548", False))
        self.assertEqual(recreate_imei(stock), ("35208900526587", False))

    def test_recreate_imei_companion_matches_known_c55_block_76(self) -> None:
        cust = bytes.fromhex("ddd26a32f2052278caee")
        stock = bytes.fromhex("df64412cca0346fb7b24")
        self.assertEqual(recreate_imei_companion(cust), ("35335000894548", False))
        self.assertEqual(recreate_imei_companion(stock), ("35208900526587", False))

    def test_forward_records_match_both_known_c55_identities(self) -> None:
        self.assertEqual(
            tuple(x.hex() for x in create_imei_records("35335000894548")),
            ("284d24635566e4c77b05", "ddd26a32f2052278caee"),
        )
        self.assertEqual(
            tuple(x.hex() for x in create_imei_records("35208900526587")),
            ("239a687f5c66e57aace9", "df64412cca0346fb7b24"),
        )

    def test_freia_luhn_nibble(self) -> None:
        self.assertEqual(imei_check_digit("35335000894548"), 9)
        self.assertEqual(imei_check_digit("35208900526587"), 8)

    def test_generator_rejects_nonexact_imei(self) -> None:
        for value in ("123", "123456789012345", "1234567890123x", ""):
            with self.subTest(value=value), self.assertRaises(ValueError):
                create_imei_records(value)


class IdentityBundleTests(unittest.TestCase):
    def test_fsn_parser_accepts_exact_public_form(self) -> None:
        self.assertEqual(parse_fsn("1234ABCD"), 0x1234ABCD)
        self.assertEqual(parse_fsn("0x1234ABCD"), CUST_FSN)

    def test_fsn_parser_rejects_malformed_values(self) -> None:
        for value in ("", "00", "G234ABCD", "1234ABCDE"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_fsn(value)

    def test_unlocked_plaintext_matches_shared_corpus(self) -> None:
        plain5008, plain5077 = create_unlocked_plaintexts()
        self.assertEqual(
            hashlib.sha256(plain5008).hexdigest(),
            "7ef1a6e57d88f52dcd0b17e328bbe9f250f564f242bc9bab4ba7b33419e6c2bf",
        )
        self.assertEqual(
            hashlib.sha256(plain5077).hexdigest(),
            "b5f4c5bd57efde5ebaab67c415e1ce90cbd5a2779d5844de96c07e4e691cac84",
        )
        checks = [(name, p8, p77) for name, p8, p77 in verify_checks(plain5008, plain5077)]
        self.assertEqual(checks, [
            ("5008.header", 0x62, 0x64),
            ("5008.body", 0x70, 0xE0),
            ("5077.body", 0xEF, 0x3D),
        ])

    def test_bundle_decrypts_and_reencrypts_byte_exactly(self) -> None:
        bundle = generate_identity_bundle(CUST_IMEI, CUST_FSN)
        self.assertEqual(bundle["schema"], BUNDLE_SCHEMA)
        self.assertEqual(bundle["profile"], BUNDLE_PROFILE)
        self.assertEqual(bundle["crypto_model_id"], 7)
        self.assertEqual(bundle["schema_version"], 2)
        self.assertEqual(bundle["fsn"], "1234ABCD")
        self.assertEqual(
            set(bundle["blocks"]), {"76", "5008", "5009", "5077"}
        )

        bcd = convert_to_bcd(CUST_IMEI)
        enc5008 = bytes.fromhex(bundle["blocks"]["5008"])
        enc5077 = bytes.fromhex(bundle["blocks"]["5077"])
        plain5008 = bytes(decrypt_5008(enc5008, CUST_FSN, bcd))
        plain5077 = bytes(decrypt_5077(enc5077, CUST_FSN, bcd))
        self.assertEqual((plain5008, plain5077), create_unlocked_plaintexts())
        self.assertEqual(bytes(encrypt_5008(plain5008, CUST_FSN, bcd)), enc5008)
        self.assertEqual(bytes(encrypt_5077(plain5077, CUST_FSN, bcd)), enc5077)
        self.assertEqual(recreate_imei(bytes.fromhex(bundle["blocks"]["5009"])),
                         (CUST_IMEI, False))
        self.assertEqual(recreate_imei_companion(bytes.fromhex(bundle["blocks"]["76"])),
                         (CUST_IMEI, False))

    def test_embedded_default_record_oracle(self) -> None:
        bundle = generate_identity_bundle("11223344556677", 0x1234ABCD)
        expected = {
            "76": "c63737dc17fa7b288aca1d86a637b17ea8ea8f6826a914681ec3b0fc79fc1223",
            "5008": "04246558ff06899de298724696d338a7bd49049e554d73543de4058c37c96394",
            "5009": "74a484454600f3f0437360512ec1390d03fdc8cdf833d212695b14406abe7fe9",
            "5077": "939cd010b4d6088a6434a375149d7d4a60ebc2c98bc138686c44b088a0fcb999",
        }
        self.assertEqual(
            {
                block_id: hashlib.sha256(bytes.fromhex(payload)).hexdigest()
                for block_id, payload in bundle["blocks"].items()
            },
            expected,
        )
        fixture = __import__("json").loads(
            (REPO_ROOT / "cemu/eeprom.json").read_text()
        )
        self.assertEqual(fixture, generate_overlay_bundle("11223344556677", 0x1234ABCD))

    def test_bundle_accepts_custom_battery_calibration(self) -> None:
        calibration = BatteryCalibration(
            low_raw=4903,
            low_mv=3179,
            high_raw=-982,
            high_mv=4177,
            tbat=AffineCorrection(97, -300),
            tenv=AffineCorrection(99, -200),
            reserved=AffineCorrection(100, 0),
        )
        bundle = generate_overlay_bundle(
            CUST_IMEI, CUST_FSN, battery_calibration=calibration
        )
        self.assertEqual(
            bytes.fromhex(bundle["blocks"]["67"]),
            encode_battery_calibration(calibration),
        )

    def test_generate_cli_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            a = Path(td) / "a.json"
            b = Path(td) / "b.json"
            argv = ["generate", "--imei", CUST_IMEI,
                    "--fsn", f"{CUST_FSN:08X}", "--output"]
            self.assertEqual(main(argv + [str(a)]), 0)
            self.assertEqual(main(argv + [str(b)]), 0)
            self.assertEqual(a.read_bytes(), b.read_bytes())
            self.assertEqual(
                __import__("json").loads(a.read_text()),
                generate_overlay_bundle(CUST_IMEI, CUST_FSN),
            )

    def test_generate_cli_identity_only_omits_battery_calibration(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / "identity-only.json"
            argv = [
                "generate", "--imei", CUST_IMEI,
                "--fsn", f"{CUST_FSN:08X}", "--identity-only",
                "--output", str(output),
            ]
            self.assertEqual(main(argv), 0)
            bundle = __import__("json").loads(output.read_text())
        self.assertEqual(bundle, generate_identity_bundle(CUST_IMEI, CUST_FSN))
        self.assertEqual(set(bundle["blocks"]), {"76", "5008", "5009", "5077"})


class RoundTripTests(unittest.TestCase):
    """Encrypt then decrypt returns the plaintext (KSA/PRGA self-consistency)."""

    def test_5008_round_trip(self) -> None:
        bcd = convert_to_bcd(CUST_IMEI)
        plain = bytes((i * 7 + 3) & 0xFF for i in range(B5008_LEN))
        enc = encrypt_5008(plain, CUST_FSN, bcd, UNKNOWN_KEY1_C55)
        dec = decrypt_5008(bytes(enc), CUST_FSN, bcd, UNKNOWN_KEY1_C55)
        self.assertEqual(bytes(dec), plain)

    def test_5077_round_trip(self) -> None:
        bcd = convert_to_bcd(CUST_IMEI)
        plain = bytes((255 - i) & 0xFF for i in range(B5077_LEN))
        enc = encrypt_5077(plain, CUST_FSN, bcd, UNKNOWN_KEY1_C55)
        dec = decrypt_5077(bytes(enc), CUST_FSN, bcd, UNKNOWN_KEY1_C55)
        self.assertEqual(bytes(dec), plain)


class ModelKeyAliasTests(unittest.TestCase):
    """Every EEPROM-bearing Freia 15 crypto-model-7 alias selects one row."""

    EXPECTED = {
        "C55", "M55", "S55", "SL55", "A50", "A52", "A55", "A60",
        "C60", "MC60", "SX1", "1168", "2128",
    }

    def test_alias_inventory_matches_freia_15_model_table(self) -> None:
        self.assertEqual(set(MODEL_ID7_KEY1_ALIASES), self.EXPECTED)

    def test_every_alias_resolves_byte_identically_to_c55(self) -> None:
        expected = b"".join(word.to_bytes(4, "little") for word in UNKNOWN_KEY1_C55)
        for alias in MODEL_ID7_KEY1_ALIASES:
            with self.subTest(alias=alias):
                resolved = parse_key1(alias)
                actual = b"".join(word.to_bytes(4, "little") for word in resolved)
                self.assertEqual(actual, expected)
                self.assertEqual(MODEL_KEY1[alias], UNKNOWN_KEY1_C55)

class KnownAnswerTests(unittest.TestCase):
    """Decrypting bundled identity records reproduces the known template."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.flash = FLASH.read_bytes()
        _, cls.inv = load_eeprom_source(FLASH)
        bcd = convert_to_bcd(CUST_IMEI)
        cls.dec5008 = decrypt_5008(cls.inv[0x1390].payload, CUST_FSN, bcd)
        cls.dec5077 = decrypt_5077(cls.inv[0x13D5].payload, CUST_FSN, bcd)

    def test_known_5008_prefix_matches_dump(self) -> None:
        # Pin the source record used by the known-answer decryptions below.
        self.assertEqual(self.inv[0x1390].payload[:8].hex(), "ddef51ec17eaae91")

    def test_dd2476_check_pairs_match_firmware(self) -> None:
        self.assertEqual(verify_checks(self.dec5008, self.dec5077), EXPECTED_CHECKS)

    def test_5008_header_decodes_to_recorded_strange_header(self) -> None:
        # The bundled unlocked template includes its own sum/xor check.
        header = bytes(self.dec5008[8:32])
        self.assertEqual(header.hex(), "000300000067000000000000ff00ffffffffffff00ff6264")
        self.assertEqual(crc_check_bytes(header, 0x16), (0x62, 0x64))


A55_FLASH = bundled_image("a55")
A55_LINEAR_5009 = 0x00FC028E

KNOWN_SECURITY_CORPUS = [
    (model.upper(), bundled_image(model), CUST_FSN, CUST_IMEI,
     EXPECTED_CHECKS,
     "7ef1a6e57d88f52dcd0b17e328bbe9f250f564f242bc9bab4ba7b33419e6c2bf",
     "b5f4c5bd57efde5ebaab67c415e1ce90cbd5a2779d5844de96c07e4e691cac84")
    for model in ("c55", "a52", "a55", "m55", "s55")
]


class KnownSecurityCorpusTests(unittest.TestCase):
    """Known IMEI/FSN pairs recover checksum-valid 5008/5077 plaintext."""

    @staticmethod
    def _decrypt(path: Path, fsn: int, imei: str) -> tuple[bytes, bytes, dict[int, Block]]:
        _, inv = load_eeprom_source(path)
        bcd = convert_to_bcd(imei)
        return (
            bytes(decrypt_5008(inv[5008].payload, fsn, bcd)),
            bytes(decrypt_5077(inv[5077].payload, fsn, bcd)),
            inv,
        )

    def test_known_pairs_match_stored_checks_and_plaintext_hashes(self) -> None:
        for label, path, fsn, imei, checks, hash5008, hash5077 in KNOWN_SECURITY_CORPUS:
            with self.subTest(label=label):
                dec5008, dec5077, inv = self._decrypt(path, fsn, imei)
                stored = [
                    ("5008.header", dec5008[0x1E], dec5008[0x1F]),
                    ("5008.body", dec5008[0xD8], dec5008[0xD9]),
                    ("5077.body", dec5077[0xE0], dec5077[0xE1]),
                ]
                self.assertEqual(verify_checks(dec5008, dec5077), checks)
                self.assertEqual(stored, checks)
                self.assertEqual(hashlib.sha256(dec5008).hexdigest(), hash5008)
                self.assertEqual(hashlib.sha256(dec5077).hexdigest(), hash5077)

                bcd = convert_to_bcd(imei)
                self.assertEqual(
                    bytes(encrypt_5008(dec5008, fsn, bcd)),
                    inv[5008].payload,
                )
                self.assertEqual(
                    bytes(encrypt_5077(dec5077, fsn, bcd)),
                    inv[5077].payload,
                )

    def test_three_identity_pairs_share_exact_plaintext(self) -> None:
        shared = [
            self._decrypt(path, fsn, imei)[:2]
            for _, path, fsn, imei, *_ in (
                KNOWN_SECURITY_CORPUS[1],
                KNOWN_SECURITY_CORPUS[2],
                KNOWN_SECURITY_CORPUS[4],
            )
        ]
        self.assertEqual(shared[0], shared[1])
        self.assertEqual(shared[0], shared[2])

    def test_documented_5008_fields(self) -> None:
        expected = {
            label: (((0x00, 0x03), (0x00, 0x00), (0x00, 0x67),
                     (0x00, 0x00), (0x00, 0x00)), bytes.fromhex("ffffffffff"))
            for label, *_ in KNOWN_SECURITY_CORPUS
        }
        lock_offsets = ((0x08, 0x09), (0x0A, 0x0B), (0x0C, 0x0D),
                        (0x0F, 0x10), (0x11, 0x12))

        for label, lock_pairs, operator1 in (
            (label, *expected[label]) for label in expected
        ):
            corpus = next(row for row in KNOWN_SECURITY_CORPUS if row[0] == label)
            with self.subTest(label=label):
                dec5008, _, _ = self._decrypt(corpus[1], corpus[2], corpus[3])
                self.assertEqual(
                    tuple((dec5008[a], dec5008[b]) for a, b in lock_offsets),
                    lock_pairs,
                )
                self.assertEqual(dec5008[0x13] & 0x03, 0)
                self.assertEqual(dec5008[0x14], 0xFF)
                self.assertEqual(dec5008[0x16:0x1A], b"\xFF" * 4)
                self.assertEqual(dec5008[0x28:0x2B], b"\xFF" * 3)
                self.assertEqual(dec5008[0x2B:0x30], operator1)
                self.assertEqual(dec5008[0x31:0x36], b"\xFF" * 5)


class A55ImeiTests(unittest.TestCase):
    """The A55 sibling shares the C55/A50 cipher tables; its IMEI decodes the same."""

    def test_a55_5009_decodes_to_valid_imei(self) -> None:
        _, inv = load_eeprom_source(A55_FLASH)
        b5009 = inv[0x1391].payload
        self.assertEqual(inv[0x1391].linear, A55_LINEAR_5009)
        self.assertEqual(b5009.hex(), "76caac2cb4a0c403a6b9")
        imei, empty = recreate_imei(b5009)
        self.assertFalse(empty)
        self.assertEqual(imei, CUST_IMEI)


if __name__ == "__main__":
    unittest.main()
