#!/usr/bin/env python3
"""Manual-referenced static C166 disassembly and byte naming gate."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import socket
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

from .gdb_registers import (DescriptionServer, Scenario, Remote, connect, gdb_run, machine)
from ..shared.support import ROOT, FIXTURE_DIR, binary, digest

FIXTURES = FIXTURE_DIR / 'c166-disassembly.json'
MARKER = 'C166_DISASSEMBLY_OK'


def check_listing(output, expected):
    rows = []
    for line in output.splitlines():
        match = re.match(r'^\s*(?:=>\s*)?0x([0-9a-f]+)(?:\s+<[^>]*>)?:\s+(.*)', line)
        if match:
            raw = re.match(r'^(?:[0-9a-f]{2}\s+)+', match[2])
            text = match[2][raw.end():].strip() if raw else match[2].strip()
            row = (int(match[1], 16), text)
            if expected and len(expected[0]) == 3:
                row += (''.join(raw[0].split()) if raw else None,)
            rows.append(row)
    assert rows == expected, next((f'row {i}: {a!r} != {b!r}'
                                 for i, (a, b) in enumerate(zip(rows, expected))
                                 if a != b), f'{len(rows)} rows != {len(expected)}')


def listing_case(gdb, qemu, out, kind, fixtures, state):
    name = f'{kind}-state{state}'
    expected, lines = [], []
    with machine(qemu, kind, out, name) as sock:
        lines += [f'target remote {sock}', 'set $pc = 0x2000',
                  f'set $cp = {0xf800 + state*0x100}',
                  f'set $extr = {state}', f'set $ext_kind = {state}',
                  f'set $ext_value = {state*0xab}', f'set $ext_count = {state*4}']
        lines += [f'set $dpp{i} = {i+state*0x100}' for i in range(4)]
        for case in fixtures:
            addr, raw = case['address'], bytes.fromhex(case['bytes'])
            assert len(raw) == case['length']
            for i, value in enumerate(raw + b'\xcc\0'):
                lines.append(f'set {{unsigned char}}0x{addr+i:x} = {value}')
            lines += [f'echo CASE {case["bytes"]} {case["manual"]}\\n',
                      f'x/2i 0x{addr:x}',
                      f'disassemble /r 0x{addr:x},0x{addr+len(raw)+2:x}']
            expected += [(addr, case['text'], None), (addr+len(raw), 'nop', None),
                         (addr, case['text'], raw.hex()), (addr+len(raw), 'nop', 'cc00')]
        # One contiguous mixed sequence checks all subsequent addresses.
        addr = 0x3200
        sequence = ['e7f15a00', 'f112', 'd7f0ff03', 'cc00', '4400', 'b54ab5b5']
        text = ['movb rh0,#0x5a', 'movb rh0,rl1', 'extpr #0x3ff,#4',
                'nop', '.byte 0x44,0x00', 'einit']
        sequence_expected = []
        for raw, desc in zip(sequence, text):
            sequence_expected.append((addr, desc, raw))
            for value in bytes.fromhex(raw):
                lines.append(f'set {{unsigned char}}0x{addr:x} = {value}')
                addr += 1
        lines += ['x/6i 0x3200', f'disassemble /r 0x3200,0x{addr:x}',
                  f'echo {MARKER}\\n', 'disconnect', 'quit 0']
        expected += [(a, t, None) for a, t, _ in sequence_expected] + sequence_expected
        output = gdb_run(gdb, out, name, lines, success_marker=MARKER)
        # Initial remote connection prints the reset instruction before fixtures.
        output = output[output.index('CASE '):]
        check_listing(output, expected)
    print(f'PASS {name}: {len(fixtures)} fixtures, x/i and disassemble /r, all addresses')


class MemoryServer(DescriptionServer):
    """ACK-mode RSP with an exact readable range and an otherwise failing bus."""
    def __init__(self, listener, memory):
        feature = ET.parse(ROOT/'qemu/gdbstub/gdb-xml/c166-core.xml').getroot()
        xml = ('<target><architecture>c166</architecture>' +
               ET.tostring(feature, encoding='unicode') + '</target>')
        super().__init__(listener, xml)
        self.memory = memory

    def response(self, command):
        if command.startswith('m'):
            addr, size = (int(x, 16) for x in command[1:].split(','))
            try:
                return bytes(self.memory[a] for a in range(addr, addr+size)).hex()
            except KeyError:
                return 'E01'
        return super().response(command)


def read_errors(gdb, out):
    cases = [('unreadable-first', b'', False),
             ('unreadable-continuation', b'\xe7\xf1', False),
             ('last-readable-word', b'\xcc\0', True)]
    for command in ('x/i 0x2400', 'disassemble /r 0x2400,0x2402'):
        for name, raw, succeeds in cases:
            name += '-raw' if command.startswith('disassemble') else '-instruction'
            with tempfile.TemporaryDirectory(prefix='c166-dis-', dir='/tmp') as tmp:
                path = Path(tmp)/'gdb.sock'
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                    listener.bind(str(path)); listener.listen(1)
                    server = MemoryServer(listener, dict(enumerate(raw, 0x2400)))
                    thread = threading.Thread(target=server.run, daemon=True)
                    thread.start()
                    try:
                        output = gdb_run(gdb, out, name,
                                         [f'target remote {path}', 'echo START\\n',
                                          command, f'echo {MARKER}\\n', 'disconnect'],
                                         None if succeeds else 'Cannot access memory at address',
                                         success_marker=MARKER)
                        if succeeds:
                            check_listing(output[output.index('START'):], [(0x2400, 'nop')])
                        else:
                            assert 'movb' not in output and '.byte' not in output
                    finally:
                        thread.join(timeout=12)
                        (out/f'{name}-rsp.json').write_text(json.dumps(server.commands, indent=2)+'\n')
                    assert not thread.is_alive()
                    assert server.error is None, server.error
            print(f'PASS {name}')


def naming(qemu, out, kind):
    # Diagnostic execution only: one MOVB per byte index, with independent
    # word expectations for every neighbor. V1 PDF pp.82-85 owns the mapping.
    name = kind+'-byte-execution'
    with machine(qemu, kind, out, name) as path:
        with connect(path, time.monotonic()+5) as sock:
            s = Scenario(Remote(sock))
            try:
                s.packet('?'); s.setreg('cp', 0xfc00)
                for i in range(16):
                    for reg in range(16): s.setreg(f'r{reg}', 0xa1b2)
                    s.setmem(0x2400, 0xe7 | ((0xf0+i)<<8))
                    s.setmem(0x2402, 0x005a)
                    s.setreg('pc', 0x2400)
                    assert s.packet('s').startswith('T05')
                    for reg in range(16):
                        expected = (0x5ab2 if i%2 else 0xa15a) if reg==i//2 else 0xa1b2
                        s.reg(f'r{reg}', expected)
                    s.reg('pc', 0x2404)
            finally:
                (out/f'{name}.log').write_text('\n'.join(s.transcript)+'\n')
    print(f'PASS {kind}: all 16 byte indices and all neighboring words')


def fixture_coverage(fixtures):
    coverage = {}
    assert {int(c['bytes'][:2], 16) for c in fixtures} == set(range(256))
    for line in (ROOT/'qemu/target/c166/insns.decode').read_text().splitlines():
        if not line or not line[0].isupper():
            continue
        name, *fields = line.split()
        bits = ''.join(f for f in fields if re.fullmatch('[.01]+', f))
        assert len(bits) == 16, (name, bits)
        mask = int(''.join('0' if c == '.' else '1' for c in bits), 2)
        value = int(bits.replace('.', '0'), 2)
        coverage[name] = [c['bytes'] for c in fixtures
                          if not c['text'].startswith('.byte') and
                          int.from_bytes(bytes.fromhex(c['bytes'])[:2], 'little') & mask == value]
        assert coverage[name], f'No manual fixture for QEMU pattern {name}'
    return coverage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', type=binary, required=True)
    parser.add_argument('--qemu', type=binary, required=True)
    parser.add_argument('--output', type=Path, default=ROOT/'emu/shots/gdb-c166-phase4/disassembly')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    (out/'binaries.json').write_text(json.dumps({str(p):digest(p) for p in
                                               (args.gdb,args.qemu,FIXTURES)},indent=2)+'\n')
    fixtures = json.loads(FIXTURES.read_text())['cases']
    (out/'coverage.json').write_text(json.dumps(fixture_coverage(fixtures), indent=2)+'\n')
    for kind in ('c166-test', 'pmb7850-test'):
        for state in (0, 1): listing_case(args.gdb,args.qemu,out,kind,fixtures,state)
        naming(args.qemu,out,kind)
    read_errors(args.gdb,out)
    print('PASS C166 disassembly gate')


if __name__ == '__main__':
    main()
