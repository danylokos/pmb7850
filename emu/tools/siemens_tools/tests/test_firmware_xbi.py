#!/usr/bin/env python3
"""XBI package parsing and materialization tests."""

from .firmware_fixtures import *  # noqa: F401,F403


class XbiTests(unittest.TestCase):
    def test_signed_v24_uncompressed_metadata_and_flash(self) -> None:
        data = build_xbi(
            signed=True,
            writes=[(0x10, b"abc"), (0x20, b"xyz")],
        )
        info = fw.parse_xbi(data)
        flash = fw.convert_xbi_to_flash(data, info)

        self.assertTrue(info.signed)
        self.assertEqual(info.format_version, 24)
        self.assertEqual(info.get("model"), "T55")
        self.assertEqual(info.get("svn"), 24)
        self.assertEqual(info.get("cpu_type_name"), "EGOLD Plus V3")
        self.assertEqual(info.get("data_flash"), [{"from": 0, "to": 0xFF}])
        self.assertEqual(flash[0x10:0x13], b"abc")
        self.assertEqual(flash[0x20:0x23], b"xyz")
        self.assertEqual(flash[0], 0xFF)

    def test_unsigned_v32_and_checksum_failure(self) -> None:
        data = build_xbi(version=32, writes=[(0xF0000040, b"v32")])
        info = fw.parse_xbi(data)
        self.assertFalse(info.signed)
        self.assertEqual(info.format_version, 32)
        self.assertEqual(fw.convert_xbi_to_flash(data)[0x40:0x43], b"v32")

        broken = bytearray(data)
        broken[-1] ^= 1
        with self.assertRaisesRegex(fw.FirmwareError, "write checksum"):
            fw.parse_xbi(bytes(broken))

    def test_final_lzss_padding_is_accepted(self) -> None:
        payload = b"ABCDEF"
        transport = transport_stream(0x30, payload)
        self.assertEqual(len(transport), 15)
        compressed = literal_lzss(transport, add_partial_backref=True)
        data = build_xbi(compression_type=3, compressed_stream=compressed)

        flash = fw.convert_xbi_to_flash(data)
        self.assertEqual(flash[0x30:0x36], payload)

    def test_midstream_transport_corruption_is_rejected(self) -> None:
        transport = bytearray(transport_stream(0x30, b"ABCDEF"))
        transport[6] ^= 1
        compressed = literal_lzss(bytes(transport), add_partial_backref=True)
        data = build_xbi(compression_type=3, compressed_stream=compressed)

        with self.assertRaisesRegex(fw.FirmwareError, "address checksum"):
            fw.convert_xbi_to_flash(data)

    def test_unknown_compression_and_write_bounds_are_rejected(self) -> None:
        unknown = build_xbi(compression_type=2, writes=[(0, b"x")])
        with self.assertRaisesRegex(fw.FirmwareError, "compression type"):
            fw.convert_xbi_to_flash(unknown)

        outside = build_xbi(writes=[(0xFF, b"too long")])
        with self.assertRaisesRegex(fw.FirmwareError, "exceeds flash size"):
            fw.convert_xbi_to_flash(outside)

    def test_materialization_distinguishes_written_ff_and_declared_erase(self) -> None:
        data = build_xbi(writes=[(0x20, b"\xffA")])
        materialized = fw.materialize_xbi(data)

        self.assertEqual(materialized.flash[0x20:0x22], b"\xffA")
        self.assertEqual(materialized.write_mask[0x20:0x22], b"\x01\x01")
        self.assertEqual(materialized.write_mask[0x22], 0)
        self.assertEqual(materialized.erase_mask[0x10:0x80], b"\x01" * 0x70)
        self.assertEqual(materialized.erase_mask[0x0F], 0)


if __name__ == "__main__":
    unittest.main()
