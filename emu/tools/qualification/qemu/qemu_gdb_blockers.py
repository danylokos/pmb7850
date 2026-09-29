#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Standalone synthetic x86 reproducers for the C166 Phase 6 upstream blockers.

Python standard library only; copy this file anywhere to use it. Adapted from
the retained Phase 6 test_watchpoint_step.py/test_mmio_code.py experiments and
the acknowledgement-mode transport in QEMU's C166 run_vectors.py. No GDB client,
C166 register map, installed firmware, or project imports participate.
"""
from __future__ import annotations

import argparse
from contextlib import ExitStack, contextmanager
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


class CheckFailure(Exception):
    """A complete protocol exchange produced unexpected guest behavior."""


def check(actual, expected, label):
    if actual != expected:
        raise CheckFailure(f'{label}: expected {expected!r}, got {actual!r}')


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


class Wire:
    def __init__(self, stream, path, timeout):
        self.stream, self.path, self.timeout = stream, path, timeout
        self.records = []

    def read(self, count, deadline):
        data = bytearray()
        while len(data) < count:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('protocol exchange deadline exceeded')
            self.stream.settimeout(remaining)
            part = self.stream.recv(count - len(data))
            if not part:
                raise EOFError('peer closed during protocol exchange')
            data.extend(part)
        return bytes(data)

    @contextmanager
    def record(self, command):
        entry = {'command': command, 'started_at': datetime.now(timezone.utc).isoformat()}
        self.records.append(entry)
        save(self.path, self.records)
        try:
            yield entry
        except (OSError, EOFError, RuntimeError, ValueError) as error:
            entry['error'] = f'{type(error).__name__}: {error}'
            raise
        finally:
            save(self.path, self.records)


class RSP(Wire):
    @staticmethod
    def decode(payload):
        data = bytearray()
        index = 0
        while index < len(payload):
            byte = payload[index]
            index += 1
            if byte in (ord('}'), ord('*')):
                if index == len(payload):
                    raise RuntimeError('truncated RSP escape/repeat')
                following = payload[index]
                index += 1
                if byte == ord('}'):
                    data.append(following ^ 0x20)
                else:
                    count = following - 29
                    if not data or not 3 <= count <= 97:
                        raise RuntimeError('invalid RSP repeat')
                    data.extend(data[-1:] * count)
            else:
                data.append(byte)
            if len(data) > 1024 * 1024:
                raise RuntimeError('RSP response exceeds size bound')
        return data.decode('ascii')

    def packet(self, command):
        with self.record(command) as entry:
            deadline = time.monotonic() + self.timeout
            payload = command.encode('ascii')
            self.stream.settimeout(self.timeout)
            self.stream.sendall(b'$' + payload + f'#{sum(payload) & 255:02x}'.encode())
            if self.read(1, deadline) != b'+':
                raise RuntimeError('RSP command was not acknowledged')
            if self.read(1, deadline) != b'$':
                raise RuntimeError('missing RSP response marker')
            raw = bytearray()
            while True:
                byte = self.read(1, deadline)
                if byte == b'#':
                    break
                raw.extend(byte)
                if len(raw) > 1024 * 1024:
                    raise RuntimeError('RSP packet exceeds size bound')
            received = self.read(2, deadline)
            entry['wire_payload_hex'] = raw.hex()
            if received.lower() != f'{sum(raw) & 255:02x}'.encode():
                self.stream.sendall(b'-')
                raise RuntimeError('bad RSP response checksum')
            self.stream.sendall(b'+')
            entry['response'] = self.decode(raw)
            return entry['response']

    def xml(self, name):
        data = ''
        while len(data) < 1024 * 1024:
            reply = self.packet(f'qXfer:features:read:{name}:{len(data):x},1000')
            if not reply or reply[0] not in 'ml' or (reply[0] == 'm' and len(reply) == 1):
                raise RuntimeError(f'invalid XML transfer for {name}: {reply!r}')
            data += reply[1:]
            if reply[0] == 'l':
                # QEMU relies on gdb-target.dtd -> xinclude.dtd to supply
                # xmlns:xi on xi:include. ElementTree does not load that DTD;
                # provide its fixed namespace locally without fetching it.
                if '<xi:include' in data and not re.search(r'\bxmlns:xi\s*=', data):
                    data = re.sub(r'<(target|feature)\b',
                                  r'<\1 xmlns:xi="http://www.w3.org/2001/XInclude"',
                                  data, count=1)
                return ET.fromstring(data)
        raise RuntimeError('target XML exceeds size bound')

    def discover_pc(self):
        features = self.packet('qSupported:qXfer:features:read+')
        if 'qXfer:features:read+' not in features.split(';'):
            raise RuntimeError('QEMU does not advertise target XML')
        root = self.xml('target.xml')
        if root.findtext('architecture') not in ('i386', 'i386:x86-64'):
            raise RuntimeError('reproducer requires an x86 target description')
        # Expand includes in document order so implicit register numbers remain
        # correct. Detect cycles and bound the number of annexes.
        seen = {'target.xml'}
        def expand(node):
            for child in node:
                if child.tag == '{http://www.w3.org/2001/XInclude}include':
                    name = child.get('href')
                    if not name or name in seen or len(seen) >= 32:
                        raise RuntimeError('invalid, repeated or excessive XML includes')
                    seen.add(name)
                    yield from expand(self.xml(name))
                elif child.tag == 'reg':
                    yield child
                else:
                    yield from expand(child)
        number = 0
        for reg in expand(root):
            number = int(reg.get('regnum', str(number)), 0)
            if reg.get('name') in ('rip', 'eip'):
                bits = int(reg.get('bitsize', '0'))
                if bits not in (32, 64):
                    raise RuntimeError(f'invalid x86 instruction-pointer width: {bits}')
                self.pc_register = number
                self.pc_bytes = bits // 8
                return
            number += 1
        raise RuntimeError('target XML has no x86 instruction pointer')

    def stop(self, command, pc=None, watch=None):
        reply = self.packet(command)
        if not re.fullmatch(r'T05(?:[^;]+;)*|S05', reply):
            raise CheckFailure(f'{command}: expected SIGTRAP stop, got {reply!r}')
        fields = dict(re.findall(r'([^:;]+):([^;]*);', reply[3:]))
        hits = {key: value for key, value in fields.items()
                if key in ('watch', 'rwatch', 'awatch')}
        expected = {} if watch is None else {'watch': watch}
        check({key: int(value, 16) for key, value in hits.items()}, expected,
              f'{command} watch identity')
        raw_pc = self.packet(f'p{self.pc_register:x}')
        if not re.fullmatch(r'[0-9a-fA-F]{' + str(self.pc_bytes * 2) + '}', raw_pc):
            raise RuntimeError(f'invalid instruction-pointer reply: {raw_pc!r}')
        actual_pc = int.from_bytes(bytes.fromhex(raw_pc), 'little')
        if pc is not None:
            check(actual_pc, pc, f'{command} instruction pointer')
        return actual_pc


class QTest(Wire):
    def command(self, command):
        with self.record(command) as entry:
            deadline = time.monotonic() + self.timeout
            self.stream.settimeout(self.timeout)
            self.stream.sendall(command.encode('ascii') + b'\n')
            data = bytearray()
            while not data.endswith(b'\n'):
                data.extend(self.read(1, deadline))
                if len(data) > 4096:
                    raise RuntimeError('qtest response exceeds size bound')
            entry['response'] = data.decode('ascii').strip()
            if entry['response'] != 'OK':
                raise RuntimeError(f'qtest command failed: {entry["response"]}')


def connect(path, process, deadline):
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f'QEMU exited during startup ({process.returncode}); see qemu.log')
        stream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            stream.connect(str(path))
            return stream
        except (FileNotFoundError, ConnectionRefusedError):
            stream.close()
            time.sleep(0.01)
        except BaseException:
            stream.close()
            raise
    raise TimeoutError(f'QEMU socket did not appear: {path}')


def watchpoint_step(remote):
    check(remote.packet('M1000,d:c70600303412c7060230785690'), 'OK', 'write code')
    check(remote.packet('M3000,4:00000000'), 'OK', 'initialize destination')
    remote.stop('s', 0x1000)  # reset far jump
    check(remote.packet('Z2,3000,2'), 'OK', 'insert write watchpoint')
    remote.stop('s', 0x1006, watch=0x3000)
    check(remote.packet('m3000,2'), '3412', 'watched write committed')
    check(remote.packet('z2,3000,2'), 'OK', 'remove write watchpoint')
    check(remote.packet('Z0,100c,1'), 'OK', 'insert final breakpoint')
    pc = remote.stop('c')
    second = remote.packet('m3002,2')
    check((pc, second), (0x100c, '7856'), 'resume PC and second write')
    check(remote.packet('z0,100c,1'), 'OK', 'remove final breakpoint')


def mmio_code(remote, qt):
    # Planar VGA aperture: all write planes enabled, read plane zero.
    for index, value in ((2, 15), (4, 6)):
        qt.command(f'outb 0x3c4 {index:#x}')
        qt.command(f'outb 0x3c5 {value:#x}')
    for index, value in ((4, 0), (5, 0), (6, 5), (8, 255)):
        qt.command(f'outb 0x3ce {index:#x}')
        qt.command(f'outb 0x3cf {value:#x}')
    # movw $0x1234,%ax; movw %ax,0x3000; ljmp a000:0000
    code = 'b83412a30030ea000000a0'
    check(remote.packet(f'Ma0000,b:{code}'), 'OK', 'write VGA code')
    check(remote.packet('ma0000,b'), code, 'initial VGA code readback')
    remote.stop('s', 0)  # reset far jump into a000:0000
    remote.stop('s', 3)
    remote.stop('s', 6)
    check(remote.packet('m3000,2'), '3412', 'original instruction executed')
    check(remote.packet('Ma0000,3:b87856'), 'OK', 'modify VGA instruction')
    check(remote.packet('ma0000,3'), 'b87856', 'modified VGA code readback')
    remote.stop('s', 0)  # far jump back
    remote.stop('s', 3)
    remote.stop('s', 6)
    check(remote.packet('m3000,2'), '7856', 'modified MMIO instruction executed')


def run_case(args, name):
    out = args.output / name
    out.mkdir()
    bios = bytearray(65536)
    reset = 'ea00100000' if name == 'watchpoint-step' else 'ea000000a0'
    bios[0xfff0:0xfff5] = bytes.fromhex(reset)
    bios_path = out / 'bios.bin'
    bios_path.write_bytes(bios)
    result = {'status': 'error', 'stage': 'startup', 'bios_sha256': digest(bios_path)}
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix='qemu-gdb-', dir='/tmp') as tmp:
            rsp_path, qt_path = Path(tmp) / 'gdb.sock', Path(tmp) / 'qtest.sock'
            command = [str(args.qemu), '-M', 'pc', '-accel', 'tcg', '-S',
                       '-display', 'none', '-nodefaults', '-bios', str(bios_path),
                       '-gdb', f'unix:{rsp_path},server=on,wait=off']
            if name == 'mmio-code':
                command += ['-device', 'VGA', '-qtest', f'unix:{qt_path},server=on,wait=off']
            save(out / 'command.json', command)
            with (out / 'qemu.log').open('w') as log:
                process = subprocess.Popen(command, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + args.startup_timeout
                    with ExitStack() as connections:
                        stream = connections.enter_context(connect(rsp_path, process, deadline))
                        if name == 'mmio-code':
                            qt_stream = connections.enter_context(connect(qt_path, process, deadline))
                        remote = RSP(stream, out / 'rsp.json', args.timeout)
                        result['stage'] = 'protocol'
                        remote.discover_pc()
                        remote.stop('?')
                        result['stage'] = 'probe'
                        if name == 'watchpoint-step':
                            watchpoint_step(remote)
                        else:
                            mmio_code(remote, QTest(qt_stream, out / 'qtest.json', args.timeout))
                    result.update(status='pass', stage='complete')
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
    except CheckFailure as error:
        result.update(status='fail', error=str(error))
    except (OSError, EOFError, RuntimeError, ValueError, ET.ParseError,
            subprocess.SubprocessError) as error:
        result.update(status='error', error=f'{type(error).__name__}: {error}')
    result['elapsed_seconds'] = time.monotonic() - started
    save(out / 'result.json', result)
    return result


def positive(value):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError('must be a finite positive number')
    return value


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', required=True, type=Path)
    parser.add_argument('--case', choices=('all', 'watchpoint-step', 'mmio-code'), default='all')
    parser.add_argument('--output', required=True, type=Path, help='new artifact directory')
    parser.add_argument('--startup-timeout', type=positive, default=5)
    parser.add_argument('--timeout', type=positive, default=30, help='seconds per protocol exchange')
    args = parser.parse_args(argv)
    args.qemu, args.output = args.qemu.resolve(), args.output.resolve()
    if args.output.exists():
        parser.error('--output must not already exist; preserve previous evidence')
    args.output.mkdir(parents=True)
    provenance = {'qemu': str(args.qemu), 'runner_sha256': digest(Path(__file__)),
                  'started_at': datetime.now(timezone.utc).isoformat(),
                  'python': sys.version, 'arguments': vars(args) | {'qemu': str(args.qemu),
                                                                  'output': str(args.output)}}
    try:
        if not args.qemu.is_file() or not os.access(args.qemu, os.X_OK):
            raise RuntimeError(f'missing executable: {args.qemu}')
        provenance['qemu_sha256'] = digest(args.qemu)
        version = subprocess.run([str(args.qemu), '--version'], capture_output=True,
                                 text=True, timeout=args.startup_timeout, check=True)
        provenance['qemu_version'] = version.stdout.strip()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        save(args.output / 'provenance.json', provenance)
        save(args.output / 'results.json', {'preflight': {'status': 'error', 'stage': 'preflight',
                                                        'error': str(error)}})
        print(f'ERROR preflight: {error}', file=sys.stderr)
        return 2
    save(args.output / 'provenance.json', provenance)
    names = ('watchpoint-step', 'mmio-code') if args.case == 'all' else (args.case,)
    results = {}
    for name in names:
        results[name] = run_case(args, name)
        save(args.output / 'results.json', results)
        result = results[name]
        print(f'{result["status"].upper()} {name}: {result.get("error", "expected behavior")}', flush=True)
    return max({'pass': 0, 'fail': 1, 'error': 2}[result['status']] for result in results.values())


if __name__ == '__main__':
    sys.exit(main())
