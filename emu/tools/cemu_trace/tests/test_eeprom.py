#!/usr/bin/env python3
"""Logical EEPROM trace analyzer and CLI tests."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

EMU_ROOT = Path(__file__).resolve().parents[3]
REPO_ROOT = EMU_ROOT.parent
sys.path.insert(0, str(EMU_ROOT))

from tools.cemu_trace.eeprom import EepromReportError, analyze_trace, create_report

WRITER = EMU_ROOT / "build/tests/trace_fixture"
PYTHON = REPO_ROOT / ".venv/bin/python"


class EepromTraceReportTests(unittest.TestCase):
    def fixture(self, directory: Path, mode: str = "--eeprom-fixture") -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "trace.parquet"
        subprocess.run([str(WRITER), mode, str(path)], check=True)
        return path

    @staticmethod
    def keyed_blocks(report: dict) -> dict[tuple[int, int], dict]:
        return {
            (block["chip"]["index"], block["block_id"]): block
            for block in report["blocks"]
        }

    def test_activity_coverage_pcs_timing_and_ordering(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = analyze_trace(self.fixture(Path(tmp)))
        blocks = self.keyed_blocks(report)
        self.assertEqual(list(blocks), [
            (0, 0x10), (0, 0x20), (0, 0x30), (0, 0x40),
            (0, 0x70), (0, 0x80), (0, 0x90), (1, 0x10),
        ])
        self.assertEqual(
            {key: block["ordering"] for key, block in blocks.items()},
            {
                (0, 0x10): "read-before-write",
                (0, 0x20): "write-before-read",
                (0, 0x30): "read-only",
                (0, 0x40): "write-only",
                (0, 0x70): "read-only",
                (0, 0x80): "write-only",
                (0, 0x90): "write-before-read",
                (1, 0x10): "read-only",
            },
        )

        block = blocks[(0, 0x10)]
        self.assertEqual(
            block["activity"]["resolved"],
            {
                "counts": {"read": 2, "program": 1, "erase": 0},
                "transferred_bytes": {
                    "read": 8, "program": 2, "erase": 0, "total": 10,
                },
                "coverage_space": "block-payload-offset",
                "coverage": [[0, 7]],
            },
        )
        self.assertEqual(block["pcs"], {
            "read": [0x900010, 0x900011], "write": [0x900012],
        })
        self.assertEqual(block["timing"], {
            "first": {"seq": 10, "icount": 200, "tick": 30},
            "last": {"seq": 12, "icount": 202, "tick": 32},
        })
        self.assertEqual(
            blocks[(0, 0x30)]["activity"]["resolved"]["coverage"],
            [[8, 10], [12, 14]],
        )
        repeated = blocks[(0, 0x40)]["activity"]["resolved"]
        self.assertEqual(repeated["counts"], {"read": 0, "program": 2, "erase": 1})
        self.assertEqual(repeated["transferred_bytes"]["total"], 12)
        self.assertEqual(repeated["coverage"], [[4, 8]])
        self.assertEqual(
            blocks[(0, 0x70)]["activity"]["shadowed"]["counts"]["read"], 1
        )

    def test_post_hoc_splitting_mapping_history_and_unresolved_journal(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            blocks = self.keyed_blocks(analyze_trace(self.fixture(Path(tmp)), show_all=True))
            report = analyze_trace(Path(tmp) / "trace.parquet", show_all=True)

        added = blocks[(0, 0x80)]
        self.assertEqual(added["mappings"]["generations"], [0, 1])
        self.assertEqual(added["mappings"]["observed_lengths"], [8])
        self.assertEqual(added["mappings"]["changes"][0]["change"], "added")
        self.assertEqual(
            added["activity"]["post_hoc"]["descriptor"]["coverage"], [[0, 12]]
        )
        self.assertEqual(
            added["activity"]["post_hoc"]["payload"]["coverage"], [[0, 8]]
        )
        self.assertEqual(added["timing"]["first"]["seq"], 24)
        self.assertLess(
            added["timing"]["first"]["seq"],
            added["mappings"]["changes"][0]["seq"],
        )

        remapped = blocks[(0, 0x90)]
        self.assertEqual(remapped["mappings"]["observed_lengths"], [8, 20])
        self.assertEqual([item["change"] for item in remapped["mappings"]["changes"]],
                         ["initial", "remapped"])
        self.assertEqual(remapped["mappings"]["final"]["length"], 20)
        self.assertEqual(blocks[(0, 0xA0)]["mappings"]["final"], None)
        self.assertEqual(blocks[(0, 0xA0)]["ordering"], "unobserved")

        non_block = report["non_block_activity"][0]["activity"]
        self.assertEqual(non_block["unattributed-journal"]["counts"]["program"], 2)
        self.assertEqual(non_block["unattributed-journal"]["transferred_bytes"]["total"], 296)
        self.assertEqual(non_block["unresolved-journal"]["counts"]["program"], 2)
        self.assertEqual(non_block["unresolved-journal"]["transferred_bytes"]["total"], 256)
        self.assertEqual(non_block["unresolved-journal"]["coverage"], [
            [0x10C, 0x200], [0x208, 0x210], [0x40C, 0x410],
        ])

    def test_default_all_empty_and_multiple_trace_order(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = self.fixture(root / "first")
            empty = self.fixture(root / "empty", "--empty-fixture")
            map_only = self.fixture(root / "map", "--eeprom-map-only-fixture")
            default = analyze_trace(first)
            complete = analyze_trace(first, show_all=True)
            self.assertEqual(len(default["blocks"]), 8)
            self.assertEqual(len(complete["blocks"]), 11)
            blocks = self.keyed_blocks(complete)
            self.assertEqual(blocks[(0, 0x50)]["ordering"], "unobserved")
            self.assertEqual(
                blocks[(0, 0x60)]["activity"]["metadata"]["counts"]["read"], 1
            )
            self.assertEqual(analyze_trace(empty), {
                "path": str(empty), "blocks": [], "non_block_activity": [],
            })
            self.assertEqual(create_report([empty]), {
                "format": "cemu-eeprom-report-v1",
                "traces": [{
                    "path": str(empty), "blocks": [], "non_block_activity": [],
                }],
            })
            self.assertEqual(analyze_trace(map_only)["blocks"], [])
            map_blocks = analyze_trace(map_only, show_all=True)["blocks"]
            self.assertEqual(
                [(block["block_id"], block["ordering"]) for block in map_blocks],
                [(0x55, "unobserved")],
            )
            envelope = create_report([empty, first])
            self.assertEqual(envelope["format"], "cemu-eeprom-report-v1")
            self.assertEqual(
                [trace["path"] for trace in envelope["traces"]],
                [str(empty), str(first)],
            )

    def test_cli_json_structure_and_deterministic_text(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            trace = self.fixture(Path(tmp))
            command = [
                str(PYTHON), "-m", "tools.cemu_trace", "eeprom", str(trace),
            ]
            first = subprocess.run(
                command, cwd=EMU_ROOT, text=True, capture_output=True, check=True,
            )
            second = subprocess.run(
                command, cwd=EMU_ROOT, text=True, capture_output=True, check=True,
            )
            self.assertEqual(first.stdout, second.stdout)
            self.assertIn("block 0x0010 (16)", first.stdout)
            self.assertIn("post-hoc payload", first.stdout)
            self.assertIn("unresolved-journal", first.stdout)
            self.assertNotIn("block 0x0050 (80)", first.stdout)

            completed = subprocess.run(
                [*command[:-1], "--all", "--json", str(trace)],
                cwd=EMU_ROOT, text=True, capture_output=True, check=True,
            )
            document = json.loads(completed.stdout)
            self.assertEqual(list(document), ["format", "traces"])
            self.assertEqual(list(document["traces"][0]), [
                "path", "blocks", "non_block_activity",
            ])
            self.assertEqual(list(document["traces"][0]["blocks"][0]), [
                "chip", "block_id", "mappings", "activity", "pcs", "timing", "ordering",
            ])
            self.assertEqual(document["format"], "cemu-eeprom-report-v1")
            self.assertEqual(len(document["traces"][0]["blocks"]), 11)

    def test_rejects_access_only_and_malformed_eeprom_schema(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            access_only = self.fixture(root / "access", "--eeprom-access-only-fixture")
            with self.assertRaisesRegex(EepromReportError, "requires eeprom_map"):
                analyze_trace(access_only)

            trace = self.fixture(root / "malformed")
            manifest_path = trace / "manifest.json"
            manifest = json.loads(manifest_path.read_text())
            partition = next(
                item for item in manifest["partitions"] if item["kind"] == "eeprom_map"
            )
            partition["columns"] = [
                column for column in partition["columns"]
                if column["name"] != "info_current_length_i64"
            ]
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(EepromReportError, "malformed eeprom_map schema"):
                analyze_trace(trace)

            completed = subprocess.run(
                [str(PYTHON), "-m", "tools.cemu_trace", "eeprom", str(access_only)],
                cwd=EMU_ROOT, text=True, capture_output=True,
            )
            self.assertEqual(completed.returncode, 1)
            self.assertIn("requires eeprom_map", completed.stderr)


if __name__ == "__main__":
    unittest.main()
