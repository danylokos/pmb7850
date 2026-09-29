#!/usr/bin/env python3
"""Firmware-dependent C55/M55 GDB qualification through the stable EMU host."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import queue
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

from tools import ui_protocol as wire
from ..shared import qmp, run_x55_boot, support, ui_observer
from ..shared.ui_observer import Observer
from . import gdb_execution, gdb_registers
from .gdb_execution import MI, breakpoint
from ..shared.support import ROOT, binary, digest, revision
from ..shared.run_x55_boot import common_command, default_manifest, load_manifest

OVERLAY_SHA256 = 'fb8c973218f3688985562712e05a06a945fa4ab885face40302847542a249e2f'
ENTRY = {'c55': 0x802fc4, 'm55': 0x800}
SESSION_SECONDS = 180


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def remaining(deadline, maximum):
    value = min(maximum, deadline - time.monotonic())
    if value <= 0:
        raise TimeoutError('handset session deadline expired')
    return value


class HandsetMI(MI):
    def __init__(self, gdb, out, deadline):
        self.deadline = deadline
        super().__init__(gdb, out, 'session')

    def command(self, command, timeout=30):
        return super().command(command, remaining(self.deadline, timeout))

    def wait(self, predicate, deadline):
        return super().wait(predicate, min(deadline, self.deadline))

    def no_pending_stop(self):
        # Include unread reader-queue events, not only already parsed records.
        while True:
            try:
                line = self.lines.get_nowait()
            except queue.Empty:
                break
            if line is None:
                raise EOFError(f'GDB exited: {self.proc.poll()}')
            self.log.write('< ' + line + '\n'); self.log.flush()
            self.records.append(line)
        if any(x.startswith(('*running,', '*stopped,')) for x in self.records):
            raise AssertionError('unconsumed GDB execution event')

    def resume(self, command):
        self.no_pending_stop()
        return super().resume(command)


def child_commands(process):
    """Linux ownership evidence: EMU's own children, never a global process match."""
    path = Path(f'/proc/{process.pid}/task/{process.pid}/children')
    result = {}
    for pid in path.read_text().split():
        try:
            result[int(pid)] = Path(f'/proc/{pid}/cmdline').read_bytes().decode().rstrip('\0').split('\0')
        except FileNotFoundError:
            pass
    return result


def stop_owned(process, children, socket_path, private, port, grace=15):
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
    forced = False
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        forced = True
    # The process group is created by this harness and contains EMU and QEMU only.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    if process.poll() is None:
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
    end = time.monotonic() + 2
    while any(Path(f'/proc/{pid}').exists() for pid in children) and time.monotonic() < end:
        time.sleep(0.02)
    survivors = [pid for pid in children if Path(f'/proc/{pid}').exists()]
    if survivors:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    with socket.socket() as probe:
        probe.settimeout(1)
        listening = port is not None and probe.connect_ex(('127.0.0.1', port)) == 0
    return dict(returncode=process.returncode, forced=forced, surviving_children=survivors,
                ui_socket_removed=not socket_path.exists(), gdb_listener_removed=not listening,
                private_removed=all(not path.exists() for path in private))


def memory(mi, addr, size):
    values = mi.command(f'-data-read-memory-bytes 0x{addr:x} {size}')['memory']
    return ''.join(v['contents'] for v in values)


def private_flash(children):
    return [Path(p) for cmd in children.values() for arg in cmd
            for p in re.findall(r'file=(/tmp/emu-qemu-[^,]+/flash-\d+\.bin)', arg)]


def run_session(args, device, entry, sim, out, *, debugger=True):
    out.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    deadline = started + SESSION_SECONDS
    result = dict(device=device, sim=sim, debugger=debugger, status='fail', stages={})
    config = entry['sim' if sim else 'no_sim']
    process = observer = mi = None
    children, private = {}, []
    port = None
    native = None
    with tempfile.TemporaryDirectory(prefix='gdb-handset-', dir='/tmp') as temporary:
        socket_path = Path(temporary) / 'ui.sock'
        command = common_command(args.emu, ROOT / entry['image'], device, entry, ROOT, sim)
        command += ['--engine', 'qemu', '--qemu-binary', str(args.qemu),
                    '--trace', 'sim', '--lcd-frames', '--label', 'run', '--ui-socket', str(socket_path)]
        if debugger:
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
            command += ['--gdb', str(port)]
        save(out / 'command.json', command)
        with (out / 'stdout.log').open('w') as stdout, (out / 'stderr.log').open('w') as stderr:
            try:
                process = subprocess.Popen(command, cwd=out, stdout=stdout, stderr=stderr,
                                           start_new_session=True)
                observer = Observer(process, socket_path, out, deadline)
                children = child_commands(process)
                assert len(children) == 1, children
                save(out / 'children.json', children)
                flash = private_flash(children)
                assert flash, children
                private = list({p.parent for p in flash})
                save(out / 'prepared-flash.json', {str(p): digest(p) for p in flash})
                native = private[0] / 'native-trace.log'
                assert observer.state['hello'][2].lower() == device
                if debugger:
                    mi = HandsetMI(args.gdb, out, deadline)
                    mi.wait(lambda line: line.startswith('(gdb)'), time.monotonic() + 5)
                    for setup in ('set pagination off', 'set confirm off', 'set remotetimeout 5',
                                  'set mi-async on', 'set breakpoint condition-evaluation host',
                                  f'set remotelogfile {out / "remote.log"}', 'set remotelogbase hex'):
                        mi.cli(setup)
                    mi.cli(f'target remote 127.0.0.1:{port}', timeout=5)
                    mi.event('stopped', time.monotonic() + 5)
                    resets = [observer.stats(0), observer.stats(0)]
                    assert all(s['pc'] == 0 for s in resets), resets
                    assert resets[1]['elapsed'] > resets[0]['elapsed'], resets
                    assert mi.evaluate('$pc') == mi.evaluate('$guest_icount') == 0
                    result['stages']['reset'] = resets
                    inspection = dict(registers=mi.command('-data-list-register-values x'),
                                      reset=memory(mi, 0, 16), dpram=memory(mi, 0xf600, 32),
                                      entry=memory(mi, ENTRY[device], 8),
                                      disassembly=mi.command(f'-data-disassemble -s {ENTRY[device]} -e {ENTRY[device]+4} -- 2'))
                    save(out / 'inspection.json', inspection)
                    assert inspection['entry'].startswith('a55aa5a5'), inspection
                    b = breakpoint(mi, f'break *0x{ENTRY[device]:x}')
                    result['stages']['breakpoint'] = mi.execute('continue', 'breakpoint-hit', ENTRY[device], b)
                    result['stages']['breakpoint']['guest_icount'] = mi.evaluate('$guest_icount')
                    assert result['stages']['breakpoint']['guest_icount'] == 1
                    mi.cli(f'delete {b}')
                    result['stages']['step'] = mi.execute('stepi', 'end-stepping-range', ENTRY[device]+4)
                    result['stages']['step']['guest_icount'] = mi.evaluate('$guest_icount')
                    assert result['stages']['step']['guest_icount'] == 2
                    result['stages']['step']['code_after'] = memory(mi, ENTRY[device], 8)
                    assert result['stages']['step']['code_after'] == inspection['entry']
                    mi.resume('continue')
                while True:
                    stats = observer.stats()
                    apdus = native.read_text().count('pmb7850_sim_apdu ')
                    if (stats['ticks'] >= config['minimum_ticks'] and
                            any(f['nonblank'] for f in observer.frames) and
                            apdus >= config.get('minimum_apdus', 0)):
                        break
                result['stages']['boot'] = dict(stats=stats, apdus=apdus,
                                                frames=list(observer.frames))
                if debugger:
                    mi.no_pending_stop()
                    mi.cli('interrupt')
                    event = mi.stop(time.monotonic()+30, 'signal-received')
                    assert event['signal-name'] == 'SIGINT', event
                    count = mi.evaluate('$guest_icount')
                    paused = [observer.stats(count), observer.stats(count)]
                    assert paused[1]['elapsed'] > paused[0]['elapsed'], paused
                    result['stages']['interrupt'] = dict(event=event, stats=paused,
                                                         serial=observer.serial('paused'))
                    assert mi.evaluate('$guest_icount') == count
                    mi.cli('detach')
                    mi.command('-gdb-exit')
                    mi.proc.wait(timeout=remaining(deadline, 5))
                    assert mi.proc.returncode == 0
                    progress = observer.stats()
                    assert progress['guest_icount'] > count, progress
                    result['stages']['detach'] = dict(stats=progress, serial=observer.serial('detached'))
                result['status'] = 'pass'
            except Exception as error:
                result['error'] = f'{type(error).__name__}: {error}'
            finally:
                if mi:
                    mi.close()
                if observer:
                    observer.close()
                if process:
                    if not children and process.poll() is None:
                        children = child_commands(process)
                        private = list({p.parent for p in private_flash(children)})
                        if private:
                            native = private[0] / 'native-trace.log'
                    # Preserve the native SIM stream before EMU removes its private directory.
                    if native and native.exists():
                        (out / 'native-sim.log').write_bytes(native.read_bytes())
                    cleanup = stop_owned(process, children, socket_path, private, port)
                    if mi:
                        cleanup['gdb_reaped'] = mi.proc.poll() is not None
                    save(out / 'cleanup.json', cleanup)
                    if (cleanup['forced'] or cleanup['surviving_children'] or
                            cleanup['returncode'] not in (0, 130) or
                            not all(cleanup[k] for k in ('ui_socket_removed', 'gdb_listener_removed', 'private_removed'))):
                        result['status'] = 'fail'
                        result['cleanup_error'] = cleanup
    result['wall_seconds'] = time.monotonic() - started
    save(out / 'result.json', result)
    return result


def preflight(args):
    for name in ('emu', 'gdb', 'qemu'):
        setattr(args, name, binary(getattr(args, name)))
    manifest = load_manifest(getattr(args, 'manifest', default_manifest()), ROOT,
                             ['c55', 'm55'])
    overlays = []
    for device, entry in manifest['devices'].items():
        if device not in ENTRY:
            continue
        identity = entry['identity']
        if identity['kind'] == 'overlay':
            overlay = ROOT / identity['path']
            if digest(overlay) != OVERLAY_SHA256:
                raise ValueError('EEPROM overlay SHA-256 mismatch')
            if overlay not in overlays:
                overlays.append(overlay)
    return manifest, overlays


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, default=default_manifest())
    parser.add_argument('--emu', type=Path, default=os.environ.get('EMU_BINARY', ROOT/'emu/bin/emu'))
    parser.add_argument('--gdb', type=Path, default=os.environ.get('GDB_BINARY', ROOT/'gdb/build/c166/gdb/gdb'))
    parser.add_argument('--qemu', type=Path, default=os.environ.get('QEMU_BINARY', ROOT/'qemu/build/release/qemu-system-c166'))
    parser.add_argument('--output', type=Path, default=os.environ.get('GDB_HANDSET_OUTPUT', ROOT/'emu/shots/gdb-c166-phase8/handsets'))
    args = parser.parse_args()
    try:
        manifest, overlays = preflight(args)
    except (ValueError, OSError, RuntimeError) as error:
        parser.error(str(error))
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any((out / f'{device}-{mode}').exists() for device in ENTRY for mode in ('no-sim', 'sim')):
        parser.error('session output already exists; choose a fresh output directory')
    inputs = [Path(__file__), Path(wire.__file__), args.manifest.resolve(), *overlays,
              *[Path(module.__file__) for module in
                (ui_observer, gdb_execution, gdb_registers, run_x55_boot, qmp, support)]]
    for source in inputs:
        relative = (source.relative_to(ROOT) if source.is_relative_to(ROOT)
                    else Path('external') / source.name)
        target = out / 'sources' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes())
    save(out / 'provenance.json', dict(command=sys.argv, cwd=str(Path.cwd()),
         revisions={name: revision(ROOT/name) for name in ('.', 'gdb', 'qemu')},
         binaries={str(p): digest(p) for p in (args.emu, args.gdb, args.qemu)},
         sources={str(p): digest(p) for p in inputs},
         devices={d: manifest['devices'][d] for d in ENTRY},
         deadlines=dict(session=180, transport=5, debugger=30, teardown=15),
         scope='Phase 8; does not close Phase 6 or full Phase 7'))
    results = []
    for device in ENTRY:
        for sim in (False, True):
            name = device + ('-sim' if sim else '-no-sim')
            result = run_session(args, device, manifest['devices'][device], sim, out/name)
            if result['status'] != 'pass':
                result['control'] = run_session(args, device, manifest['devices'][device], sim,
                                                out/(name+'-without-gdb'), debugger=False)
            results.append(result)
            save(out / 'results.json', results)
            print(f'{result["status"].upper()} {name}: {result.get("error", "workflow complete")}', flush=True)
    return int(any(r['status'] != 'pass' for r in results))


if __name__ == '__main__':
    sys.exit(main())
