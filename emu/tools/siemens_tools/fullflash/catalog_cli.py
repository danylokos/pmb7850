from __future__ import annotations

import argparse
from pathlib import Path

from .catalog_info import command_catalog_info
from .corpus import command_corpus
from .patch_tagging import command_tag_patches
from .reconstruct import command_reconstruct


def add_catalog_parser(commands: argparse._SubParsersAction) -> None:
    parser = commands.add_parser(
        "catalog",
        help="build, inspect, tag, and reconstruct community fullflash catalogs",
    )
    catalog_commands = parser.add_subparsers(
        dest="catalog_command", required=True
    )

    build_parser = catalog_commands.add_parser(
        "build",
        help="catalog complete and partial fullflashes plus validated standalone regions",
        description=(
            "Catalog regions using primary fullflash LG except for LangPack and "
            "T9, which use the embedded or paired LangPack LG when valid."
        ),
    )
    build_parser.add_argument(
        "inputs", type=Path, nargs="+",
        help="raw files or recursively searched directories",
    )
    build_parser.add_argument("--layout-file", type=Path)
    build_parser.add_argument("--layout", required=True)
    build_parser.add_argument(
        "--official-catalog", type=Path, required=True,
        help="final official corpus root supplying evidence and payload backing",
    )
    build_parser.add_argument(
        "--patch-archive", type=Path,
        help="tag unmatched payloads from a VKP archive before minimization",
    )
    build_parser.add_argument(
        "--collection-manifest", type=Path,
        help="FULLFLASH_COLLECTION.json provenance source (auto-detected by default)",
    )
    build_parser.add_argument(
        "--materialize", "--write-splits", dest="materialize",
        action="store_true",
        help="materialize canonical region payloads and fullflash recipes",
    )
    build_parser.add_argument(
        "--catalog", type=Path, required=True,
        help="community corpus root receiving catalog.json",
    )
    build_parser.add_argument(
        "--report-output", type=Path,
        help="generated Markdown report output",
    )
    build_parser.add_argument(
        "--force", action="store_true",
        help="replace stale files owned by an existing schema-14 catalog",
    )
    build_parser.set_defaults(func=command_corpus)

    info_parser = catalog_commands.add_parser(
        "info", help="show stored patch analysis for a catalog artifact or payload"
    )
    info_parser.add_argument(
        "--catalog", type=Path, required=True,
        help="community corpus root containing catalog.json",
    )
    info_parser.add_argument("identifier")
    info_parser.add_argument(
        "--json", action="store_true", help="emit machine-readable JSON"
    )
    info_parser.set_defaults(func=command_catalog_info)

    tag_parser = catalog_commands.add_parser(
        "tag-patches",
        help="identify curated patches and plan compact payload names",
    )
    tag_parser.add_argument("--model", required=True)
    tag_parser.add_argument(
        "--catalog", type=Path, required=True,
        help="community corpus root containing catalog.json",
    )
    tag_parser.add_argument(
        "--official-catalog", type=Path, required=True,
        help="official corpus root containing catalog.json",
    )
    tag_parser.add_argument("--patch-archive", type=Path, required=True)
    tag_parser.add_argument(
        "--dry-run", action="store_true",
        help="validate and print the tagging plan without writing",
    )
    tag_parser.set_defaults(func=command_tag_patches)

    reconstruct_parser = catalog_commands.add_parser(
        "reconstruct",
        help="reconstruct and verify a fullflash from a catalog recipe",
    )
    reconstruct_parser.add_argument("recipe", type=Path)
    reconstruct_parser.add_argument("-o", "--output", type=Path, required=True)
    reconstruct_parser.add_argument(
        "--force", action="store_true", help="replace an existing output image"
    )
    reconstruct_parser.set_defaults(func=command_reconstruct)
