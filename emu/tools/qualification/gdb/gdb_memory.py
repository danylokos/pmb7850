#!/usr/bin/env python3
"""Bounded real-GDB memory/watchpoint and instruction replay qualification."""
from __future__ import annotations
import argparse
import fnmatch
import json
from pathlib import Path
import re
import traceback

from .gdb_execution import session as execution_session, breakpoint
from . import gdb_stack_semantics as stack_contracts
from ..shared.support import ROOT, FIXTURE_DIR, binary, digest

def session(*args, access_trace=True, **kwargs):
    return execution_session(*args, **kwargs, extra_args=[
        '-trace', 'enable=c166_entry', '-trace', 'enable=c166_restore',
        *(['-trace', 'enable=c166_access'] if access_trace else []),
        '-trace', 'enable=c166_watch_stop',
        '-trace', 'enable=c166_stack_trap',
        '-trace', 'enable=c166_trap_pending', '-trace', 'enable=c166_trap_delivery',
        '-trace', 'enable=pmb7850_reg', '-trace', 'enable=c166_test_mmio',
        '-trace', 'enable=c166_test_mmio_read'])


FIXTURES = FIXTURE_DIR / 'c166-memory.json'
REASONS = {'watch': ('watchpoint-trigger', 'wpt'),
           'rwatch': ('read-watchpoint-trigger', 'hw-rwpt'),
           'awatch': ('access-watchpoint-trigger', 'hw-awpt')}


def watch(m, command, addr, size):
    typ = {1: 'unsigned char', 2: 'unsigned short', 4: 'unsigned long'}[size]
    return breakpoint(m, f'{command} *({typ} *)0x{addr:x}')


def stop(m, deadline, command, number, pc):
    reason, field = REASONS[command]
    event = m.stop(deadline, reason, pc)
    assert event[field]['number'] == number, event
    return event


def setup(m, s, fixture, case):
    for name, value in (fixture['initial'] | case['initial']).items():
        s.setreg(name, value)
    s.flush()
    s.write(0x2000, case['code']+'cc00cc00')
    if case['pc'] != 0x2000+len(bytes.fromhex(case['code'])):
        s.write(case['pc'], 'cc00cc00')
    for addr, raw in case['setup_memory'].items():
        s.write(int(addr, 0), raw)


def replay(args, out, fixture, kind):
    count = 0
    for case in fixture['cases']:
        if kind not in case.get('machines', ('c166-test','pmb7850-test')):
            continue
        for i, (command, addr, size) in enumerate(case['watches']):
            name = f'{kind}-{case["name"]}-{i}'
            with session(args.gdb, args.qemu, out, name, kind) as (m,s,q,_):
                setup(m,s,fixture,case)
                number = watch(m, command, addr, size)
                stop(m,m.resume('continue'),command,number,case['pc'])
                s.expect(case['expected'] | {'pc':case['pc'], 'guest_icount':1},case['memory'])
                # Physical RAM snapshot is independent of GDB's register cache.
                for a,raw in case['memory'].items():
                    actual = q.snapshot(out/f'{name}-{a}.bin',int(a,0),len(bytes.fromhex(raw)))
                    assert actual.hex() == raw, (name,a,actual.hex(),raw)
                m.cli(f'delete {number}')
                m.execute('stepi','end-stepping-range',case['pc']+2)
                s.reg('guest_icount',2)
                count += 1
                print('PASS '+name,flush=True)
    return count


def semantics(args,out,kind):
    name=kind+'-semantics'
    with session(args.gdb,args.qemu,out,name,kind) as (m,s,q,_):
        # two same writes, one read, a changed write, and a loop
        s.write(0x2000,'f6f00030f6f00030f2f10030e6f07856f6f00030cc000df3')
        s.setreg('pc',0x2000);s.setreg('r0',0x1234);s.flush()
        w=watch(m,'watch',0x3000,2)
        stop(m,m.resume('continue'),'watch',w,0x2004)
        s.reg('guest_icount',1)
        event=stop(m,m.resume('continue'),'watch',w,0x2014)
        assert event['value']=={'old':'4660','new':'22136'},event
        s.reg('guest_icount',5)
        # Debugger edits and inspection never raise CPU watches.
        m.cli('set breakpoint always-inserted on')
        s.setmem(0x3000,0x5678);s.flush();s.mem(0x3000,0x5678)
        m.execute('stepi','end-stepping-range',0x2016);s.reg('guest_icount',6)
        m.cli(f'disable {w}')
        s.setreg('pc',0x2000);s.setreg('r0',0x9999);s.flush()
        m.execute('stepi','end-stepping-range',0x2004)
        m.cli(f'enable {w}')
        s.setreg('r0',0xabcd);s.flush()
        stop(m,m.resume('stepi'),'watch',w,0x2008)
        m.cli(f'delete {w}')
        r=watch(m,'rwatch',0x3000,2)
        stop(m,m.resume('continue'),'rwatch',r,0x200c)
        m.cli(f'delete {r}')
        a=watch(m,'awatch',0x3000,2)
        stop(m,m.resume('continue'),'awatch',a,0x2014)
        m.cli(f'delete {a}')
    print('PASS semantics '+kind,flush=True)


def access_paths(args,out,kind):
    name=kind+'-four-byte-range'
    with session(args.gdb,args.qemu,out,name,kind) as (m,s,q,_):
        s.write(0x2000,'f6f00030f6f00030cc00')
        s.setreg('pc',0x2000);s.setreg('r0',0x1234);s.flush()
        number=watch(m,'awatch',0x3000,4)
        stop(m,m.resume('continue'),'awatch',number,0x2004)
        # Access watches report an unchanged second write as well.
        stop(m,m.resume('continue'),'awatch',number,0x2008)
        s.expect({'guest_icount':2},{'0x3000':'34120000'})
        m.cli(f'delete {number}')
    # Adjacent byte negative case, CP bank change, and warm insertion/removal.
    for cp in (0xfc00,0xfc20):
        name=f'{kind}-warm-{cp:x}'
        with session(args.gdb,args.qemu,out,name,kind,access_trace=False) as (m,s,q,_):
            s.write(0xf600,'e7f13400cc000dfb')
            s.setreg('cp',cp);s.setreg('r0',0x5678);s.setreg('pc',0xf600);s.flush()
            end=breakpoint(m,'break *0xf604')
            m.execute('continue','breakpoint-hit',0xf604,end)
            neighbor=watch(m,'awatch',cp,1)
            s.setreg('pc',0xf600);s.flush()
            m.execute('continue','breakpoint-hit',0xf604,end)
            s.reg('r0',0x3478)
            m.cli(f'delete {neighbor}');m.cli(f'delete {end}')
            w=watch(m,'awatch',cp+1,1)
            s.setreg('pc',0xf600);s.flush()
            stop(m,m.resume('continue'),'awatch',w,0xf604)
            m.cli(f'delete {w}')
            s.setreg('pc',0xf600);s.flush()
            m.execute('stepi','end-stepping-range',0xf604)
    name=kind+'-multiple-overlap'
    with session(args.gdb,args.qemu,out,name,kind) as (m,s,q,_):
        s.write(0x2000,'f6f00030f6f00031cc00');s.setreg('pc',0x2000)
        s.setreg('r0',0x1234);s.flush()
        first=watch(m,'awatch',0x3001,2)
        second=watch(m,'awatch',0x3100,2)
        stop(m,m.resume('continue'),'awatch',first,0x2004)
        stop(m,m.resume('continue'),'awatch',second,0x2008)
        s.reg('guest_icount',2)
        m.execute('stepi','end-stepping-range',0x200a)
    name=kind+'-warm-word-read'
    with session(args.gdb,args.qemu,out,name,kind,access_trace=False) as (m,s,q,_):
        s.write(0xf600,'f6f00030cc00');s.setreg('pc',0xf600);s.flush()
        end=breakpoint(m,'break *0xf604')
        m.execute('continue','breakpoint-hit',0xf604,end)
        m.cli(f'delete {end}')
        w=watch(m,'rwatch',0xfc00,2)
        s.setreg('pc',0xf600);s.flush()
        stop(m,m.resume('continue'),'rwatch',w,0xf604)
        m.cli(f'delete {w}')
        s.setreg('pc',0xf600);s.flush()
        m.execute('stepi','end-stepping-range',0xf604)
    print('PASS access paths '+kind,flush=True)


def interrupt(args,out,kind):
    for addr in (0xfbfe,0xfbfc,0xfbfa):
        name=f'{kind}-irq-{addr:x}'
        with session(args.gdb,args.qemu,out,name,kind) as (m,s,q,_):
            s.write(0x2000,'cc00cc00');s.write(0xa4,'fb88')
            s.setreg('pc',0x2000);s.setreg('psw',0x800);s.flush()
            s.setmem(0xff9a if kind=='pmb7850-test' else 0xe002,
                     0xfc if kind=='pmb7850-test' else 0x8f29);s.flush()
            w=watch(m,'awatch',addr,2)
            # Default NOIRQ still holds during an ordinary step.
            m.execute('stepi','end-stepping-range',0x2002)
            s.expect({'sp':0xfc00,'guest_icount':1})
            stop(m,m.resume('continue'),'awatch',w,0xa4)
            s.expect({'sp':0xfbfa,'guest_icount':1,'psw':0xf800},
                     {'0xfbfa':'022000000008'})
            assert q.snapshot(out/f'{name}-stack.bin',0xfbfa,6).hex()=='022000000008'
            if kind=='c166-test': s.setmem(0xe002,0);s.flush()
            else: s.mem(0xff9a,0x7c)
            m.cli(f'delete {w}')
            m.execute('stepi','end-stepping-range',0x2002)
            s.expect({'sp':0xfc00,'guest_icount':2,'psw':0x800})
    print('PASS interrupt entry '+kind,flush=True)


def mmio(args,out,kind):
    if kind=='c166-test':
        for label,code,watched in [('copy','c801',0x3000),('post','d801',0xfc00)]:
            name=kind+'-mmio-'+label
            with session(args.gdb,args.qemu,out,name,kind) as (m,s,q,_):
                s.write(0x2000,code+'cc00');s.setreg('pc',0x2000)
                s.setreg('r0',0x3000);s.setreg('r1',0xe004);s.flush()
                w=watch(m,'watch',watched,2)
                stop(m,m.resume('continue'),'watch',w,0x2002)
                s.expect({'guest_icount':1,'r0':0x3002 if label=='post' else 0x3000},
                         {'0x3000':'0100'})
                lines=(out/(name+'-qemu.log')).read_text()
                assert lines.count('c166_test_mmio_read offset=0x4')==1,lines
                # Inspection is outside CPU watchpoints but is a device access.
                s.mem(0xe004,2)
                assert q.snapshot(out/(name+'-inspection.bin'),0xe004,2).hex()=='0300'
                m.cli(f'delete {w}')
                m.execute('stepi','end-stepping-range',0x2004)
    else:
        name=kind+'-mmio-load'
        with session(args.gdb,args.qemu,out,name,kind) as (m,s,q,_):
            s.write(0x2000,'f2f042fecc00');s.setreg('pc',0x2000);s.setreg('r0',0xa5);s.flush()
            w=watch(m,'watch',0xfc00,2)
            stop(m,m.resume('continue'),'watch',w,0x2004)
            s.expect({'r0':0,'guest_icount':1})
            lines=(out/(name+'-qemu.log')).read_text()
            assert lines.count('pmb7850_reg write=0 addr=0x00fe42 ')==1,lines
    print('PASS MMIO effects '+kind,flush=True)


def conditional_cases():
    """M166 PDF pp.75/81; C166S V1 PDF pp.49,88-89: bitoff and EXTR."""
    operands = [('ram', 0x10, 0xfd20, 0), ('gpr', 0xf0, 0xfc00, 0),
                ('sfr', 0x9e, 0xff3c, 0), ('esfr-last', 0x9e, 0xf13c, 1),
                ('esfr-retained', 0x9e, 0xf13c, 2)]
    for mnemonic, opcode in [('jbc', 0xaa), ('jnbs', 0xba)]:
        for operand, bitoff, addr, window in operands:
            for bit in (0, 15):
                for taken in (False, True):
                    previous = int(taken == (mnemonic == 'jbc'))
                    before = (0xa55a & ~(1 << bit)) | (previous << bit)
                    after = before ^ (1 << bit) if taken else before
                    pc = 0x2002 if window else 0x2000
                    yield {
                        'name': f'{mnemonic}-{operand}-b{bit}-' +
                                ('taken' if taken else 'fallthrough'),
                        'manual': 'M166 PDF pp.75/81; C166S V1 PDF pp.49,88-89',
                        'code': (f'd1{0x80 + (window-1)*0x10:02x}' if window else '') +
                                bytes([opcode, bitoff, 2, bit << 4]).hex(),
                        'addr': addr, 'window': window, 'before': before,
                        'after': after, 'taken': taken, 'instruction_pc': pc,
                        'pc': pc + (8 if taken else 4),
                        'psw': 0x40 | (1 if previous else 8),
                    }


def committed_accesses(log, pc):
    """Only committed CPU accesses; debugger reads have no c166_access event."""
    pattern = (rf'c166_access pc=0x{pc:06x} count=(\d+) addr=0x([0-9a-f]+) '
               r'size=(\d+) write=([01]) value=0x([0-9a-f]+)')
    return [[int(count), int(addr, 16), int(size), int(write), int(value, 16)]
            for count, addr, size, write, value in re.findall(pattern, log)]


def conditional(args, out, fixture, kind, case, command):
    name = f'{kind}-conditional-{case["name"]}-{command}'
    addr, pc = case['addr'], case['instruction_pc']
    count = 2 if case['window'] else 1
    expected_accesses = [[count, addr, 2, 0, case['before']]]
    if case['taken']:
        expected_accesses.append([count, addr, 2, 1, case['after']])
    watched = command in ('rwatch', 'awatch') or (command == 'watch' and case['taken'])
    with session(args.gdb, args.qemu, out, name, kind) as (m, s, q, _):
        for reg, value in (fixture['initial'] | {'psw': 0x5f}).items():
            s.setreg(reg, value)
        s.flush()
        s.write(0x2000, case['code'] + 'cc00cc00cc00cc00')
        # Distinct normal/extended operands expose an early EXTR retirement.
        if case['window']:
            s.setmem(0xff3c, 0xbeef)
        s.setmem(addr, case['before']); s.flush()
        if case['window']:
            m.execute('stepi', 'end-stepping-range', pc)
        number = watch(m, command, addr, 2) if command != 'none' else None
        # A code breakpoint at the watch stop PC changes GDB's MI attribution.
        # Put timeout guards one NOP later when a watch is expected to stop us.
        ends = {dest: breakpoint(m, f'break *0x{dest:x}')
                for dest in ((pc+6, pc+10) if watched else (pc+4, pc+8))}
        # Continue avoids the separately retained generic step/watch/resume bug.
        event = m.event('stopped', m.resume('continue'))
        registers = {reg: m.evaluate('$'+reg) for reg in
                     ('pc', 'psw', 'guest_icount', 'extr', 'ext_count', 'sp', 'cp')}
        memory = q.snapshot(out/(name+'-operand.bin'), addr, 2).hex()
        sentinel = (q.snapshot(out/(name+'-sfr.bin'), 0xff3c, 2).hex()
                    if case['window'] else None)
        log = (out/(name+'-qemu.log')).read_text()
        accesses = committed_accesses(log, pc)
        # Bound device callbacks by CPU entry and its last committed access,
        # excluding GDB/QMP expression reads before execution and after stopping.
        start = log.index(f'c166_entry pc=0x{pc:06x} ')
        end = log.rindex(f'c166_access pc=0x{pc:06x} ')
        device_reads = re.findall(r'c166_test_mmio_read offset=0x6 value=0x([0-9a-f]+)',
                                  log[start:end])
        observed = {'registers': registers, 'memory': memory, 'sentinel': sentinel,
                    'accesses': accesses, 'device_reads': device_reads}
        (out/(name+'.json')).write_text(json.dumps(
            {'case': case, 'command': command, 'stop': event, 'observed': observed,
             'expected_accesses': expected_accesses}, indent=2)+'\n')
        if watched:
            reason, field = REASONS[command]
            assert event.get('reason') == reason and event[field]['number'] == number, event
        else:
            assert event.get('reason') == 'breakpoint-hit', event
            assert event.get('bkptno') == ends[case['pc']], event
        assert int(event['frame']['addr'], 16) == case['pc'], event
        assert registers == {'pc': case['pc'], 'psw': case['psw'], 'guest_icount': count,
                             'extr': int(case['window'] == 2),
                             'ext_count': max(0, case['window']-1),
                             'sp': 0xfc00, 'cp': 0xfc00}, observed
        assert memory == case['after'].to_bytes(2, 'little').hex(), observed
        assert sentinel == ('efbe' if case['window'] else None), observed
        assert accesses == expected_accesses, observed
        assert device_reads == ([f'{case["before"]:x}']
                                if kind == 'c166-test' and case['window'] else []), observed
        if command != 'none':
            baseline = out/f'{kind}-conditional-{case["name"]}-none.json'
            assert observed == json.loads(baseline.read_text())['observed'], observed
        if command == 'watch' and case['taken']:
            # Re-enter the same instruction with another value and another
            # watch: a saved operand must not leak past instruction completion.
            m.cli('delete')
            s.setmem(addr, case['before'] ^ 2)
            for reg, value in {'pc': 0x2000, 'psw': 0x5f, 'guest_icount': 0,
                               'extr': 0, 'ext_count': 0}.items():
                s.setreg(reg, value)
            s.flush()
            if case['window']:
                m.execute('stepi', 'end-stepping-range', pc)
            again = watch(m, 'watch', addr, 2)
            breakpoint(m, f'break *0x{pc+6:x}')
            breakpoint(m, f'break *0x{pc+10:x}')
            stop(m, m.resume('continue'), 'watch', again, case['pc'])
            s.expect({'guest_icount': count, 'psw': case['psw']},
                     {hex(addr): (case['after'] ^ 2).to_bytes(2, 'little').hex()})
    print('PASS '+name, flush=True)


def stack_cases(kind):
    """C166S V1 PDF pp.53,62-66,97-101; M166 pp.94,96,104.

    This matrix qualifies existing PUSH/POP/SCXT trap paths in linear-stack
    mode, outside protected windows. SCXT memory ordering preserves the
    existing measured model; it does not resolve the documented alias question.
    """
    for op in ('push', 'pop', 'pop-cp', 'scxt-imm', 'scxt-memory', 'scxt-cp'):
        for segmented in (False, True):
            for crossed in (False, True):
                pop = op.startswith('pop')
                pc = 0x402000 if segmented else 0x2000
                after_sp = 0xfb82 if pop else 0xfb7e
                source = 0xe006 if kind == 'c166-test' else 0x3000
                code = {'push': 'ecf0', 'pop': 'fcf0', 'pop-cp': 'fc08',
                        'scxt-imm': 'c6f07856', 'scxt-cp': 'c60820fc',
                        'scxt-memory': 'd6f0'+source.to_bytes(2, 'little').hex()}[op]
                next_pc = pc+len(bytes.fromhex(code))
                psw = 0x2057 if op == 'pop' else 0x2047 if op == 'pop-cp' else 0x2046
                initial = {'pc': pc, 'psw': 0x2046, 'sp': 0xfb80, 'r0': 0x1234,
                           'syscon': 0xe000 | (0 if segmented else 0x800),
                           'dpp0': 0, 'dpp1': 1, 'dpp2': 2, 'dpp3': 3}
                initial['stkun' if pop else 'stkov'] = after_sp + (
                    (-2 if pop else 2) if crossed else 0)
                accesses = []
                def access(addr, write, value):
                    accesses.append([1, addr, 2, int(write), value])
                if pop:
                    access(0xfb80, False, 0xfc20 if op == 'pop-cp' else 0x8000)
                    if op == 'pop':
                        access(0xfc00, True, 0x8000)
                else:
                    if op != 'scxt-cp':
                        access(0xfc00, False, 0x1234)
                    access(0xfb7e, True, 0xfc00 if op == 'scxt-cp' else 0x1234)
                    if op == 'scxt-memory':
                        access(source, False, 0x5678)
                    if op in ('scxt-imm', 'scxt-memory'):
                        access(0xfc00, True, 0x5678)
                trigger = list(accesses)
                words = [psw] + ([pc >> 16] if segmented else []) + [next_pc & 0xffff]
                frame = [after_sp-2*(i+1) for i in range(len(words))]
                if crossed:
                    for addr, word in zip(frame, words):
                        access(addr, True, word)
                cp = 0xfc20 if op in ('pop-cp', 'scxt-cp') else 0xfc00
                expected = {'pc': (0x18 if pop else 0x10) if crossed else next_pc,
                            'sp': frame[-1] if crossed else after_sp, 'cp': cp,
                            'r0': 0x9abc if cp == 0xfc20 else 0x8000 if op == 'pop'
                                  else 0x5678 if op.startswith('scxt') else 0x1234,
                            'psw': psw | 0xf000 if crossed else psw,
                            'tfr': (0x2000 if pop else 0x4000) if crossed else 0,
                            'guest_icount': 1, 'extr': 0, 'ext_count': 0}
                variants = [('none', 0)]
                for addr in dict.fromkeys(frame + [a[1] for a in trigger if a[3]]):
                    variants += [('watch', addr), ('awatch', addr)]
                for addr in dict.fromkeys([a[1] for a in trigger if not a[3]] + [frame[-1]]):
                    variants += [('rwatch', addr), ('awatch', addr)]
                yield {'name': f'{op}-' + ('seg' if segmented else 'noseg') +
                               ('-crossed' if crossed else '-equal'),
                       'manual': 'C166S V1 PDF pp.53,62-66,97-101; M166 pp.94,96,104',
                       'op': op, 'code': code, 'initial': initial, 'expected': expected,
                       'pc': pc, 'next_pc': next_pc, 'source': source,
                       'crossed': crossed, 'frame': frame, 'accesses': accesses,
                       'variants': list(dict.fromkeys(variants))}


def stack_setup(s, fixture, case):
    for reg, value in (fixture['initial'] | case['initial']).items():
        s.setreg(reg, value)
    s.flush()
    s.write(case['pc'], case['code']+'cc00cc00')
    s.write(0x10, 'cc00cc00cc00cc00cc00cc00')
    s.write(0xfb60, 'a5a5'*32)
    s.setmem(0xfc20, 0x9abc)
    if case['op'].startswith('pop'):
        s.setmem(0xfb80, 0xfc20 if case['op'] == 'pop-cp' else 0x8000)
    if case['op'] == 'scxt-memory':
        s.setmem(case['source'], 0x5678)
    s.flush()


def stack_bounds(args, out, fixture, kind, case, command, addr):
    name = f'{kind}-stack-{case["name"]}-{command}-{addr:x}'
    with session(args.gdb, args.qemu, out, name, kind) as (m, s, q, _):
        stack_setup(s, fixture, case)
        before = q.snapshot(out/(name+'-before.bin'), 0xfb60, 64)
        expected_memory = bytearray(before)
        for _, address, size, write, value in case['accesses']:
            if write and 0xfb60 <= address < 0xfba0:
                expected_memory[address-0xfb60:address-0xfb60+size] = value.to_bytes(size, 'little')
        hit = command != 'none' and any(
            a[1] == addr and (command == 'awatch' or bool(a[3]) == (command == 'watch'))
            for a in case['accesses'])
        number = watch(m, command, addr, 2) if command != 'none' else None
        # Keep guards off expected watch-stop PCs to preserve MI attribution.
        ends = {dest: breakpoint(m, f'break *0x{dest:x}') for dest in
                (case['next_pc']+(2 if hit else 0), 0x12 if hit else 0x10,
                 0x1a if hit else 0x18)}
        event = m.event('stopped', m.resume('continue'))
        registers = {reg: m.evaluate('$'+reg) for reg in case['expected']}
        memory = q.snapshot(out/(name+'-after.bin'), 0xfb60, 64).hex()
        log = (out/(name+'-qemu.log')).read_text()
        accesses = committed_accesses(log, case['pc'])
        start = log.index(f'c166_entry pc=0x{case["pc"]:06x} ')
        end = log.rindex(f'c166_access pc=0x{case["pc"]:06x} ')
        device_reads = re.findall(r'c166_test_mmio_read offset=0x6 value=0x([0-9a-f]+)',
                                  log[start:end])
        observed = {'registers': registers, 'memory': memory,
                    'accesses': accesses, 'device_reads': device_reads}
        (out/(name+'.json')).write_text(json.dumps(
            {'case': case, 'command': command, 'address': addr, 'stop': event,
             'observed': observed, 'expected_memory': expected_memory.hex()}, indent=2)+'\n')
        if hit:
            reason, field = REASONS[command]
            assert event.get('reason') == reason and event[field]['number'] == number, event
        else:
            assert event.get('reason') == 'breakpoint-hit', event
            assert event.get('bkptno') == ends[case['expected']['pc']], event
        assert int(event['frame']['addr'], 16) == case['expected']['pc'], event
        assert registers == case['expected'], observed
        assert memory == expected_memory.hex(), observed
        assert accesses == case['accesses'], observed
        assert device_reads == (['5678'] if kind == 'c166-test' and
                                case['op'] == 'scxt-memory' else []), observed
        if command != 'none':
            baseline = out/f'{kind}-stack-{case["name"]}-none-0.json'
            assert observed == json.loads(baseline.read_text())['observed'], observed
        if command == 'watch' and case['crossed'] and addr == case['frame'][-1]:
            # A completed trap must not leave its producer-skipping state set.
            m.cli('delete')
            leave_stack_handler(m, s)
            stack_setup(s, fixture, case)
            number = watch(m, command, addr, 2)
            for dest in (case['next_pc']+2, 0x12, 0x1a):
                breakpoint(m, f'break *0x{dest:x}')
            stop(m, m.resume('continue'), command, number, case['expected']['pc'])
            s.expect(case['expected'])
            assert q.snapshot(out/(name+'-repeat.bin'), 0xfb60, 64).hex() == expected_memory.hex()
    print('PASS '+name, flush=True)


def leave_stack_handler(m, s):
    # A PC override is not an architectural RETI. Unwind hardware priority
    # before reseeding a same-CPU repeat; do not reset internal replay fields.
    s.write(0x3000, 'e6d60000e60a00f0e60bfefffb88')
    s.setreg('pc', 0x3000)
    s.flush()
    for _ in range(4):
        event = m.event('stopped', m.resume('stepi'))
        assert event['reason'] == 'end-stepping-range', event


def stack_contract_setup(m, s, fixture, case):
    # Shared architectural seeding; never reset internal replay/service fields.
    m.command('-data-write-memory-bytes 0xf600 '+json.dumps('a5a5'*1024))
    for reg, value in (fixture['initial'] | case['initial']).items():
        s.setreg(reg, value)
    s.flush()
    for address, value in case['seeds'].items():
        s.setmem(address, value)
    s.write(case['initial']['pc'], case['code']+'cc00'*4)
    for address in case['ends']:
        s.write(address, 'cc00cc00')
    for address, code in case.get('programs', {}).items():
        s.write(address, code)
    s.flush()


def trap_replay_observation(m, q, out, name, log, states):
    accesses = [[int(a, 16), int(w), int(v, 16)] for a, w, v in re.findall(
        r'c166_access pc=0x[0-9a-f]+ count=\d+ addr=0x([0-9a-f]+) '
        r'size=2 write=([01]) value=0x([0-9a-f]+)', log)
        if 0xf600 <= int(a, 16) < 0xfc80]
    return dict(states=states, memory=q.snapshot(out/(name+'.bin'), 0xf600, 2048).hex(),
                stack_accesses=accesses,
                deliveries=re.findall(r'c166_trap_delivery [^\n]+', log),
                counted_reads=len(re.findall(r'c166_test_mmio_read offset=0x6 value=0x5678', log)))


def deferred_replay(args, out, fixture, kind, case, spec):
    command, address, hit = spec['command'], spec['address'], spec['hit']
    name = f'{kind}-{case["name"]}-{command}-{address:x}'
    record = dict(case=case, machine=kind, watch=spec, stage='setup')
    path = out/(name+'.json')
    baseline_path = out/f'{kind}-{case["name"]}-none-0.json'
    try:
        with session(args.gdb, args.qemu, out, name, kind) as (m, s, q, _):
            logpath = out/(name+'-qemu.log')
            repeat = (case['replay_repeat'] and command == 'awatch' and
                      address == case['replay_phases'][-1][2][-1][0])
            for iteration in range(2 if repeat else 1):
                if iteration:
                    leave_stack_handler(m, s)
                stack_contract_setup(m, s, fixture, case)
                offset = len(logpath.read_text())
                for _ in range(case['replay_prepare']):
                    event = m.event('stopped', m.resume('stepi'))
                    assert event['reason'] == 'end-stepping-range', event
                if case.get('irq'):
                    s.setmem(0xff9a if kind == 'pmb7850-test' else 0xe002,
                             0xfc if kind == 'pmb7850-test' else 0x8f29)
                    s.flush()
                before = {reg: m.evaluate('$'+reg) for reg in stack_contracts.REGISTERS}
                record['prepared' if not iteration else 'repeat_prepared'] = before
                armed = q.snapshot(out/(name+('-repeat-armed' if iteration else '-armed')+'.bin'),
                                   0xf600, 2048)
                if command != 'none':
                    actual = int.from_bytes(armed[address-0xf600:address-0xf600+2], 'little')
                    assert actual == spec['initial_value'], (spec, actual)
                number = watch(m, command, address, 2) if command != 'none' else None
                guards = {pc: breakpoint(m, f'break *0x{pc:x}') for pc in case['ends']
                          if not hit or pc != hit['pc']}
                # When the watch is the final stop, still bound missing delivery.
                if hit and hit['pc'] == case['expected']['pc']:
                    guards[hit['pc']+2] = breakpoint(m, f'break *0x{hit["pc"]+2:x}')
                states = []
                record['stage'] = 'repeat_watch_stop' if iteration else 'watch_stop'
                if hit:
                    event = m.event('stopped', m.resume('continue'))
                    regs = {reg: m.evaluate('$'+reg) for reg in stack_contracts.REGISTERS}
                    states.append(dict(stop=event, registers=regs))
                    record['repeat_watch_stop' if iteration else 'watch_stop'] = states[-1]
                    reason, field = REASONS[command]
                    assert event.get('reason') == reason, event
                    assert event[field]['number'] == number, event
                    assert all(regs[k] == v for k, v in hit.items()), (hit, regs)
                    m.cli(f'delete {number}')
                if not hit or hit['pc'] != case['expected']['pc']:
                    event = m.event('stopped', m.resume('continue'))
                    regs = {reg: m.evaluate('$'+reg) for reg in stack_contracts.REGISTERS}
                    states.append(dict(stop=event, registers=regs))
                    assert event.get('reason') == 'breakpoint-hit', event
                    assert event.get('bkptno') == guards.get(regs['pc']), event
                observed = trap_replay_observation(m, q, out, name+('-repeat' if iteration else '-after'),
                                                  logpath.read_text()[offset:], states)
                key = 'repeat' if iteration else 'observed'
                record[key] = observed
                record['stage'] = 'repeat_validation' if iteration else 'validation'
                failures = stack_contracts.assess(case, observed)
                if observed['counted_reads'] != case.get('counted_reads', 0):
                    failures['counted_reads'] = dict(expected=case.get('counted_reads', 0),
                                                     actual=observed['counted_reads'])
                record['failures'] = failures
                assert not failures, failures
                if command != 'none':
                    control = json.loads(baseline_path.read_text())
                    record['stage'] = 'equivalence'
                    differences = stack_contracts.replay_differences(observed, control['observed'])
                    record['equivalence_differences'] = differences
                    assert not differences, differences
                record['stage'] = 'post_step'
                m.cli('delete')
                event = m.event('stopped', m.resume('stepi'))
                regs = {reg: m.evaluate('$'+reg) for reg in stack_contracts.REGISTERS}
                post = trap_replay_observation(m, q, out, name+('-repeat-next' if iteration else '-next'),
                                               logpath.read_text()[offset:], [dict(stop=event, registers=regs)])
                record['repeat_post_step' if iteration else 'post_step'] = post
                assert event.get('reason') == 'end-stepping-range', event
                assert all(regs[k] == v for k, v in case['replay_next'].items()), (case['replay_next'], regs)
                if command != 'none':
                    differences = stack_contracts.replay_differences(post, control['post_step'])
                    record['post_step_differences'] = differences
                    assert not differences, differences
                record['stage'] = 'complete'
    finally:
        path.write_text(json.dumps(record, indent=2)+'\n')
    print('PASS '+name, flush=True)


def stack_semantics(args, out, fixture, kind, case, execution, watch_spec=None):
    name = f'{kind}-contract-{case["name"]}-{execution}'
    if watch_spec is not None:
        name += f'-{watch_spec[0]}-{watch_spec[1]:x}'
    record = dict(case=case, execution=execution, machine=kind, stage='setup')
    record['watch'] = watch_spec
    path = out/(name+'.json')
    path.write_text(json.dumps(record, indent=2)+'\n')
    with session(args.gdb, args.qemu, out, name, kind) as (m, s, q, _):
        stack_contract_setup(m, s, fixture, case)
        m.cli('x/8i $pc')
        q.snapshot(out/(name+'-before.bin'), 0xf600, 2048)
        for _ in range(case.get('pre_steps', 0)):
            event = m.event('stopped', m.resume('stepi'))
            assert event['reason'] == 'end-stepping-range', event
        if case.get('irq'):
            s.setmem(0xff9a if kind == 'pmb7850-test' else 0xe002,
                     0xfc if kind == 'pmb7850-test' else 0x8f29)
            s.flush()
        guards = {}
        command, address, hit, hit_pc = watch_spec or ('none', 0, False, 0)
        number = watch(m, command, address, 2) if command != 'none' else None
        if execution == 'continue':
            guards = {address: breakpoint(m, f'break *0x{address:x}')
                      for address in case['ends'] if not hit or address != hit_pc}
            if hit:
                guards[case['expected']['pc']+2] = breakpoint(
                    m, f'break *0x{case["expected"]["pc"]+2:x}')
        watch_event = None
        if hit:
            watch_event = stop(m, m.resume('continue'), command, number, hit_pc)
            m.cli(f'delete {number}')
        states = []
        for _ in range(case['steps'] if execution == 'stepi' else 1):
            event = (watch_event if hit and hit_pc == case['expected']['pc'] else
                     m.event('stopped', m.resume(execution)))
            registers = {reg: m.evaluate('$'+reg) for reg in stack_contracts.REGISTERS}
            states.append(dict(stop=event, registers=registers))
            if (registers['pc'] in case['ends'] and
                    len(states) >= case.get('minimum_steps', 1)):
                break
        memory = q.snapshot(out/(name+'-after.bin'), 0xf600, 2048).hex()
        log = (out/(name+'-qemu.log')).read_text()
        accesses = [[int(a, 16), int(w), int(v, 16)] for a, w, v in re.findall(
            r'c166_access pc=0x[0-9a-f]+ count=\d+ addr=0x([0-9a-f]+) '
            r'size=2 write=([01]) value=0x([0-9a-f]+)', log)
            if 0xf600 <= int(a, 16) < 0xfc80]
        observed = dict(states=states, memory=memory, stack_accesses=accesses)
        failures = stack_contracts.assess(case, observed)
        if 'counted_reads' in case:
            reads = re.findall(r'c166_test_mmio_read offset=0x6 value=0x5678', log)
            record['counted_reads'] = len(reads)
            if len(reads) != case['counted_reads']:
                failures['counted_reads'] = dict(expected=case['counted_reads'], actual=len(reads))
        if case.get('qualification', '').startswith('model-policy:'):
            traps = [[int(count), *(int(value, 16) for value in values)]
                     for count, *values in re.findall(
                         r'c166_stack_trap count=(\d+) source=0x([0-9a-f]+) '
                         r'next=0x([0-9a-f]+) sp=0x([0-9a-f]+) '
                         r'psw=0x([0-9a-f]+) tfr=0x([0-9a-f]+) '
                         r'request=0x([0-9a-f]+)', log)]
            e = case['expected']
            wanted = ([[e['guest_icount'], case['initial']['pc'], case['resume_pc'],
                        e['sp'], e['psw'], e['tfr'], e['tfr']]] if e['tfr'] else [])
            record['stack_traps'] = traps
            if traps != wanted:
                failures['stack_traps'] = dict(expected=wanted, actual=traps)
        for state in states:
            event = state['stop']
            if event is watch_event:
                continue  # stop() already checked reason, identity and PC.
            reason = 'end-stepping-range' if execution == 'stepi' else 'breakpoint-hit'
            if event.get('reason') != reason:
                failures['stop_reason'] = event
            if execution == 'continue' and event.get('bkptno') != guards.get(state['registers']['pc']):
                failures['breakpoint_identity'] = event
        record.update(stage='validation', observed=observed, failures=failures,
                      watch_stop=watch_event)
        path.write_text(json.dumps(record, indent=2)+'\n')
        assert not failures, failures
        if watch_spec is not None:
            comparable = dict(registers=states[-1]['registers'], memory=memory,
                              stack_accesses=accesses)
            if command != 'none':
                baseline = out/f'{kind}-contract-{case["name"]}-continue-none-0.json'
                previous = json.loads(baseline.read_text())['observed']
                assert comparable == dict(registers=previous['states'][-1]['registers'],
                                          memory=previous['memory'],
                                          stack_accesses=previous['stack_accesses'])
            # Remove watches/guards, then prove retirement did not leave a replay.
            m.cli('delete')
            m.execute('stepi', 'end-stepping-range', case['expected']['pc']+2)
            s.reg('guest_icount', case['expected']['guest_icount']+1)
            if (case.get('qualification', '').startswith('model-policy:') and
                    command == 'awatch' and hit and
                    address == case['policy_accesses'][-1][0]):
                # Re-enter this same producer in the same CPU: stale replay
                # state must not skip its operands or reuse an old successor.
                if case['expected']['tfr']:
                    leave_stack_handler(m, s)
                for reg, value in (fixture['initial'] | case['initial'] | {'tfr': 0}).items():
                    s.setreg(reg, value)
                s.flush()
                before = (out/(name+'-before.bin')).read_bytes().hex()
                m.command('-data-write-memory-bytes 0xf600 '+json.dumps(before))
                if case.get('irq'):
                    s.setmem(0xff9a if kind == 'pmb7850-test' else 0xe002,
                             0xfc if kind == 'pmb7850-test' else 0x8f29)
                    s.flush()
                number = watch(m, command, address, 2)
                for dest in set(case['ends']+[case['expected']['pc']+2]) - {case['expected']['pc']}:
                    breakpoint(m, f'break *0x{dest:x}')
                offset = len((out/(name+'-qemu.log')).read_text())
                stop(m, m.resume('continue'), command, number, case['expected']['pc'])
                repeated = {reg: m.evaluate('$'+reg) for reg in stack_contracts.REGISTERS}
                repeated_memory = q.snapshot(out/(name+'-repeat.bin'), 0xf600, 2048).hex()
                tail = (out/(name+'-qemu.log')).read_text()[offset:]
                repeated_accesses = [[int(a, 16), int(w), int(v, 16)] for a, w, v in re.findall(
                    r'c166_access pc=0x[0-9a-f]+ count=\d+ addr=0x([0-9a-f]+) '
                    r'size=2 write=([01]) value=0x([0-9a-f]+)', tail)
                    if 0xf600 <= int(a, 16) < 0xfc80]
                record['repeat'] = dict(registers=repeated, memory=repeated_memory,
                                       stack_accesses=repeated_accesses)
                path.write_text(json.dumps(record, indent=2)+'\n')
                assert record['repeat'] == comparable, record['repeat']
    print('PASS '+name, flush=True)


def memory(args,out,kind):
    name=kind+'-memory'
    with session(args.gdb,args.qemu,out,name,kind) as (m,s,q,_):
        # Crossing target pages, high 24-bit addresses, executable DPRAM.
        addresses=[0x2ffb,0xf5fb]
        if kind=='c166-test': addresses += [0xffffe0]
        else: addresses += [0x400ffb] # mapped external SRAM
        for a in addresses:
            raw=bytes(range(32)).hex()
            m.command(f'-data-write-memory-bytes 0x{a:x} {raw}')
            result=m.command(f'-data-read-memory-bytes 0x{a:x} 32')
            assert result['memory'][0]['contents']==raw,result
            assert q.snapshot(out/f'{name}-{a:x}.bin',a,32).hex()==raw
        s.write(0xf600,'e6f03412cc00');s.setreg('pc',0xf600);s.flush()
        m.execute('stepi','end-stepping-range',0xf604);s.reg('r0',0x1234)
        s.write(0xf600,'e6f07856');s.setreg('pc',0xf600);s.flush()
        m.execute('stepi','end-stepping-range',0xf604);s.reg('r0',0x5678)
    print('PASS memory '+kind,flush=True)


def gpr_code(args,out,kind):
    name=kind+'-gpr-code-write'
    with session(args.gdb,args.qemu,out,name,kind,access_trace=False) as (m,s,q,_):
        s.write(0xfc00,'e6f13412');s.write(0xf600,'e6f0cc00')
        s.setreg('pc',0xfc00);s.flush()
        m.execute('stepi','end-stepping-range',0xfc04)
        s.setreg('pc',0xf600);s.flush()
        m.execute('stepi','end-stepping-range',0xf604)
        s.reg('r0',0xcc);s.mem(0xfc00,0xcc)
        assert q.snapshot(out/(name+'-modified.bin'),0xfc00,2).hex()=='cc00'
        s.setreg('pc',0xfc00);s.flush()
        m.execute('stepi','end-stepping-range',0xfc02)
        s.expect({'guest_icount':3,'r1':0x1234})
    print('PASS GPR code write '+kind,flush=True)


def mapping(args,out,kind):
    if kind=='pmb7850-test':
        name=kind+'-mapping'
        with session(args.gdb,args.qemu,out,name,kind) as (m,s,q,_):
            s.setmem(0x200,0x1111);s.setreg('syscon',0x400);s.flush()
            s.setmem(0x200,0x2222);s.flush()
            for control,addr,value in [(0,0x200,0x1111),(0x400,0x200,0x2222),
                                       (0x1400,0x10200,0x2222)]:
                s.setreg('syscon',control);s.flush();s.mem(addr,value)
                assert q.snapshot(out/f'{name}-{control:x}.bin',addr,2)==value.to_bytes(2,'little')
    print('PASS mapping '+kind,flush=True)


def flash(args,out):
    raw=bytearray(b'\xff'*(4*1024*1024))
    raw[0x2000:0x2020]=bytes(range(32))
    # Low external addresses alias boot flash when a chip is installed. Use
    # internal DPRAM for the destination, keeping flash programming out of scope.
    raw[0x3000:0x3006]=bytes.fromhex('f6f000f6cc00')
    path=out/'memory-flash.bin';path.write_bytes(raw)
    with session(args.gdb,args.qemu,out,'pmb7850-test-flash','pmb7850-test',path) as (m,s,q,_):
        result=m.command('-data-read-memory-bytes 0x802000 32')
        assert result['memory'][0]['contents']==bytes(range(32)).hex(),result
        assert q.snapshot(out/'flash-data.bin',0x802000,32)==bytes(range(32))
        s.setreg('pc',0x803000);s.setreg('r0',0x1234);s.flush()
        w=watch(m,'watch',0xf600,2)
        stop(m,m.resume('continue'),'watch',w,0x803004)
        s.expect({'guest_icount':1},{'0xf600':'3412'})
    print('PASS generated flash reads and execution',flush=True)


def packets(out):
    found=set();summary={}
    for path in out.glob('*-rsp.log'):
        wire=b''.join(bytes.fromhex(line[3:].split('<')[0]) for line in path.read_text().splitlines() if line.startswith('w  '))
        values=[p.decode() for p in re.findall(rb'\$([Zz][234],[^#]*)#',wire)]
        if values:
            assert all(re.fullmatch('[Zz][234],[0-9a-f]+,[124]',p) for p in values),values
            summary[path.name]=values
            found.update(p[:2] for p in values)
    assert found=={'Z2','z2','Z3','z3','Z4','z4'},found
    (out/'watchpoint-packets.json').write_text(json.dumps(summary,indent=2)+'\n')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--gdb',type=binary,required=True)
    p.add_argument('--qemu',type=binary,required=True)
    p.add_argument('--output',type=Path,default=ROOT/'emu/shots/gdb-c166-phase6/memory')
    p.add_argument('--section', choices=('all', 'conditional', 'stack', 'stack-semantics', 'stack-mapping', 'stack-producers', 'stack-deferred', 'stack-deferred-replay'), default='all',
                   help='run the strict memory gate, a replay audit, or separate CPU stack conformance')
    p.add_argument('--case-pattern', default='*',
                   help='shell-style case-name filter for a stack contract section')
    args=p.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    contract_sections = ('stack-semantics', 'stack-mapping', 'stack-producers', 'stack-deferred', 'stack-deferred-replay')
    if args.case_pattern != '*' and args.section not in contract_sections:
        p.error('--case-pattern requires a stack contract section')
    contract_cases = ([case for case in (stack_contracts.mapping_replay_cases()
                       if args.section == 'stack-mapping' else stack_contracts.producer_cases()
                       if args.section == 'stack-producers' else stack_contracts.deferred_cases()
                       if args.section == 'stack-deferred' else stack_contracts.deferred_replay_cases()
                       if args.section == 'stack-deferred-replay' else stack_contracts.cases())
                       if fnmatch.fnmatchcase(case['name'], args.case_pattern)]
                      if args.section in contract_sections else [])
    if args.section in contract_sections and not contract_cases:
        p.error('--case-pattern matches no stack semantics cases')
    fixture=json.loads(FIXTURES.read_text());(out/'fixtures.json').write_text(FIXTURES.read_text())
    (out/'hashes.json').write_text(json.dumps({str(x):digest(x) for x in (args.gdb,args.qemu,FIXTURES,Path(__file__),Path(stack_contracts.__file__),ROOT/'manuals/cpu/M166.PDF',ROOT/'manuals/cpu/c166s_v1.pdf')},indent=2)+'\n')
    count=0
    results={}
    def run(name, function, *params):
        try:
            value=function(*params)
            results[name]={'passed':True}
            return value
        except (AssertionError, TimeoutError, OSError, EOFError, RuntimeError) as error:
            results[name]={'passed':False, 'error':str(error),
                           'traceback':traceback.format_exc()}
            print('FAIL '+name+': '+repr(error),flush=True)
            traceback.print_exc()
            return 0

    for kind in ('c166-test','pmb7850-test'):
        if args.section == 'stack-deferred-replay':
            for case in contract_cases:
                if kind not in case.get('machines', ('c166-test', 'pmb7850-test')):
                    continue
                before = dict(case['seeds'])
                if case['replay_prepare']:
                    accesses = case.get('policy_accesses', case['accesses'])
                    remaining = sum(len(a) for _, _, a in case['replay_phases'])
                    before.update({a: v for a, w, v in accesses[:-remaining] if w})
                for spec in stack_contracts.deferred_replay_watches(case, before):
                    run(f'{kind}-{case["name"]}-{spec["command"]}-{spec["address"]:x}',
                        deferred_replay, args, out, fixture, kind, case, spec)
        if args.section in ('stack-mapping', 'stack-producers'):
            variants = (stack_contracts.producer_watches if args.section == 'stack-producers'
                        else stack_contracts.mapping_watches)
            for case in contract_cases:
                # A POP followed by its trap frame can revisit the same slot.
                # Run each watch once so a later result cannot overwrite it.
                for spec in dict.fromkeys(variants(case)):
                    prefix = 'mapping' if args.section == 'stack-mapping' else 'producer'
                    run(f'{kind}-{prefix}-{case["name"]}-{spec[0]}-{spec[1]:x}',
                        stack_semantics, args, out, fixture, kind, case, 'continue', spec)
        if args.section in ('stack-semantics', 'stack-deferred'):
            for case in contract_cases:
                if kind not in case.get('machines', ('c166-test', 'pmb7850-test')):
                    continue
                for execution in case['modes']:
                    run(f'{kind}-contract-{case["name"]}-{execution}', stack_semantics,
                        args, out, fixture, kind, case, execution)
        if args.section in ('all', 'conditional'):
            for case in conditional_cases():
                for command in ('none', 'watch', 'rwatch', 'awatch'):
                    run(f'{kind}-conditional-{case["name"]}-{command}', conditional,
                        args, out, fixture, kind, case, command)
        if args.section in ('all', 'stack'):
            for case in stack_cases(kind):
                for command, addr in case['variants']:
                    run(f'{kind}-stack-{case["name"]}-{command}-{addr:x}', stack_bounds,
                        args, out, fixture, kind, case, command, addr)
        if args.section != 'all':
            continue
        count+=run(kind+'-replay',replay,args,out,fixture,kind)
        for section in (mmio,interrupt,semantics,access_paths,memory,gpr_code,mapping):
            run(kind+'-'+section.__name__,section,args,out,kind)
    if args.section == 'all':
        run('flash',flash,args,out)
    if args.section not in ('stack-semantics', 'stack-deferred'):
        run('packets',packets,out)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    label = 'stack semantics gate' if args.section in ('stack-semantics', 'stack-deferred') else 'memory/watchpoint gate'
    if not all(result['passed'] for result in results.values()):
        print(f'FAIL {label}; see results.json',flush=True)
        return 1
    print(f'PASS {args.section} {label}: {len(results)} checks, '
          f'{count} ordinary replay cases',flush=True)
    return 0

if __name__=='__main__': raise SystemExit(main())
