#!/usr/bin/env python3
"""Tests for the SQL-backed XBUS trace views."""
from __future__ import annotations

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
from tools.cemu_trace.xbus import create_xbus_views

WRITER = EMU_ROOT / "build/tests/trace_fixture"
PYTHON = REPO_ROOT / ".venv/bin/python"


class XbusTraceTests(unittest.TestCase):
    def fixture(self, directory: Path) -> Path:
        path = directory / "trace.parquet"
        subprocess.run([str(WRITER), "--xbus-fixture", str(path)], check=True)
        return path

    def test_views_reconstruct_accesses_registers_transactions_and_effects(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            trace = self.fixture(Path(tmp))
            con = open_parquet(trace)
            try:
                self.assertEqual(
                    con.execute(
                        "SELECT seq, access, addr, value "
                        "FROM xbus_accesses ORDER BY seq"
                    ).fetchall(),
                    [
                        (0, "write", 0xEC14, 2),
                        (1, "write", 0xEC12, 1),
                        (3, "read", 0xEC12, 2),
                        (4, "read", 0xEC16, 16),
                        (5, "write", 0xEC12, 0),
                        (7, "write", 0xEC12, 1),
                    ],
                )
                self.assertEqual(
                    con.execute(
                        "SELECT addr, reads, writes, read_values, write_values "
                        "FROM xbus_registers ORDER BY addr"
                    ).fetchall(),
                    [
                        (0xEC12, 1, 3, [2], [0, 1]),
                        (0xEC14, 0, 1, None, [2]),
                        (0xEC16, 1, 0, [16], None),
                    ],
                )
                self.assertEqual(
                    con.execute(
                        "SELECT transaction, transaction_id, doorbell_seq, "
                        "next_doorbell_seq, ctrl, first_completion_seq "
                        "FROM xbus_transactions ORDER BY transaction"
                    ).fetchall(),
                    [(1, 7, 1, 7, 2, 2), (2, 8, 7, None, 2, 8)],
                )
                self.assertEqual(
                    con.execute(
                        "SELECT transaction_id, seq, effect, phase, "
                        "irq_asserted, result FROM xbus_effects ORDER BY seq"
                    ).fetchall(),
                    [
                        (7, 2, "completion", "sync-ready", False, 16),
                        (7, 3, "status_poll", None, None, None),
                        (7, 4, "result_read", None, None, None),
                        (7, 5, "status_clear", None, None, None),
                        (8, 8, "completion", "irq-promoted", True, 16),
                    ],
                )
                self.assertEqual(
                    con.execute(
                        "SELECT addr, value FROM trace "
                        "WHERE kind='mem_write' "
                        "AND addr BETWEEN 14336 AND 14591"
                    ).fetchall(),
                    [(0x3828, 0x1234)],
                )
            finally:
                con.close()

    def test_null_id_completion_uses_nearest_bounded_doorbell(self) -> None:
        import duckdb

        con = duckdb.connect()
        try:
            con.execute("""
                CREATE VIEW trace AS
                SELECT * FROM (VALUES
                    (0::BIGINT, 10::BIGINT, 1::INTEGER, 60434::INTEGER,
                     2::INTEGER, 1::BIGINT, ''::VARCHAR, 'xbus_access'::VARCHAR,
                     'write'::VARCHAR, NULL::BIGINT, NULL::VARCHAR),
                    (1::BIGINT, 11::BIGINT, 1::INTEGER, 60434::INTEGER,
                     2::INTEGER, 1::BIGINT, ''::VARCHAR, 'xbus_access'::VARCHAR,
                     'write'::VARCHAR, NULL::BIGINT, NULL::VARCHAR),
                    (2::BIGINT, 12::BIGINT, 1::INTEGER, 60434::INTEGER,
                     2::INTEGER, 2::BIGINT, ''::VARCHAR,
                     'xbus_mailbox_complete'::VARCHAR, NULL::VARCHAR,
                     NULL::BIGINT, 'legacy'::VARCHAR)
                ) rows(seq, icount, pc, addr, size, value, detail, kind,
                       info_access_str, info_transaction_id_i64, info_phase_str)
            """)
            create_xbus_views(con)
            self.assertEqual(
                con.execute(
                    "SELECT transaction, transaction_id, doorbell_seq, "
                    "first_completion_seq FROM xbus_transactions "
                    "ORDER BY transaction"
                ).fetchall(),
                [(1, None, 0, None), (2, None, 1, 2)],
            )
            self.assertEqual(
                con.execute(
                    "SELECT transaction, transaction_id, seq, phase "
                    "FROM xbus_effects"
                ).fetchall(),
                [(2, None, 2, "legacy")],
            )
        finally:
            con.close()

    def test_xbus_cli_exposes_each_view(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            trace = self.fixture(Path(tmp))
            for view, header in (
                ("accesses", "seq"),
                ("registers", "window_name"),
                ("transactions", "transaction"),
                ("effects", "transaction"),
            ):
                completed = subprocess.run(
                    [
                        str(PYTHON), "-m", "tools.cemu_trace",
                        "xbus", str(trace), view,
                    ],
                    cwd=ROOT, text=True, capture_output=True,
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertEqual(completed.stdout.splitlines()[0].split("\t")[0].strip(),
                                 header)

            completed = subprocess.run(
                [
                    str(PYTHON), "-m", "tools.cemu_trace",
                    "xbus", str(trace), "transactions",
                ],
                cwd=ROOT, text=True, capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(completed.stdout.splitlines()[1].startswith("1\t7\t"))
            completed = subprocess.run(
                [
                    str(PYTHON), "-m", "tools.cemu_trace",
                    "xbus", "--align", str(trace), "transactions",
                ],
                cwd=ROOT, text=True, capture_output=True,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(completed.stdout.splitlines()[1].startswith("          1\t"))


if __name__ == "__main__":
    unittest.main()
