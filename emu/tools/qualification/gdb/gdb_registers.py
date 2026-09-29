#!/usr/bin/env python3
"""Focused real-GDB C166 register gate; no embedded Python or XML override."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import re
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

from ..shared.support import ROOT, binary, digest, terminate

sys.path.insert(0, str(ROOT / 'qemu/tests/functional/c166'))
from run_vectors import REGISTERS, Remote, connect, checksum


@contextmanager
def machine(qemu, kind, out, name, *, flash=None, qmp=False, extra_args=()):
    # Short temporary socket paths avoid the Unix-domain length limit.
    with tempfile.TemporaryDirectory(prefix='c166-reg-', dir='/tmp') as tmp:
        sock = Path(tmp) / 'gdb.sock'
        args = ['-M', kind, '-S', '-display', 'none', '-no-reboot',
                '-gdb', f'unix:{sock},server=on,wait=off',
                '-trace', 'enable=pmb7850_fast_lm']
        if flash is not None:
            args[1] += ',flash-type=m58-compatible-32mbit'
            args += ['-drive', f'if=pflash,format=raw,file={flash}']
        if qmp:
            args += ['-qmp', f'unix:{sock.with_name("qmp.sock")},server=on,wait=off']
        args += extra_args
        (out / f'{name}-command.json').write_text(
            json.dumps([str(qemu), *args], indent=2) + '\n')
        with (out / f'{name}-qemu.log').open('w') as log:
            proc = subprocess.Popen([str(qemu), *args], stdout=log, stderr=log)
            try:
                # Wait without connecting: GDB must be the first client.
                deadline = time.monotonic() + 5
                while not sock.exists():
                    if proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError(f'QEMU did not listen: {out / (name + "-qemu.log")}')
                    time.sleep(0.01)
                yield sock
            finally:
                terminate(proc)
                (out / f'{name}-qemu-result.json').write_text(json.dumps({
                    'pid': proc.pid, 'returncode': proc.returncode,
                    'socket': str(sock), 'reaped': proc.returncode is not None,
                }, indent=2) + '\n')


class Scenario:
    """Run the same expectations via raw RSP and a generated GDB command file."""
    def __init__(self, remote=None):
        self.remote = remote
        self.lines = []
        self.count = 0
        self.transcript = []

    def packet(self, command):
        result = self.remote.packet(command)
        self.transcript.append(f'{command} -> {result}')
        return result

    def check(self, expression, actual, expected):
        self.count += 1
        if self.remote:
            if actual != expected:
                raise AssertionError(f'{expression}: {actual:#x} != {expected:#x}')
        else:
            self.lines += [f'if ({expression}) != 0x{expected:x}',
                           f'  echo ASSERTION_{self.count}_FAILED {expression}\\n',
                           '  quit 1', 'end']

    def reg(self, name, expected):
        actual = (int.from_bytes(bytes.fromhex(self.packet(
            f'p{REGISTERS[name]:x}')), 'little') if self.remote else None)
        self.check(f'${name}', actual, expected)

    def setreg(self, name, value):
        if self.remote:
            size = 4 if name == 'pc' else 8 if name == 'guest_icount' else 2
            assert self.packet(f'P{REGISTERS[name]:x}=' +
                               value.to_bytes(size, 'little').hex()) == 'OK'
        else:
            self.lines.append(f'set ${name} = 0x{value:x}')

    def mem(self, address, expected):
        actual = (int.from_bytes(bytes.fromhex(self.packet(
            f'm{address:x},2')), 'little') if self.remote else None)
        self.check(f'*(unsigned short *)0x{address:x}', actual, expected)

    def setmem(self, address, value):
        if self.remote:
            assert self.packet(f'M{address:x},2:' +
                               value.to_bytes(2, 'little').hex()) == 'OK'
        else:
            self.lines.append(f'set {{unsigned short}}0x{address:x} = 0x{value:x}')

    def step_einit(self):
        # Only this setup instruction executes; execution control is Phase 5.
        self.setmem(0xf600, 0x4ab5)
        self.setmem(0xf602, 0xb5b5)
        self.setreg('pc', 0xf600)
        self.setreg('guest_icount', 0)
        if self.remote:
            assert self.packet('s').startswith('T05')
        else:
            self.lines.append('stepi')
        self.reg('pc', 0xf604)
        self.reg('guest_icount', 1)


def registers(s, kind):
    reset = dict.fromkeys(REGISTERS, 0)
    reset.update(dpp1=1, dpp2=2, dpp3=3, cp=0xfc00, sp=0xfc00,
                 stkov=0xfa00, stkun=0xfc00)
    for name, value in reset.items():
        s.reg(name, value)
        if not s.remote:
            size = 4 if name == 'pc' else 8 if name == 'guest_icount' else 2
            s.check(f'sizeof(${name})', None, size)
    if s.remote:
        assert len(bytes.fromhex(s.packet('g'))) == 86
    else:
        s.lines += ['maintenance print raw-registers', 'info registers general',
                    'info registers system']
        s.check('sizeof(void *)', None, 4)
    for name in REGISTERS:
        # CP and all registers with side effects are tested separately below.
        if name in ('cp', 'csp', 'ip', 'pc', 'syscon', 'guest_icount'):
            continue
        value = 0xa5b7
        masked = value
        if name.startswith('dpp'):
            masked &= 0x3ff
        elif name in ('sp', 'stkov', 'stkun'):
            masked &= 0xfffe
        elif name in ('extr', 'idle'):
            masked &= 1
        s.setreg(name, value)
        s.reg(name, masked)
    # Register round trips deliberately set TFR request flags too. Clear them
    # before executing EINIT: V1 PDF p.64 makes software-set flags actionable.
    for name in ('ext_kind', 'ext_value', 'ext_count', 'extr', 'idle', 'psw', 'tfr'):
        s.setreg(name, 0)
    s.reg('tfr', 0)
    s.setreg('syscon', 0x1234)
    s.reg('syscon', 0x1234)
    s.setreg('syscon', 0)
    for value in (0x00abcdef, 0xff987654):
        s.setreg('pc', value)
        s.reg('pc', value & 0xffffff)
        s.reg('csp', (value >> 16) & 0xff)
        s.reg('ip', value & 0xffff)
    s.setreg('csp', 0xffab)
    s.reg('csp', 0xab)
    s.reg('pc', 0xab7654)
    s.setreg('ip', 0x1235)
    s.reg('pc', 0xab1235)
    s.reg('ip', 0x1235)
    for value in (0x123456789abcdef0, 0xfedcba9876543210):
        s.setreg('guest_icount', value)
        s.reg('guest_icount', value)
    for bank, base in ((0xf800, 0x1100), (0xf900, 0x2200)):
        s.setreg('cp', bank | 1)
        s.reg('cp', bank)
        for i in range(16):
            s.setreg(f'r{i}', base+i)
            s.reg(f'r{i}', base+i)
            s.mem(bank+2*i, base+i)
    for bank, base in ((0xf800, 0x1100), (0xf900, 0x2200)):
        s.setreg('cp', bank)
        for i in range(16):
            s.reg(f'r{i}', base+i)
            s.setmem(bank+2*i, base+0x100+i)
            s.reg(f'r{i}', base+0x100+i)
    if kind == 'pmb7850-test':
        aliases = {'dpp0':0xfe00, 'dpp1':0xfe02, 'dpp2':0xfe04,
                   'dpp3':0xfe06, 'csp':0xfe08, 'mdh':0xfe0c,
                   'mdl':0xfe0e, 'cp':0xfe10, 'sp':0xfe12,
                   'stkov':0xfe14, 'stkun':0xfe16, 'mdc':0xff0e,
                   'psw':0xff10, 'syscon':0xff12}
        for name, address in aliases.items():
            value = 0x124 if name != 'cp' else 0xf800
            s.setreg(name, value)
            s.mem(address, value & 0xff if name == 'csp' else value)
            s.setmem(address, 0x235)
            expected = (0x24 if name == 'csp' else 0xf234
                        if name in ('sp','stkov','stkun') else 0x234
                        if name == 'cp' else 0x235)
            s.reg(name, expected)
            s.mem(address, expected)
        s.setreg('psw', 0)
        s.setreg('syscon', 0)


def mapping(s, kind):
    if kind != 'pmb7850-test':
        return
    s.setmem(0x200, 0x1111)  # external backing
    s.setmem(0xff12, 0x400)
    s.setmem(0x200, 0x2222)  # internal local-memory backing
    s.mem(0x200, 0x2222)
    for via_reg in (False, True):
        setter = (lambda v: s.setreg('syscon', v)) if via_reg else (
            lambda v: s.setmem(0xff12, v))
        setter(0)
        s.reg('syscon', 0)
        s.mem(0x200, 0x1111)
        setter(0x400)
        s.mem(0x200, 0x2222)
        setter(0x1400)
        s.mem(0x10200, 0x2222)
        s.mem(0x200, 0x1111)
        setter(0x400)
    s.step_einit()
    s.setmem(0xff12, 0)  # guest-visible CSFR protection remains intact
    s.reg('syscon', 0x400)
    s.mem(0x200, 0x2222)
    s.setreg('syscon', 0)  # debugger override is deliberately allowed
    s.reg('syscon', 0)
    s.mem(0xff12, 0)
    s.mem(0x200, 0x1111)
    s.setreg('syscon', 0x1400)
    s.mem(0x10200, 0x2222)
    s.mem(0x200, 0x1111)


def gdb_run(gdb, out, name, lines, expected_error=None,
            success_marker="C166_REGISTERS_OK", *, timeout=30, cwd=None):
    script = out / f'{name}.gdb'
    script.write_text('\n'.join(['set pagination off', 'set confirm off',
                                'set remotetimeout 5', *lines]) + '\n')
    command = [str(gdb), '-nx', '-nh', '-batch', '-iex', 'set auto-load off',
               '-x', str(script)]
    (out / f'{name}-gdb-command.json').write_text(json.dumps(command) + '\n')
    status = {'timeout_seconds': timeout, 'cwd': str(cwd) if cwd else None}
    # A file survives timeout/interrupt, unlike output captured only after run().
    # subprocess.run kills and reaps GDB on timeout or an interrupted wait.
    try:
        with (out / f'{name}.log').open('w') as log:
            result = subprocess.run(command, text=True, stdin=subprocess.DEVNULL,
                                    stdout=log, stderr=subprocess.STDOUT,
                                    timeout=timeout, cwd=cwd)
        status.update(status='exited', returncode=result.returncode)
    except subprocess.TimeoutExpired:
        status.update(status='timeout')
        raise
    except BaseException:
        status.update(status='interrupted-or-launch-failed')
        raise
    finally:
        (out / f'{name}-result.json').write_text(json.dumps(status, indent=2) + '\n')
    output = (out / f'{name}.log').read_text()
    if expected_error:
        assert result.returncode != 0 and expected_error in output, output
    else:
        assert result.returncode == 0, output
        assert success_marker in output.splitlines(), f'Missing completion marker: {output}'
        for message in ('Architecture rejected', 'Truncated register',
                        'Could not load XML', 'ASSERTION_', 'internal-error'):
            assert message not in output, output
    return output


def real_case(gdb, qemu, out, kind):
    name = kind + '-raw'
    with machine(qemu, kind, out, name) as path:
        with connect(path, time.monotonic()+5) as sock:
            s = Scenario(Remote(sock))
            try:
                s.packet('?')
                assert 'qXfer:features:read+' in s.packet('qSupported')
                xml = s.packet('qXfer:features:read:target.xml:0,1000')
                assert '<architecture>c166</architecture>' in xml, xml
                registers(s, kind)
                mapping(s, kind)
            finally:
                (out / f'{name}.log').write_text('\n'.join(s.transcript)+'\n')
    name = kind + '-gdb'
    with machine(qemu, kind, out, name) as path:
        s = Scenario()
        registers(s, kind)
        mapping(s, kind)
        output = gdb_run(gdb, out, name,
                        [f'target remote {path}', 'show architecture', *s.lines,
                         'bt', 'echo C166_REGISTERS_OK\\n', 'disconnect', 'quit 0'])
        assert 'currently "c166"' in output, output
        assert '#0 ' in output and '#1 ' not in output, output
    print(f'PASS {kind}: {s.count} real-GDB assertions plus raw RSP comparison')


class DescriptionServer:
    """Small ACK-mode RSP peer for invalid *remote* XML (never a local override)."""
    def __init__(self, listener, xml):
        self.listener, self.xml = listener, xml
        self.commands = []
        self.error = None

    def response(self, command):
        if command.startswith('qSupported'):
            response = 'PacketSize=1000;qXfer:features:read+'
        elif command.startswith('qXfer:features:read:target.xml:'):
            offset, size = (int(x, 16) for x in command.rsplit(':',1)[1].split(','))
            response = ('l' if offset+size >= len(self.xml) else 'm') + self.xml[offset:offset+size]
        elif command == '?': response = 'T05thread:1;'
        elif command.startswith('H'): response = 'OK'
        elif command == 'qC': response = 'QC1'
        elif command == 'qfThreadInfo': response = 'm1'
        elif command == 'qsThreadInfo': response = 'l'
        elif command == 'g': response = '00'*86
        elif command.startswith('D'): response = 'OK'
        else: response = ''
        return response

    def run(self):
        try:
            self.listener.settimeout(10)
            conn, _ = self.listener.accept()
            with conn:
                conn.settimeout(10)
                while True:
                    c = conn.recv(1)
                    if not c:
                        return
                    if c != b'$':
                        continue
                    payload = bytearray()
                    while True:
                        c = conn.recv(1)
                        if not c:
                            return
                        if c == b'#':
                            break
                        payload += c
                    received = conn.recv(2)
                    assert received == checksum(payload)
                    conn.sendall(b'+')
                    command = payload.decode()
                    self.commands.append(command)
                    response = self.response(command)
                    data = response.encode()
                    conn.sendall(b'$'+data+b'#'+checksum(data))
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as error:
            self.error = error


def malformed(gdb, out):
    for case in ('feature', 'missing', 'name', 'number', 'width', 'pointer', 'pc-pointer', 'shadow-type'):
        feature = ET.parse(ROOT / 'qemu/gdbstub/gdb-xml/c166-core.xml').getroot()
        if case == 'feature': feature.set('name', 'org.invalid.core')
        if case == 'missing': feature.remove(feature[-1])
        if case == 'name': feature[0].set('name','wrong')
        if case == 'number': feature[0].set('regnum','1')
        if case == 'width': feature[0].set('bitsize','32')
        if case == 'pointer': feature[20].set('type','data_ptr')
        if case == 'pc-pointer': feature[30].set('type','data_ptr')
        if case == 'shadow-type':
            feature.insert(0, ET.Element('vector', id='uint16', type='uint8', count='4'))
        xml = '<target><architecture>c166</architecture>'+ET.tostring(feature,encoding='unicode')+'</target>'
        with tempfile.TemporaryDirectory(prefix='c166-xml-',dir='/tmp') as tmp:
            path = Path(tmp)/'gdb.sock'
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(str(path)); listener.listen(1)
                server = DescriptionServer(listener, xml)
                thread = threading.Thread(target=server.run, daemon=True)
                thread.start()
                try:
                    gdb_run(gdb,out,'invalid-'+case,[f'target remote {path}'],
                            'Invalid C166 target description:')
                finally:
                    thread.join(timeout=12)
                    (out/f'invalid-{case}-rsp.json').write_text(json.dumps(server.commands,indent=2)+'\n')
                assert not thread.is_alive()
                assert server.error is None, server.error
                assert 'g' not in server.commands, 'Invalid XML must fail before register access'
        print(f'PASS invalid remote description: {case}')


def unsupported(gdb, qemu, out):
    cases = {
        'inferior-call': (['call ((void (*)())0xf700)()'], 'does not support function calls'),
        'memory-breakpoint': (['set remote software-breakpoint-packet off',
                              'set breakpoint always-inserted on', 'break *0xf700'],
                             'memory patching is disabled'),
    }
    for name, (commands, error) in cases.items():
        with machine(qemu, 'c166-test', out, name) as path:
            gdb_run(gdb, out, name, [f'target remote {path}', *commands], error)
    print('PASS explicit unsupported-operation boundaries')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', type=binary, required=True)
    parser.add_argument('--qemu', type=binary, required=True)
    parser.add_argument('--output', type=Path, default=ROOT/'emu/shots/gdb-c166-phase3/registers')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out/'binaries.json').write_text(json.dumps({str(p):digest(p) for p in (args.gdb,args.qemu)},indent=2)+'\n')
    disconnected = gdb_run(args.gdb,out,'disconnected',[
        'set architecture c166', 'show architecture', 'maintenance print registers',
        'echo C166_REGISTERS_OK\\n'])
    rows = re.findall(r'^(\w+)\s+(\d+)\s+\d+\s+\d+\s+(\d+)\s+',
                      disconnected, re.MULTILINE)
    assert [(n,int(i),int(size)) for n,i,size in rows] == [
        (n,i,4 if n == 'pc' else 8 if n == 'guest_icount' else 2)
        for n,i in REGISTERS.items()], rows
    for kind in ('c166-test', 'pmb7850-test'):
        real_case(args.gdb,args.qemu,out,kind)
    malformed(args.gdb,out)
    unsupported(args.gdb,args.qemu,out)
    print('PASS C166 register gate')
    return 0


if __name__ == '__main__':
    sys.exit(main())
