"""Independent protocol peers exercise the runner, not upstream CPU behavior."""
from collections import deque
from contextlib import redirect_stdout, redirect_stderr
import io
import json
from pathlib import Path
import socket
import shutil
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from tools.qualification.qemu import qemu_gdb_blockers as probe


class Peer:
    """Scripted peer with deliberately fragmented reads, including checksums."""
    def __init__(self, exchanges=(), *, bad_checksum=False, truncate=False):
        self.exchanges = deque(exchanges)
        self.incoming = bytearray()
        self.sent = []
        self.bad_checksum, self.truncate = bad_checksum, truncate

    def settimeout(self, timeout):
        self.timeout = timeout

    def sendall(self, data):
        self.sent.append(data)
        if data in (b'+', b'-'):
            return
        expected, reply = self.exchanges.popleft()
        command = data[1:data.rindex(b'#')].decode()
        if command != expected:
            raise AssertionError(f'expected command {expected!r}, got {command!r}')
        checksum = f'{sum(reply) & 255:02x}'.encode()
        self.incoming.extend(b'+$' + reply + b'#' +
                             (b'xx' if self.bad_checksum else checksum))
        if self.truncate:
            del self.incoming[-1:]

    def recv(self, count):
        value = bytes(self.incoming[:1])
        del self.incoming[:1]
        return value

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass


def register(value):
    return value.to_bytes(8, 'little').hex().encode()


def stopped(command, pc, reply=b'T05thread:01;'):
    return [(command, reply), ('p10', register(pc))]


def handshake():
    xml = (b'l<target><architecture>i386:x86-64</architecture>'
           b'<feature name="org.gnu.gdb.i386.core">'
           b'<reg name="rip" regnum="16" bitsize="64"/></feature></target>')
    return [('qSupported:qXfer:features:read+', b'qXfer:features:read+'),
            ('qXfer:features:read:target.xml:0,1000', xml), *stopped('?', 0xfff0)]


def watch_exchanges(stale=False):
    return [
        ('M1000,d:c70600303412c7060230785690', b'OK'),
        ('M3000,4:00000000', b'OK'), *stopped('s', 0x1000),
        ('Z2,3000,2', b'OK'), *stopped('s', 0x1006, b'T05thread:01;watch:3000;'),
        ('m3000,2', b'3412'), ('z2,3000,2', b'OK'), ('Z0,100c,1', b'OK'),
        *stopped('c', 0x1006 if stale else 0x100c),
        ('m3002,2', b'0000' if stale else b'7856'),
        *([] if stale else [('z0,100c,1', b'OK')]),
    ]


def mmio_exchanges(stale=False):
    return [
        ('Ma0000,b:b83412a30030ea000000a0', b'OK'),
        ('ma0000,b', b'b83412a30030ea000000a0'),
        *stopped('s', 0), *stopped('s', 3), *stopped('s', 6),
        ('m3000,2', b'3412'), ('Ma0000,3:b87856', b'OK'),
        ('ma0000,3', b'b87856'),
        *stopped('s', 0), *stopped('s', 3), *stopped('s', 6),
        ('m3000,2', b'3412' if stale else b'7856'),
    ]


class BlockerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def client(self, exchanges, **kwargs):
        peer = Peer(exchanges, **kwargs)
        client = probe.RSP(peer, self.root / 'rsp.json', 0.1)
        client.pc_register, client.pc_bytes = 16, 8
        return client, peer

    def test_fragmented_checksum_escapes_and_repeat(self):
        client, peer = self.client([('test', b'0* }\x04}\x03}\x0a}]')])
        # '0* ' means four zeroes, followed by escaped $, #, *, }.
        self.assertEqual(client.packet('test'), '0000$#*}')
        self.assertEqual(peer.sent[-1], b'+')

    def test_bad_checksum_and_eof_are_errors_with_transcripts(self):
        for option, error in [('bad_checksum', RuntimeError), ('truncate', EOFError)]:
            with self.subTest(option=option):
                client, peer = self.client([('?', b'T05')], **{option: True})
                with self.assertRaises(error):
                    client.packet('?')
                records = json.loads((self.root / 'rsp.json').read_text())
                self.assertIn('error', records[-1])
                if option == 'bad_checksum':
                    self.assertEqual(peer.sent[-1], b'-')

    def test_timeout_does_not_send_interrupt_or_manufacture_stop(self):
        client, peer = self.client([('s', b'T05')])
        peer.recv = Mock(side_effect=socket.timeout('deadline'))
        with self.assertRaises(TimeoutError):
            client.packet('s')
        self.assertEqual(len(peer.sent), 1)
        self.assertNotIn(b'\x03', peer.sent)

    def test_wrong_signal_watch_identity_and_pc_rejected(self):
        cases = [([('s', b'T02')], {}),
                 ([('s', b'T05watch:3002;')], {'watch': 0x3000}),
                 ([('s', b'T05')], {'watch': 0x3000}),
                 (stopped('s', 0x1000), {'pc': 0x1006})]
        for exchanges, kwargs in cases:
            with self.subTest(exchanges=exchanges):
                client, _ = self.client(exchanges)
                with self.assertRaises(probe.CheckFailure):
                    client.stop('s', **kwargs)

    def test_xml_include_register_number_and_chunking(self):
        first = b'<target xmlns:xi="http://www.w3.org/2001/XInclude">'
        second = b'<architecture>i386</architecture><xi:include href="core.xml"/></target>'
        core = b'l<feature><reg name="eax" regnum="7" bitsize="32"/><reg name="eip" bitsize="32"/></feature>'
        client, _ = self.client([
            ('qSupported:qXfer:features:read+', b'qXfer:features:read+'),
            ('qXfer:features:read:target.xml:0,1000', b'm' + first),
            (f'qXfer:features:read:target.xml:{len(first):x},1000', b'l' + second),
            ('qXfer:features:read:core.xml:0,1000', core),
        ])
        client.discover_pc()
        self.assertEqual((client.pc_register, client.pc_bytes), (8, 4))

    def test_qemu_target_xml_uses_dtd_supplied_xinclude_namespace(self):
        # Exact target.xml received from pristine upstream v11.1.1. GDB's
        # xinclude.dtd supplies the namespace; ElementTree does not load it.
        xml = (b'l<?xml version="1.0"?><!DOCTYPE target SYSTEM "gdb-target.dtd">'
               b'<target><architecture>i386:x86-64</architecture>'
               b'<xi:include href="i386-64bit.xml"/></target>')
        core = b'l<feature><reg name="rip" regnum="16" bitsize="64"/></feature>'
        client, _ = self.client([
            ('qSupported:qXfer:features:read+', b'qXfer:features:read+'),
            ('qXfer:features:read:target.xml:0,1000', xml),
            ('qXfer:features:read:i386-64bit.xml:0,1000', core),
        ])
        client.discover_pc()
        self.assertEqual((client.pc_register, client.pc_bytes), (16, 8))

    def test_both_probes_reject_historical_failure_and_accept_expected_effects(self):
        for name, factory, function in [('watch', watch_exchanges, probe.watchpoint_step),
                                         ('mmio', mmio_exchanges, probe.mmio_code)]:
            for stale in (False, True):
                with self.subTest(name=name, stale=stale):
                    client, peer = self.client(factory(stale))
                    args = (client,) if name == 'watch' else (client, Mock())
                    if stale:
                        with self.assertRaises(probe.CheckFailure):
                            function(*args)
                    else:
                        function(*args)
                    self.assertFalse(peer.exchanges)

    def test_qtest_fragmented_line_and_error(self):
        for response in (b'OK\n', b'FAIL bad command\n'):
            peer = Peer()
            peer.incoming.extend(response)
            peer.sendall = Mock()
            qt = probe.QTest(peer, self.root / 'qtest.json', 0.1)
            if response.startswith(b'OK'):
                qt.command('outb 0x3c4 2')
            else:
                with self.assertRaises(RuntimeError):
                    qt.command('outb 0x3c4 2')
            self.assertEqual(json.loads((self.root / 'qtest.json').read_text())[0]['response'],
                             response.decode().strip())

    def test_case_writes_results_and_cleans_process_on_pass_and_failure(self):
        for stale in (False, True):
            out = self.root / str(stale)
            out.mkdir()
            args = SimpleNamespace(qemu=Path('/synthetic/qemu'), output=out,
                                   timeout=0.1, startup_timeout=0.1)
            peer = Peer(handshake() + watch_exchanges(stale))
            process = Mock()
            with patch.object(probe.subprocess, 'Popen', return_value=process), \
                    patch.object(probe, 'connect', return_value=peer):
                result = probe.run_case(args, 'watchpoint-step')
            self.assertEqual(result['status'], 'fail' if stale else 'pass')
            self.assertEqual(result, json.loads((out / 'watchpoint-step/result.json').read_text()))
            process.terminate.assert_called_once()
            process.wait.assert_called_once_with(timeout=5)
            self.assertFalse(peer.exchanges)

    def test_mmio_case_connects_qtest_and_preserves_port_transcript(self):
        args = SimpleNamespace(qemu=Path('/synthetic/qemu'), output=self.root,
                               timeout=0.1, startup_timeout=0.1)
        rsp = Peer(handshake() + mmio_exchanges())
        qt = Peer()
        qt.incoming.extend(b'OK\n' * 12)
        qt.sendall = Mock()
        process = Mock()
        with patch.object(probe.subprocess, 'Popen', return_value=process), \
                patch.object(probe, 'connect', side_effect=[rsp, qt]):
            result = probe.run_case(args, 'mmio-code')
        self.assertEqual(result['status'], 'pass')
        ports = json.loads((self.root / 'mmio-code/qtest.json').read_text())
        self.assertEqual(len(ports), 12)
        self.assertEqual(ports[0]['command'], 'outb 0x3c4 0x2')
        self.assertEqual(ports[-1]['command'], 'outb 0x3cf 0xff')
        self.assertFalse(rsp.exchanges)
        process.terminate.assert_called_once()

    def test_startup_failure_is_error_and_forces_cleanup(self):
        args = SimpleNamespace(qemu=Path('/synthetic/qemu'), output=self.root,
                               timeout=0.1, startup_timeout=0.1)
        process = Mock()
        process.wait.side_effect = [subprocess.TimeoutExpired('qemu', 5), 0]
        with patch.object(probe.subprocess, 'Popen', return_value=process), \
                patch.object(probe, 'connect', side_effect=RuntimeError('startup failure')):
            result = probe.run_case(args, 'watchpoint-step')
        self.assertEqual((result['status'], result['stage']), ('error', 'startup'))
        process.kill.assert_called_once()

    def test_startup_detects_early_exit(self):
        process = Mock(returncode=1)
        process.poll.return_value = 1
        with self.assertRaisesRegex(RuntimeError, 'exited during startup'):
            probe.connect(Path('/missing/socket'), process, time.monotonic() + 1)

    def test_missing_binary_and_existing_output_fail(self):
        out = self.root / 'missing'
        argv = ['--qemu', '/does/not/exist', '--output', str(out)]
        with redirect_stderr(io.StringIO()):
            self.assertEqual(probe.main(argv), 2)
            with self.assertRaises(SystemExit) as error:
                probe.main(argv)
        self.assertEqual(error.exception.code, 2)
        self.assertEqual(json.loads((out / 'results.json').read_text())['preflight']['status'], 'error')

    def test_all_cases_continue_after_failure_and_error_dominates_exit(self):
        for statuses, expected in [(('pass', 'pass'), 0), (('fail', 'pass'), 1),
                                   (('fail', 'error'), 2)]:
            with self.subTest(statuses=statuses):
                argv = ['--qemu', sys.executable, '--output', str(self.root / '-'.join(statuses))]
                with patch.object(probe, 'run_case', side_effect=[{'status': s} for s in statuses]) as run, \
                        redirect_stdout(io.StringIO()):
                    self.assertEqual(probe.main(argv), expected)
                self.assertEqual(run.call_count, 2)

    def test_copied_runner_works_without_project_on_python_path(self):
        copied = self.root / 'reproduce.py'
        shutil.copyfile(probe.__file__, copied)
        command = [sys.executable, '-I', str(copied)]
        help_result = subprocess.run(command + ['--help'], cwd=self.root,
                                     capture_output=True, text=True, timeout=5)
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn('watchpoint-step', help_result.stdout)
        out = self.root / 'isolated'
        failure = subprocess.run(command + ['--qemu', '/does/not/exist', '--output', str(out)],
                                 cwd=self.root, capture_output=True, text=True, timeout=5)
        self.assertEqual(failure.returncode, 2, failure.stderr)
        self.assertEqual(json.loads((out / 'results.json').read_text())['preflight']['stage'], 'preflight')

    def test_timeout_limits_reject_unbounded_values(self):
        for value in ('0', '-1', 'inf', 'nan'):
            with self.subTest(value=value), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    probe.main(['--qemu', sys.executable, '--output', str(self.root / 'unused'),
                                '--timeout', value])


if __name__ == '__main__':
    unittest.main()
