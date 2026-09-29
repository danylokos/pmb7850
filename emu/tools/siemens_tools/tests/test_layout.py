#!/usr/bin/env python3
"""Shared flash layout CLI and rendering tests."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from . import EMU_ROOT


class LayoutCliTests(unittest.TestCase):
    def run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-m", "tools.siemens_tools", "layout", *args],
            cwd=EMU_ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def test_lowercase_model_renders_aligned_text(self) -> None:
        completed = self.run_cli("m55")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, "")
        self.assertTrue(completed.stdout.startswith("M55/M56 flash layout\n\n"))
        self.assertIn(
            "Region       Start     End                Size  File Start  File End",
            completed.stdout,
        )
        self.assertIn(
            "FullFlash   000000  FFFFFF     1000000 (16 MB)",
            completed.stdout,
        )
        self.assertIn(
            "UNKNOWN_10  FF0000  FFFFFF      010000 (64 KB)",
            completed.stdout,
        )
        self.assertNotIn("|", completed.stdout)
        self.assertNotIn("`", completed.stdout)

    def test_all_markdown_matches_generated_reference(self) -> None:
        completed = self.run_cli("--all", "--format", "markdown")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(completed.stdout.startswith("# Siemens Phone Flash Layouts"))
        for heading in ("## C55/C56/CT56", "## M55/M56", "## S55/S56/S57"):
            self.assertIn(heading, completed.stdout)
        self.assertIn("|", completed.stdout)
        self.assertIn("EEPROM", completed.stdout)
        self.assertIn("UNKNOWN_1", completed.stdout)
        self.assertEqual(completed.stdout, self.run_cli("--all", "--format", "markdown").stdout)

    def test_single_markdown_and_output_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "m55.md"
            completed = self.run_cli(
                "M55", "--format", "markdown", "--output", str(output)
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "")
            rendered = output.read_text(encoding="ascii")
        self.assertTrue(rendered.startswith("## M55/M56\n\n"))
        self.assertNotIn("# Siemens Phone Flash Layouts", rendered)
        self.assertNotIn("## S55/S56/S57", rendered)

    def test_custom_catalog_is_resolved_case_insensitively(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            catalog = Path(directory) / "layouts.yaml"
            catalog.write_text(
                """phones:
  - name: TestPhone/TP
    base: 0x100000
    length: 0x20000
    regions:
      - {name: DATA, offset: 0, length: 0x10000}
""",
                encoding="ascii",
            )
            completed = self.run_cli(
                "tp", "--layout-file", str(catalog)
            )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("TestPhone/TP flash layout", completed.stdout)
        self.assertIn("DATA       100000  10FFFF", completed.stdout)
        self.assertIn("UNKNOWN_1  110000  11FFFF", completed.stdout)

    def test_selection_and_unknown_model_fail_cleanly(self) -> None:
        missing = self.run_cli()
        self.assertEqual(missing.returncode, 2)
        both = self.run_cli("M55", "--all")
        self.assertEqual(both.returncode, 2)
        unknown = self.run_cli("NOT-A-PHONE")
        self.assertEqual(unknown.returncode, 1)
        self.assertIn("not found exactly once", unknown.stderr)


if __name__ == "__main__":
    unittest.main()
