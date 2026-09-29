#!/usr/bin/env python3
"""Black-box direct Parquet trace artifact tests."""
from __future__ import annotations

import json
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from firmware_fixture import seed_metadata_file

ROOT = Path(__file__).resolve().parents[2]
CEMU = ROOT / "bin/cemu_inst"


def make_flash(path: Path) -> None:
    with path.open("wb") as stream:
        stream.write(bytes.fromhex("FA 80 04 00"))  # jmps 0x80,0x0004
        stream.write(b"\x0d\xff")                  # jmpr cc_UC, self
        stream.seek(8 * 1024 * 1024 - 1)
        stream.write(b"\xff")
    seed_metadata_file(path)


def run(cwd: Path, flash: Path, *args: str, env: dict[str, str] | None = None):
    return subprocess.run(
        [str(CEMU), str(flash), "--device", "c55", *args],
        cwd=cwd, env=env, text=True, capture_output=True,
    )


class TraceArtifactTests(unittest.TestCase):
    def test_labeled_benchmark_json_direct_capture(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cemu-trace-auto-") as tmp:
            cwd = Path(tmp)
            flash = cwd / "spin.bin"
            make_flash(flash)
            completed = run(
                cwd, flash, "--limit", "20", "--trace", "--label", "labeled",
                "--benchmark-json",
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout.count("\n"), 1)
            self.assertEqual(json.loads(completed.stdout)["status"], "limit")
            trace_dir = cwd / "shots/labeled/trace"
            self.assertTrue((trace_dir / "trace.parquet/manifest.json").is_file())

    def test_existing_dataset_is_replaced_atomically(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cemu-trace-replace-") as tmp:
            cwd = Path(tmp)
            flash = cwd / "spin.bin"
            make_flash(flash)
            event_counts = []
            for limit in (10, 20):
                completed = run(
                    cwd, flash, "--limit", str(limit), "--trace",
                    "--label", "replace", "--benchmark-json",
                )
                self.assertEqual(completed.returncode, 0, completed.stderr)
                manifest = json.loads(
                    (cwd / "shots/replace/trace/trace.parquet/manifest.json").read_text()
                )
                event_counts.append(manifest["events"])
            self.assertGreater(event_counts[1], event_counts[0])
            trace_dir = cwd / "shots/replace/trace"
            self.assertEqual(list(trace_dir.glob("trace.parquet.partial-*")), [])
            self.assertEqual(list(trace_dir.glob("trace.parquet.old-*")), [])

    def test_auto_name_deferred_arm_and_debugger_capture(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cemu-trace-paths-") as tmp:
            cwd = Path(tmp)
            flash = cwd / "spin.bin"
            make_flash(flash)
            automatic = run(
                cwd, flash, "--limit", "12", "--trace", "--trace-from-icount", "5",
            )
            self.assertEqual(automatic.returncode, 0, automatic.stderr)
            outputs = list((cwd / "shots").glob("c55-icount12-*"))
            self.assertEqual(len(outputs), 1)
            self.assertTrue((outputs[0] / "trace/trace.parquet/manifest.json").is_file())

            debug = run(
                cwd, flash, "--trace", "--label", "debugger", "-c", "si 3",
            )
            self.assertEqual(debug.returncode, 0, debug.stderr)
            self.assertTrue(
                (cwd / "shots/debugger/trace/trace.parquet/manifest.json").is_file()
            )

    def test_interrupted_capture_is_finalized(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cemu-trace-signal-") as tmp:
            cwd = Path(tmp)
            flash = cwd / "spin.bin"
            make_flash(flash)
            trace = cwd / "shots/interrupted/trace/trace.parquet"
            process = subprocess.Popen(
                [str(CEMU), str(flash), "--device", "c55", "--limit", str(2**64 - 1),
                 "--trace", "--label", "interrupted"],
                cwd=cwd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            try:
                deadline = time.monotonic() + 5
                while not list(trace.parent.glob("trace.parquet.partial-*")) and process.poll() is None:
                    if time.monotonic() >= deadline:
                        self.fail("trace capture did not start")
                    time.sleep(0.01)
                time.sleep(0.05)
                process.send_signal(signal.SIGINT)
                stdout, stderr = process.communicate(timeout=20)
                self.assertEqual(process.returncode, 128 + signal.SIGINT, stderr)
                self.assertIn("status: interrupted", stdout)
                self.assertTrue((trace / "manifest.json").is_file())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    unittest.main()
