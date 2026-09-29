#!/usr/bin/env python3
"""Direct CEMU Parquet capture and query tests."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

EMU_ROOT = Path(__file__).resolve().parents[3]
REPO_ROOT = EMU_ROOT.parent
ROOT = EMU_ROOT
sys.path.insert(0, str(EMU_ROOT))

from tools.cemu_trace import open_parquet

WRITER = EMU_ROOT / "build/tests/trace_fixture"
PYTHON = REPO_ROOT / ".venv/bin/python"


class CemuTraceParquetTests(unittest.TestCase):
    def fixture(self, directory: Path, mode: str = "--fixture") -> Path:
        path = directory / "trace.parquet"
        subprocess.run([str(WRITER), mode, str(path)], check=True)
        return path

    def test_partitions_schema_order_types_and_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            parquet = self.fixture(root)
            manifest = json.loads((parquet / "manifest.json").read_text())
            self.assertEqual(parquet, root / "trace.parquet")
            self.assertEqual(manifest["format"], "cemu-trace-parquet-v1")
            self.assertEqual(manifest["events"], 3)
            self.assertEqual(
                {item["kind"]: item["rows"] for item in manifest["partitions"]},
                {"lcd_select": 1, "xbus_access": 2},
            )
            self.assertTrue((parquet / "kind=xbus_access/part-00000.parquet").is_file())
            self.assertTrue((parquet / "kind=lcd_select/part-00000.parquet").is_file())

            con = open_parquet(parquet)
            try:
                rows = con.execute(
                    "SELECT seq, icount, kind, info_access_str, "
                    "info_xbus_start_i64, info_xbus_start_type, "
                    "info_selected_bool FROM trace ORDER BY seq"
                ).fetchall()
                self.assertEqual(rows, [
                    (0, 10, "xbus_access", "write", None, "null", None),
                    (1, 10, "lcd_select", None, None, None, True),
                    (2, 11, "xbus_access", "read", 0xE000, "i64", None),
                ])
                columns = {
                    row[1]: row[2]
                    for row in con.execute("PRAGMA table_info('trace')").fetchall()
                }
                self.assertEqual(columns["seq"], "BIGINT")
                self.assertEqual(columns["info_selected_bool"], "BOOLEAN")
                self.assertEqual(columns["info_access_str"], "VARCHAR")
            finally:
                con.close()

    def test_empty_trace_creates_queryable_empty_views(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            parquet = self.fixture(Path(tmp), "--empty-fixture")
            con = open_parquet(parquet / "manifest.json")
            try:
                self.assertEqual(con.execute("SELECT count(*) FROM trace").fetchone()[0], 0)
                self.assertEqual(
                    [row[1] for row in con.execute("PRAGMA table_info('trace')").fetchall()],
                    ["seq", "icount", "pc", "addr", "size", "value", "detail", "kind"],
                )
                for view in (
                    "xbus_accesses",
                    "xbus_registers",
                    "xbus_transactions",
                    "xbus_effects",
                ):
                    self.assertEqual(
                        con.execute(f"SELECT count(*) FROM {view}").fetchone()[0], 0
                    )
            finally:
                con.close()

    def test_multiple_batches_preserve_rows_and_row_groups(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            parquet = self.fixture(Path(tmp), "--many-fixture")
            con = open_parquet(parquet)
            try:
                self.assertEqual(con.execute("SELECT count(*) FROM trace").fetchone()[0], 65537)
                self.assertEqual(
                    con.execute(
                        "SELECT min(seq), max(seq), count(DISTINCT seq) FROM trace"
                    ).fetchone(),
                    (0, 65536, 65537),
                )
                metadata = con.execute(
                    "SELECT row_group_id, row_group_num_rows, compression "
                    "FROM parquet_metadata(?) WHERE path_in_schema = 'seq' "
                    "ORDER BY row_group_id",
                    [str(parquet / "kind=exec/part-00000.parquet")],
                ).fetchall()
                self.assertEqual(metadata, [(0, 65536, "ZSTD"), (1, 1, "ZSTD")])
            finally:
                con.close()

    def test_every_declared_kind_field_and_type_serializes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            trace = self.fixture(Path(tmp), "--catalog-fixture")
            completed = subprocess.run(
                [str(PYTHON), "-m", "tools.cemu_trace", "verify", str(trace)],
                cwd=ROOT, text=True, capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            manifest = json.loads((trace / "manifest.json").read_text())
            self.assertEqual(manifest["events"], 81)
            self.assertEqual(len(manifest["partitions"]), 70)

            con = open_parquet(trace)
            try:
                self.assertEqual(
                    con.execute(
                        "SELECT info_command_i64, info_parameter_index_i64 "
                        "FROM trace WHERE kind='lcd_data'"
                    ).fetchall(),
                    [(42, 42)],
                )
                self.assertEqual(
                    con.execute(
                        "SELECT info_owner_str, info_changed_bool, info_result_i64 "
                        "FROM trace WHERE kind='input_owner'"
                    ).fetchall(),
                    [("fixture", True, 42)],
                )
                self.assertEqual(
                    con.execute(
                        "SELECT info_command_i64, info_payload_hex_str "
                        "FROM trace WHERE kind='xbus_audio_packet'"
                    ).fetchall(),
                    [(42, "fixture")],
                )
                self.assertEqual(
                    con.execute(
                        "SELECT count(*) FROM trace "
                        "WHERE kind='xbus_access' "
                        "AND info_xbus_start_type='null'"
                    ).fetchone()[0],
                    1,
                )
                self.assertEqual(
                    con.execute(
                        "SELECT info_chip_index_i64, info_chip_offset_i64, "
                        "info_expected_str, info_patch_str, info_provenance_str, "
                        "info_replacement_str, info_status_str FROM trace "
                        "WHERE kind='firmware_patch'"
                    ).fetchall(),
                    [(42, 42, "fixture", "fixture", "fixture", "fixture", "fixture")],
                )
            finally:
                con.close()

    def test_cli_sql_info_verify_and_alignment(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            trace = self.fixture(Path(tmp))
            for args in (["info", str(trace)], ["verify", str(trace)]):
                completed = subprocess.run(
                    [str(PYTHON), "-m", "tools.cemu_trace", *args],
                    cwd=ROOT, text=True, capture_output=True,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)

            completed = subprocess.run(
                [
                    str(PYTHON), "-m", "tools.cemu_trace", "sql", "--align",
                    str(trace),
                    "SELECT 'x' AS label, 1 AS value "
                    "UNION ALL SELECT 'long', NULL "
                    "UNION ALL SELECT 'mid', 12345",
                ],
                cwd=ROOT, text=True, capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout.splitlines(), [
                "label\tvalue",
                "    x\t    1",
                " long\t NULL",
                "  mid\t12345",
            ])

            completed = subprocess.run(
                [
                    str(PYTHON), "-m", "tools.cemu_trace", "sql", "--align",
                    str(trace),
                    "SELECT i AS n FROM range(1025) rows(i) ORDER BY i",
                ],
                cwd=ROOT, text=True, capture_output=True,
            )
            lines = completed.stdout.splitlines()
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual((len(lines), lines[0], lines[1], lines[-1]),
                             (1026, "   n", "   0", "1024"))

            completed = subprocess.run(
                [
                    str(PYTHON), "-m", "tools.cemu_trace", "sql", str(trace),
                    "SELECT 'x' AS label, 1 AS value "
                    "UNION ALL SELECT 'long', 12345",
                ],
                cwd=ROOT, text=True, capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout.splitlines(), [
                "label\tvalue",
                "x\t1",
                "long\t12345",
            ])

    def test_verify_rejects_incomplete_or_extra_dataset(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            trace = self.fixture(Path(tmp))
            extra = trace / "unexpected.txt"
            extra.write_text("unexpected")
            completed = subprocess.run(
                [str(PYTHON), "-m", "tools.cemu_trace", "verify", str(trace)],
                cwd=ROOT, text=True, capture_output=True,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("dataset files differ", completed.stderr)
            extra.unlink()

            manifest_path = trace / "manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["finalized"] = False
            manifest_path.write_text(json.dumps(manifest))
            completed = subprocess.run(
                [str(PYTHON), "-m", "tools.cemu_trace", "verify", str(trace)],
                cwd=ROOT, text=True, capture_output=True,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("invalid finalized", completed.stderr)


if __name__ == "__main__":
    unittest.main()
