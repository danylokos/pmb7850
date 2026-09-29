from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path

from .codecs import (
    AUTO_ENCODING_BY_TYPE,
    COMPRESSED_BITMAP_TYPE,
    DESCRIPTOR_SIZE,
    RAW_BITMAP_TYPE,
    SUPPORTED_ENCODINGS,
    parse_int,
)
from .extraction import extract_table
from .reports import scan_flash

def parse_strides(text: str) -> tuple[int, ...]:
    try:
        strides = tuple(parse_int(value.strip()) for value in text.split(","))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("strides must be comma-separated integers") from exc
    if not strides:
        raise argparse.ArgumentTypeError("at least one scan stride is required")
    return strides


def parse_types(text: str) -> tuple[int, ...]:
    try:
        kinds = tuple(dict.fromkeys(parse_int(value.strip()) for value in text.split(",")))
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "types must be comma-separated integers"
        ) from exc
    if not kinds:
        raise argparse.ArgumentTypeError("at least one bitmap type is required")
    invalid = [kind for kind in kinds if kind < 0 or kind > 0xFF]
    if invalid:
        values = ", ".join(str(kind) for kind in invalid)
        raise argparse.ArgumentTypeError(f"bitmap types must be bytes: {values}")
    return kinds


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.siemens_tools bitmap",description=__doc__)
    parser.add_argument("flash", type=Path, help="full-flash image")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--table-address", type=parse_int,
                      help="native address of descriptor index zero")
    mode.add_argument("--scan-flash", action="store_true",
                      help="locate descriptor candidates and fixed-stride runs")
    parser.add_argument("--flash-base", type=parse_int, default=0x800000,
                        help="native address corresponding to file offset zero")
    parser.add_argument("--descriptor-stride", type=parse_int, default=DESCRIPTOR_SIZE,
                        help="bytes between table records (default: 8)")
    parser.add_argument("--first", type=parse_int, default=0,
                        help="first descriptor index to extract")
    parser.add_argument("--count", type=parse_int,
                        help="exact descriptor count; default scans until invalid")
    parser.add_argument(
        "--extract-types",
        type=parse_types,
        help="comma-separated descriptor types to retain from a table extraction",
    )
    parser.add_argument("--max-count", type=parse_int, default=4096,
                        help="safety bound when scanning without --count")
    parser.add_argument("--output", type=Path, required=True, help="output directory")
    parser.add_argument("--format", choices=("png", "bmp", "both"), default="png")
    parser.add_argument("--preview-scale", type=parse_int, default=1,
                        help="also write nearest-neighbor PNG previews at this scale")
    parser.add_argument(
        "--encoding", choices=SUPPORTED_ENCODINGS, default="auto",
        help="pixel encoding override; auto selects from the descriptor type",
    )
    parser.add_argument(
        "--transparent-index", type=parse_int,
        help="RGB332 byte value to emit with zero alpha",
    )
    parser.add_argument(
        "--compose",
        action="store_true",
        help="cumulatively compose 16-byte table records using their x/y trailers",
    )
    parser.add_argument("--compose-width", type=parse_int,
                        help="composition canvas width; default fits all records")
    parser.add_argument("--compose-height", type=parse_int,
                        help="composition canvas height; default fits all records")
    parser.add_argument("--compose-origin-x", type=parse_int, default=0,
                        help="X offset added to every composed record (default: 0)")
    parser.add_argument("--compose-origin-y", type=parse_int, default=0,
                        help="Y offset added to every composed record (default: 0)")
    parser.add_argument(
        "--scan-types",
        type=parse_types,
        default=(RAW_BITMAP_TYPE, COMPRESSED_BITMAP_TYPE),
        help="comma-separated descriptor type bytes (default: 0x01,0x81)",
    )
    parser.add_argument(
        "--scan-strides",
        type=parse_strides,
        default=(8, 12, 16, 20, 24, 32, 48, 64),
        help="comma-separated record strides to group",
    )
    parser.add_argument("--scan-min-run", type=parse_int, default=2,
                        help="minimum descriptors in a reported run (default: 2)")
    parser.add_argument("--scan-min-pixels", type=parse_int, default=1,
                        help="minimum candidate pixel count (default: 1)")
    parser.add_argument("--scan-max-width", type=parse_int, default=255,
                        help="maximum candidate width (default: 255)")
    parser.add_argument("--scan-max-height", type=parse_int, default=255,
                        help="maximum candidate height (default: 255)")
    parser.add_argument("--scan-start", type=parse_int,
                        help="first native descriptor address to scan (inclusive)")
    parser.add_argument("--scan-end", type=parse_int,
                        help="last native descriptor address to scan (inclusive)")
    parser.add_argument(
        "--carve-scan",
        action="store_true",
        help="decode every scan candidate and write a ranked HTML gallery",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.preview_scale < 1:
        parser.error("--preview-scale must be at least one")
    if args.transparent_index is not None and not 0 <= args.transparent_index <= 0xFF:
        parser.error("--transparent-index must be a byte")
    if args.encoding == "auto":
        unsupported = [kind for kind in args.scan_types if kind not in AUTO_ENCODING_BY_TYPE]
        if unsupported:
            values = ", ".join(f"0x{kind:02x}" for kind in unsupported)
            parser.error(f"unsupported bitmap type(s) in auto mode: {values}")
    if args.descriptor_stride < DESCRIPTOR_SIZE:
        parser.error("--descriptor-stride must be at least eight")
    if args.scan_max_width < 1 or args.scan_max_height < 1:
        parser.error("scan dimensions must be at least one")
    if args.scan_min_pixels < 1:
        parser.error("--scan-min-pixels must be at least one")
    if args.scan_min_run < 2:
        parser.error("--scan-min-run must be at least two")
    if any(stride < DESCRIPTOR_SIZE for stride in args.scan_strides):
        parser.error("--scan-strides values must be at least eight")
    if args.scan_start is not None and args.scan_end is not None:
        if args.scan_start > args.scan_end:
            parser.error("--scan-start must not exceed --scan-end")
    if args.carve_scan and not args.scan_flash:
        parser.error("--carve-scan requires --scan-flash")
    if args.extract_types is not None and args.scan_flash:
        parser.error("--extract-types requires --table-address")
    if args.compose and args.scan_flash:
        parser.error("--compose requires --table-address")
    if args.compose and args.descriptor_stride < 16:
        parser.error("--compose requires --descriptor-stride of at least 16")
    if args.compose_width is not None and args.compose_width < 1:
        parser.error("--compose-width must be at least one")
    if args.compose_height is not None and args.compose_height < 1:
        parser.error("--compose-height must be at least one")
    if args.compose_origin_x < 0 or args.compose_origin_y < 0:
        parser.error("composition origins must not be negative")
    try:
        if args.scan_flash:
            candidates, runs = scan_flash(args)
        else:
            extracted = extract_table(args)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    if args.scan_flash:
        print(
            f"found {len(candidates)} candidate descriptor(s) "
            f"in {len(runs)} fixed-stride run(s)"
        )
        print(f"output: {args.output}")
        return 0
    dimensions = Counter((entry.width, entry.height) for entry in extracted)
    print(f"extracted {len(extracted)} bitmap(s) across {len(dimensions)} resolution(s)")
    print(f"output: {args.output}")
    return 0
