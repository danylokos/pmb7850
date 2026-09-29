"""Batch exit, timeout and cleanup contracts, using real bounded host children."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.qualification.gdb.gdb_registers import gdb_run, machine
from tools.qualification.gdb.gdb_scripting import ROOT, MARKER


class ScriptingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='c166 scripting ')
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name)

    def executable(self, name, body):
        path = self.out / name
        path.write_text(f'#!{sys.executable}\nimport os, sys, time\n' + body)
        path.chmod(0o755)
        return path

    def run_batch(self, body, **kwargs):
        return gdb_run(self.executable('fake gdb', body), self.out, 'case', [],
                       success_marker=MARKER, cwd=self.out, **kwargs)

    def assert_reaped(self, pid):
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_success_and_paths_with_spaces(self):
        output = self.run_batch(f'print({MARKER!r})\n')
        self.assertEqual(output.strip(), MARKER)
        result = json.loads((self.out / 'case-result.json').read_text())
        self.assertEqual(result['returncode'], 0)
        self.assertEqual(result['cwd'], str(self.out))

    def test_echoed_marker_is_not_completion(self):
        with self.assertRaisesRegex(AssertionError, 'Missing completion marker'):
            self.run_batch(f'print("+echo {MARKER}\\\\n")\n')

    def test_nonzero_exit_with_marker_still_fails(self):
        with self.assertRaises(AssertionError):
            self.run_batch(f'print({MARKER!r})\nsys.exit(7)\n')
        self.assertEqual(json.loads((self.out / 'case-result.json').read_text())['returncode'], 7)

    def test_zero_exit_without_marker_fails(self):
        with self.assertRaisesRegex(AssertionError, 'Missing completion marker'):
            self.run_batch('sys.exit(0)\n')

    def test_expected_error_requires_nonzero_exit_and_matching_diagnostic(self):
        self.run_batch('print("intended failure")\nsys.exit(1)\n', expected_error='intended failure')
        for body in ('print("intended failure")\n', 'print("different failure")\nsys.exit(1)\n'):
            with self.subTest(body=body), self.assertRaises(AssertionError):
                self.run_batch(body, expected_error='intended failure')

    def test_timeout_retains_output_and_reaps_gdb_and_qemu(self):
        qemu = self.executable('fake qemu',
            'from pathlib import Path\n'
            'sock = sys.argv[sys.argv.index("-gdb") + 1].split(",")[0][5:]\n'
            'Path(sock).touch()\n'
            'while True: time.sleep(1)\n')
        with self.assertRaises(subprocess.TimeoutExpired):
            with machine(qemu, 'c166-test', self.out, 'qemu') as sock:
                self.run_batch('print(os.getpid(), flush=True)\nwhile True: time.sleep(1)\n', timeout=1)
        self.assert_reaped(int((self.out / 'case.log').read_text()))
        qemu_result = json.loads((self.out / 'qemu-qemu-result.json').read_text())
        self.assertTrue(qemu_result['reaped'])
        self.assert_reaped(qemu_result['pid'])
        self.assertFalse(sock.parent.exists())
        self.assertEqual(json.loads((self.out / 'case-result.json').read_text())['status'], 'timeout')

    def test_qemu_startup_failure_reaps_child_and_retains_log(self):
        qemu = self.executable('failed qemu', 'print("startup failed", flush=True)\nsys.exit(9)\n')
        with self.assertRaisesRegex(RuntimeError, 'QEMU did not listen'):
            with machine(qemu, 'c166-test', self.out, 'qemu'):
                self.fail('Startup failure yielded a session')
        result = json.loads((self.out / 'qemu-qemu-result.json').read_text())
        self.assertEqual(result['returncode'], 9)
        self.assert_reaped(result['pid'])
        self.assertFalse(Path(result['socket']).parent.exists())
        self.assertIn('startup failed', (self.out / 'qemu-qemu.log').read_text())

    def test_missing_binaries_fail_dedicated_cli(self):
        for option in ('--gdb', '--qemu'):
            with self.subTest(option=option):
                args = {'--gdb': '/bin/true', '--qemu': '/bin/true'}
                args[option] = str(self.out / 'missing executable')
                result = subprocess.run([sys.executable, '-m', 'tools.qualification.gdb.gdb_scripting',
                                         *[v for pair in args.items() for v in pair],
                                         '--output', str(self.out / 'unused')],
                                        cwd=ROOT / 'emu', capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f'argument {option}:', result.stderr)
                self.assertIn('missing executable', result.stderr)
                self.assertFalse((self.out / 'unused').exists())


if __name__ == '__main__':
    unittest.main()
