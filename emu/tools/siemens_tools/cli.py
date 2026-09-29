from __future__ import annotations

import argparse
import sys
from collections.abc import Callable

from .bitmaps.cli import main as bitmap_main
from .eeprom.cli import main as eeprom_main
from .firmware.cli import main as firmware_main
from .fullflash.cli import main as fullflash_main
from .layout.cli import main as layout_main


COMMANDS: dict[str, Callable[[list[str] | None], int]] = {
    "firmware": firmware_main,
    "fullflash": fullflash_main,
    "layout": layout_main,
    "eeprom": eeprom_main,
    "bitmap": bitmap_main,
}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.siemens_tools",
        description=(
            "Inspect Siemens firmware, flash layouts, EEPROM, and bitmap resources."
        ),
    )
    domains = parser.add_subparsers(dest="domain", required=True)
    domains.add_parser(
        "firmware", add_help=False, help="firmware packages"
    )
    domains.add_parser(
        "fullflash", add_help=False, help="raw flash images and assembly"
    )
    domains.add_parser(
        "layout", add_help=False, help="phone flash memory layouts"
    )
    domains.add_parser(
        "eeprom", add_help=False, help="EEPROM records and identity overlays"
    )
    domains.add_parser(
        "bitmap", add_help=False, help="bitmap tables and flash scans"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    parser = build_parser()
    namespace, remainder = parser.parse_known_args(arguments)
    return COMMANDS[namespace.domain](remainder)
