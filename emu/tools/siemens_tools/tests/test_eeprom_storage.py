#!/usr/bin/env python3
"""EEPROM directory and map storage tests."""

from .eeprom_fixtures import *  # noqa: F401,F403


class DirectoryParserTests(unittest.TestCase):
    """Synthetic-region tests for the tCOMM_BLOCKHEADER directory walk (no dump)."""

    STRIDE = 0x20000

    def _region(self, dir_records: bytes) -> bytes:
        # A realistic region: EELITE at +0x12 (preceded by FEFE) plus the two
        # EEFULL mirrors one/two sub-banks in (find_eeprom_region requires them to
        # reject stray EELITE strings). Directory records sit near the region top
        # of the first bank so their segments derive the linear base.
        buf = bytearray(b"\xff" * (3 * self.STRIDE))
        for bank, name in ((0, b"EELITE"), (1, b"EEFULL"), (2, b"EEFULL")):
            off = bank * self.STRIDE
            buf[off + 0x10:off + 0x12] = b"\xfe\xfe"
            buf[off + 0x12:off + 0x18] = name
        # Directory near the top of the first bank (below the EEFULL mirror region).
        dir_at = self.STRIDE - 0x100
        buf[dir_at:dir_at + len(dir_records)] = dir_records
        return bytes(buf)

    def test_locates_region_by_magic(self) -> None:
        # Region needs at least one seg-bearing record to derive the linear base.
        base = DEFAULT_REGION_LINEAR_BASE
        rec = blockheader(0x00FC, 0x0A, (base + 0x400) & 0xFFFF, (base + 0x400) >> 16,
                           0x1391, 0xFC00)
        region = self._region(rec)
        loc = find_eeprom_region(region)
        self.assertEqual(loc.region_file_base, 0)
        self.assertEqual(loc.region_linear_base, DEFAULT_REGION_LINEAR_BASE)

    def test_locates_s55_compact_mirrors(self) -> None:
        buf = bytearray(b"\xff" * 0x20000)
        buf[0x80:0x82] = b"\xfe\xfe"
        buf[0x82:0x88] = b"EELITE"
        buf[0x10082:0x10088] = b"EEFULL"
        buf[0x12082:0x12088] = b"EEFULL"
        linear = 0xFE2148
        rec = blockheader(
            0x00FC, 10, linear & 0xFFFF, linear >> 16, 76, 0xFC00
        )
        buf[0xF000:0xF000 + len(rec)] = rec
        region = find_eeprom_region(bytes(buf))
        directory = parse_directory(bytes(buf), region)
        self.assertEqual(region.region_file_base, 0)
        self.assertEqual(region.region_linear_base, 0xFE0000)
        self.assertEqual(directory[76].linear, linear)
        self.assertEqual(directory[76].file_off, 0x2148)

    def test_parses_and_resolves_records(self) -> None:
        base = DEFAULT_REGION_LINEAR_BASE
        # Two records: id 0x1391 (10 bytes) and id 0x1390 (0xE0), addressed within region.
        recs = (blockheader(0x00FC, 0x0A, (base + 0x400) & 0xFFFF, (base + 0x400) >> 16,
                             0x1391, 0xFC00)
                + blockheader(0x00FC, 0xE0, (base + 0x500) & 0xFFFF, (base + 0x500) >> 16,
                               0x1390, 0xFC00))
        flash = self._region(recs)
        loc = find_eeprom_region(flash)
        directory = parse_directory(flash, loc)
        self.assertIn(0x1391, directory)
        self.assertIn(0x1390, directory)
        self.assertEqual(directory[0x1391].linear, base + 0x400)
        self.assertEqual(directory[0x1391].length, 0x0A)
        self.assertEqual(directory[0x1390].file_off, 0x500)

    def test_rejects_bad_terminator(self) -> None:
        base = DEFAULT_REGION_LINEAR_BASE
        # One good record (so the region/base is derivable) + one with a bad endid.
        recs = (blockheader(0x00FC, 0x0A, (base + 0x400) & 0xFFFF, (base + 0x400) >> 16,
                             0x1390, 0xFC00)
                + blockheader(0x00FC, 0x0A, (base + 0x600) & 0xFFFF, (base + 0x600) >> 16,
                               0x1391, 0x1234))  # endid not in {FC00,F800,F000}
        flash = self._region(recs)
        directory = parse_directory(flash, find_eeprom_region(flash))
        self.assertIn(0x1390, directory)
        self.assertNotIn(0x1391, directory)

    def test_first_copy_wins(self) -> None:
        base = DEFAULT_REGION_LINEAR_BASE
        recs = (blockheader(0x00FC, 0x0A, (base + 0x400) & 0xFFFF, (base + 0x400) >> 16,
                             0x1391, 0xFC00)
                + blockheader(0x00FC, 0x0A, (base + 0x800) & 0xFFFF, (base + 0x800) >> 16,
                               0x1391, 0xFC00))
        flash = self._region(recs)
        directory = parse_directory(flash, find_eeprom_region(flash))
        self.assertEqual(directory[0x1391].linear, base + 0x400)  # first record kept

    def test_active_record_wins_over_earlier_fallback(self) -> None:
        base = DEFAULT_REGION_LINEAR_BASE
        recs = (blockheader(0x00F0, 4, 0x400, base >> 16, 5006, 0xF000)
                + blockheader(0x02FC, 4, 0x800, base >> 16, 5006, 0xFC00))
        flash = self._region(recs)
        directory = parse_directory(flash, find_eeprom_region(flash))
        self.assertEqual(directory[5006].linear, base + 0x800)
        self.assertEqual(directory[5006].version, 2)
        self.assertTrue(directory[5006].active)

    def test_fallback_only_record_still_resolves(self) -> None:
        base = DEFAULT_REGION_LINEAR_BASE
        rec = blockheader(0x03F0, 4, 0x400, base >> 16, 5006, 0xF000)
        flash = self._region(rec)
        entry = parse_directory(flash, find_eeprom_region(flash))[5006]
        self.assertEqual(entry.linear, base + 0x400)
        self.assertEqual(entry.version, 3)
        self.assertFalse(entry.active)

    def test_all_records_preserve_physical_scan_order(self) -> None:
        base = DEFAULT_REGION_LINEAR_BASE
        recs = (blockheader(0x00F0, 4, 0x400, base >> 16, 1, 0xF000)
                + blockheader(0x00FC, 4, 0x800, base >> 16, 2, 0xFC00))
        flash = self._region(recs)
        records = parse_directory_records(flash, find_eeprom_region(flash))
        self.assertEqual([entry.block_id for entry in records], [1, 2])

    def test_derives_nondefault_linear_base(self) -> None:
        # A region whose blocks sit at 0xFC0000 (M55-style) must derive that base,
        # not the C55/A55 0xFA0000 default.
        base = 0xFC0000
        recs = blockheader(0x00FC, 0x0A, (base + 0x400) & 0xFFFF, (base + 0x400) >> 16,
                            0x1391, 0xFC00)
        flash = self._region(recs)
        loc = find_eeprom_region(flash)
        self.assertEqual(loc.region_linear_base, base)
        self.assertEqual(parse_directory(flash, loc)[0x1391].file_off, 0x400)

    def test_derives_low_a60_style_linear_base(self) -> None:
        base = 0x7C0000
        rec = blockheader(
            0x00FC, 0x0A, (base + 0x400) & 0xFFFF,
            (base + 0x400) >> 16, 0x1391, 0xFC00
        )
        flash = self._region(rec)
        loc = find_eeprom_region(flash)
        self.assertEqual(loc.region_linear_base, base)
        self.assertEqual(parse_directory(flash, loc)[0x1391].file_off, 0x400)

    def test_block_class_dispatch(self) -> None:
        self.assertEqual(block_class(0x1390), "cipher")   # 5008
        self.assertEqual(block_class(0x13D5), "cipher")   # 5077
        self.assertEqual(block_class(0x1391), "scramble")  # 5009
        self.assertEqual(block_class(0x004C), "imei-companion")  # 76
        self.assertEqual(block_class(0x0043), "plaintext")  # 67
        self.assertEqual(block_class(0x1401), "plaintext")  # 5121


from tools.bundled_firmware import image as bundled_image


class AutoLocateC55Tests(unittest.TestCase):
    """The directory parser locates the known C55 blocks with no manual offsets."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.flash = FLASH.read_bytes()
        region = find_eeprom_region(cls.flash)
        cls.dir = parse_directory(cls.flash, region)
        cls.inv = load_dump_blocks(cls.flash, region)

    def test_active_and_fallback_record_inventory(self) -> None:
        region = find_eeprom_region(self.flash)
        records = parse_directory_records(self.flash, region)
        self.assertEqual(len(records), 305)
        self.assertEqual(sum(entry.active for entry in records), 305)
        self.assertEqual(sum(not entry.active for entry in records), 0)

    def test_active_records_supply_current_payloads(self) -> None:
        self.assertEqual(
            self.inv[5006].payload,
            bytes.fromhex(
                "9488728e000006800000ff0bdcffdbd9ffff600100008d4f0100938f1000461dffff"
            ),
        )
        self.assertEqual(self.inv[5122].payload, bytes.fromhex("000000000000"))
        self.assertEqual(
            self.inv[5123].payload,
            bytes.fromhex("0000000000000000000000000000"),
        )

    def test_finds_known_blocks_at_recorded_addresses(self) -> None:
        self.assertEqual(self.dir[0x1390].linear, C55_LINEAR_5008)  # 5008
        self.assertEqual(self.dir[0x13D5].linear, C55_LINEAR_5077)  # 5077
        self.assertEqual(self.dir[0x1391].linear, C55_LINEAR_5009)  # 5009
        self.assertEqual(self.dir[0x1390].length, B5008_LEN)
        self.assertEqual(self.dir[0x13D5].length, B5077_LEN)

    def test_extract_5009_imei_without_fsn(self) -> None:
        info = extract_block(self.inv, 0x1391)
        self.assertEqual(info["imei"], CUST_IMEI)

    def test_extract_76_companion_and_67_adjustment_block(self) -> None:
        companion = extract_block(self.inv, 76)
        self.assertEqual(companion["class"], "imei-companion")
        self.assertEqual(companion["imei"], CUST_IMEI)

        adjustment = extract_block(self.inv, 67)
        self.assertEqual(adjustment["class"], "plaintext")
        self.assertEqual(
            adjustment["raw_hex"],
            "93115210a4f9e9036200a9fe620027ff64000000",
        )
        self.assertNotIn("imei", adjustment)
        self.assertNotIn("imei_error", adjustment)

    def test_cli_prints_companion_imei(self) -> None:
        output = StringIO()
        with redirect_stdout(output):
            result = main(["extract", str(FLASH), "--block", "76"])
        self.assertEqual(result, 0)
        self.assertIn(f"imei: {CUST_IMEI}", output.getvalue())

    def test_extract_cipher_matches_known_answer(self) -> None:
        d5008 = bytes.fromhex(
            extract_block(self.inv, 0x1390, CUST_FSN, CUST_IMEI)["plaintext_hex"])
        d5077 = bytes.fromhex(
            extract_block(self.inv, 0x13D5, CUST_FSN, CUST_IMEI)["plaintext_hex"])
        self.assertEqual(verify_checks(d5008, d5077), EXPECTED_CHECKS)


class AutoLocateA55Tests(unittest.TestCase):
    """Device-generic: same code auto-locates and decodes an A55 dump, no offsets/model flag."""

    def test_a55_5009_auto_located(self) -> None:
        _, inv = load_eeprom_source(A55_FLASH)
        self.assertEqual(inv[0x1391].linear, A55_LINEAR_5009)
        # Full 8 MiB image: file offset = linear - 0x800000.
        self.assertEqual(inv[0x1391].file_off, A55_LINEAR_5009 - 0x800000)
        info = extract_block(inv, 0x1391)
        self.assertEqual(info["imei"], CUST_IMEI)


A60_FLASH = bundled_image("a60")


class AutoLocateA60Tests(unittest.TestCase):
    def test_a60_low_segment_directory_is_auto_located(self) -> None:
        flash = A60_FLASH.read_bytes()
        region = find_eeprom_region(flash)
        directory = parse_directory(flash, region)
        self.assertEqual(region.region_linear_base, 0x7C0000)
        self.assertEqual(directory[76].file_off, 0x7C0EE4)
        self.assertEqual(directory[5008].file_off, 0x7D0192)
        self.assertEqual(directory[5009].file_off, 0x7D0272)
        self.assertEqual(directory[5077].file_off, 0x7D1FCC)





class StandaloneSliceTests(unittest.TestCase):
    """A standalone EEPROM slice has region base != 0x800000; auto-detect must map it."""

    def test_slice_region_base_is_file_zero(self) -> None:
        flash = A55_FLASH.read_bytes()[0x7A0000:]
        region = find_eeprom_region(flash)
        self.assertEqual(region.region_file_base, 0)
        self.assertEqual(region.region_linear_base, DEFAULT_REGION_LINEAR_BASE)
        # linear->file for this slice is linear - 0xFA0000
        self.assertEqual(region.linear_to_file(0xFF7572), 0xFF7572 - 0xFA0000)

    def test_slice_5009_decodes(self) -> None:
        data = A55_FLASH.read_bytes()[0x7A0000:]
        inv = load_dump_blocks(data, find_eeprom_region(data))
        info = extract_block(inv, 0x1391)
        self.assertEqual(info["imei"], CUST_IMEI)


M55_FLASH = bundled_image("m55")


class M55LocationTests(unittest.TestCase):
    """M55 v91 is a 16 MiB dump whose EEPROM sits at linear 0xFC0000 with 0x10000
    sub-banks and a 0x70-byte bank wrapper. Block location must generalize to it
    without shifting absolute directory pointers by that wrapper."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.flash = M55_FLASH.read_bytes()
        cls.region = find_eeprom_region(cls.flash)
        cls.dir = parse_directory(cls.flash, cls.region)
        cls.inv = load_dump_blocks(cls.flash, cls.region)

    def test_region_linear_base_derived_as_0xfc0000(self) -> None:
        self.assertEqual(self.region.region_linear_base, 0xFC0000)
        self.assertEqual(self.region.region_file_base, 0xFC0000)

    def test_directory_parses_and_5009_maps_to_real_data(self) -> None:
        self.assertIn(0x1391, self.dir)
        entry = self.dir[0x1391]
        self.assertEqual(entry.linear, 0xFD0272)
        # Correct mapping lands on real (non-erased) bytes, not 0xFF filler.
        data = self.flash[entry.file_off:entry.file_off + entry.length]
        self.assertNotEqual(data, b"\xff" * entry.length)

    def test_primary_m55_semantic_block_inventory(self) -> None:
        expected_lengths = {
            1: 348, 2: 348, 55: 24, 67: 20, 75: 46, 76: 10, 167: 348,
            5002: 136, 5005: 64, 5006: 34, 5007: 10, 5008: 224,
            5009: 10, 5012: 12, 5077: 232, 5093: 32, 5121: 56,
            5122: 6, 5123: 12, 5352: 564,
        }
        self.assertEqual(len(self.inv), 383)
        self.assertEqual(
            {block_id: self.inv[block_id].length for block_id in expected_lengths},
            expected_lengths,
        )

    def test_security_records_decode_to_the_same_imei(self) -> None:
        primary = extract_block(self.inv, 5009)
        companion = extract_block(self.inv, 76)
        self.assertEqual(primary["raw_hex"], "76caac2cb4a0c403a6b9")
        self.assertEqual(companion["raw_hex"], "36e60c6e2e0760fe837d")
        self.assertEqual(primary["imei"], "11223344556677")
        self.assertEqual(companion["imei"], "11223344556677")
        self.assertEqual(
            self.inv[5121].payload,
            bytes(56),
        )
        self.assertEqual(self.inv[5122].payload, bytes.fromhex("000000000000"))
        self.assertEqual(
            self.inv[5123].payload,
            bytes.fromhex("000000000000000000000000"),
        )


S55_FLASH = (
    bundled_image("s55")
)


class S55LocationTests(unittest.TestCase):
    """S55 stores a wrapped EELITE segment followed by compact EEFULL banks."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.flash = S55_FLASH.read_bytes()
        cls.region = find_eeprom_region(cls.flash)
        cls.inv = load_dump_blocks(cls.flash, cls.region)

    def test_region_is_auto_located(self) -> None:
        self.assertEqual(self.region.region_file_base, 0xBE0000)
        self.assertEqual(self.region.region_linear_base, 0xFE0000)

    def test_identity_records_map_to_canonical_offsets(self) -> None:
        expected = {
            76: (0xFE0E7A, 0xBE0E7A, 10),
            5008: (0xFF0192, 0xBF0192, 224),
            5009: (0xFF0272, 0xBF0272, 10),
            5077: (0xFF135A, 0xBF135A, 232),
        }
        self.assertEqual(
            {block_id: (self.inv[block_id].linear,
                        self.inv[block_id].file_off,
                        self.inv[block_id].length)
             for block_id in expected},
            expected,
        )



_SYNTH_MAP = """\
[MapFileInfo]
Product = 130
SWVersion = 24

[67]                ; Test measurement values +NEW+
Offset =       0    ; Offset
Memory =       2    ; EELiteBlock
Version =      3
DataSize =     4    ; Anzahl der Datenbytes
Data     {
0x01 0x02 0x03 0x04
         }
[5121]              ; Some plaintext block
Offset =       0    ; Offset
Memory =       8    ; EEFullBlock
Version =      0
DataSize =     3    ; Anzahl der Datenbytes
Data     {
0xAA 0xBB 0xCC
         }
[CheckSum]
Key = 12345    ; CheckByte: 0xF6
"""


class MapParserTests(unittest.TestCase):
    """Synthetic-map tests for parse_map_file (no real file needed)."""

    def _write(self, text: str) -> Path:
        tmp = tempfile.NamedTemporaryFile("w", suffix=".map", delete=False, encoding="latin-1")
        tmp.write(text)
        tmp.close()
        self.addCleanup(lambda: Path(tmp.name).unlink())
        return Path(tmp.name)

    def test_parses_ids_payloads_names_and_skips_meta(self) -> None:
        inv = parse_map_file(self._write(_SYNTH_MAP))
        self.assertEqual(sorted(inv), [67, 5121])  # [MapFileInfo]/[CheckSum] skipped
        self.assertEqual(inv[67].payload, bytes([0x01, 0x02, 0x03, 0x04]))
        self.assertEqual(inv[67].length, 4)
        self.assertEqual(inv[67].source_kind, "map")
        self.assertIsNone(inv[67].linear)
        self.assertIsNone(inv[67].file_off)
        self.assertEqual(inv[67].memory_class, 2)
        self.assertEqual(inv[67].version, 3)
        self.assertIn("measurement values", inv[67].name)
        self.assertEqual(inv[5121].payload, bytes([0xAA, 0xBB, 0xCC]))

    def test_datasize_mismatch_is_not_fatal(self) -> None:
        bad = _SYNTH_MAP.replace("DataSize =     4", "DataSize =     9")
        inv = parse_map_file(self._write(bad))  # must not raise
        self.assertEqual(inv[67].payload, bytes([0x01, 0x02, 0x03, 0x04]))






if __name__ == "__main__":
    unittest.main()
