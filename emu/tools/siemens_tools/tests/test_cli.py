#!/usr/bin/env python3
"""Unified Siemens tooling CLI tests."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from . import EMU_ROOT
from tools.siemens_tools.fullflash.cli import build_parser as fullflash_parser
from tools.siemens_tools.firmware.cli import build_parser as firmware_parser


class UnifiedCliTests(unittest.TestCase):
    def run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-m", "tools.siemens_tools", *args],
            cwd=EMU_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def test_top_level_help_lists_domains(self) -> None:
        completed = self.run_cli("--help")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("{firmware,fullflash,layout,eeprom,bitmap}", completed.stdout)

    def test_each_domain_owns_its_help(self) -> None:
        for domain, expected in (
            ("firmware", "convert"),
            ("fullflash", "assemble"),
            ("layout", "--format"),
            ("eeprom", "generate"),
            ("bitmap", "--table-address"),
        ):
            with self.subTest(domain=domain):
                completed = self.run_cli(domain, "--help")
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertIn(expected, completed.stdout)
                self.assertIn(f"tools.siemens_tools {domain}", completed.stdout)

    def test_catalog_groups_own_nested_help(self) -> None:
        for domain, expected in (
            ("fullflash", "tag-patches"),
            ("firmware", "build"),
        ):
            with self.subTest(domain=domain):
                completed = self.run_cli(domain, "catalog", "--help")
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertIn(expected, completed.stdout)

    def test_removed_fullflash_catalog_paths_are_unknown(self) -> None:
        for command in (
            "corpus", "official-corpus", "tag-patches", "reconstruct",
        ):
            with self.subTest(command=command):
                completed = self.run_cli("fullflash", command)
                self.assertEqual(completed.returncode, 2)
                self.assertIn("invalid choice", completed.stderr)
        completed = self.run_cli("fullflash", "catalog", "minimize")
        self.assertEqual(completed.returncode, 2)
        self.assertIn("invalid choice", completed.stderr)

    def test_removed_fullflash_layout_docs_is_unknown(self) -> None:
        completed = self.run_cli("fullflash", "layout-docs")
        self.assertEqual(completed.returncode, 2)
        self.assertIn("invalid choice", completed.stderr)


    def test_removed_catalog_category_options_are_unknown(self) -> None:
        cases = (
            (
                "fullflash", "catalog", "build", "input.bin", "--layout",
                "C55", "--catalog", "community", "--official-catalog",
                "official", "--firmware-catalog", "official",
            ),
            (
                "firmware", "catalog", "build", "input.xbi", "--layout",
                "C55", "--catalog", "official", "--fullflash-catalog",
                "community",
            ),
            (
                "fullflash", "catalog", "build", "input.bin", "--layout",
                "C55", "--catalog", "community", "--official-catalog",
                "official", "--official-root", "official-packages",
            ),
            (
                "firmware", "catalog", "build", "input.xbi", "--layout",
                "C55", "--catalog", "official", "--community-catalog",
                "community",
            ),
        )
        for arguments in cases:
            with self.subTest(arguments=arguments):
                completed = self.run_cli(*arguments)
                self.assertEqual(completed.returncode, 2)
                self.assertIn("unrecognized arguments", completed.stderr)

    def test_catalog_options_are_corpus_roots(self) -> None:
        fullflash = fullflash_parser().parse_args([
            "catalog", "build", "input.bin", "--layout", "C55",
            "--catalog", "community", "--official-catalog", "official",
        ])
        self.assertEqual(fullflash.catalog, Path("community"))
        self.assertEqual(fullflash.official_catalog, Path("official"))
        firmware = firmware_parser().parse_args([
            "catalog", "build", "input.xbi", "--layout", "C55",
            "--catalog", "official",
        ])
        self.assertEqual(firmware.catalog, Path("official"))
        self.assertFalse(hasattr(firmware, "peer_catalogs"))
        self.assertEqual(fullflash.patch_archive, None)

        tagged = fullflash_parser().parse_args([
            "catalog", "build", "input.bin", "--layout", "C55",
            "--catalog", "community", "--official-catalog", "official",
            "--patch-archive", "patches.zip",
        ])
        self.assertEqual(tagged.patch_archive, Path("patches.zip"))

    def test_catalog_info_json_is_publicly_routed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fullflash_root = root / "fullflash"
            fullflash_root.mkdir()
            fullflash_catalog = fullflash_root / "catalog.json"
            fullflash_catalog.write_text(json.dumps({
                "schema": "siemens-community-fullflash-corpus",
                "schema_version": 14,
                "artifacts": [{
                    "id": "fullflash-1",
                    "kind": "complete-fullflash",
                    "path": "fixture.bin",
                    "sha256": "a" * 64,
                    "recipe": {"path": "fullflashes/fixture.json"},
                    "metadata": {"patch_analysis": {
                        "identified_patches": [],
                        "unidentified_regions": [],
                    }},
                }],
                "occurrences": [],
                "regions": [],
                "summary": {
                    "official_backed_variants": 0,
                    "official_backed_paths": 0,
                    "official_backed_bytes": 0,
                },
            }))
            firmware_root = root / "firmware"
            firmware_root.mkdir()
            firmware_catalog = firmware_root / "catalog.json"
            firmware_catalog.write_text(json.dumps({
                "schema": "siemens-official-corpus",
                "schema_version": 9,
                "packages": [{
                    "sha256": "b" * 64,
                    "recipe": {"path": "packages/fixture.json"},
                    "sources": [],
                    "regions": [],
                }],
                "regions": [],
            }))

            for domain, catalog_root, digest, schema in (
                ("fullflash", fullflash_root, "a" * 12,
                 "siemens-community-catalog-info"),
                ("firmware", firmware_root, "b" * 12,
                 "siemens-firmware-catalog-info"),
            ):
                with self.subTest(domain=domain):
                    completed = self.run_cli(
                        domain, "catalog", "info", "--catalog", str(catalog_root), digest,
                        "--json",
                    )
                    self.assertEqual(
                        completed.returncode, 0, completed.stderr
                    )
                    self.assertEqual(json.loads(completed.stdout)["schema"], schema)
                    legacy = self.run_cli(
                        domain, "catalog", "info", str(catalog_root), digest, "--json",
                    )
                    self.assertEqual(legacy.returncode, 2)



if __name__ == "__main__":
    unittest.main()
