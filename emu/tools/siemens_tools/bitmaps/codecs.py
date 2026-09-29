#!/usr/bin/env python3
"""Detect and extract Siemens bitmap descriptors from a full-flash image.

The firmware stores eight-byte descriptors as:

    width, height, type, reserved, source_offset_le16, source_page_le16

Type 0x01 stores MSB-first, byte-padded rows. Provider source boundaries
identify type 0x03 as row-padded 2bpp, the one known S55 type 0x04 record as
row-padded 1bpp, and type 0x05 as 8bpp. Indexed previews remain grayscale while
type 0x05 defaults to an RGB332 preview palette; its runtime palette semantics
remain unknown. Type 0x81 uses the packet format recovered from firmware routine
FUN_96A1FA: bytes with bit 7 clear carry seven literal pixels in bits 6..0;
bytes with bit 7 set carry a run whose value is bit 6 and whose length is bits
5..0. M55 type 0x07 stores little-endian ARGB4444; firmware treats a zero alpha
nibble as transparent and any non-zero alpha as opaque. Explicit encoding
overrides permit trial decoding independently of the descriptor type byte.
descriptor type byte.

"""

from __future__ import annotations

import argparse
import binascii
import html
import json
import struct
import zlib
from collections import Counter
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Union


DESCRIPTOR_SIZE = 8
RAW_BITMAP_TYPE = 0x01
INDEXED2_BITMAP_TYPE = 0x03
INDEXED4_BITMAP_TYPE = 0x04
RGB332_BITMAP_TYPE = 0x05
ARGB4444_BITMAP_TYPE = 0x07
COMPRESSED_BITMAP_TYPE = 0x81
SUPPORTED_BITMAP_TYPES = frozenset(
    (
        RAW_BITMAP_TYPE,
        INDEXED2_BITMAP_TYPE,
        INDEXED4_BITMAP_TYPE,
        RGB332_BITMAP_TYPE,
        ARGB4444_BITMAP_TYPE,
        COMPRESSED_BITMAP_TYPE,
    )
)

AUTO_ENCODING_BY_TYPE = {
    RAW_BITMAP_TYPE: "mono1",
    INDEXED2_BITMAP_TYPE: "indexed2-msb",
    INDEXED4_BITMAP_TYPE: "mono1",
    RGB332_BITMAP_TYPE: "rgb332",
    ARGB4444_BITMAP_TYPE: "argb4444le",
    COMPRESSED_BITMAP_TYPE: "mono-rle",
}
SUPPORTED_ENCODINGS = (
    "auto", "mono1", "indexed2-msb", "indexed2-lsb",
    "indexed4-msb", "indexed4-lsb", "rgb332", "rgb555le",
    "rgb565le", "argb4444le", "mono-rle",
)

RgbaPixel = tuple[int, int, int, int]
Pixel = Union[int, RgbaPixel]


def parse_int(text: str) -> int:
    return int(text, 0)


@dataclass(frozen=True)
class BitmapDescriptor:
    index: int
    address: int
    width: int
    height: int
    kind: int
    reserved: int
    source_offset: int
    source_page: int
    source_address: int

    @classmethod
    def from_bytes(cls, index: int, address: int, raw: bytes) -> "BitmapDescriptor":
        if len(raw) != DESCRIPTOR_SIZE:
            raise ValueError("bitmap descriptor must be eight bytes")
        width, height, kind, reserved, source_offset, source_page = struct.unpack(
            "<BBBBHH", raw
        )
        source_address = ((source_page & 0x3FF) << 14) | (source_offset & 0x3FFF)
        return cls(
            index=index,
            address=address,
            width=width,
            height=height,
            kind=kind,
            reserved=reserved,
            source_offset=source_offset,
            source_page=source_page,
            source_address=source_address,
        )

    def is_structurally_valid(self, flash_base: int, image_size: int) -> bool:
        return (
            self.width > 0
            and self.height > 0
            and self.reserved == 0
            and flash_base <= self.source_address < flash_base + image_size
        )

    def is_valid(self, flash_base: int, image_size: int) -> bool:
        return (
            self.is_structurally_valid(flash_base, image_size)
            and self.kind in SUPPORTED_BITMAP_TYPES
        )


@dataclass(frozen=True)
class ExtractedBitmap:
    index: int
    descriptor_address: str
    source_address: str
    width: int
    height: int
    encoded_bytes: int
    kind: str
    encoding: str = "auto"
    record_suffix: str | None = None
    x: int | None = None
    y: int | None = None
    delay: int | None = None
    flags: int | None = None
    png: str | None = None
    bmp: str | None = None
    preview_png: str | None = None


@dataclass(frozen=True)
class ScannedDescriptor:
    descriptor: BitmapDescriptor
    encoded_bytes: int
    encoding: str = "auto"


@dataclass(frozen=True)
class DescriptorRun:
    stride: int
    entries: tuple[ScannedDescriptor, ...]


@dataclass(frozen=True)
class FramePlacement:
    x: int
    y: int
    delay: int
    flags: int


def address_offset(address: int, flash_base: int, image_size: int) -> int:
    offset = address - flash_base
    if offset < 0 or offset >= image_size:
        raise ValueError(f"address 0x{address:06x} is outside the flash image")
    return offset


def resolve_encoding(kind: int, encoding: str = "auto") -> str:
    if encoding not in SUPPORTED_ENCODINGS:
        raise ValueError(f"unsupported bitmap encoding {encoding}")
    if encoding != "auto":
        return encoding
    try:
        return AUTO_ENCODING_BY_TYPE[kind]
    except KeyError as exc:
        raise ValueError(f"unsupported bitmap type 0x{kind:02x} in auto mode") from exc


def scan_descriptors(
    image: bytes,
    table_address: int,
    flash_base: int,
    first: int = 0,
    count: int | None = None,
    max_count: int = 4096,
    stride: int = DESCRIPTOR_SIZE,
    encoding: str = "auto",
) -> list[BitmapDescriptor]:
    if stride < DESCRIPTOR_SIZE:
        raise ValueError("descriptor stride must be at least eight bytes")
    descriptors: list[BitmapDescriptor] = []
    scan_count = count if count is not None else max_count
    for relative in range(scan_count):
        index = first + relative
        address = table_address + index * stride
        offset = address_offset(address, flash_base, len(image))
        descriptor = BitmapDescriptor.from_bytes(index, address, image[offset:offset + 8])
        valid = descriptor.is_structurally_valid(flash_base, len(image))
        if encoding == "auto":
            valid = valid and descriptor.kind in SUPPORTED_BITMAP_TYPES
        if not valid:
            if count is None:
                break
            raise ValueError(
                f"invalid descriptor index 0x{index:x} at 0x{address:06x}"
            )
        resolve_encoding(descriptor.kind, encoding)
        descriptors.append(descriptor)
    if count is None and len(descriptors) == max_count:
        raise ValueError(f"descriptor scan reached --max-count {max_count}")
    return descriptors


def decode_compressed(data: bytes, pixel_count: int) -> tuple[list[int], int]:
    pixels: list[int] = []
    for used, value in enumerate(data, start=1):
        if value & 0x80:
            run_length = value & 0x3F
            pixels.extend([1 if value & 0x40 else 0] * run_length)
        else:
            pixels.extend((value >> shift) & 1 for shift in range(6, -1, -1))
        if len(pixels) >= pixel_count:
            return pixels[:pixel_count], used
    raise ValueError(f"compressed stream ended after {len(pixels)}/{pixel_count} pixels")


def decode_raw(data: bytes, width: int, height: int) -> tuple[list[int], int]:
    row_bytes = (width + 7) // 8
    required = row_bytes * height
    if len(data) < required:
        raise ValueError(f"raw stream ended after {len(data)}/{required} bytes")
    pixels: list[int] = []
    for y in range(height):
        row = data[y * row_bytes:(y + 1) * row_bytes]
        pixels.extend(
            (row[x // 8] >> (7 - (x & 7))) & 1
            for x in range(width)
        )
    return pixels, required


def _grayscale_pixel(index: int, maximum: int) -> RgbaPixel:
    value = (index * 255 + maximum // 2) // maximum
    return value, value, value, 255


def decode_indexed(
    data: bytes, width: int, height: int, bits: int, msb_first: bool
) -> tuple[list[RgbaPixel], int]:
    if bits not in (2, 4):
        raise ValueError("indexed bitmap depth must be two or four bits")
    pixels_per_byte = 8 // bits
    row_bytes = (width + pixels_per_byte - 1) // pixels_per_byte
    required = row_bytes * height
    if len(data) < required:
        raise ValueError(
            f"indexed{bits} stream ended after {len(data)}/{required} bytes"
        )
    mask = (1 << bits) - 1
    pixels: list[RgbaPixel] = []
    for y in range(height):
        row = data[y * row_bytes:(y + 1) * row_bytes]
        for x in range(width):
            lane = x % pixels_per_byte
            shift = (pixels_per_byte - 1 - lane) * bits if msb_first else lane * bits
            pixels.append(_grayscale_pixel((row[x // pixels_per_byte] >> shift) & mask, mask))
    return pixels, required


def decode_indexed2(
    data: bytes, width: int, height: int, msb_first: bool = True
) -> tuple[list[RgbaPixel], int]:
    return decode_indexed(data, width, height, 2, msb_first)


def decode_indexed4(
    data: bytes, width: int, height: int, msb_first: bool = True
) -> tuple[list[RgbaPixel], int]:
    return decode_indexed(data, width, height, 4, msb_first)


def _expand_channel(value: int, bits: int) -> int:
    maximum = (1 << bits) - 1
    return (value * 255 + maximum // 2) // maximum


def decode_rgb332(
    data: bytes, width: int, height: int, transparent_index: int | None = None
) -> tuple[list[RgbaPixel], int]:
    required = width * height
    if len(data) < required:
        raise ValueError(f"RGB332 stream ended after {len(data)}/{required} bytes")
    pixels = [
        (
            _expand_channel(value >> 5, 3),
            _expand_channel((value >> 2) & 7, 3),
            _expand_channel(value & 3, 2),
            0 if value == transparent_index else 255,
        )
        for value in data[:required]
    ]
    return pixels, required


def _decode_rgb16(
    data: bytes, width: int, height: int, green_bits: int
) -> tuple[list[RgbaPixel], int]:
    required = width * height * 2
    name = "RGB565" if green_bits == 6 else "RGB555"
    if len(data) < required:
        raise ValueError(f"{name} stream ended after {len(data)}/{required} bytes")
    pixels: list[RgbaPixel] = []
    for offset in range(0, required, 2):
        value = data[offset] | (data[offset + 1] << 8)
        if green_bits == 6:
            red, green, blue = value >> 11, (value >> 5) & 0x3F, value & 0x1F
        else:
            red, green, blue = (value >> 10) & 0x1F, (value >> 5) & 0x1F, value & 0x1F
        pixels.append(
            (
                _expand_channel(red, 5),
                _expand_channel(green, green_bits),
                _expand_channel(blue, 5),
                255,
            )
        )
    return pixels, required


def decode_rgb555le(
    data: bytes, width: int, height: int
) -> tuple[list[RgbaPixel], int]:
    return _decode_rgb16(data, width, height, 5)


def decode_rgb565le(
    data: bytes, width: int, height: int
) -> tuple[list[RgbaPixel], int]:
    return _decode_rgb16(data, width, height, 6)


def decode_argb4444(
    data: bytes, width: int, height: int
) -> tuple[list[RgbaPixel], int]:
    required = width * height * 2
    if len(data) < required:
        raise ValueError(f"ARGB4444 stream ended after {len(data)}/{required} bytes")
    pixels: list[RgbaPixel] = []
    for offset in range(0, required, 2):
        value = data[offset] | (data[offset + 1] << 8)
        pixels.append(
            (
                ((value >> 8) & 0xF) * 17,
                ((value >> 4) & 0xF) * 17,
                (value & 0xF) * 17,
                255 if value & 0xF000 else 0,
            )
        )
    return pixels, required


def decode_bitmap(
    data: bytes, descriptor: BitmapDescriptor, encoding: str = "auto",
    transparent_index: int | None = None,
) -> tuple[list[Pixel], int]:
    selected = resolve_encoding(descriptor.kind, encoding)
    if selected == "mono1":
        return decode_raw(data, descriptor.width, descriptor.height)
    if selected == "indexed2-msb":
        return decode_indexed2(data, descriptor.width, descriptor.height, True)
    if selected == "indexed2-lsb":
        return decode_indexed2(data, descriptor.width, descriptor.height, False)
    if selected == "indexed4-msb":
        return decode_indexed4(data, descriptor.width, descriptor.height, True)
    if selected == "indexed4-lsb":
        return decode_indexed4(data, descriptor.width, descriptor.height, False)
    if selected == "rgb332":
        return decode_rgb332(data, descriptor.width, descriptor.height, transparent_index)
    if selected == "rgb555le":
        return decode_rgb555le(data, descriptor.width, descriptor.height)
    if selected == "rgb565le":
        return decode_rgb565le(data, descriptor.width, descriptor.height)
    if selected == "argb4444le":
        return decode_argb4444(data, descriptor.width, descriptor.height)
    if selected == "mono-rle":
        return decode_compressed(data, descriptor.width * descriptor.height)
    raise ValueError(f"unsupported bitmap encoding {selected}")


def parse_frame_placement(suffix: bytes) -> FramePlacement:
    if len(suffix) < 8:
        raise ValueError("composed bitmap records require an eight-byte trailer")
    return FramePlacement(*struct.unpack("<HHHH", suffix[:8]))


def blit_bitmap(
    canvas: list[Pixel],
    canvas_width: int,
    canvas_height: int,
    pixels: list[Pixel],
    width: int,
    height: int,
    x: int,
    y: int,
) -> None:
    if x < 0 or y < 0 or x + width > canvas_width or y + height > canvas_height:
        raise ValueError(
            f"bitmap {width}x{height} at ({x},{y}) exceeds "
            f"canvas {canvas_width}x{canvas_height}"
        )
    if len(pixels) != width * height:
        raise ValueError("bitmap pixel count does not match its dimensions")
    for source_y in range(height):
        source_start = source_y * width
        target_start = (y + source_y) * canvas_width + x
        canvas[target_start:target_start + width] = pixels[
            source_start:source_start + width
        ]


def compose_bitmap_stages(
    frames: list[tuple[BitmapDescriptor, list[Pixel], FramePlacement]],
    canvas_width: int | None = None,
    canvas_height: int | None = None,
    origin_x: int = 0,
    origin_y: int = 0,
) -> tuple[list[list[Pixel]], int, int]:
    if not frames:
        raise ValueError("no bitmap frames to compose")
    required_width = max(
        origin_x + placement.x + descriptor.width
        for descriptor, _, placement in frames
    )
    required_height = max(
        origin_y + placement.y + descriptor.height
        for descriptor, _, placement in frames
    )
    width = required_width if canvas_width is None else canvas_width
    height = required_height if canvas_height is None else canvas_height
    if width < required_width or height < required_height:
        raise ValueError(
            f"composition requires at least {required_width}x{required_height}, "
            f"got {width}x{height}"
        )

    canvas = [0] * (width * height)
    stages: list[list[Pixel]] = []
    for descriptor, pixels, placement in frames:
        blit_bitmap(
            canvas,
            width,
            height,
            pixels,
            descriptor.width,
            descriptor.height,
            origin_x + placement.x,
            origin_y + placement.y,
        )
        stages.append(canvas.copy())
    return stages, width, height
