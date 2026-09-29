"""Stable CLI shutdown snapshots, immutable inputs, and rejected restores."""
import hashlib
from tools.bundled_firmware import image as bundled_image

import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

from ...shared.support import ROOT
EMU = ROOT / 'emu/bin/emu'
QEMU = Path(os.environ.get('QEMU_SNAPSHOT_BINARY',
                          ROOT / 'qemu/build/release/qemu-system-c166'))
FLASH = bundled_image("c55")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def unused_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class QemuSnapshotTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not EMU.exists() or not QEMU.exists():
            raise unittest.SkipTest('build EMU and QEMU before snapshot integration tests')
        cls.temp = tempfile.TemporaryDirectory(prefix='emu-snapshot-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.original = digest(FLASH)
        cls.seed, _ = cls.run_session('seed', [str(FLASH), '--sim'], paused=True)
        cls.meta = json.loads((cls.seed / 'snapshot.json').read_text())

    @classmethod
    def run_session(cls, label, options, paused=False, sig=signal.SIGINT,
                    obstruct=False):
        ui = cls.work / (label + '.sock')
        args = [str(EMU), 'run', '--engine', 'qemu', '--qemu-binary', str(QEMU),
                '--snapshot', '--label', label, '--ui-socket', str(ui), *options]
        if paused:
            args += ['--gdb', str(unused_port())]
        with (cls.work / (label + '.log')).open('w+') as log:
            process = subprocess.Popen(args, cwd=cls.work, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 10
                while not ui.exists() and process.poll() is None and time.monotonic() < deadline:
                    time.sleep(.02)
                if not ui.exists():
                    log.seek(0)
                    raise AssertionError(log.read())
                time.sleep(.15)
                snapshot = cls.work / 'shots' / label / 'snapshot'
                if obstruct:
                    (snapshot / 'vmstate.bin').write_bytes(b'existing file')
                process.send_signal(sig)
                code = process.wait(timeout=20)
                log.seek(0)
                output = log.read()
                if code != (2 if obstruct else 128 + sig):
                    raise AssertionError(f'{code}: {output}')
                return snapshot, output
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()

    def reject(self, path, extra=(), expected='snapshot'):
        result = subprocess.run([str(EMU), 'run', '--engine', 'qemu',
            '--qemu-binary', str(QEMU), '--from-snapshot', str(path), *extra],
            cwd=self.work, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, timeout=15)
        self.assertEqual(result.returncode, 2, result.stdout)
        self.assertIn(expected, result.stdout.lower())

    def test_resume_and_save_again(self):
        before = {p.name: digest(p) for p in self.seed.iterdir()}
        restored, _ = self.run_session('restored', ['--from-snapshot', str(self.seed)],
                                       paused=True, sig=signal.SIGTERM)
        meta = json.loads((restored / 'snapshot.json').read_text())
        for key in ('icount', 'ticks', 'pc', 'sim_stub', 'chips'):
            self.assertEqual(meta[key], self.meta[key], key)
        running, output = self.run_session('running', ['--from-snapshot', str(restored),
            '--trace', 'lifecycle,flash,serial'])
        progressed = json.loads((running / 'snapshot.json').read_text())
        self.assertGreater(progressed['icount'], meta['icount'])
        paused, text = self.run_session('paused-again',
            ['--from-snapshot', str(running)], paused=True)
        self.assertEqual(json.loads((paused / 'snapshot.json').read_text())['icount'],
                         progressed['icount'])
        self.assertRegex(text, r'ticks: 0  icount: [1-9][0-9]*')
        self.assertNotIn('error:', output)
        self.assertGreater((running.parent / 'trace/trace.parquet').stat().st_size, 0)
        self.assertEqual(before, {p.name: digest(p) for p in self.seed.iterdir()})
        self.assertEqual(digest(FLASH), self.original)

    def test_bad_metadata_and_payloads(self):
        mutations = [
            ('engine', 'cemu'), ('version', 999), ('native_version', 999),
            ('device', 'm55'), ('flash_sha256', '0' * 64),
            ('pc', self.meta['pc'] + 2),
        ]
        for key, value in mutations:
            with self.subTest(key=key), tempfile.TemporaryDirectory(dir=self.work) as tmp:
                target = Path(tmp) / 'snapshot'
                shutil.copytree(self.seed, target)
                meta = dict(self.meta)
                meta[key] = value
                (target / 'snapshot.json').write_text(json.dumps(meta))
                self.reject(target, expected='conflicts' if key == 'device'
                            else 'snapshot')
        for name in ('flash-0.bin', 'vmstate.bin'):
            with self.subTest(file=name), tempfile.TemporaryDirectory(dir=self.work) as tmp:
                target = Path(tmp) / 'snapshot'
                shutil.copytree(self.seed, target)
                (target / name).write_bytes(b'truncated')
                self.reject(target, expected='corrupt')
                (target / name).unlink()
                self.reject(target, expected='missing')
        self.reject(self.seed, ['--patch', 'c55-aircheck-off'], expected='conflict')
        with tempfile.TemporaryDirectory(dir=self.work) as tmp:
            target = Path(tmp) / 'snapshot'
            shutil.copytree(self.seed, target)
            payload = target / 'vmstate.bin'
            payload.write_bytes(b'invalid native stream')
            meta = dict(self.meta)
            meta['vmstate'] = {'size': payload.stat().st_size, 'sha256': digest(payload)}
            (target / 'snapshot.json').write_text(json.dumps(meta))
            self.reject(target, expected='error:')

    def test_existing_output_is_preserved(self):
        saved, output = self.run_session('blocked', [str(FLASH)],
                                         paused=True, obstruct=True)
        self.assertFalse((saved / 'snapshot.json').exists())
        self.assertEqual((saved / 'vmstate.bin').read_bytes(), b'existing file')
        self.assertIn('error: snapshot:', output)

    def test_cross_engine_and_deferred_flags(self):
        result = subprocess.run([str(EMU), 'run', '--from-snapshot', str(self.seed)],
            cwd=self.work, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 2)
        self.assertIn('another engine', result.stderr)
        for flag in (['--snapshot', '--snapshot-at', '100'], ['--snapshot-full-bins']):
            self.reject(self.seed, flag, expected='unsupported')


if __name__ == '__main__':
    unittest.main()
