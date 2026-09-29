#!/usr/bin/env python3
"""Focused tests for tools.siemens_tools.bitmaps."""

from __future__ import annotations

from tools.bundled_firmware import image as bundled_image

import argparse
import hashlib
from collections import Counter
import struct
import tempfile
import unittest
from pathlib import Path

from tools.siemens_tools.bitmaps import (
    ARGB4444_BITMAP_TYPE,
    BitmapDescriptor,
    FramePlacement,
    compose_bitmap_stages,
    decode_argb4444,
    decode_bitmap,
    decode_indexed2,
    decode_indexed4,
    decode_raw,
    decode_rgb332,
    decode_rgb555le,
    decode_rgb565le,
    decode_compressed,
    extract_table,
    find_descriptor_runs,
    scan_flash,
    scan_descriptors,
    scan_image_descriptors,
    write_bmp,
    write_png,
)


M55_FLASH = bundled_image("m55")
S55_FLASH = (
    bundled_image("s55")
)


class BitmapExtractorTests(unittest.TestCase):
    def test_raw_rows_are_msb_first_and_byte_padded(self) -> None:
        pixels, used = decode_raw(bytes((0xA0, 0x40)), width=3, height=2)
        self.assertEqual(pixels, [1, 0, 1, 0, 1, 0])
        self.assertEqual(used, 2)

    def test_argb4444_is_little_endian_with_binary_alpha(self) -> None:
        pixels, used = decode_argb4444(bytes.fromhex("2301bcfa"), 2, 1)
        self.assertEqual(
            pixels,
            [
                (0x11, 0x22, 0x33, 0),
                (0xAA, 0xBB, 0xCC, 255),
            ],
        )
        self.assertEqual(used, 4)

    def test_argb4444_rejects_a_truncated_stream(self) -> None:
        with self.assertRaisesRegex(ValueError, "ended after 3/4 bytes"):
            decode_argb4444(bytes.fromhex("ffff00"), 2, 1)

    def test_rgb332_uses_controller_native_rrrgggbb_order(self) -> None:
        pixels, used = decode_rgb332(bytes((0x00, 0xE0, 0x1C, 0x03, 0xFF)), 5, 1)
        self.assertEqual(
            pixels,
            [
                (0, 0, 0, 255),
                (255, 0, 0, 255),
                (0, 255, 0, 255),
                (0, 0, 255, 255),
                (255, 255, 255, 255),
            ],
        )
        self.assertEqual(used, 5)
        keyed, keyed_used = decode_rgb332(bytes((0xC0, 0xE0)), 2, 1, 0xC0)
        self.assertEqual(keyed, [(219, 0, 0, 0), (255, 0, 0, 255)])
        self.assertEqual(keyed_used, 2)


    def test_rgb555_and_rgb565_are_little_endian(self) -> None:
        rgb555, used555 = decode_rgb555le(bytes.fromhex("007ce0031f00"), 3, 1)
        rgb565, used565 = decode_rgb565le(bytes.fromhex("00f8e0071f00"), 3, 1)
        expected = [
            (255, 0, 0, 255),
            (0, 255, 0, 255),
            (0, 0, 255, 255),
        ]
        self.assertEqual(rgb555, expected)
        self.assertEqual(rgb565, expected)
        self.assertEqual((used555, used565), (6, 6))

    def test_indexed2_is_row_padded_and_supports_both_bit_orders(self) -> None:
        data = bytes.fromhex("1b40e480")
        msb, used = decode_indexed2(data, 5, 2)
        lsb, _ = decode_indexed2(bytes((0x1B,)), 4, 1, msb_first=False)
        levels = [0, 85, 170, 255]
        self.assertEqual([pixel[0] for pixel in msb], [levels[i] for i in (0,1,2,3,1,3,2,1,0,2)])
        self.assertEqual([pixel[0] for pixel in lsb], [levels[i] for i in (3,2,1,0)])
        self.assertEqual(used, 4)

    def test_indexed4_is_row_padded_and_supports_both_nibble_orders(self) -> None:
        msb, used = decode_indexed4(bytes.fromhex("12304560"), 3, 2)
        lsb, _ = decode_indexed4(bytes((0x12,)), 2, 1, msb_first=False)
        self.assertEqual([pixel[0] for pixel in msb], [17 * i for i in (1,2,3,4,5,6)])
        self.assertEqual([pixel[0] for pixel in lsb], [34, 17])
        self.assertEqual(used, 4)
        with self.assertRaisesRegex(ValueError, "ended after 3/4 bytes"):
            decode_indexed4(bytes(3), 3, 2)

    def test_explicit_encoding_accepts_an_unknown_descriptor_tag(self) -> None:
        image = bytearray(b"\xff" * 0x400)
        image[0:8] = struct.pack("<BBBBHH", 2, 2, 0xAA, 0, 0x200, 0x200)
        image[0x200:0x204] = bytes((0x00, 0xE0, 0x1C, 0x03))
        candidates = scan_image_descriptors(
            bytes(image), 0x800000, kinds=(0xAA,), encoding="rgb332"
        )
        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0].encoding, "rgb332")
        self.assertEqual(candidates[0].encoded_bytes, 4)
        with self.assertRaisesRegex(ValueError, "unsupported bitmap type 0xaa"):
            scan_image_descriptors(bytes(image), 0x800000, kinds=(0xAA,))

    def test_composition_places_and_overwrites_cumulative_stages(self) -> None:
        first = BitmapDescriptor.from_bytes(
            0, 0x800000, struct.pack("<BBBBHH", 2, 2, 0x01, 0, 0, 0x200)
        )
        second = BitmapDescriptor.from_bytes(
            1, 0x800010, struct.pack("<BBBBHH", 2, 1, 0x01, 0, 0, 0x200)
        )
        stages, width, height = compose_bitmap_stages(
            [
                (first, [1, 0, 0, 1], FramePlacement(0, 0, 100, 1)),
                (second, [0, 1], FramePlacement(1, 1, 200, 1)),
            ]
        )

        self.assertEqual((width, height), (3, 2))
        self.assertEqual(stages[0], [1, 0, 0, 0, 1, 0])
        self.assertEqual(stages[1], [1, 0, 0, 0, 0, 1])

    def test_known_icon_e7_stream(self) -> None:
        stream = bytes.fromhex(
            "1f d1 10 88 40 7c 8b 60 7d ce 07 60 88 60 7a 8b 40 79 d5 00"
        )
        expected = (
            "..######################."
            ".#............#......####"
            "#.............##.....####"
            "#.###############....####"
            "#.............##.....####"
            ".#............#......####"
            "..######################."
        )
        pixels, used = decode_compressed(stream, 25 * 7)
        actual = "".join("#" if pixel else "." for pixel in pixels)
        self.assertEqual(actual, expected)
        self.assertEqual(used, 20)

    def test_descriptor_scan_stops_at_invalid_record(self) -> None:
        image = bytearray(b"\xff" * 0x100)
        descriptor = struct.pack("<BBBBHH", 4, 3, 0x81, 0, 0x40, 0x200)
        image[0:8] = descriptor
        image[8:16] = descriptor
        descriptors = scan_descriptors(bytes(image), 0x800000, 0x800000)
        self.assertEqual([item.index for item in descriptors], [0, 1])
        self.assertEqual(descriptors[0].source_address, 0x800040)

    def test_descriptor_scan_supports_record_stride(self) -> None:
        image = bytearray(b"\xff" * 0x100)
        descriptor = struct.pack("<BBBBHH", 4, 3, 0x81, 0, 0x40, 0x200)
        image[0:8] = descriptor
        image[16:24] = descriptor
        descriptors = scan_descriptors(
            bytes(image), 0x800000, 0x800000, stride=16
        )
        self.assertEqual(
            [item.address for item in descriptors], [0x800000, 0x800010]
        )

    def test_full_scan_groups_smallest_stride_without_subsamples(self) -> None:
        image = bytearray(b"\xff" * 0x400)
        descriptor = struct.pack("<BBBBHH", 4, 3, 0x81, 0, 0x200, 0x200)
        for offset in (0, 8, 16, 0x80, 0x90):
            image[offset:offset + 8] = descriptor
        image[0x200] = 0xCC

        candidates = scan_image_descriptors(bytes(image), 0x800000)
        runs = find_descriptor_runs(candidates, (8, 16))

        self.assertEqual(len(candidates), 5)
        self.assertEqual(
            [
                (run.stride, [entry.descriptor.address for entry in run.entries])
                for run in runs
            ],
            [
                (8, [0x800000, 0x800008, 0x800010]),
                (16, [0x800080, 0x800090]),
            ],
        )

    def test_full_scan_detects_raw_descriptor(self) -> None:
        image = bytearray(b"\xff" * 0x400)
        image[0:8] = struct.pack("<BBBBHH", 9, 2, 0x01, 0, 0x200, 0x200)
        image[0x200:0x204] = bytes((0x80, 0x00, 0x40, 0x00))

        candidates = scan_image_descriptors(
            bytes(image), 0x800000, kinds=(0x01,)
        )

        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0].descriptor.address, 0x800000)
        self.assertEqual(candidates[0].encoded_bytes, 4)

    def test_full_scan_detects_argb4444_descriptor(self) -> None:
        image = bytearray(b"\xff" * 0x400)
        image[0:8] = struct.pack(
            "<BBBBHH", 2, 1, ARGB4444_BITMAP_TYPE, 0, 0x200, 0x200
        )
        image[0x200:0x204] = bytes.fromhex("00f0000f")

        candidates = scan_image_descriptors(
            bytes(image), 0x800000, kinds=(ARGB4444_BITMAP_TYPE,)
        )

        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0].descriptor.address, 0x800000)
        self.assertEqual(candidates[0].encoded_bytes, 4)

    def test_scan_can_carve_candidates_and_write_gallery(self) -> None:
        image = bytearray(b"\xff" * 0x400)
        image[0:8] = struct.pack("<BBBBHH", 9, 2, 0x01, 0, 0x200, 0x200)
        image[0x200:0x204] = bytes((0x80, 0x00, 0x40, 0x00))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            flash = root / "flash.bin"
            flash.write_bytes(image)
            args = argparse.Namespace(
                flash=flash,
                flash_base=0x800000,
                output=root / "output",
                format="png",
                preview_scale=2,
                scan_types=(0x01,),
                scan_strides=(8,),
                scan_min_run=2,
                scan_min_pixels=1,
                scan_max_width=255,
                scan_max_height=255,
                scan_start=None,
                scan_end=None,
                carve_scan=True,
            )

            candidates, runs = scan_flash(args)
            manifest = (args.output / "scan-manifest.json").read_text(
                encoding="ascii"
            )
            gallery = (args.output / "gallery.html").read_text(encoding="ascii")
            carved = list((args.output / "carved").glob("*.png"))

        self.assertEqual(len(candidates), 1)
        self.assertEqual(runs, [])
        self.assertEqual(len(carved), 2)
        self.assertIn('"kind": "0x01"', manifest)
        self.assertIn('"encoding": "mono1"', manifest)
        self.assertIn('"source_end": "0x800204"', manifest)
        self.assertIn('"preview_png": "carved/', manifest)
        self.assertIn("0x800000 | 9x2 | type 0x01", gallery)

    def test_extraction_preserves_record_suffix(self) -> None:
        image = bytearray(b"\xff" * 0x400)
        image[0:8] = struct.pack("<BBBBHH", 4, 3, 0x81, 0, 0x200, 0x200)
        image[8:16] = bytes.fromhex("000000002c010100")
        image[0x200] = 0xCC
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            flash = root / "flash.bin"
            flash.write_bytes(image)
            args = argparse.Namespace(
                flash=flash,
                table_address=0x800000,
                flash_base=0x800000,
                first=0,
                count=1,
                max_count=1,
                descriptor_stride=16,
                output=root / "output",
                format="png",
                preview_scale=1,
                compose=True,
                compose_width=None,
                compose_height=None,
                compose_origin_x=0,
                compose_origin_y=0,
            )
            extracted = extract_table(args)
            manifest = (args.output / "manifest.json").read_text(encoding="ascii")
            index = (args.output / "index.html").read_text(encoding="utf-8")
            final_exists = (args.output / "composite" / "final.png").exists()

        self.assertEqual(extracted[0].record_suffix, "000000002c010100")
        self.assertIn('"record_suffix": "000000002c010100"', manifest)
        self.assertIn('"delay": 300', manifest)
        self.assertIn('"stage_count": 1', manifest)
        self.assertIn("picture 0", index)
        self.assertIn('href="manifest.json"', index)
        self.assertIn('href="composite/final.png"', index)
        self.assertTrue(final_exists)

    def test_direct_index_lists_images_in_extraction_order(self) -> None:
        image = bytearray(b"\xff" * 0x500)
        image[0:8] = struct.pack("<BBBBHH", 2, 1, 0x07, 0, 0x200, 0x200)
        image[8:16] = struct.pack("<BBBBHH", 1, 1, 0x07, 0, 0x204, 0x200)
        image[0x200:0x206] = bytes.fromhex("00f0000fbcfa")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            flash = root / "flash.bin"
            flash.write_bytes(image)
            args = argparse.Namespace(
                flash=flash,
                table_address=0x800000,
                flash_base=0x800000,
                first=0,
                count=2,
                max_count=2,
                descriptor_stride=8,
                output=root / "output",
                format="both",
                preview_scale=3,
                compose=False,
                compose_width=None,
                compose_height=None,
                compose_origin_x=0,
                compose_origin_y=0,
            )
            extracted = extract_table(args)
            index = (args.output / "index.html").read_text(encoding="utf-8")
            output_files = {path.name for path in args.output.iterdir()}

        self.assertEqual([item.index for item in extracted], [0, 1])
        self.assertLess(index.index("picture 0"), index.index("picture 1"))
        self.assertIn('href="bitmap-000-2x1.png"', index)
        self.assertIn('href="bitmap-000-2x1.bmp"', index)
        self.assertIn('src="bitmap-000-2x1-3x.png"', index)
        self.assertTrue(
            {"bitmap-000-2x1.png", "bitmap-000-2x1.bmp", "bitmap-000-2x1-3x.png"}
            <= output_files
        )

    def test_table_extraction_filters_types_without_renumbering(self) -> None:
        image = bytearray(b"\xff" * 0x500)
        image[0:8] = struct.pack("<BBBBHH", 2, 1, 0x01, 0, 0x200, 0x200)
        image[8:16] = struct.pack("<BBBBHH", 2, 1, 0x03, 0, 0x204, 0x200)
        image[16:24] = struct.pack("<BBBBHH", 2, 1, 0x01, 0, 0x208, 0x200)
        image[0x200:0x209] = bytes.fromhex("804000000000000080")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            flash = root / "flash.bin"
            flash.write_bytes(image)
            args = argparse.Namespace(
                flash=flash,
                table_address=0x800000,
                flash_base=0x800000,
                first=0,
                count=3,
                max_count=3,
                descriptor_stride=8,
                extract_types=(0x01,),
                output=root / "output",
                format="png",
                preview_scale=1,
                compose=False,
                compose_width=None,
                compose_height=None,
                compose_origin_x=0,
                compose_origin_y=0,
            )
            extracted = extract_table(args)
            manifest = (args.output / "manifest.json").read_text(encoding="ascii")

        self.assertEqual([item.index for item in extracted], [0, 2])
        self.assertIn('"extract_types": [', manifest)
        self.assertIn('"0x01"', manifest)

    def test_png_and_bmp_dimensions(self) -> None:
        pixels = [0, 1, 1, 0]
        with tempfile.TemporaryDirectory() as directory:
            png_path = Path(directory) / "test.png"
            bmp_path = Path(directory) / "test.bmp"
            write_png(png_path, pixels, 2, 2, scale=3)
            write_bmp(bmp_path, pixels, 2, 2, scale=3)
            png = png_path.read_bytes()
            bmp = bmp_path.read_bytes()
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack_from(">II", png, 16), (6, 6))
        self.assertEqual(bmp[:2], b"BM")
        self.assertEqual(struct.unpack_from("<II", bmp, 18), (6, 6))

    def test_argb4444_png_and_bmp_pixels(self) -> None:
        pixels = [(0x12, 0x34, 0x56, 0), (0x11, 0x22, 0x33, 255)]
        with tempfile.TemporaryDirectory() as directory:
            png_path = Path(directory) / "test.png"
            bmp_path = Path(directory) / "test.bmp"
            write_png(png_path, pixels, 2, 1)
            write_bmp(bmp_path, pixels, 2, 1)
            png = png_path.read_bytes()
            bmp = bmp_path.read_bytes()
        self.assertEqual(png[25], 6)
        self.assertEqual(bmp[54:60], bytes((255, 255, 255, 0x33, 0x22, 0x11)))

    def test_s55_provider_schema_and_decoded_hashes(self) -> None:
        image = S55_FLASH.read_bytes()
        descriptors = scan_descriptors(image, 0x4CC000, 0x400000)
        self.assertEqual(len(descriptors), 731)
        self.assertEqual(
            Counter(descriptor.kind for descriptor in descriptors),
            Counter({0x01: 128, 0x03: 143, 0x04: 1, 0x05: 459}),
        )

        expected = {
            0x01: "0cef7021df5a572f82351070c2b5a456adc4b161e5ab6931f135a686f915bc9f",
            0x03: "9feec5fd6ad8da9233cd41ff9aeafccfdeae085680e0a0254aa767b5a83bc572",
            0x04: "396cd957cbff10adb97b25abb408f649ee01f66d94e3c7aa8210afd18cf9f41b",
            0x05: "64354b03ac224e9d392103344999694bf1929b6fbea253265272c82dcfcdac00",
        }
        for kind, digest in expected.items():
            decoded = bytearray()
            for descriptor in descriptors:
                if descriptor.kind != kind:
                    continue
                offset = descriptor.source_address - 0x400000
                pixels, _ = decode_bitmap(image[offset:], descriptor)
                for pixel in pixels:
                    decoded.extend((pixel,) if isinstance(pixel, int) else pixel)
            self.assertEqual(hashlib.sha256(decoded).hexdigest(), digest)

    def test_m55_boot_frames_known_answer(self) -> None:
        image = M55_FLASH.read_bytes()
        descriptors = scan_descriptors(
            image,
            table_address=0x508000,
            flash_base=0,
            first=107,
            count=6,
        )
        rgba = bytearray()
        used = []
        for descriptor in descriptors:
            source = descriptor.source_address
            pixels, encoded_bytes = decode_bitmap(image[source:], descriptor)
            used.append(encoded_bytes)
            for pixel in pixels:
                self.assertIsInstance(pixel, tuple)
                rgba.extend(pixel)

        self.assertEqual(
            [(item.width, item.height, item.kind) for item in descriptors],
            [(32, 66, ARGB4444_BITMAP_TYPE)] * 6,
        )
        self.assertEqual(
            [item.source_address for item in descriptors],
            [0x44CBE2, 0x44DC62, 0x44ECE2, 0x450000, 0x451080, 0x452100],
        )
        self.assertEqual(used, [4224] * 6)
        self.assertEqual(
            hashlib.sha256(rgba).hexdigest(),
            "00b98f43a30647edd270f7653dfff4cf07bb825c7ee6f6e6956238f9d7ef2cb1",
        )

    def test_m55_fullscreen_boot_sequences_known_answer(self) -> None:
        image = M55_FLASH.read_bytes()
        descriptors = scan_descriptors(
            image,
            table_address=0x508000,
            flash_base=0,
            first=189,
            count=18,
        )
        rgba = bytearray()
        for descriptor in descriptors:
            pixels, encoded_bytes = decode_bitmap(
                image[descriptor.source_address:], descriptor
            )
            self.assertEqual(encoded_bytes, 16160)
            for pixel in pixels:
                self.assertIsInstance(pixel, tuple)
                rgba.extend(pixel)

        self.assertEqual(
            [(item.width, item.height, item.kind) for item in descriptors],
            [(101, 80, ARGB4444_BITMAP_TYPE)] * 18,
        )
        self.assertEqual(
            [item.source_address for item in descriptors],
            list(range(0x494000, 0x4D8001, 0x4000)),
        )
        self.assertEqual(
            hashlib.sha256(rgba).hexdigest(),
            "0b6299752427fe93f45a3c152c771efbab7460c0232e3aab7fe57259054faf2a",
        )


if __name__ == "__main__":
    unittest.main()
