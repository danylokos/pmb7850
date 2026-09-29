from __future__ import annotations

import argparse
import binascii
import struct
import zlib
from pathlib import Path

from .codecs import (
    BitmapDescriptor,
    FramePlacement,
    Pixel,
    RgbaPixel,
    compose_bitmap_stages,
)

def pixel_rgba(pixel: Pixel) -> RgbaPixel:
    if isinstance(pixel, int):
        value = 0 if pixel else 255
        return value, value, value, 255
    return pixel


def scaled_raster(
    pixels: list[Pixel], width: int, height: int, scale: int, rgba: bool
) -> list[bytes]:
    if scale < 1:
        raise ValueError("scale must be at least one")
    rows: list[bytes] = []
    for y in range(height):
        source = pixels[y * width:(y + 1) * width]
        if rgba:
            row = b"".join(bytes(pixel_rgba(pixel)) * scale for pixel in source)
        else:
            row = bytes(
                value
                for pixel in source
                for value in [0 if pixel else 255] * scale
            )
        rows.extend([row] * scale)
    return rows


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    checksum = binascii.crc32(kind)
    checksum = binascii.crc32(payload, checksum) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", checksum)


def write_png(path: Path, pixels: list[Pixel], width: int, height: int, scale: int = 1) -> None:
    rgba = any(not isinstance(pixel, int) for pixel in pixels)
    rows = scaled_raster(pixels, width, height, scale, rgba)
    raster = b"".join(b"\x00" + row for row in rows)
    png = b"\x89PNG\r\n\x1a\n"
    color_type = 6 if rgba else 0
    png += png_chunk(
        b"IHDR",
        struct.pack(">IIBBBBB", width * scale, height * scale, 8, color_type, 0, 0, 0),
    )
    png += png_chunk(b"IDAT", zlib.compress(raster, 9))
    png += png_chunk(b"IEND", b"")
    path.write_bytes(png)


def write_bmp(path: Path, pixels: list[Pixel], width: int, height: int, scale: int = 1) -> None:
    rows: list[list[RgbaPixel]] = []
    for y in range(height):
        source = pixels[y * width:(y + 1) * width]
        row = [pixel_rgba(pixel) for pixel in source for _ in range(scale)]
        rows.extend([row] * scale)
    out_width = width * scale
    out_height = height * scale
    row_size = (out_width * 3 + 3) & ~3
    image_size = row_size * out_height
    header_size = 14 + 40
    header = struct.pack("<2sIHHI", b"BM", header_size + image_size, 0, 0, header_size)
    header += struct.pack(
        "<IIIHHIIIIII", 40, out_width, out_height, 1, 24, 0, image_size, 2835, 2835, 0, 0
    )
    body = bytearray()
    padding = b"\x00" * (row_size - out_width * 3)
    for row in reversed(rows):
        for red, green, blue, alpha in row:
            # BMP output remains the existing portable 24-bit form. Flatten
            # transparent ARGB4444 pixels over white instead of discarding alpha.
            red = (red * alpha + 255 * (255 - alpha) + 127) // 255
            green = (green * alpha + 255 * (255 - alpha) + 127) // 255
            blue = (blue * alpha + 255 * (255 - alpha) + 127) // 255
            body.extend((blue, green, red))
        body.extend(padding)
    path.write_bytes(header + body)


def write_composition(
    args: argparse.Namespace,
    frames: list[tuple[BitmapDescriptor, list[Pixel], FramePlacement]],
) -> dict[str, object]:
    stages, width, height = compose_bitmap_stages(
        frames,
        canvas_width=getattr(args, "compose_width", None),
        canvas_height=getattr(args, "compose_height", None),
        origin_x=getattr(args, "compose_origin_x", 0),
        origin_y=getattr(args, "compose_origin_y", 0),
    )
    output = args.output / "composite"
    output.mkdir(parents=True, exist_ok=True)
    stage_entries: list[dict[str, object]] = []
    for index, ((descriptor, _, placement), pixels) in enumerate(zip(frames, stages)):
        stem = f"stage-{index:03d}-after-{descriptor.address:06x}"
        files: dict[str, str] = {}
        if args.format in ("png", "both"):
            name = f"{stem}.png"
            write_png(output / name, pixels, width, height)
            files["png"] = f"composite/{name}"
        if args.format in ("bmp", "both"):
            name = f"{stem}.bmp"
            write_bmp(output / name, pixels, width, height)
            files["bmp"] = f"composite/{name}"
        if args.preview_scale > 1:
            name = f"{stem}-{args.preview_scale}x.png"
            write_png(output / name, pixels, width, height, args.preview_scale)
            files["preview_png"] = f"composite/{name}"
        stage_entries.append(
            {
                "index": index,
                "after_descriptor_address": f"0x{descriptor.address:06x}",
                "x": placement.x,
                "y": placement.y,
                "delay": placement.delay,
                "flags": placement.flags,
                "artifacts": files,
            }
        )

    final_files: dict[str, str] = {}
    final_pixels = stages[-1]
    if args.format in ("png", "both"):
        write_png(output / "final.png", final_pixels, width, height)
        final_files["png"] = "composite/final.png"
    if args.format in ("bmp", "both"):
        write_bmp(output / "final.bmp", final_pixels, width, height)
        final_files["bmp"] = "composite/final.bmp"
    if args.preview_scale > 1:
        name = f"final-{args.preview_scale}x.png"
        write_png(output / name, final_pixels, width, height, args.preview_scale)
        final_files["preview_png"] = f"composite/{name}"

    return {
        "canvas_width": width,
        "canvas_height": height,
        "origin_x": getattr(args, "compose_origin_x", 0),
        "origin_y": getattr(args, "compose_origin_y", 0),
        "stage_count": len(stages),
        "stages": stage_entries,
        "final_artifacts": final_files,
    }
