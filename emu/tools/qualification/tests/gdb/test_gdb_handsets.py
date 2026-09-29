"""Failure attribution and ownership controls for the firmware-dependent gate."""
import io
import json
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from tools.qualification.gdb import gdb_handsets as gate


class HandsetTests(unittest.TestCase):
    def test_stale_mi_events_in_both_queues(self):
        for parsed in (True, False):
            mi = gate.HandsetMI.__new__(gate.HandsetMI)
            mi.records, mi.lines, mi.log = [], queue.Queue(), io.StringIO()
            line = '*stopped,reason="signal-received"'
            (mi.records.append if parsed else mi.lines.put)(line)
            with self.assertRaisesRegex(AssertionError, 'unconsumed'):
                mi.no_pending_stop()

    def test_mi_exit_and_expired_session(self):
        mi = gate.HandsetMI.__new__(gate.HandsetMI)
        mi.records, mi.lines, mi.log, mi.proc = [], queue.Queue(), io.StringIO(), Mock()
        mi.lines.put(None)
        with self.assertRaises(EOFError):
            mi.no_pending_stop()
        mi.deadline = time.monotonic() - 1
        with self.assertRaises(TimeoutError):
            mi.command('-exec-continue')

    def observer(self):
        observer = gate.Observer.__new__(gate.Observer)
        observer.condition = threading.Condition()
        observer.state = {'sequence': 7}
        observer.deadline = time.monotonic() + 1
        observer.error = None
        observer.process = Mock()
        observer.process.poll.return_value = None
        observer.packets = [gate.wire.Packet(gate.wire.STATS, b'', 7, 0)]
        return observer

    def test_fresh_ui_rejects_stale_and_expired_packet(self):
        observer = self.observer()
        with self.assertRaises(TimeoutError):
            observer.fresh(gate.wire.STATS, timeout=0.01)
        self.assertEqual(observer.fresh(gate.wire.STATS, after=6).sequence, 7)
        observer.deadline = time.monotonic() - 1
        with self.assertRaises(TimeoutError):
            observer.fresh(gate.wire.STATS, after=6)

    def test_ui_exit_is_not_success_even_with_queued_packet(self):
        observer = self.observer()
        observer.process.poll.return_value = 0
        with self.assertRaises(EOFError):
            observer.fresh(gate.wire.STATS, after=6)

    def test_preflight_missing_binaries_images_and_overlay_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            args = SimpleNamespace(emu=root/'missing', gdb=Path(sys.executable), qemu=Path(sys.executable))
            with self.assertRaisesRegex(ValueError, 'Missing executable'):
                gate.preflight(args)
            args.emu = Path(sys.executable)
            with patch.object(gate, 'ROOT', root):
                with self.assertRaisesRegex(RuntimeError, 'image is missing'):
                    gate.preflight(args)
            overlay = root/'overlay.json'
            overlay.write_text('{}')
            manifest = {'devices': {'m55': {'identity': {'kind': 'overlay', 'path': str(overlay)}}}}
            with patch.object(gate, 'load_manifest', return_value=manifest):
                with self.assertRaisesRegex(ValueError, 'overlay SHA-256 mismatch'):
                    gate.preflight(args)

    def test_matrix_hash_mismatch_fails_before_launch(self):
        from tools.qualification.shared.run_x55_boot import validate_image_entry
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root/'image.bin').write_bytes(b'bad')
            with self.assertRaisesRegex(RuntimeError, 'SHA-256 mismatch'):
                validate_image_entry('c55', {'image': 'image.bin', 'sha256': '0'*64}, root)

    def test_synthesized_inputs_need_no_external_overlay(self):
        args = SimpleNamespace(emu=Path(sys.executable),
                               gdb=Path(sys.executable), qemu=Path(sys.executable))
        manifest, overlays = gate.preflight(args)
        self.assertEqual(overlays, [])
        for name in ('c55', 'm55'):
            self.assertEqual(manifest['devices'][name]['identity'],
                             {'kind': 'fsn', 'value': '1234ABCD'})

    def test_child_ownership_and_graceful_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            script = root/'parent.py'
            # The parent owns and reaps the child, just as EMU owns QEMU.
            script.write_text('import signal, subprocess, sys, time\n'
                              'child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])\n'
                              'def stop(*args):\n'
                              ' child.terminate(); child.wait(); sys.exit(0)\n'
                              'signal.signal(signal.SIGINT, stop)\n'
                              'print("ready", flush=True)\n'
                              'time.sleep(60)\n')
            proc = subprocess.Popen([sys.executable, str(script)], start_new_session=True,
                                    stdout=subprocess.PIPE, text=True)
            try:
                self.assertEqual(proc.stdout.readline().strip(), 'ready')
                children = gate.child_commands(proc)
                self.assertEqual(len(children), 1)
                cleanup = gate.stop_owned(proc, children, root/'ui.sock', [root/'private'], None)
                self.assertFalse(cleanup['forced'])
                self.assertEqual(cleanup['surviving_children'], [])
                self.assertEqual(cleanup['returncode'], 0)
                self.assertTrue(cleanup['private_removed'])
            finally:
                if proc.poll() is None:
                    proc.kill(); proc.wait()
                proc.stdout.close()

    def test_grace_deadline_forces_termination_and_preserves_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            proc = subprocess.Popen([sys.executable, '-c',
                                     'import signal,time; signal.signal(signal.SIGINT, signal.SIG_IGN); '
                                     'print("ready",flush=True); time.sleep(60)'],
                                    start_new_session=True, stdout=subprocess.PIPE, text=True)
            try:
                self.assertEqual(proc.stdout.readline().strip(), 'ready')
                cleanup = gate.stop_owned(proc, {}, root/'socket', [], None, grace=0.01)
                self.assertTrue(cleanup['forced'])
                self.assertEqual(cleanup['returncode'], -signal.SIGTERM)
            finally:
                if proc.poll() is None:
                    proc.kill(); proc.wait()
                proc.stdout.close()

    def test_unexpected_launcher_exit_records_failure_and_cleanup(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            fake = root/'emu'
            fake.write_text('#!/bin/sh\nexit 3\n')
            fake.chmod(0o755)
            args = SimpleNamespace(emu=fake, qemu=Path(sys.executable), gdb=Path(sys.executable))
            entry = {'image': 'unused', 'identity': {'kind': 'fsn', 'value': 'A35F2F28'},
                     'no_sim': {'minimum_ticks': 1}}
            result = gate.run_session(args, 'c55', entry, False, root/'case', debugger=False)
            self.assertEqual(result['status'], 'fail')
            self.assertEqual(result['error'], 'EOFError: EMU exited: 3')
            cleanup = json.loads((root/'case/cleanup.json').read_text())
            self.assertEqual(cleanup['returncode'], 3)
            self.assertTrue(cleanup['ui_socket_removed'])


if __name__ == '__main__':
    unittest.main()
