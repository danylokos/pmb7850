#!/usr/bin/env python3
"""Bounded native GDB scripting gate; excludes unresolved Phase 6 workflows."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from . import gdb_registers
from .gdb_registers import Scenario, gdb_run, machine
from ..shared import support
from ..shared.support import ROOT, FIXTURE_DIR, binary, digest, revision

FIXTURES = FIXTURE_DIR / 'c166-execution.json'
EXAMPLES = Path(__file__).with_name('examples')
MARKER = 'C166_SCRIPTING_OK'
WORKFLOWS = ('inspect-step', 'breakpoint-loop')
FAILURES = ('assertion', 'invalid-source', 'early-exit', 'timeout')


def seed(out, workflow, fixtures):
    """Reuse fixture bytes, initial state and expected steps, without a compiler."""
    initial = dict(fixtures['initial'])
    if workflow == 'inspect-step':
        case = next(c for c in fixtures['cases'] if c['name'] == 'mixed-width-byte-write')
        initial.update(case['initial'])
    else:
        case = fixtures['breakpoint']
    setup = Scenario()
    for reg, value in initial.items():
        setup.setreg(reg, value)
    regions = {initial['pc']: case['code']}
    regions.update({int(addr, 0): raw for addr, raw in case.get('memory', {}).items()})
    for addr, raw in regions.items():
        data = bytes.fromhex(raw)
        if len(data) % 2:
            raise ValueError('Fixture requires aligned word writes')
        for i in range(0, len(data), 2):
            setup.setmem(addr + i, int.from_bytes(data[i:i+2], 'little'))
    setup.lines.append(f'set $base = {initial["pc"]}')
    (out / 'seed.gdb').write_text('\n'.join(setup.lines) + '\n')
    if workflow == 'inspect-step':
        lines = []
        for step in case['steps']:
            lines.append(step['command'])
            for reg, expected in step['registers'].items():
                lines.append(f'c166-assert-eq ${reg} {expected}')
            for addr, raw in step['memory'].items():
                for i, value in enumerate(bytes.fromhex(raw)):
                    lines.append(f'c166-assert-eq (*(unsigned char*){int(addr, 0)+i}) {value}')
        (out / 'step-checks.gdb').write_text('\n'.join(lines) + '\n')
        return 5 + sum(len(s['registers']) + sum(len(bytes.fromhex(v))
                       for v in s['memory'].values()) for s in case['steps'])
    return 19  # Four assertions per hit, four at final stop, three after stepi.


def run_case(gdb, qemu, out, kind, case, fixtures):
    out.mkdir(parents=True, exist_ok=True)
    for path in EXAMPLES.glob('*.gdb'):
        shutil.copyfile(path, out / path.name)
    expected_checks = seed(out, case if case in WORKFLOWS else 'breakpoint-loop', fixtures)
    expected_error = None
    if case in WORKFLOWS:
        script = case + '.gdb'
    else:
        script = 'failure.gdb'
        body = {
            'assertion': 'source common.gdb\nc166-assert-eq 1 2\n',
            'invalid-source': 'c166-deliberately-invalid-command\n',
            'early-exit': 'quit 0\n',
            'timeout': 'source seed.gdb\necho C166_TIMEOUT_ARMED\\n\ncontinue\n',
        }[case]
        (out / script).write_text(body)
        expected_error = {'assertion': 'C166_SCRIPT_FAILURE actual=1 expected=2',
                          'invalid-source': 'Undefined command: "c166-deliberately-invalid-command".'}.get(case)
    with machine(qemu, kind, out, 'session', extra_args=('-icount', 'shift=0,align=off,sleep=off')) as sock:
        lines = ['set logging file session-console.log', 'set logging overwrite on',
                 'set logging enabled on', 'set remotelogfile remote.log',
                 'set remotelogbase hex', f'target remote {sock}',
                 f'source -v {script}', 'disconnect',
                 f'echo {MARKER}\\n', 'set logging enabled off', 'quit 0']
        try:
            output = gdb_run(gdb, out, 'session', lines, expected_error,
                             success_marker=MARKER, cwd=out,
                             timeout=2 if case == 'timeout' else 30)
        except subprocess.TimeoutExpired:
            if case != 'timeout':
                raise
            output = (out / 'session.log').read_text()
            if 'C166_TIMEOUT_ARMED' not in output.splitlines():
                raise AssertionError('Timeout happened before the intended infinite guest loop')
        except AssertionError as error:
            if case != 'early-exit' or not str(error).startswith('Missing completion marker:'):
                raise
            output = (out / 'session.log').read_text()
        else:
            if case in ('timeout', 'early-exit'):
                raise AssertionError(f'{case} incorrectly reported success')
    lifecycle = json.loads((out / 'session-qemu-result.json').read_text())
    if not lifecycle['reaped'] or Path(lifecycle['socket']).parent.exists():
        raise AssertionError('QEMU process or temporary socket directory survived cleanup')
    if case in WORKFLOWS:
        counts = re.findall(r'^C166_SCRIPT_CHECKS=(\d+)$', output, re.MULTILINE)
        if counts != [str(expected_checks)]:
            raise AssertionError(f'Wrong assertion count: {counts}, expected {expected_checks}')
        logged = (out / 'session-console.log').read_text()
        if MARKER not in logged.splitlines() or not (out / 'remote.log').stat().st_size:
            raise AssertionError('Missing console completion or remote transcript')
    elif MARKER in output.splitlines():
        raise AssertionError('Failure reached the success marker')
    return {'case': case, 'machine': kind, 'status': 'pass',
            'assertions': expected_checks if case in WORKFLOWS else 0,
            'expected_failure': case in FAILURES}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', type=binary, required=True)
    parser.add_argument('--qemu', type=binary, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'emu/shots/gdb-c166-phase7/scripting')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    inputs = [Path(__file__), Path(gdb_registers.__file__), Path(support.__file__),
              FIXTURES, *sorted(EXAMPLES.glob('*.gdb'))]
    provenance = {'scope': 'bounded scripting; Phase 6 and full Phase 7 remain incomplete',
                  'synthetic': 'debugger-written state; no firmware, overlays or handset claim',
                  'command': sys.argv, 'cwd': str(Path.cwd()),
                  'revisions': {name: revision(path) for name, path in
                                [('root', ROOT), ('gdb', ROOT / 'gdb'), ('qemu', ROOT / 'qemu')]},
                  'binaries': {str(p): digest(p) for p in (args.gdb, args.qemu)},
                  'inputs': {str(p.relative_to(ROOT)): digest(p) for p in inputs}}
    (out / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    fixtures = json.loads(FIXTURES.read_text())
    results = []
    for kind in ('c166-test', 'pmb7850-test'):
        for case in (*WORKFLOWS, *FAILURES):
            try:
                result = run_case(args.gdb, args.qemu, out / f'{kind}-{case}', kind, case, fixtures)
            except Exception as error:
                result = {'case': case, 'machine': kind, 'status': 'fail',
                          'error': f'{type(error).__name__}: {error}'}
            results.append(result)
            print(f'{result["status"].upper()} {kind}/{case}' +
                  (f': {result["error"]}' if 'error' in result else ''), flush=True)
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return int(any(result['status'] != 'pass' for result in results))


if __name__ == '__main__':
    sys.exit(main())
