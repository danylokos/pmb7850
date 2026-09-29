from __future__ import annotations

import argparse
import html
import json
from dataclasses import asdict
from pathlib import Path

from .codecs import (
    DESCRIPTOR_SIZE,
    BitmapDescriptor,
    ExtractedBitmap,
    FramePlacement,
    Pixel,
    address_offset,
    decode_bitmap,
    parse_frame_placement,
    resolve_encoding,
    scan_descriptors,
)
from .raster import write_bmp, write_composition, write_png

def extract_table(args: argparse.Namespace) -> list[ExtractedBitmap]:
    image = args.flash.read_bytes()
    encoding = getattr(args, "encoding", "auto")
    descriptors = scan_descriptors(
        image,
        args.table_address,
        args.flash_base,
        first=args.first,
        count=args.count,
        max_count=args.max_count,
        stride=args.descriptor_stride,
        encoding=encoding,
    )
    if not descriptors:
        raise ValueError("no valid bitmap descriptors found")
    extract_types = getattr(args, "extract_types", None)
    if extract_types is not None:
        descriptors = [
            descriptor for descriptor in descriptors
            if descriptor.kind in extract_types
        ]
        if not descriptors:
            values = ", ".join(f"0x{kind:02x}" for kind in extract_types)
            raise ValueError(f"no descriptors match --extract-types {values}")

    args.output.mkdir(parents=True, exist_ok=True)
    extracted: list[ExtractedBitmap] = []
    composition_frames: list[
        tuple[BitmapDescriptor, list[Pixel], FramePlacement]
    ] = []
    for descriptor in descriptors:
        descriptor_offset = address_offset(
            descriptor.address, args.flash_base, len(image)
        )
        suffix = image[descriptor_offset + DESCRIPTOR_SIZE:
                       descriptor_offset + args.descriptor_stride]
        placement = (
            parse_frame_placement(suffix)
            if args.descriptor_stride >= 16
            else None
        )
        source_offset = address_offset(descriptor.source_address, args.flash_base, len(image))
        selected_encoding = resolve_encoding(descriptor.kind, encoding)
        pixels, encoded_bytes = decode_bitmap(
            image[source_offset:],
            descriptor,
            encoding,
            getattr(args, "transparent_index", None),
        )
        if getattr(args, "compose", False):
            if selected_encoding == "argb4444le":
                raise ValueError("--compose currently supports monochrome records only")
            if placement is None:
                raise ValueError("--compose requires 16-byte bitmap records")
            composition_frames.append((descriptor, pixels, placement))
        stem = f"bitmap-{descriptor.index:03x}-{descriptor.width}x{descriptor.height}"
        png_name = bmp_name = preview_name = None
        if args.format in ("png", "both"):
            png_name = f"{stem}.png"
            write_png(args.output / png_name, pixels, descriptor.width, descriptor.height)
        if args.format in ("bmp", "both"):
            bmp_name = f"{stem}.bmp"
            write_bmp(args.output / bmp_name, pixels, descriptor.width, descriptor.height)
        if args.preview_scale > 1:
            preview_name = f"{stem}-{args.preview_scale}x.png"
            write_png(
                args.output / preview_name,
                pixels,
                descriptor.width,
                descriptor.height,
                args.preview_scale,
            )
        extracted.append(
            ExtractedBitmap(
                index=descriptor.index,
                descriptor_address=f"0x{descriptor.address:06x}",
                source_address=f"0x{descriptor.source_address:06x}",
                width=descriptor.width,
                height=descriptor.height,
                kind=f"0x{descriptor.kind:02x}",
                encoding=selected_encoding,
                encoded_bytes=encoded_bytes,
                record_suffix=suffix.hex() if suffix else None,
                x=None if placement is None else placement.x,
                y=None if placement is None else placement.y,
                delay=None if placement is None else placement.delay,
                flags=None if placement is None else placement.flags,
                png=png_name,
                bmp=bmp_name,
                preview_png=preview_name,
            )
        )

    composition = (
        write_composition(args, composition_frames)
        if getattr(args, "compose", False)
        else None
    )
    manifest = {
        "flash": str(args.flash),
        "flash_base": f"0x{args.flash_base:06x}",
        "table_address": f"0x{args.table_address:06x}",
        "descriptor_stride": args.descriptor_stride,
        "encoding": encoding,
        "transparent_index": (
            None
            if getattr(args, "transparent_index", None) is None
            else f"0x{args.transparent_index:02x}"
        ),
        "extract_types": (
            None
            if extract_types is None
            else [f"0x{kind:02x}" for kind in extract_types]
        ),
        "count": len(extracted),
        "entries": [asdict(entry) for entry in extracted],
    }
    if composition is not None:
        manifest["composition"] = composition
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="ascii"
    )
    write_extraction_index(args.output / "index.html", extracted, composition)
    return extracted


def write_extraction_index(
    path: Path,
    extracted: list[ExtractedBitmap],
    composition: dict[str, object] | None,
) -> None:
    cards: list[str] = []
    for entry in extracted:
        image = entry.preview_png or entry.png or entry.bmp
        if image is None:
            continue
        target = entry.png or entry.bmp or image
        links = []
        for label, artifact in (
            ("PNG", entry.png),
            ("BMP", entry.bmp),
            ("preview", entry.preview_png),
        ):
            if artifact is not None:
                links.append(f'<a href="{html.escape(artifact)}">{label}</a>')
        cards.append(
            "<figure>"
            f'<a href="{html.escape(target)}"><img src="{html.escape(image)}" '
            'loading="lazy" alt=""></a>'
            f"<figcaption>picture {entry.index} (0x{entry.index:X}) | "
            f"{entry.width}x{entry.height} | type {entry.kind} | {entry.encoding}<br>"
            f"descriptor {entry.descriptor_address} | source {entry.source_address}<br>"
            f"{entry.encoded_bytes} encoded bytes | {' | '.join(links)}"
            "</figcaption></figure>"
        )

    composition_html = ""
    if composition is not None:
        artifacts = composition.get("final_artifacts", {})
        if isinstance(artifacts, dict):
            links = " | ".join(
                f'<a href="{html.escape(str(artifact))}">{html.escape(str(label))}</a>'
                for label, artifact in artifacts.items()
            )
            composition_html = (
                "<section><h2>Composition</h2>"
                f"<p>{html.escape(str(composition.get('stage_count', 0)))} stages | {links}</p>"
                "</section>"
            )

    document = (
        '<!doctype html><html lang="en"><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        "<title>Bitmap table extraction</title>"
        "<style>body{font:14px ui-monospace,monospace;background:#eee;color:#111;margin:16px}"
        "header{display:flex;align-items:baseline;gap:16px;flex-wrap:wrap}"
        "main{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:12px}"
        "figure{margin:0;padding:8px;background:#fff;border:1px solid #aaa}"
        "img{display:block;max-width:100%;height:auto;image-rendering:pixelated;margin:auto}"
        "figcaption{margin-top:8px;line-height:1.5}a{color:#0645ad}</style>"
        '<header><h1>Bitmap table extraction</h1><a href="manifest.json">manifest.json</a></header>'
        + composition_html
        + "<main>"
        + "".join(cards)
        + "</main></html>\n"
    )
    path.write_text(document, encoding="utf-8")
