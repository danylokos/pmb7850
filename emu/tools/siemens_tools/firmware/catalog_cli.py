from __future__ import annotations

import argparse
from pathlib import Path

from .catalog_info import command_catalog_info
from ..fullflash.official_corpus import command_official_corpus


def add_catalog_parser(commands: argparse._SubParsersAction) -> None:
    parser = commands.add_parser(
        "catalog",
        help="build and inspect official firmware package catalogs",
    )
    catalog_commands = parser.add_subparsers(
        dest="catalog_command", required=True
    )

    build_parser = catalog_commands.add_parser(
        "build",
        help="catalog raw and executable official firmware packages",
        description=(
            "Catalog packages using materialized firmware LG, then declared LG; "
            "LangPack and T9 use embedded LangPack pairing when valid."
        ),
    )
    build_parser.add_argument(
        "inputs", type=Path, nargs="+",
        help="package files or recursively searched directories",
    )
    build_parser.add_argument("--layout-file", type=Path)
    build_parser.add_argument("--layout", required=True)
    build_parser.add_argument(
        "--materialize", action="store_true",
        help="materialize canonical payloads, fragments, and package recipes",
    )
    build_parser.add_argument(
        "--catalog", type=Path, required=True,
        help="firmware corpus root receiving catalog.json",
    )
    build_parser.add_argument("--report-output", type=Path)
    build_parser.add_argument(
        "--force", action="store_true",
        help="replace stale files owned by an existing schema-9 catalog",
    )
    build_parser.set_defaults(func=command_official_corpus)

    info_parser = catalog_commands.add_parser(
        "info", help="inspect a package or payload recorded in a firmware catalog"
    )
    info_parser.add_argument(
        "--catalog", type=Path, required=True,
        help="firmware corpus root containing catalog.json",
    )
    info_parser.add_argument("identifier")
    info_parser.add_argument(
        "--json", action="store_true", help="emit machine-readable JSON"
    )
    info_parser.set_defaults(func=command_catalog_info)
