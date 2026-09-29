"""Manual-derived, unwatched stack contracts for gdb_memory's dedicated gate.

These are conformance expectations, not descriptions of current QEMU behavior.
Reserved STKSZ values and unspecified pipeline hazards are deliberately absent.
"""
from __future__ import annotations
import copy


MANUAL = 'C166S V1 PDF pp.49-53,60-66,97-103,117-121; M166 instruction pages'
WIDTHS = {0: 512, 1: 256, 2: 128, 3: 64, 4: 1024}
REGISTERS = ('pc', 'sp', 'cp', 'psw', 'tfr', 'r0', 'r1', 'guest_icount',
             'ext_kind', 'ext_count', 'extr')


def physical(sp, mode):
    if mode == 7:
        return sp
    mask = WIDTHS[mode] - 1
    return (0xfbfe & ~mask) | (sp & mask)


def base(name, group, segmented=True):
    pc = 0x402000 if segmented else 0x2000
    return dict(name=name + ('-seg' if segmented else '-noseg'), group=group,
                manual=MANUAL, segmented=segmented, steps=1,
                initial=dict(pc=pc, cp=0xfc80, sp=0xfb80, psw=0x2046,
                             stkov=0xf600, stkun=0xfdfe,
                             syscon=0xe000 | (0 if segmented else 0x800),
                             r0=0x1234, r1=0x2100,
                             dpp0=0, dpp1=1, dpp2=2, dpp3=3),
                seeds={}, code='', ends=[], expected={}, memory={},
                accesses=[], access_prefix=False, modes=['stepi', 'continue'])


def frame(sp, psw, pc, segmented, mode=7):
    words = [psw] + ([pc >> 16] if segmented else []) + [pc & 0xffff]
    return [[physical(sp-2*(i+1), mode), 1, word]
            for i, word in enumerate(words)]


def finish(case, pc, sp, psw=0x2046, count=1, tfr=0):
    case['expected'].update(pc=pc, sp=sp, psw=psw, guest_icount=count,
                            tfr=tfr, cp=0xfc80, ext_kind=0, ext_count=0, extr=0)
    case['ends'] = sorted(set(case['ends'] + [pc, 0x10, 0x18, 0x28]))
    return case


def bounds_cases():
    # M166 pp.50-53,93-101,104,113. Both first/final multiword crossings
    # are included; transfer-trap saved IP is recorded, not guessed here.
    for segmented in (False, True):
        for op in ('push', 'pop', 'scxt', 'calla', 'calli', 'callr', 'calls',
                   'pcall', 'ret', 'rets', 'retp', 'reti', 'trap', 'irq'):
            c = base(op, 'bounds', segmented)
            pc = c['initial']['pc']; target = (pc & 0xff0000) | 0x2100
            count = 1; psw = 0x2046; reads = []; writes = []
            c['code'] = {'push': 'ecf0', 'pop': 'fcf0', 'scxt': 'c6f07856',
                         'calla': 'ca000021', 'calli': 'ab01', 'callr': 'bb7f',
                         'calls': 'da510021', 'pcall': 'e2f00021', 'ret': 'cb00',
                         'rets': 'db00', 'retp': 'ebf0', 'reti': 'fb88',
                         'trap': '9b40', 'irq': 'cc00'}[op]
            successor = pc + len(bytes.fromhex(c['code']))
            if op in ('push', 'scxt'):
                writes = [0x1234]
                c['expected']['r0'] = 0x5678 if op == 'scxt' else 0x1234
            elif op == 'pop':
                reads = [0x8000]; psw = 0x2057; c['expected']['r0'] = 0x8000
            elif op.startswith('call'):
                writes = ([pc >> 16] if op == 'calls' else []) + [successor & 0xffff]
                successor = 0x512100 if op == 'calls' and segmented else target
            elif op == 'pcall':
                writes = [0x1234, successor & 0xffff]; successor = target
            elif op.startswith('ret'):
                reads = [0x2100]
                if op == 'rets' or (op == 'reti' and segmented):
                    reads += [0x51]
                    if segmented:
                        target = 0x512100
                if op == 'retp':
                    reads += [0x8000]; psw = 0x2057; c['expected']['r0'] = 0x8000
                if op == 'reti':
                    reads += [0x3046]; psw = 0x3046
                successor = target
            elif op in ('trap', 'irq'):
                writes = [0x2846 if op == 'irq' else psw]
                writes += ([pc >> 16] if segmented else [])
                writes += [pc & 0xffff if op == 'irq' else successor & 0xffff]
                successor = 0xa4 if op == 'irq' else 0x80
                if op == 'irq':
                    c['irq'] = True; c['modes'] = ['continue']
                    c['initial']['psw'] = 0x2846; psw = 0xf846; count = 0
            post = 0xfb80 + 2*(len(reads)-len(writes))
            accesses = [[0xfb80+2*i, 0, value] for i, value in enumerate(reads)]
            accesses += [[0xfb7e-2*i, 1, value] for i, value in enumerate(writes)]
            c['seeds'] = {a: v for a, w, v in accesses if not w}
            c['memory'] = {a: v for a, w, v in accesses if w}
            c['accesses'] = None if op == 'irq' else accesses
            c['ends'].append(successor)
            for limit in ('inside', 'equal', 'crossed', 'first-crossed'):
                if limit == 'first-crossed' and len(reads)+len(writes) < 2:
                    continue
                case = copy.deepcopy(c); case['name'] += '-'+limit
                crossing = limit in ('crossed', 'first-crossed')
                up = bool(reads); sign = -1 if up else 1
                bound = post + sign*({'inside': -2, 'equal': 0,
                                      'crossed': 2, 'first-crossed': 2*(len(reads)+len(writes))}[limit])
                case['initial']['stkun' if up else 'stkov'] = bound
                if crossing:
                    flag = 0x2000 if up else 0x4000
                    f = frame(post, psw, successor, segmented)
                    if op in ('push', 'pop', 'scxt'):
                        case['accesses'] += f
                        case['memory'].update({a: v for a, _, v in f})
                    else:
                        case['access_prefix'] = True
                        case['open'] = ('Transfer/entry trap saved IP and partial-frame boundary '
                                        'require separate qualification; only trigger, priority '
                                        'and first producer access are asserted on crossed bounds.')
                    finish(case, 0x18 if up else 0x10, post-2*len(f),
                           psw | 0xf000, count, flag)
                    if op not in ('push', 'pop', 'scxt'):
                        # Do not turn an unresolved transfer/entry frame layout
                        # into a conformance requirement. The absent trap is
                        # independently discriminated by vector and TFR.
                        del case['expected']['sp'], case['expected']['psw']
                        case['masked'] = {'psw': [0xf000, 0xf000]}
                        if case['accesses'] is not None:
                            case['accesses'] = case['accesses'][:1]
                        case['memory'] = dict(list(case['memory'].items())[:1])
                else:
                    finish(case, successor, post, psw, count)
                yield case
        # False conditions must not touch the stack or test a pre-existing limit.
        for op, code in (('calla', 'ca200021'), ('calli', 'ab21')):
            c = base(op+'-not-taken', 'negative', segmented)
            c['code'] = code; c['initial']['stkov'] = 0xfbc0
            c['ends'] = [(c['initial']['pc'] & 0xff0000) | 0x2100]
            yield finish(c, c['initial']['pc']+len(bytes.fromhex(code)), 0xfb80)


def arithmetic_cases():
    for segmented in (False, True):
        for op, opcode, delta in (('add', 0x00, 2), ('sub', 0x20, -2)):
            for encoding, code in (
                    ('immediate', bytes([opcode+6, 9, 2, 0]).hex()),
                    ('source-memory', bytes([opcode+2, 9, 0, 0x30]).hex()),
                    ('destination-memory', bytes([opcode+4, 0xf0, 0x12, 0xfe]).hex())):
                for crossed in (False, True):
                    c = base(f'{op}-{encoding}-'+('crossed' if crossed else 'equal'),
                             'arithmetic', segmented)
                    if encoding == 'destination-memory':
                        # c166-test intentionally has flat RAM at FE12, not
                        # the PMB CSFR bridge. It cannot test this SP alias.
                        c['machines'] = ['pmb7850-test']
                    c['code'] = code+'cc00cc00'; c['steps'] = 3
                    c['initial']['r0'] = 2; c['seeds'][0x3000] = 2
                    pc = c['initial']['pc']; post = 0xfb80+delta
                    c['initial']['stkun' if delta > 0 else 'stkov'] = (
                        post-delta if crossed else post)
                    c['ends'] = [pc+8]
                    if crossed:
                        c['allowed_counts'] = [2, 3]
                        c['delayed_frame'] = dict(sp=post, psw=0x2041, pc=pc)
                        flag = 0x2000 if delta > 0 else 0x4000
                        finish(c, 0x18 if delta > 0 else 0x10,
                               post-(6 if segmented else 4), 0xf041, 3, flag)
                        del c['expected']['guest_icount']
                    else:
                        finish(c, pc+8, post, 0x2041, 3)
                    yield c
        for name, reg, value, initial in (
                ('mov-sp-below', 9, 0xf900, {'stkov': 0xfb80}),
                ('mov-sp-above', 9, 0xfbc0, {'stkun': 0xfb80}),
                ('move-overflow-bound', 10, 0xfbc0, {}),
                ('move-underflow-bound', 11, 0xf900, {})):
            c = base(name, 'negative', segmented)
            c['initial'].update(initial)
            c['code'] = bytes([0xe6, reg]).hex()+value.to_bytes(2, 'little').hex()+'cc00cc00'
            c['steps'] = 3
            # MOV sets N, clears E/Z and preserves V/C for these high words.
            yield finish(c, c['initial']['pc']+8,
                         value if reg == 9 else 0xfb80, 0x2047, 3)


def window_cases():
    for segmented in (False, True):
        for window, opcode, high, operand in (
                ('atomic', 0xd1, 0x00, ''), ('extr', 0xd1, 0x80, ''),
                ('exts', 0xd7, 0x00, '0500'), ('extp', 0xd7, 0x40, '0500'),
                ('extsr', 0xd7, 0x80, '0500'), ('extpr', 0xd7, 0xc0, '0500')):
            for length in (1, 2, 4):
                for position in sorted({0, length-1}):
                    for pop in (False, True):
                        # One equality control per prefix/operation/mode.
                        for crossed in ((False, True) if length == 4 and position == 3 else (True,)):
                            c = base(f'{window}-{length}-{position}-'+('pop' if pop else 'push')+
                                     ('-crossed' if crossed else '-equal'), 'window', segmented)
                            prefix = bytes([opcode, high+16*(length-1)]).hex()+operand
                            c['code'] = prefix+'cc00'*position+('fcf0' if pop else 'ecf0')+'cc00'*(length-position-1)
                            c['steps'] = length+1
                            pc = c['initial']['pc']; after = pc+len(bytes.fromhex(c['code']))
                            post = 0xfb82 if pop else 0xfb7e
                            psw = 0x2057 if pop else 0x2046
                            c['initial']['stkun' if pop else 'stkov'] = (
                                0xfb80 if crossed else post)
                            c['seeds'] = {0xfb80: 0x8000} if pop else {}
                            c['expected']['r0'] = 0x8000 if pop else 0x1234
                            c['accesses'] = [[0xfb80, 0, 0x8000]] if pop else [[0xfb7e, 1, 0x1234]]
                            c['memory'] = {} if pop else {0xfb7e: 0x1234}
                            c['ends'] = [after]
                            if crossed:
                                f = frame(post, psw, after, segmented)
                                c['accesses'] += f; c['memory'].update({a: v for a, _, v in f})
                                finish(c, 0x18 if pop else 0x10, post-2*len(f),
                                       psw | 0xf000, length+1, 0x2000 if pop else 0x4000)
                            else:
                                finish(c, after, post, psw, length+1)
                            yield c


def mapping_cases():
    for mode in (*WIDTHS, 7):
        low = 0xfc00-WIDTHS.get(mode, 512)
        for op in ('push-pair', 'pop-pair', 'calls', 'rets', 'trap', 'reti', 'irq'):
            c = base(f'stksz-{mode}-{op}', 'mapping')
            c['initial']['syscon'] = mode << 13
            pc = c['initial']['pc']; sp = low+2; psw = 0x2046; count = 1
            if op in ('pop-pair', 'rets', 'reti'):
                sp = 0xfbfe
            c['initial']['sp'] = sp
            c['code'] = {'push-pair': 'ecf0ecf1', 'pop-pair': 'fcf0fcf1',
                         'calls': 'da510021', 'rets': 'db00',
                         'trap': '9b40', 'reti': 'fb88', 'irq': 'cc00'}[op]
            reads = []; writes = []; successor = pc+len(bytes.fromhex(c['code']))
            if op == 'push-pair':
                writes = [0x1234, 0x2100]; count = c['steps'] = 2
            elif op == 'pop-pair':
                reads = [0x1234, 0x5678]; count = c['steps'] = 2
                c['expected'].update(r0=0x1234, r1=0x5678)
            elif op == 'calls':
                writes = [0x40, successor & 0xffff]; successor = 0x512100
            elif op == 'rets':
                reads = [0x2100, 0x51]; successor = 0x512100
            elif op == 'reti':
                reads = [0x2100, 0x51, 0x3046]; successor = 0x512100; psw = 0x3046
            elif op in ('trap', 'irq'):
                writes = [0x2846 if op == 'irq' else 0x2046, 0x40,
                          0x2000 if op == 'irq' else 0x2002]
                successor = 0xa4 if op == 'irq' else 0x80
                if op == 'irq':
                    c['irq'] = True; c['modes'] = ['continue']; count = 0
                    c['initial']['psw'] = 0x2846; psw = 0xf846
            accesses = [[physical(sp+2*i, mode), 0, v] for i, v in enumerate(reads)]
            accesses += [[physical(sp-2*(i+1), mode), 1, v] for i, v in enumerate(writes)]
            # Wrong linear reads still lead to a bounded code breakpoint.
            if reads:
                c['seeds'].update({sp+2*i: ((0xbeef if op == 'pop-pair' else 0x40)
                                          if i == 1 else v)
                                   for i, v in enumerate(reads)})
            c['seeds'].update({a: v for a, w, v in accesses if not w})
            c['accesses'] = None if op == 'irq' else accesses
            c['memory'] = {a: v for a, w, v in accesses if w}
            mapped_writes = set(c['memory'])
            for i in range(len(writes)):
                linear = sp-2*(i+1)
                if linear not in mapped_writes:
                    c['memory'][linear] = 0xa5a5
            c['ends'] = [successor, 0x402100, 0x2100]
            yield finish(c, successor, sp+2*(len(reads)-len(writes)), psw, count)


def cases():
    yield from bounds_cases()
    yield from arithmetic_cases()
    yield from window_cases()
    yield from mapping_cases()
    # Circular addressing makes the full virtual SP range legal. V1 PDF p.97
    # fixes SP[15:12]=1111 even when an implicit update wraps its writable bits.
    for pop in (False, True):
        c = base('pop-sp-wrap' if pop else 'push-sp-wrap', 'pointer')
        c['initial'].update(syscon=0, sp=0xfffe if pop else 0xf000,
                            stkov=0xf000, stkun=0xfffe)
        c['code'] = 'fcf0' if pop else 'ecf0'
        c['seeds'] = {0xfbfe: 0x1234} if pop else {}
        c['accesses'] = [[0xfbfe, 0 if pop else 1, 0x1234]]
        c['memory'] = {0xfbfe: 0x1234}
        c['expected']['r0'] = 0x1234
        yield finish(c, c['initial']['pc']+2, 0xf000 if pop else 0xfffe)


def mapping_watches(case):
    """Watch every physical slot, with wrong-direction and virtual-only controls."""
    accesses = case['accesses']
    if case.get('irq'):
        accesses = frame(case['initial']['sp'], case['initial']['psw'],
                         case['initial']['pc'], case['segmented'],
                         case['initial']['syscon'] >> 13)
    yield ('none', 0, False, 0)
    for index, (address, write, _) in enumerate(accesses):
        pc = case['expected']['pc']
        if '-pair-' in case['name']:
            pc = case['initial']['pc'] + 2*(index+1)
        yield ('watch' if write else 'rwatch', address, True, pc)
        yield ('awatch', address, True, pc)
    address, write, _ = accesses[-1]
    yield ('rwatch' if write else 'watch', address, False, 0)
    virtual = ((case['initial']['sp'] + (-2*len(accesses) if write else
                2*(len(accesses)-1))) & 0x0ffe) | 0xf000
    if virtual not in {a for a, _, _ in accesses}:
        yield ('awatch', virtual, False, 0)


def mapping_replay_cases():
    yield from (case for case in cases() if case['group'] in ('mapping', 'pointer'))
    for mode in (*WIDTHS, 7):
        for segmented in (False, True):
            for op in ('push', 'pop', 'scxt'):
                c = base(f'mapped-trap-{mode}-{op}', 'mapping-replay', segmented)
                pop = op == 'pop'
                sp = 0xfc00-WIDTHS.get(mode, 512)+2
                c['initial'].update(sp=sp, syscon=(mode << 13) | (0 if segmented else 0x800))
                c['initial']['stkun' if pop else 'stkov'] = sp
                c['code'] = {'push': 'ecf0', 'pop': 'fcf0', 'scxt': 'c6f07856'}[op]
                after = sp + (2 if pop else -2)
                address = physical(sp if pop else after, mode)
                c['accesses'] = [[address, 0 if pop else 1, 0x1234]]
                if pop:
                    c['seeds'][address] = 0x1234
                else:
                    c['memory'][address] = 0x1234
                psw = 0x2046
                words = frame(after, psw, c['initial']['pc']+len(bytes.fromhex(c['code'])),
                              segmented, mode)
                c['accesses'] += words
                c['memory'].update({a: v for a, _, v in words})
                c['expected']['r0'] = 0x5678 if op == 'scxt' else 0x1234
                yield finish(c, 0x18 if pop else 0x10, after-2*len(words),
                             psw | 0xf000, tfr=0x2000 if pop else 0x4000)


def producer_cases():
    """Full-producer completion is model policy, not hardware recovery evidence."""
    originals = {c['name']: c for c in bounds_cases()}
    for original in originals.values():
        op = original['name'].split('-')[0]
        if op in ('push', 'pop', 'scxt') or original['name'].endswith('-inside'):
            continue
        for mode in (7, 0):
            c = copy.deepcopy(original)
            segmented = c['segmented']
            tag = 'seg' if segmented else 'noseg'
            c['name'] = 'producer-'+c['name']+('-linear' if mode == 7 else '-circular')
            c['qualification'] = 'model-policy: complete producer, save logical successor'
            c['initial']['syscon'] = (mode << 13) | (0 if segmented else 0x800)
            if original['group'] == 'negative':
                c['policy_accesses'] = []
                yield c
                continue
            plain = originals[f'{op}-{tag}-inside']
            pop = op.startswith('ret')
            old_sp = c['initial']['sp']
            sp = old_sp if mode == 7 else 0xfbfe if pop else 0xfa02
            delta = sp-old_sp
            c['initial']['sp'] = sp
            bound = 'stkun' if pop else 'stkov'
            c['initial'][bound] += delta
            c['seeds'] = {physical(a+delta, mode): v for a, v in plain['seeds'].items()}
            accesses = plain['accesses']
            if c.get('irq'):
                accesses = frame(old_sp, plain['initial']['psw'], plain['initial']['pc'], segmented)
            accesses = [[physical(a+delta, mode), w, v] for a, w, v in accesses]
            post = plain['expected']['sp'] + delta
            expected = plain['expected'].copy()
            c['resume_pc'] = expected['pc']
            expected['sp'] = post
            crossed = original['name'].endswith('-crossed')
            if crossed:
                accesses += frame(post, expected['psw'], expected['pc'], segmented, mode)
                expected.update(pc=0x18 if pop else 0x10,
                                sp=post-(6 if segmented else 4),
                                psw=expected['psw'] | 0xf000,
                                tfr=0x2000 if pop else 0x4000)
            c['expected'] = expected
            c['memory'] = {a: v for a, w, v in accesses if w}
            c['policy_accesses'] = accesses
            c['accesses'] = None if c.get('irq') else accesses
            c['access_prefix'] = False
            c.pop('masked', None)
            c.pop('open', None)
            yield c
    # A later update can wrap back inside the bound. The first crossing survives.
    for pop in (False, True):
        c = base('producer-return-wrap-latch' if pop else 'producer-call-wrap-latch',
                 'producer-policy')
        c['qualification'] = 'model-policy: complete producer, save logical successor'
        c['initial'].update(syscon=0, sp=0xfffc if pop else 0xf002,
                            stkun=0xfffc, stkov=0xf002)
        c['code'] = 'db00' if pop else 'da510021'
        c['ends'] = [0x512100]
        c['resume_pc'] = 0x512100
        accesses = ([[0xfbfc, 0, 0x2100], [0xfbfe, 0, 0x51]] if pop else
                    [[0xfa00, 1, 0x40], [0xfbfe, 1, 0x2004]])
        c['seeds'] = {a: v for a, w, v in accesses if not w}
        post = 0xf000 if pop else 0xfffe
        words = frame(post, 0x2046, 0x512100, True, 0)
        accesses += words
        c['policy_accesses'] = c['accesses'] = accesses
        c['memory'] = {a: v for a, w, v in accesses if w}
        yield finish(c, 0x18 if pop else 0x10, (post-6 & 0x0ffe) | 0xf000,
                     0xf046, tfr=0x2000 if pop else 0x4000)
    # RETP SP must retain the implicit-pop request even if its destination write
    # subsequently puts SP back in bounds. The final frame placement is policy.
    c = base('producer-retp-sp-latch', 'producer-policy')
    c['qualification'] = 'model-policy: complete producer, save logical successor'
    c['initial']['stkun'] = 0xfb80
    c['code'] = 'eb09'
    c['ends'] = [0x402100]
    c['resume_pc'] = 0x402100
    c['seeds'] = {0xfb80: 0x2100, 0xfb82: 0xfa80}
    accesses = [[0xfb80, 0, 0x2100], [0xfb82, 0, 0xfa80]]
    accesses += frame(0xfa80, 0x2047, 0x402100, True)
    c['policy_accesses'] = c['accesses'] = accesses
    c['memory'] = {a: v for a, w, v in accesses if w}
    yield finish(c, 0x18, 0xfa7a, 0xf047, tfr=0x2000)


def producer_watches(case):
    accesses = case['policy_accesses']
    yield ('none', 0, False, 0)
    addresses = list(dict.fromkeys(a for a, _, _ in accesses))
    if not addresses:
        addresses = [physical(case['initial']['sp']-2, case['initial']['syscon'] >> 13)]
    for address in addresses:
        directions = {w for a, w, _ in accesses if a == address}
        initial = case['seeds'].get(address, 0xa5a5)
        # GDB watch tests the final value; RETI can restack identical words.
        for command, hit in (
                ('watch', 1 in directions and case['memory'][address] != initial),
                ('rwatch', 0 in directions), ('awatch', bool(directions))):
            yield (command, address, hit, case['expected']['pc'] if hit else 0)


def deferred_cases():
    """V1 pp.49,64-67: priority, RETI reconsideration and deferred state."""
    for segmented in (False, True):
        for mode in (7, 0):
            pc = 0x402000 if segmented else 0x2000
            size = 6 if segmented else 4
            def build(name, code, count, target, sp, psw, tfr, accesses,
                      programs=None, initial=None, expected=None, used=()):
                c = base('deferred-'+name+('-linear' if mode == 7 else '-circular'),
                         'deferred', segmented)
                c['initial'].update(syscon=(mode << 13) | (0 if segmented else 0x800))
                c['initial'].update(initial or {})
                c.update(code=code, steps=count, programs=programs or {})
                c['expected'].update(expected or {})
                c['accesses'] = accesses
                c['memory'] = {a: v for a, w, v in accesses if w}
                c['policy_accesses'] = accesses
                finish(c, target, sp, psw, count, tfr)
                c['ends'] = sorted({target, *({8, 0x10, 0x18, 0x28}-set(used))})
                if name.startswith('uncleared-'):
                    c['modes'] = ['stepi']; c['minimum_steps'] = count
                c['manual'] = 'C166S V1 PDF pp.49,64-67,98-100; complete-entry recovery is model policy'
                return c
            def f(sp, psw, dest):
                return frame(sp, psw, dest, segmented, mode)
            def pop_frame(sp, psw, dest):
                return [[a, 0, v] for a, _, v in reversed(f(sp, psw, dest))]
            for flag, dest in ((0xe000, 8), (0x6000, 0x10), (0x4080, 0x10)):
                yield build(f'priority-{flag:x}', 'e6d6'+flag.to_bytes(2,'little').hex(),
                            1, dest, 0xfb80-size, 0xf047 if flag==0xe000 else 0xf046,
                            flag, f(0xfb80, 0x2047 if flag==0xe000 else 0x2046, pc+4))
            # The service routine clears its own flag or both flags, then RETI.
            for clear_all in (False, True):
                words=f(0xfb80,0x2046,pc+4)
                accesses=words+pop_frame(0xfb80,0x2046,pc+4)
                if not clear_all:accesses+=words
                yield build('clear-all' if clear_all else 'pending-after-reti',
                            'e6d60060', 3, pc+4 if clear_all else 0x18,
                            0xfb80 if clear_all else 0xfb80-size,
                            0x2046 if clear_all else 0xf046, 0 if clear_all else 0x2000,
                            accesses, {0x10:'e6d60000fb88' if clear_all else 'e6d60020fb88'},
                            used=(0x10,))
            for flag,dest in ((0x4000,0x10),(0x80,0x28)):
                words=f(0xfb80,0x2046,pc+4)
                yield build(f'uncleared-{flag:x}', 'e6d6'+flag.to_bytes(2,'little').hex(),
                            2,dest,0xfb80-size,0xf046,flag,
                            words+pop_frame(0xfb80,0x2046,pc+4)+words,
                            {dest:'fb88'},used=(dest,))
                # Continuing needs a later breakpoint to observe the second
                # entry; stepi directly observes the RETI arbitration boundary.
                # The final PC equals the first entry, so avoid early continue.
            # Class B breaks a protected sequence, then waits for Class A RETI.
            words=f(0xfb80,0x2046,pc+6)
            yield build('class-b-breaks-window', 'd130e6d68040', 2,0x10,
                        0xfb80-size,0xf046,0x4080,words)
            yield build('class-b-waits-for-reti', 'd130e6d68040', 5,0x28,
                        0xfb80-size,0xf046,0x80,
                        words+pop_frame(0xfb80,0x2046,pc+6)+words,
                        {0x10:'e6d68000cc00fb88'},used=(0x10,))
            # A real fault, rather than a software flag write, breaks protection.
            producer=[[physical(0xfb7e,mode),1,0x1234]]
            yield build('fault-releases-pending', 'd130ecf04400',3,0x10,
                        0xfb7e-size,0xf046,0x4080,
                        producer+f(0xfb7e,0x2046,pc+4),initial={'stkov':0xfb80})
            words=f(0xfb80,0x2046,pc+4)
            yield build('fault-during-class-a', 'e6d60040',5,0x28,
                        0xfb80-size,0xf046,0x80,
                        words+pop_frame(0xfb80,0x2046,pc+4)+words,
                        {0x10:'4400cc00e6d68000fb88'},used=(0x10,0x18))
            # Both implicit requests accumulate before the end of ATOMIC #2.
            accesses=producer+[[physical(0xfb7e,mode),0,0x1234]]+f(0xfb80,0x2046,pc+6)
            yield build('two-implicit-requests', 'd110ecf0fcf0',3,0x10,
                        0xfb80-size,0xf046,0x6000,accesses,
                        initial={'stkov':0xfb80,'stkun':0xfb7e})
            # The original Class-B frame is completed, then stack overflow wins.
            words=f(0xfb80,0x2046,pc)
            yield build('class-b-entry-overflow', '4400',1,0x10,
                        0xfb80-2*size,0xf046,0x4080,
                        words+f(0xfb80-size,0xf046,0x28),initial={'stkov':0xfb7e})
            # Class-A entry records another request without recursively entering.
            yield build('nmi-entry-overflow', 'e6d60080',1,8,0xfb80-size,
                        0xf057,0xc000,f(0xfb80,0x2057,pc+4),initial={'stkov':0xfb80})
            # A Class-A handler may contain software TRAP/RETI; that RETI must
            # not release the outer service priority.
            words=f(0xfb80,0x2046,pc+4)
            inner=f(0xfb80-size,0xf046,0x12)
            yield build('software-reti-keeps-class-a', 'e6d68040',3,0x12,
                        0xfb80-size,0xf046,0x4080,
                        words+inner+pop_frame(0xfb80-size,0xf046,0x12),
                        {0x10:'9b40cc00',0x80:'fb88'},used=(0x10,))
            # Class A can preempt a Class-B handler. Bounds are programmed by
            # that handler; the earlier bound-register write itself is no trigger.
            words=f(0xfb80,0x2046,pc+4)
            current=0xfb80-size
            yield build('class-a-preempts-b', 'e6d68000',3,0x10,
                        current-2-size,0xf046,0x4080,
                        words+[[physical(current-2,mode),1,0x1234]]+
                        f(current-2,0xf046,0x2e),
                        {0x28:'e60a'+current.to_bytes(2,'little').hex()+'ecf0'},used=(0x28,))
            # An IRQ may be enabled by a Class-A handler. Its overflow request
            # waits for the outer Class-A RETI, including after the IRQ's RETI.
            for returned in (False, True):
                outer=f(0xfb80,0x2046,pc+4)
                irq=f(0xfb80-size,0x0846,0x1c)
                c=build('irq-return-keeps-class-a' if returned else 'irq-during-class-a',
                        'e6d60040',6 if returned else 4,
                        0x1e if returned else 0xa4,
                        0xfb80-size if returned else 0xfb80-2*size,
                        0x0846 if returned else 0xf846,0x4000,
                        outer+irq+(pop_frame(0xfb80-size,0x0846,0x1c) if returned else []),
                        {0x10:'e6d60000e60a'+(0xfb80-size).to_bytes(2,'little').hex()+
                         'e6884608cc00',0xa4:'fb88'},used=(0x10,0x18))
                # Guard the NOP after IRQ return, not the current PC at 001c:
                # a breakpoint there would step over before accepting the IRQ.
                # These remote-control counts are not hardware timing claims.
                # IRQ stores have their existing separate trace boundary.
                c['accesses']=outer+(pop_frame(0xfb80-size,0x0846,0x1c) if returned else [])
                c['pre_steps']=4;c['irq']=True;c['modes']=['continue']
                yield c
            # ADD/SUB requests survive a following MOV SP / bound update. One
            # following retirement is the selected policy within V1 p.66's range.
            for following,code,post,psw in (
                    ('move-sp','e60980fb',0xfb80,0x2041),
                    ('move-bound','e60a00f6',0xfb7e,0x2041),
                    ('counted-read','f2f106e0',0xfb7e,0x2040)):
                c=build('arithmetic-'+following,'26090200'+code,2,0x10,
                        post-size,psw|0xf000,0x4000,f(post,psw,pc+8),
                        initial={'stkov':0xfb80},
                        expected={'r1':0x5678} if following=='counted-read' else {})
                if following=='counted-read':
                    c['machines']=['c166-test'];c['seeds'][0xe006]=0x5678
                    c['counted_reads']=1
                yield c
            # Nested prefixes reload their one shared counter. Pending requests
            # remain until the new window (including its replacement addressing)
            # retires; the old window length must not leak into the handler.
            yield build('nested-prefix','d130ecf0d110cc00cc00',5,0x10,
                        0xfb7e-size,0xf046,0x4000,
                        producer+f(0xfb7e,0x2046,pc+10),initial={'stkov':0xfb80})


def deferred_replay_cases():
    """Reuse conformance fixtures; add explicit watched retirement boundaries.

    Arithmetic's one-following-instruction choice is model policy. The original
    conformance gate continues to accept either manual-permitted boundary.
    """
    for source in (*arithmetic_cases(), *window_cases()):
        if source['group'] == 'negative':
            continue
        if source['group'] == 'window':
            prefix, length, position, *_ = source['name'].split('-')
            if not ((length, position) == ('4', '0') or
                    (prefix == 'atomic' and (length, position) in (('1', '0'), ('4', '3')))):
                continue
        for mode in (7, 0):
            c = copy.deepcopy(source)
            c['name'] = 'replay-'+c['name']+('-linear' if mode == 7 else '-circular')
            c['initial']['syscon'] = (mode << 13) | (0 if c['segmented'] else 0x800)
            # FA02 crosses the 512-byte physical window during frame entry.
            delta = 0 if mode == 7 else -0x17e
            c['initial']['sp'] += delta
            c['expected']['sp'] += delta
            for bound in ('stkov', 'stkun'):
                if 0xfb70 <= c['initial'][bound] <= 0xfb90:
                    c['initial'][bound] += delta
            c['memory'] = {physical(a+delta, mode): v for a, v in c['memory'].items()}
            c['seeds'] = {physical(a+delta, mode) if 0xf600 <= a < 0xfc80 else a: v
                          for a, v in c['seeds'].items()}
            c['accesses'] = [[physical(a+delta, mode), w, v] for a, w, v in c['accesses']]
            c['replay_prepare'] = 0
            c['replay_phases'] = []
            if 'allowed_counts' in c:
                f = c.pop('delayed_frame'); c.pop('allowed_counts')
                c['expected']['guest_icount'] = 2
                c['accesses'] = frame(f['sp']+delta, f['psw'], f['pc']+6, c['segmented'], mode)
                c['memory'] = {a: v for a, _, v in c['accesses']}
                c['replay_policy'] = 'one following arithmetic retirement'
            if c['group'] == 'window':
                prefix_size = 2 if prefix in ('atomic', 'extr') else 4
                producer_pc = c['initial']['pc']+prefix_size+2*(int(position)+1)
                if int(position)+1 == int(length):
                    c['replay_phases'] = [(c['expected']['pc'], c['expected']['guest_icount'], c['accesses'])]
                else:
                    c['replay_phases'] = [(producer_pc, int(position)+2, c['accesses'][:1]),
                                          (c['expected']['pc'], c['expected']['guest_icount'], c['accesses'][1:])]
            else:
                c['replay_phases'] = [(c['expected']['pc'], c['expected']['guest_icount'], c['accesses'])]
            c['replay_next'] = {'pc': c['expected']['pc']+2,
                                'guest_icount': c['expected']['guest_icount']+1}
            c['replay_repeat'] = bool(c['expected']['tfr'])
            yield c
    for source in deferred_cases():
        c = copy.deepcopy(source)
        c['name'] = 'replay-'+c['name']
        words = 3 if c['segmented'] else 2
        count = c['expected']['guest_icount']
        c['replay_prepare'] = count-1
        accesses = c['accesses']
        name = source['name']
        if any(x in name for x in ('pending-after-reti', 'clear-all', 'uncleared-',
                                  'class-b-waits-for-reti', 'fault-during-class-a')):
            accesses = accesses[words:]
        elif 'software-reti' in name:
            accesses = accesses[-words:]
        elif 'class-a-preempts-b' in name or 'two-implicit-requests' in name:
            accesses = accesses[-words-1:]
        elif 'fault-releases-pending' in name or 'nested-prefix' in name:
            accesses = accesses[-words:]
        c['replay_phases'] = [(c['expected']['pc'], count, accesses)]
        c['replay_next'] = {'pc': c['expected']['pc']+2, 'guest_icount': count+1}
        if 'uncleared-' in name:
            c['replay_next']['pc'] = c['expected']['pc']
        if c.get('irq'):
            c['replay_prepare'] = c['pre_steps']
            irq = c['policy_accesses'][words:2*words]
            c['replay_phases'] = [(0xa4, 4, irq)]
            if 'irq-return' in name:
                c['replay_phases'].append((0x1c, 5, c['accesses'][words:]))
            else:
                c['replay_next'].update(pc=0x1c, sp=0xfb80-2*words, psw=0x0846)
        c['replay_repeat'] = 'arithmetic-' in name or 'nested-prefix' in name
        yield c


def deferred_replay_watches(case, before):
    """First matching retired access, including value filtering at a boundary."""
    phases = case['replay_phases']
    addresses = list(dict.fromkeys(a for _, _, accesses in phases for a, _, _ in accesses))
    if not addresses:
        addresses = [physical(0xfb7e, case['initial']['syscon'] >> 13)]
    yield dict(command='none', address=0, hit=None)
    for address in addresses:
        for command in ('watch', 'rwatch', 'awatch'):
            value = before.get(address, 0xa5a5)
            hit = None
            for pc, count, accesses in phases:
                matching = [(w, v) for a, w, v in accesses if a == address]
                writes = [v for w, v in matching if w]
                changed = bool(writes) and writes[-1] != value
                if (command == 'watch' and changed or
                        command == 'rwatch' and any(not w for w, _ in matching) or
                        command == 'awatch' and matching):
                    hit = dict(pc=pc, guest_icount=count)
                    break
                if writes:
                    value = writes[-1]
            yield dict(command=command, address=address, hit=hit,
                       initial_value=before.get(address, 0xa5a5))



def replay_state(observed):
    """Compare effects and architectural state, excluding debugger stop metadata."""
    return dict(registers=observed['states'][-1]['registers'],
                **{key: observed[key] for key in
                   ('memory', 'stack_accesses', 'deliveries', 'counted_reads')})


def replay_differences(observed, control):
    actual, expected = replay_state(observed), replay_state(control)
    return {key: dict(expected=expected[key], actual=value)
            for key, value in actual.items() if value != expected[key]}


def assess(case, observed):
    """Return every contract mismatch; retain raw observations even on failure."""
    registers = observed['states'][-1]['registers']
    failures = {}
    for reg, expected in case['expected'].items():
        if registers[reg] != expected:
            failures[reg] = dict(expected=expected, actual=registers[reg])
    for reg, (mask, expected) in case.get('masked', {}).items():
        if registers[reg] & mask != expected:
            failures[reg] = dict(mask=mask, expected=expected, actual=registers[reg])
    expected_memory = dict(case['memory'])
    expected_accesses = case['accesses']
    if 'allowed_counts' in case:
        count = registers['guest_icount']
        if count not in case['allowed_counts']:
            failures['delivery_count'] = dict(expected=case['allowed_counts'], actual=count)
        # Correlate saved IP with the observed boundary, not independent sets.
        f = case['delayed_frame']
        accesses = frame(f['sp'], f['psw'], f['pc']+4+2*(count-1), case['segmented'])
        expected_memory.update({a: v for a, _, v in accesses})
        expected_accesses = accesses
    raw = bytes.fromhex(observed['memory'])
    for address, value in expected_memory.items():
        actual = int.from_bytes(raw[address-0xf600:address-0xf600+2], 'little')
        if actual != value:
            failures[f'memory:{address:04x}'] = dict(expected=value, actual=actual)
    if expected_accesses is not None:
        actual = observed['stack_accesses']
        compared = actual[:len(expected_accesses)] if case['access_prefix'] else actual
        if compared != expected_accesses:
            failures['stack_accesses'] = dict(expected=expected_accesses, actual=actual)
    return failures
