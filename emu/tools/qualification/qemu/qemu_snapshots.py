#!/usr/bin/env python3
"""Qualify shutdown/resume using the versioned handset corpus and stable EMU CLI.

This checks snapshot continuity; it does not assert CEMU/QEMU boot parity.
"""
import argparse
import json
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

from ..shared.support import ROOT, binary, digest
from ..shared.ui_observer import Observer
from ..shared.run_x55_boot import default_manifest, load_manifest, identity_arguments


def session(emu, qemu, out, args, ticks, paused=False):
    out.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='x55-snapshot-') as tmp:
        ui = Path(tmp) / 'ui'
        command = [str(emu), 'run', '--engine', 'qemu', '--qemu-binary', str(qemu),
                   '--snapshot', '--label', 'state', '--ui-socket', str(ui), *args]
        if paused:
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            command += ['--gdb', str(port)]
        (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        with (out / 'run.log').open('w') as log:
            process = subprocess.Popen(command, cwd=out, stdout=log, stderr=log)
            observer = None
            try:
                observer = Observer(process, ui, out, time.monotonic() + 180, serial=False)
                while True:
                    observed = observer.stats()
                    if paused or observed['ticks'] >= ticks:
                        break
                process.send_signal(signal.SIGINT)
                if process.wait(timeout=20) != 130:
                    raise AssertionError((out / 'run.log').read_text())
            finally:
                if observer:
                    observer.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()
        path = out / 'shots/state/snapshot'
        meta = json.loads((path / 'snapshot.json').read_text())
        return path, meta, observed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--emu', type=Path, default=ROOT / 'emu/bin/emu')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--matrix', type=Path, default=default_manifest())
    parser.add_argument('--devices', nargs='+')
    parser.add_argument('--ticks', type=int, help='override each no-SIM frontier run bound')
    parser.add_argument('--sim', action='store_true')
    args = parser.parse_args()
    matrix = load_manifest(args.matrix, ROOT, args.devices)
    emu, qemu = binary(args.emu), binary(args.qemu)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {'qemu': str(qemu), 'qemu_sha256': digest(qemu),
              'matrix': str(args.matrix), 'sim': args.sim, 'devices': {}}
    for device in args.devices or matrix['devices']:
        entry = matrix['devices'][device]
        options = [str(ROOT / entry['image']), '--device', device]
        options += identity_arguments(entry, ROOT)
        if args.sim:
            options += ['--sim']
        ticks = args.ticks or entry['no_sim']['minimum_ticks']
        try:
            seed, before, observed = session(emu, qemu, output / device / 'seed', options, ticks)
            hashes = {p.name: digest(p) for p in seed.iterdir()}
            resumed, after, _ = session(emu, qemu, output / device / 'restored',
                ['--from-snapshot', str(seed)], 0, paused=True)
            for field in ('icount', 'ticks', 'pc', 'chips', 'sim_stub', 'firmware_patches'):
                assert before[field] == after[field], field
            assert before['ticks'] >= ticks
            assert hashes == {p.name: digest(p) for p in seed.iterdir()}
            assert digest(ROOT / entry['image']) == entry['sha256']
            report['devices'][device] = dict(status='PASS', source=entry['image'],
                source_sha256=entry['sha256'], icount=before['icount'], ticks=before['ticks'],
                pc=hex(before['pc']), seed=str(seed), restored=str(resumed), last_stats=observed)
        except Exception as exc:
            report['devices'][device] = dict(status='FAIL', error=str(exc))
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(device, report['devices'][device], flush=True)
    return any(r['status'] != 'PASS' for r in report['devices'].values())


if __name__ == '__main__':
    raise SystemExit(main())
