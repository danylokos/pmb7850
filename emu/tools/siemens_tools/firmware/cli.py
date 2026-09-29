from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .catalog_cli import add_catalog_parser
from .inspection import command_convert, command_info, command_unpack
from .updater import command_updater_extract, command_updater_info
from .xbi import FirmwareError


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.siemens_tools firmware",
        description="Inspect, unpack, and convert Siemens firmware files."
    )
    commands = parser.add_subparsers(dest="command", required=True)

    info_parser = commands.add_parser("info", help="inspect firmware metadata")
    info_parser.add_argument("input", type=Path)
    info_parser.add_argument("--json", action="store_true",
                             help="emit stable JSON instead of text")
    info_parser.set_defaults(func=command_info)

    unpack_parser = commands.add_parser(
        "unpack", help="extract a Siemens service/update executable"
    )
    unpack_parser.add_argument("input", type=Path)
    unpack_parser.add_argument("-o", "--output", type=Path,
                               help="output directory")
    unpack_parser.add_argument("--force", action="store_true",
                               help="allow existing output paths")
    unpack_parser.set_defaults(func=command_unpack)

    convert_parser = commands.add_parser(
        "convert",
        help="convert XBI firmware to BIN or validated FFSInit EXE to XFS",
    )
    convert_parser.add_argument("input", type=Path)
    convert_parser.add_argument("-o", "--output", type=Path,
                                help="output .bin path")
    convert_parser.add_argument(
        "--payload", type=int,
        help="payload index when an executable contains multiple firmware files",
    )
    convert_parser.add_argument(
        "--reference-root", type=Path, action="append", default=[],
        help="tree containing byte-exact FFSInit EXE/XFS pairs; repeatable",
    )
    convert_parser.add_argument(
        "--bin", action="store_true",
        help="materialize a validated FFSInit EXE directly as full-flash BIN",
    )
    convert_parser.add_argument("--force", action="store_true",
                                help="replace an existing output file")
    convert_parser.set_defaults(func=command_convert)

    updater_parser = commands.add_parser(
        "updater", help="inspect or extract an embedded mobile updater"
    )
    updater_commands = updater_parser.add_subparsers(
        dest="updater_command", required=True
    )

    updater_info = updater_commands.add_parser(
        "info", help="inspect embedded mobile updater frames"
    )
    updater_info.add_argument("input", type=Path)
    updater_info.add_argument(
        "--json", action="store_true", help="emit stable JSON instead of text"
    )
    updater_info.add_argument(
        "--compare-firmware", action="store_true",
        help="materialize contained XBZ payloads and search exact updater relocation",
    )
    updater_info.set_defaults(func=command_updater_info)

    updater_extract = updater_commands.add_parser(
        "extract", help="extract updater frames, C166 image, and manifest"
    )
    updater_extract.add_argument("input", type=Path)
    updater_extract.add_argument(
        "-o", "--output", type=Path, help="output directory"
    )
    updater_extract.add_argument(
        "--force", action="store_true", help="allow existing output paths"
    )
    updater_extract.set_defaults(func=command_updater_extract)

    add_catalog_parser(commands)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        args.func(args)
    except (FirmwareError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0
