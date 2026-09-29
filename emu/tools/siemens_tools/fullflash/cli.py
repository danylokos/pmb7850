from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .assembly import (
    command_assemble,
    parse_bootkey,
    parse_flash_id,
)
from .audit import command_audit_dump
from .bcore_key import (
    command_bcore_key_derive,
    command_bcore_key_recover,
    parse_bcore_hash,
    parse_fsn,
    parse_skey,
)
from .catalog_cli import add_catalog_parser
from .compose import command_compose
from .split import command_split
from ..eeprom import parse_fsn as parse_eeprom_fsn
from ..firmware.xbi import FirmwareError


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.siemens_tools fullflash",
        description="Inspect, split, and assemble Siemens raw flash images.",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    info_parser = commands.add_parser(
        "info", help="scan and inspect a raw fullflash capture"
    )
    info_parser.add_argument("input", type=Path)
    info_parser.add_argument("--layout-file", type=Path)
    info_parser.add_argument("--layout")
    info_parser.add_argument(
        "--flash-address", type=lambda value: int(value, 0),
        help="native flash address corresponding to --file-offset",
    )
    info_parser.add_argument(
        "--file-offset", type=lambda value: int(value, 0), default=0,
        help="input offset corresponding to --flash-address",
    )
    info_parser.add_argument(
        "--blocks", action="store_true",
        help="print the complete EEPROM block inventory in text output",
    )
    info_parser.add_argument(
        "--json", action="store_true", help="emit machine-readable JSON"
    )
    info_parser.set_defaults(func=command_audit_dump)

    split_parser = commands.add_parser(
        "split", help="losslessly split a raw capture at layout boundaries"
    )
    split_parser.add_argument("input", type=Path)
    split_parser.add_argument("-o", "--output", type=Path)
    split_parser.add_argument("--layout-file", type=Path)
    split_parser.add_argument("--layout")
    split_parser.add_argument(
        "--flash-address", type=lambda value: int(value, 0),
        help="native flash address corresponding to --file-offset",
    )
    split_parser.add_argument(
        "--file-offset", type=lambda value: int(value, 0), default=0,
        help="input offset corresponding to --flash-address",
    )
    split_parser.add_argument(
        "--force", action="store_true",
        help="replace generated files while preserving unrelated files",
    )
    split_parser.set_defaults(func=command_split)

    add_catalog_parser(commands)

    compose_parser = commands.add_parser(
        "compose",
        help="compose a complete fullflash from ordered manifest operations",
    )
    compose_parser.add_argument("manifest", type=Path)
    compose_parser.add_argument("-o", "--output", type=Path, required=True)
    compose_parser.add_argument(
        "--force", action="store_true", help="replace an existing output image"
    )
    compose_parser.set_defaults(func=command_compose)

    bcore_key_parser = commands.add_parser(
        "bcore-key", help="derive or recover the BCORE BOOTKEY authentication chain"
    )
    bcore_key_commands = bcore_key_parser.add_subparsers(
        dest="bcore_key_command", required=True
    )
    derive_parser = bcore_key_commands.add_parser(
        "derive", help="derive BOOTKEY and BCORE hash from FSN and SKey"
    )
    derive_parser.add_argument("--fsn", type=parse_fsn, required=True)
    derive_parser.add_argument("--skey", type=parse_skey, required=True)
    derive_parser.add_argument(
        "--json", action="store_true", help="emit machine-readable JSON"
    )
    derive_parser.set_defaults(func=command_bcore_key_derive)

    recover_parser = bcore_key_commands.add_parser(
        "recover", help="search Joker's complete eight-digit SKey domain"
    )
    recover_parser.add_argument("--fsn", type=parse_fsn, required=True)
    recover_parser.add_argument(
        "--hash", type=parse_bcore_hash, required=True, dest="hash"
    )
    recover_parser.add_argument("--workers", type=int)
    recover_parser.add_argument(
        "--json", action="store_true", help="emit machine-readable JSON"
    )
    recover_parser.set_defaults(func=command_bcore_key_recover)

    assemble_parser = commands.add_parser(
        "assemble",
        help="assemble a synthetic fullflash from community and official catalogs",
    )
    assemble_parser.add_argument(
        "model", nargs="?", help="supported handset model, for example C55"
    )
    assemble_parser.add_argument(
        "--recipe", type=Path,
        help="replay and verify an existing synthetic assembly recipe",
    )
    assemble_parser.add_argument(
        "--baseline", help="complete community baseline ID, path, or SHA prefix"
    )
    assemble_parser.add_argument(
        "--region", dest="regions", action="append", default=[],
        metavar="ROLE=SELECTION",
        help=(
            "replace one role from community:ID or official:ID, or fill it "
            "with FF using erased; repeatable"
        ),
    )
    assemble_parser.add_argument(
        "--mobsw", help="official MobSw package ID, path, or SHA prefix"
    )
    assemble_parser.add_argument(
        "--eeprom-map", type=Path,
        help="compose the EEPROM region from a Siemens factory-default map",
    )
    assemble_parser.add_argument(
        "--eeprom-profile",
        help="named EEPROM reconstruction profile applied over --eeprom-map",
    )
    assemble_parser.add_argument(
        "--eeprom-donor", type=Path,
        help="hash-pinned fullflash supplying exact records required by the profile",
    )
    assemble_parser.add_argument(
        "--eeprom-imei",
        help="14-digit IMEI for deterministic generated EEPROM identity records",
    )
    assemble_parser.add_argument(
        "--eeprom-fsn", type=parse_eeprom_fsn,
        help="8-digit hexadecimal FSN for generated EEPROM identity records",
    )
    assemble_parser.add_argument(
        "--flash-id", dest="flash_ids", type=parse_flash_id,
        action="append", default=[], metavar="MFR:DEVICE",
        help="statistics flash descriptor; may be repeated twice",
    )
    assemble_parser.add_argument(
        "--entry-target", type=lambda value: int(value, 0),
        help="write a C166 JMPS target at the layout finalization destination",
    )
    assemble_parser.add_argument(
        "--bootkey", type=parse_bootkey,
        help="16-byte BOOTKEY whose MD5 digest is installed in BCORE",
    )
    assemble_parser.add_argument("--fsn", type=parse_fsn)
    assemble_parser.add_argument("--skey", type=parse_skey)
    assemble_parser.add_argument(
        "--erase-bootkey", action="store_true",
        help="erase the normalized BCORE BOOTKEY digest field",
    )
    assemble_parser.add_argument(
        "--community-catalog", type=Path,
        help="community corpus root or catalog.json (default: model corpus)",
    )
    assemble_parser.add_argument(
        "--official-catalog", type=Path,
        help="official corpus root or catalog.json (default: model corpus)",
    )
    assemble_parser.add_argument(
        "--non-interactive", action="store_true",
        help="require an explicit baseline and inherit omitted regions",
    )
    assemble_parser.add_argument("-o", "--output", type=Path)
    assemble_parser.add_argument(
        "--force", action="store_true",
        help="replace a conflicting image/recipe pair",
    )
    assemble_parser.set_defaults(func=command_assemble)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command in ("info", "split"):
            if args.layout_file and not args.layout:
                raise FirmwareError("--layout-file requires --layout")
            if args.file_offset and args.flash_address is None:
                raise FirmwareError("--file-offset requires --flash-address")
        args.func(args)
    except (FirmwareError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0
