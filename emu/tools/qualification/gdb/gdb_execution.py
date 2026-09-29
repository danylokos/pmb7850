#!/usr/bin/env python3
"""Real MI2 GDB execution gate, with independent QMP code snapshots."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import queue
import re
import subprocess
import threading
import time

from .gdb_registers import Scenario, connect, machine
from ..shared.qmp import QMP
from ..shared.support import ROOT, FIXTURE_DIR, binary, digest, terminate

FIXTURES = FIXTURE_DIR / 'c166-execution.json'


def mi_fields(text):
    """Parse MI result tuples/lists, preserving repeated named list entries."""
    pos = 0

    def value():
        nonlocal pos
        if text[pos] == '"':
            # GDB emits C strings; the fields used here use JSON-compatible escapes.
            result, size = json.JSONDecoder().raw_decode(text[pos:])
            pos += size
            return result
        opening = text[pos]
        assert opening in '[{', text[pos:]
        pos += 1
        result = fields('}' if opening == '{' else ']')
        pos += 1
        return dict(result) if opening == '{' else result

    def fields(end=None):
        nonlocal pos
        result = []
        while pos < len(text) and text[pos] != end:
            match = re.match(r'([\w-]+)=', text[pos:])
            if match:
                pos += len(match[0])
                result.append((match[1], value()))
            else:
                result.append(value())
            if pos < len(text) and text[pos] == ',':
                pos += 1
            else:
                break
        return result

    result = dict(fields())
    assert pos == len(text), text[pos:]
    return result


class MI:
    def __init__(self, gdb, out, name):
        self.log = (out / f'{name}-mi.log').open('w')
        command = [str(gdb), '-nx', '-nh', '--interpreter=mi2',
                   '-iex', 'set auto-load off']
        (out / f'{name}-gdb-command.json').write_text(json.dumps(command)+'\n')
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1)
        self.lines = queue.Queue()
        self.records = []
        self.token = 0
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip('\n'))
        self.lines.put(None)

    def wait(self, predicate, deadline):
        while True:
            if time.monotonic() >= deadline:
                raise TimeoutError('GDB MI deadline expired')
            for i, line in enumerate(self.records):
                if predicate(line):
                    return self.records.pop(i)
            try:
                line = self.lines.get(timeout=max(0, deadline-time.monotonic()))
            except queue.Empty as error:
                raise TimeoutError('GDB MI deadline expired') from error
            if line is None:
                raise EOFError(f'GDB exited: {self.proc.poll()}')
            self.log.write('< '+line+'\n')
            self.log.flush()
            self.records.append(line)

    def command(self, command, timeout=30):
        self.token += 1
        token = str(self.token)
        self.log.write('> '+token+command+'\n')
        self.log.flush()
        self.proc.stdin.write(token+command+'\n')
        self.proc.stdin.flush()
        result = self.wait(lambda line: line.startswith(token+'^'), time.monotonic()+timeout)
        status, _, fields = result[len(token)+1:].partition(',')
        assert status in ('done', 'running', 'connected', 'exit'), result
        return mi_fields(fields) if fields else {}

    def cli(self, command, timeout=30):
        return self.command('-interpreter-exec console '+json.dumps(command), timeout)

    def event(self, name, deadline):
        line = self.wait(lambda line: line.startswith('*'+name+','), deadline)
        return mi_fields(line.split(',', 1)[1])

    def resume(self, command):
        # Consume each run/stop event once; stale events cannot satisfy a new run.
        assert not any(x.startswith(('*running,', '*stopped,')) for x in self.records)
        deadline = time.monotonic()+30
        # Console execution commands need '&' even when MI itself is async.
        self.cli(command+' &')
        self.event('running', deadline)
        return deadline

    def stop(self, deadline, reason, pc=None, bp=None):
        event = self.event('stopped', deadline)
        assert event.get('reason') == reason, event
        if pc is not None:
            assert int(event['frame']['addr'], 16) == pc, event
        if bp is not None:
            assert event.get('bkptno') == bp, event
        return event

    def execute(self, command, reason, pc=None, bp=None):
        return self.stop(self.resume(command), reason, pc, bp)

    def evaluate(self, expression):
        return int(self.command('-data-evaluate-expression '+json.dumps(
            '(unsigned long long)('+expression+')'))['value'], 0)

    def close(self):
        # Cleanup never makes an expired operation pass.
        terminate(self.proc)
        self.thread.join(timeout=5)
        self.proc.stdin.close()
        self.proc.stdout.close()
        self.log.close()


class Checks(Scenario):
    """Reuse fixture assertion and setup expressions from the register gate."""
    def __init__(self, mi):
        super().__init__()
        self.mi = mi

    def check(self, expression, actual, expected):
        self.count += 1
        actual = self.mi.evaluate(expression)
        assert actual == expected, f'{expression}: {actual:#x} != {expected:#x}'

    def flush(self):
        for line in self.lines:
            self.mi.cli(line)
        self.lines.clear()

    def write(self, addr, raw):
        data = bytes.fromhex(raw)
        # Word writes preserve mapped SFR semantics for synthetic interrupt setup.
        assert len(data) % 2 == 0
        for offset in range(0, len(data), 2):
            self.setmem(addr+offset, int.from_bytes(data[offset:offset+2], 'little'))
        self.flush()

    def expect(self, registers, memory=None):
        for name, value in registers.items():
            self.reg(name, value)
        for addr, raw in (memory or {}).items():
            data = bytes.fromhex(raw)
            for offset in range(0, len(data), 2):
                self.mem(int(addr, 0)+offset, int.from_bytes(data[offset:offset+2], 'little'))


class LoggedQMP(QMP):
    def __init__(self, sock, out, name):
        self.log = (out / f'{name}-qmp.jsonl').open('w')
        super().__init__(sock)

    def _receive(self):
        # Preserve asynchronous lifecycle events as well as replies.
        while True:
            line = self.file.readline()
            if not line:
                raise EOFError('QMP closed')
            self.log.write('< '+line.decode()); self.log.flush()
            result = json.loads(line)
            if 'event' not in result:
                return result

    def execute(self, command, arguments=None):
        self.log.write('> '+json.dumps(dict(execute=command, arguments=arguments))+'\n')
        self.log.flush()
        return super().execute(command, arguments)

    def close(self):
        super().close()
        self.log.close()

    def running(self, expected):
        result = self.execute('query-status')
        assert result['running'] == expected, result

    def snapshot(self, path, addr, size):
        self.execute('pmemsave', dict(val=addr, size=size, filename=str(path)))
        return path.read_bytes()


@contextmanager
def session(gdb, qemu, out, name, kind, flash=None, shift=0, extra_args=()):
    startup_deadline = time.monotonic()+5

    def remaining():
        seconds = startup_deadline-time.monotonic()
        if seconds <= 0:
            raise TimeoutError('GDB/QEMU startup exceeded five seconds')
        return seconds

    args = ['-icount', f'shift={shift},align=off,sleep=off',
            '-trace', 'enable=c166_irq_*', '-trace', 'enable=c166_window',
            '-trace', 'enable=pmb7850_irq', '-trace', 'enable=pmb7850_timer']
    args += list(extra_args)
    with machine(qemu, kind, out, name, flash=flash, qmp=True, extra_args=args) as sock:
        with connect(sock.with_name('qmp.sock'), startup_deadline) as qs:
            qs.settimeout(remaining())
            q = LoggedQMP(qs, out, name)
            m = MI(gdb, out, name)
            try:
                m.wait(lambda line: line.startswith('(gdb)'), startup_deadline)
                for command in ('set pagination off', 'set confirm off',
                                'set remotetimeout 5', 'set mi-async on',
                                'set breakpoint condition-evaluation host',
                                f'set remotelogfile {out / (name+"-rsp.log")}',
                                'set remotelogbase hex'):
                    m.cli(command, timeout=remaining())
                m.cli(f'target remote {sock}', timeout=remaining())
                m.event('stopped', startup_deadline)
                qs.settimeout(remaining())
                q.running(False)
                remaining()
                qs.settimeout(5)
                yield m, Checks(m), q, sock
            finally:
                m.close(); q.close()


def instructions(m, s, fixture):
    for case in fixture['cases']:
        m.log.write('# fixture '+case['name']+'\n')
        for name, value in (fixture['initial'] | case['initial']).items():
            s.setreg(name, value)
        s.flush()
        s.write(0x2000, case['code'])
        for addr, raw in case['memory'].items():
            s.write(int(addr, 0), raw)
        for step in case['steps']:
            m.execute(step['command'], 'end-stepping-range', step['registers']['pc'])
            s.expect(step['registers'], step['memory'])
    print(f'PASS instructions: {len(fixture["cases"])} fixtures, {s.count} assertions', flush=True)


def breakpoint(m, command):
    m.cli(command)
    table = m.command('-break-list')['BreakpointTable']['body']
    return table[-1][1]['number']


def breakpoints(m, s, q, out, name, addr, fixture, flash=False):
    code = fixture['code']
    if not flash:
        s.write(addr, code)
    s.setreg('pc', addr)
    s.setreg('r0', 0)
    s.setreg('guest_icount', 0)
    s.flush()

    def snapshot(label):
        raw = q.snapshot(out/f'{name}-{label}.bin', addr, 4)
        assert raw == bytes.fromhex(code), (label, raw.hex())

    snapshot('before')
    variables = dict(base=f'0x{addr:x}', next=f'0x{addr+2:x}')
    for action in fixture['actions']:
        command = action['command'].format(**variables)
        if 'save' in action:
            variables[action['save']] = breakpoint(m, command)
        elif 'stop' in action:
            stop = action['stop']
            m.execute(command, stop['reason'], addr+stop['offset'],
                      variables.get(stop.get('breakpoint')))
            s.expect(action['registers'])
        else:
            m.cli(command)
        if 'snapshot' in action:
            snapshot(action['snapshot'])
        if action.get('empty_breakpoints'):
            assert m.command('-break-list')['BreakpointTable']['body'] == []
    print(f'PASS breakpoints: {name}', flush=True)


def lifecycle(m, s, q, sock, code):
    addr = 0x2000
    s.write(addr, code)
    s.setreg('pc', addr); s.setreg('r0', 0); s.setreg('r5', 0xa55a)
    s.setreg('guest_icount', 0); s.flush()
    deadline = m.resume('continue')
    q.running(True)
    m.cli('interrupt')
    event = m.stop(deadline, 'signal-received')
    assert event['signal-name'] == 'SIGINT', event
    def consistent():
        pc, count = m.evaluate('$pc'), m.evaluate('$guest_icount')
        assert pc == addr+2*(count % 2), (pc, count)
        s.expect(dict(r0=((count+1)//2) & 0xffff, r5=0xa55a))
        return count
    first = consistent(); assert first > 0
    q.running(False)
    b = breakpoint(m, f'break *0x{addr:x}')
    m.execute('continue', 'breakpoint-hit', addr, b)
    m.cli(f'delete {b}')
    before = consistent()
    m.cli('detach')
    q.running(True)
    m.cli(f'target remote {sock}', timeout=5)
    m.event('stopped', time.monotonic()+5)
    q.running(False)
    after = consistent(); assert after > before, (after, before)
    pc = m.evaluate('$pc')
    m.execute('stepi', 'end-stepping-range', addr+2 if pc == addr else addr)
    assert consistent() == after+1
    print('PASS interrupt/detach/reconnect', flush=True)


def interrupt(m, s, kind, fixture):
    f = fixture['interrupt']
    s.write(0x2000, f['code']); s.write(f['vector'], f['vector_code'])
    s.setreg('pc', 0x2000); s.setreg('psw', f['initial_psw']); s.flush()
    if kind == 'pmb7850-test':
        s.setmem(0xff9a, f['pending_adeic'])
    else:
        s.setmem(0xe002, 0x8f29)  # synthetic CPU input: priority 15, vector A4
    s.flush()
    # Maintenance packets pass through the real GDB connection and RSP log.
    for query, expected in [('qqemu.sstepbits','ENABLE=1,NOIRQ=2,NOTIMER=4'),
                            ('qqemu.sstep','0x7')]:
        start = len(m.records)
        m.cli('maintenance packet '+query)
        streams = ''.join(x for x in m.records[start:] if x.startswith('~'))
        assert expected in streams, streams
    m.execute('stepi', 'end-stepping-range', f['step_pc'])
    s.expect(dict(guest_icount=f['step_icount'], sp=0xfc00, psw=f['initial_psw']))
    if kind == 'pmb7850-test':
        s.mem(0xff9a, f['pending_adeic'])
    b = breakpoint(m, f'break *0x{f["vector"]:x}')
    m.execute('continue', 'breakpoint-hit', f['vector'], b)
    s.expect(dict(guest_icount=1, sp=f['entry_sp'], psw=0xf800), {'0xfbfa':f['stack']})
    if kind == 'pmb7850-test':
        s.mem(0xff9a, f['acknowledged_adeic'])
    else:
        s.setmem(0xe002, 0); s.flush()  # board has no acknowledgement callback
    m.execute('stepi', 'end-stepping-range', f['return_pc'])
    s.expect(dict(guest_icount=f['return_icount'], sp=0xfc00, psw=f['initial_psw']))
    m.cli(f'delete {b}')
    print(f'PASS pending interrupt and RETI: {kind}', flush=True)


def timer(m, s, fixture, out, sampled=True):
    f = fixture['timer']
    s.write(0x2000, 'cc00' * (f['steps']+f['continue_instructions']+1))
    s.setreg('pc', 0x2000); s.setmem(f['counter'], 0)
    s.setmem(f['control'], f['control_value']); s.flush()
    values = []
    if not sampled:
        for i in range(f['unsampled_steps']):
            m.execute('stepi', 'end-stepping-range', 0x2002+2*i)
            s.reg('guest_icount', i+1)
        log = out/'pmb7850-test-timer-unsampled-qemu.log'
        # The next resume dispatches the previous step's overdue timer before
        # TCG applies NOTIMER; no debugger counter read is needed for that.
        assert 'pmb7850_timer icount=6 ' in log.read_text()
        value = m.evaluate(f'*(unsigned short *){f["counter"]}')
        assert value == f['expected_unsampled'], value
        assert 'pmb7850_timer icount=7 ' in log.read_text()
        (out/'timer-unsampled.json').write_text(json.dumps(dict(steps=7, value=value))+'\n')
        print(f'PASS T3 without intermediate reads: {value}', flush=True)
        return
    for i in range(f['steps']):
        m.execute('stepi', 'end-stepping-range', 0x2002+2*i)
        s.reg('guest_icount', i+1)
        values.append(m.evaluate(f'*(unsigned short *){f["counter"]}'))
    target = 0x2000+2*(f['steps']+f['continue_instructions'])
    b = breakpoint(m, f'break *0x{target:x}')
    m.execute('continue', 'breakpoint-hit', target, b)
    s.reg('guest_icount', f['steps']+f['continue_instructions'])
    final = m.evaluate(f'*(unsigned short *){f["counter"]}')
    (out/'timer-measurements.json').write_text(json.dumps(dict(steps=values, continued=final),indent=2)+'\n')
    assert values == f['expected_after_steps'], values
    assert final == f['expected_after_continue'], final
    print(f'PASS T3: steps={values}, continuation={final}', flush=True)


def check_packets(out):
    summary = {}
    for path in out.glob('*-rsp.log'):
        wire = b''.join(bytes.fromhex(line[3:].split('<')[0])
                        for line in path.read_text().splitlines()
                        if line.startswith('w  '))
        packets = [p.decode() for p in re.findall(rb'\$([Zz][01],[^#]*)#', wire)]
        if packets:
            assert all(re.fullmatch(r'[Zz][01],[0-9a-f]+,2', p) for p in packets), packets
            summary[path.name] = packets
    for name in ('c166-test-external', 'c166-test-dpram',
                 'pmb7850-test-external', 'pmb7850-test-dpram', 'pmb7850-test-flash'):
        packets = summary[name+'-rsp.log']
        for prefix in ('Z0,', 'z0,', 'Z1,', 'z1,'):
            assert any(p.startswith(prefix) for p in packets), (name, prefix)
    (out/'breakpoint-packets.json').write_text(json.dumps(summary, indent=2)+'\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', type=binary, required=True)
    parser.add_argument('--qemu', type=binary, required=True)
    parser.add_argument('--output', type=Path, default=ROOT/'emu/shots/gdb-c166-phase5/execution')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    fixture = json.loads(FIXTURES.read_text())
    (out/'fixtures.json').write_text(FIXTURES.read_text())
    (out/'hashes.json').write_text(json.dumps({str(p):digest(p) for p in (
        args.gdb, args.qemu, FIXTURES, ROOT/'manuals/cpu/M166.PDF',
        ROOT/'manuals/cpu/c166s_v1.pdf')},indent=2)+'\n')
    for kind in ('c166-test', 'pmb7850-test'):
        with session(args.gdb,args.qemu,out,kind+'-step',kind) as (m,s,q,sock):
            instructions(m,s,fixture)
        for label,addr in [('external',0x2000),('dpram',0xf600)]:
            name = kind+'-'+label
            with session(args.gdb,args.qemu,out,name,kind) as (m,s,q,sock):
                breakpoints(m,s,q,out,name,addr,fixture['breakpoint'])
        with session(args.gdb,args.qemu,out,kind+'-lifecycle',kind) as (m,s,q,sock):
            lifecycle(m,s,q,sock,fixture['breakpoint']['code'])
        with session(args.gdb,args.qemu,out,kind+'-irq',kind) as (m,s,q,sock):
            interrupt(m,s,kind,fixture)
    flash = bytearray(b'\xff'*(4*1024*1024))
    flash[0x2000:0x2004] = bytes.fromhex(fixture['breakpoint']['code'])
    path = out/'breakpoint-flash.bin'; path.write_bytes(flash)
    name = 'pmb7850-test-flash'
    with session(args.gdb,args.qemu,out,name,'pmb7850-test',path) as (m,s,q,sock):
        breakpoints(m,s,q,out,name,0x802000,fixture['breakpoint'],True)
    for f in fixture['boundaries']:
        flash[0xf00:0x2000] = bytes.fromhex('cc00')*(0x1100//2)
        flash[0x2000:0x2002] = bytes.fromhex('0dff')
        if 'r0' in f:
            flash[0xffe:0x1002] = bytes.fromhex('e6f03412')
        path = out/(f['name']+'-flash.bin'); path.write_bytes(flash)
        with session(args.gdb,args.qemu,out,f['name'],'pmb7850-test',path) as (m,s,q,sock):
            s.setreg('pc', f['start']); s.flush()
            before = q.snapshot(out/(f['name']+'-before.bin'),0x800f00,0x1104)
            m.cli('set breakpoint always-inserted on')
            b = breakpoint(m, f'break *0x{f["stop"]:x}')
            assert q.snapshot(out/(f['name']+'-installed.bin'),0x800f00,0x1104) == before
            m.execute('continue','breakpoint-hit',f['stop'],b)
            s.expect({k:v for k,v in f.items() if k in ('guest_icount','r0')})
            m.cli(f'delete {b}')
            assert q.snapshot(out/(f['name']+'-removed.bin'),0x800f00,0x1104) == before
            print('PASS '+f['name'],flush=True)
    with session(args.gdb,args.qemu,out,'pmb7850-test-timer','pmb7850-test',shift=10) as (m,s,q,sock):
        timer(m,s,fixture,out)
    with session(args.gdb,args.qemu,out,'pmb7850-test-timer-unsampled','pmb7850-test',shift=10) as (m,s,q,sock):
        timer(m,s,fixture,out,sampled=False)
    check_packets(out)
    (out/'generated-hashes.json').write_text(json.dumps(
        {p.name:digest(p) for p in out.glob('*.bin')},indent=2)+'\n')
    print('PASS C166 execution gate',flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
