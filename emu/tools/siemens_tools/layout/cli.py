from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .catalog import load_layout, load_layout_catalog
from .render import (
    render_layout_markdown,
    render_layout_reference,
    render_layout_text,
    render_layouts_text,
)
from ..firmware.xbi import FirmwareError


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.siemens_tools layout",
        description="Inspect Siemens phone flash memory layouts.",
    )
    selection = parser.add_mutually_exclusive_group(required=True)
    selection.add_argument("model", nargs="?", help="phone model or layout name")
    selection.add_argument(
        "--all", action="store_true", help="render every catalog layout"
    )
    parser.add_argument("--layout-file", type=Path)
    parser.add_argument(
        "--format", choices=("text", "markdown"), default="text",
        help="output format (default: text)",
    )
    parser.add_argument("-o", "--output", type=Path)
    return parser


def command_layout(args: argparse.Namespace) -> None:
    if args.all:
        if args.format == "markdown":
            rendered = render_layout_reference(args.layout_file)
        else:
            catalog = load_layout_catalog(args.layout_file)
            rendered = render_layouts_text(catalog.layouts)
    else:
        layout = load_layout(args.model, args.layout_file).layout
        if args.format == "markdown":
            rendered = render_layout_markdown(layout)
        else:
            rendered = render_layout_text(layout)

    if args.output is None:
        print(rendered, end="")
    else:
        args.output.write_text(rendered, encoding="ascii")


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        command_layout(args)
    except (FirmwareError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0
