from __future__ import annotations

import argparse
import json
from pathlib import Path

from .codecs import BitmapDescriptor, DescriptorRun, ScannedDescriptor, decode_bitmap
from .raster import write_bmp, write_png
from .scanning import find_descriptor_runs, scan_entry_dict, scan_image_descriptors

def carve_scan_candidates(
    image: bytes,
    args: argparse.Namespace,
    candidates: list[ScannedDescriptor],
) -> dict[int, dict[str, str]]:
    carved_dir = args.output / "carved"
    carved_dir.mkdir(parents=True, exist_ok=True)
    artifacts: dict[int, dict[str, str]] = {}
    for item in candidates:
        descriptor = item.descriptor
        source_offset = descriptor.source_address - args.flash_base
        pixels, _ = decode_bitmap(
            image[source_offset:],
            descriptor,
            getattr(args, "encoding", "auto"),
            getattr(args, "transparent_index", None),
        )
        stem = (
            f"bitmap-{descriptor.address:06x}-{descriptor.width}x{descriptor.height}"
            f"-t{descriptor.kind:02x}"
        )
        files: dict[str, str] = {}
        if args.format in ("png", "both"):
            name = f"{stem}.png"
            write_png(carved_dir / name, pixels, descriptor.width, descriptor.height)
            files["png"] = f"carved/{name}"
        if args.format in ("bmp", "both"):
            name = f"{stem}.bmp"
            write_bmp(carved_dir / name, pixels, descriptor.width, descriptor.height)
            files["bmp"] = f"carved/{name}"
        if args.preview_scale > 1:
            name = f"{stem}-{args.preview_scale}x.png"
            write_png(
                carved_dir / name,
                pixels,
                descriptor.width,
                descriptor.height,
                args.preview_scale,
            )
            files["preview_png"] = f"carved/{name}"
        artifacts[descriptor.address] = files
    return artifacts


def write_scan_gallery(
    path: Path,
    candidates: list[ScannedDescriptor],
    runs: list[DescriptorRun],
    artifacts: dict[int, dict[str, str]],
) -> None:
    run_addresses = {
        entry.descriptor.address
        for run in runs
        for entry in run.entries
    }

    def rank(item: ScannedDescriptor) -> tuple[bool, bool, int, int]:
        descriptor = item.descriptor
        source_end = descriptor.source_address + item.encoded_bytes
        return (
            source_end != descriptor.address,
            descriptor.address not in run_addresses,
            -(descriptor.width * descriptor.height),
            descriptor.address,
        )

    cards: list[str] = []
    for item in sorted(candidates, key=rank):
        descriptor = item.descriptor
        files = artifacts.get(descriptor.address, {})
        image = files.get("preview_png") or files.get("png")
        if image is None:
            continue
        source_end = descriptor.source_address + item.encoded_bytes
        tags = []
        if source_end == descriptor.address:
            tags.append("source ends at descriptor")
        if descriptor.address in run_addresses:
            tags.append("fixed-stride run")
        tag_text = " | ".join(tags) if tags else "standalone candidate"
        cards.append(
            "<figure>"
            f'<a href="{files.get("png", image)}"><img src="{image}" loading="lazy"></a>'
            f"<figcaption>0x{descriptor.address:06X} | {descriptor.width}x{descriptor.height} "
            f"| type 0x{descriptor.kind:02X} | {item.encoding}<br>"
            f"source 0x{descriptor.source_address:06X} "
            f"| {tag_text}</figcaption></figure>"
        )
    html = (
        "<!doctype html><meta charset=\"ascii\"><title>Firmware bitmap scan</title>"
        "<style>body{font:14px monospace;background:#eee;color:#111;margin:16px}"
        "main{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:12px}"
        "figure{margin:0;padding:8px;background:#fff;border:1px solid #aaa}"
        "img{display:block;max-width:100%;height:auto;image-rendering:pixelated;margin:auto}"
        "figcaption{margin-top:8px;line-height:1.4}</style><main>"
        + "".join(cards)
        + "</main>\n"
    )
    path.write_text(html, encoding="ascii")


def scan_manifest_entry(
    item: ScannedDescriptor,
    artifacts: dict[int, dict[str, str]],
) -> dict[str, int | str | bool | dict[str, str]]:
    entry = scan_entry_dict(item)
    entry["artifacts"] = artifacts.get(item.descriptor.address, {})
    return entry


def scan_flash(args: argparse.Namespace) -> tuple[list[ScannedDescriptor], list[DescriptorRun]]:
    image = args.flash.read_bytes()
    candidates = scan_image_descriptors(
        image,
        args.flash_base,
        max_width=args.scan_max_width,
        max_height=args.scan_max_height,
        min_pixels=args.scan_min_pixels,
        kinds=args.scan_types,
        start_address=args.scan_start,
        end_address=args.scan_end,
        encoding=getattr(args, "encoding", "auto"),
    )
    runs = find_descriptor_runs(candidates, args.scan_strides, args.scan_min_run)
    args.output.mkdir(parents=True, exist_ok=True)
    artifacts = (
        carve_scan_candidates(image, args, candidates)
        if args.carve_scan
        else {}
    )
    if artifacts:
        write_scan_gallery(args.output / "gallery.html", candidates, runs, artifacts)
    manifest = {
        "flash": str(args.flash),
        "flash_base": f"0x{args.flash_base:06x}",
        "types": [f"0x{kind:02x}" for kind in args.scan_types],
        "encoding": getattr(args, "encoding", "auto"),
        "start_address": None if args.scan_start is None else f"0x{args.scan_start:06x}",
        "end_address": None if args.scan_end is None else f"0x{args.scan_end:06x}",
        "min_pixels": args.scan_min_pixels,
        "max_width": args.scan_max_width,
        "max_height": args.scan_max_height,
        "candidate_count": len(candidates),
        "candidates": [scan_manifest_entry(item, artifacts) for item in candidates],
        "runs": [
            {
                "stride": run.stride,
                "count": len(run.entries),
                "first_address": f"0x{run.entries[0].descriptor.address:06x}",
                "last_address": f"0x{run.entries[-1].descriptor.address:06x}",
                "entries": [scan_manifest_entry(item, artifacts) for item in run.entries],
            }
            for run in runs
        ],
    }
    (args.output / "scan-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="ascii"
    )
    return candidates, runs
