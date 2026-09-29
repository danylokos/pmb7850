"""Bounded managed GDB launch, transport, and cleanup qualification."""
import json
import os
from pathlib import Path
import pty
import signal
import socket
import subprocess
import sys
import tempfile
import termios
import time

from tools.qualification.shared.transport import (
    Observer, Terminal, controlling_terminal, hold_key, parse_args, paused,
    qualify, stopped, wire,
)


class Transport:
    name = 'qemu-managed'
    native = False
    managed = True
    expected_status = 0

    def __init__(self, args):
        self.args = args

    def configure_command(self, command):
        command += ['--engine', 'qemu', '--qemu-binary', str(self.args.qemu)]
        command += ['-i', '--gdb-binary', str(self.args.gdb), '-c', 'set pagination off',
                    '-c', 'set confirm off']

    def initial_prompt(self, terminal, process, master):
        terminal.prompt()
        assert os.tcgetpgrp(master) != process.pid

    def break_and_step(self, terminal, out):
        terminal.command('break *0x800102')
        assert 'Breakpoint 1' in terminal.command('continue')
        assert '0x00800104' in terminal.command('stepi')
        terminal.command('delete breakpoints')

    def configure_serial(self, terminal):
        terminal.command('set $pc=0x800200')
        terminal.command('stepi 2')

    def resume(self, master):
        os.write(master, b'cont\n')

    def interrupt(self, terminal, process, master):
        os.write(master, b'\x03')
        assert 'SIGINT' in terminal.prompt()
        assert '0x005a' in terminal.command('x/hx 0xfeb2')

    def finish(self, terminal, process, master, observer, current,
               use_pty, serial_fd, out, original_termios):
        # Direct SIGINT to EMU is forwarded to GDB during execution.
        os.write(master, b'continue\n')
        observer.fresh(wire.STATS, lambda p: p.icount > current.icount + 1000)
        process.send_signal(signal.SIGINT)
        terminal.prompt()
        paused(observer)
        # Cancel an unfinished readline input, then suspend the native prompt.
        os.write(master, b'pending command\x03')
        terminal.prompt()
        os.write(master, b'\x1a')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if Path(f'/proc/{process.pid}/status').read_text().split('State:')[1].lstrip().startswith('T'):
                break
            time.sleep(.01)
        else:
            raise AssertionError('EMU did not suspend with GDB')
        assert os.tcgetpgrp(master) == process.pid
        assert termios.tcgetattr(master) == original_termios
        process.send_signal(signal.SIGCONT)
        terminal.command('info registers pc')
        os.write(master, b'quit\n')

    def close(self):
        pass


def children(process):
    """Capture only this runner's direct children and owned temporary paths."""
    pids = [int(p) for p in Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split()]
    owned = set()
    binaries = {}
    for pid in pids:
        try:
            argv = Path(f'/proc/{pid}/cmdline').read_bytes().decode().split('\0')
        except FileNotFoundError:
            continue  # child exited between the two procfs reads
        if not argv[0]:
            continue
        binaries[Path(argv[0]).name] = pid
        for arg in argv:
            for word in arg.replace(',', ' ').replace('=', ' ').split():
                if word.startswith('/tmp/emu-gdb-') or word.startswith('/tmp/emu-qemu-'):
                    owned.add(Path('/tmp') / word.split('/')[2])
    return pids, owned, binaries


def assert_clean(pids, owned):
    assert all(not Path(f'/proc/{pid}').exists() for pid in pids), pids
    assert all(not path.exists() for path in owned), owned


def qualify_managed_shutdown(args, mode):
    out = args.output / ('managed-' + mode)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='managed-stop-') as temp:
        temp = Path(temp)
        ui, serial = temp/'ui.sock', temp/'asc0'
        command = [str(args.emu), 'run', str(args.fixture), '--device', 'c55',
                   '--engine', 'qemu', '--qemu-binary', str(args.qemu),
                   '--gdb-binary', str(args.gdb), '--ui-socket', str(ui),
                   '--serial-pty', str(serial), '-c', 'set confirm off']
        if mode == 'stubborn-gdb':
            stubborn = temp/'stubborn gdb'
            stubborn.write_text(f'#!{sys.executable}\nimport signal, termios, time\n'
                                'signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
                                'settings=termios.tcgetattr(0)\n'
                                'settings[3] &= ~termios.ECHO\n'
                                'termios.tcsetattr(0, termios.TCSANOW, settings)\n'
                                'print("(gdb) ", end="", flush=True)\n'
                                'while True: time.sleep(1)\n')
            stubborn.chmod(0o755)
            command[command.index('--gdb-binary')+1] = str(stubborn)
        if mode == 'startup-break':
            command += ['-c', 'break *0x800100', '-c', 'continue']
        if mode == 'term':
            script = temp/'startup commands.gdb'
            script.write_text('echo SCRIPT_STARTED\\n\n')
            command += ['--script', str(script)]
        batch = mode.startswith('batch-')
        if batch:
            command += ['-c', 'set {unsigned short}0xf600=0xff0d',
                        '-c', 'set $pc=0xf600', '-c', 'continue']
        else:
            command += ['-i']
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                                   preexec_fn=controlling_terminal)
        os.close(slave)
        terminal, observer = Terminal(process, master, gdb=True), None
        pids, owned = [], set()
        try:
            if not batch:
                initial = terminal.prompt()
                if mode == 'term':
                    assert 'SCRIPT_STARTED' in initial
                if mode == 'startup-break':
                    assert 'Breakpoint 1,' in initial
                    assert '0x800100' in terminal.command('info registers pc')
            observer = Observer(process, ui, out, time.monotonic()+15, serial=False)
            if batch:
                observer.fresh(wire.STATS, lambda p: p.icount > 1000)
            else:
                hold_key(observer)
            pids, owned, binaries = children(process)
            assert len(pids) == 2, binaries
            assert len(owned) == 2, owned
            expected = 0
            if mode == 'eof':
                os.write(master, b'\x04')
            elif mode == 'gdb-crash':
                os.kill(binaries[args.gdb.name], signal.SIGKILL)
                expected = 137
            elif mode == 'qemu-crash':
                os.kill(binaries[args.qemu.name], signal.SIGKILL)
                expected = 2
            elif mode == 'batch-int':
                os.write(master, b'\x03')
                expected = 130
            else:
                process.send_signal(signal.SIGTERM)
                expected = 143
            process.wait(timeout=10)
            stopped(observer)
            assert process.returncode == expected, (mode, process.returncode)
            assert termios.tcgetattr(master) == original
            assert not ui.exists() and not serial.exists()
            assert_clean(pids, owned)
        finally:
            if observer:
                observer.close()
            if process.poll() is None:
                process.send_signal(signal.SIGCONT)
                process.terminate()
                process.wait(timeout=10)
            os.close(master)
            (out/'terminal.log').write_bytes(terminal.log)
    print('managed-' + mode + ': PASS')


def qualify_managed_launch(args):
    out = args.output / 'managed-launch'
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='managed launch ') as temp:
        temp = Path(temp)
        binary = temp/'gdb with spaces'
        binary.symlink_to(args.gdb.resolve())
        script = temp/'commands with spaces.gdb'
        script.write_text('set $order=$order*10+2\n')
        base = [str(args.emu), 'run', str(args.fixture), '--device', 'c55',
                '--engine', 'qemu', '--qemu-binary', str(args.qemu)]
        env = dict(os.environ, HOME=str(temp))
        (temp/'.gdbinit').write_text('echo UNWANTED_INIT\nquit 99\n')
        records = []

        def run(name, extra, expected=0, command=None):
            process = subprocess.Popen(command or base + extra, cwd=temp, env=env,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            pids, owned = set(), set()
            deadline = time.monotonic() + 15
            try:
                while True:
                    try:
                        current_pids, paths, _ = children(process)
                        pids.update(current_pids)
                        owned.update(paths)
                    except FileNotFoundError:
                        pass
                    try:
                        output, _ = process.communicate(timeout=.01)
                        break
                    except subprocess.TimeoutExpired:
                        if time.monotonic() >= deadline:
                            raise
                result = subprocess.CompletedProcess(process.args, process.returncode, output)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
            assert_clean(pids, owned)
            (out/(name+'.log')).write_bytes(result.stdout)
            assert result.returncode == expected, (name, result.returncode, result.stdout)
            assert b'UNWANTED_INIT' not in result.stdout
            records.append(dict(name=name, command=command or base+extra, status=result.returncode))
            return result.stdout

        common = ['--gdb-binary', str(binary)]
        order = ['-c', 'set $order=1', '--script', str(script),
                 '--command', 'set $order=$order*10+3', '-c', 'print $order']
        result = run('batch-order', common + order + ['--dump-flash', '--label', 'managed'])
        assert b'= 123' in result
        manifest = json.loads(next((temp/'shots').rglob('manifest.json')).read_text())
        assert manifest['debugger']['mode'] == 'batch'
        assert manifest['debugger']['binary'] == str(binary)
        assert manifest['debugger']['actions'][1] == dict(script=str(script))
        endpoint = manifest['transport']['gdb']
        assert endpoint.startswith('unix:/tmp/emu-gdb-')
        assert not Path(endpoint[5:]).parent.exists()
        assert (temp/'shots/managed/flash.bin').exists()
        result = run('source', common + ['-c', 'set $order=4', '-c', f'source {script}', '-c', 'print $order'])
        assert b'= 42' in result
        script_only = temp/'script only.gdb'
        script_only.write_text('echo SCRIPT_ONLY\\n\n')
        assert b'SCRIPT_ONLY' in run('script-only', common+['--script', str(script_only)])
        run('batch-error', common + ['-c', 'nonexistent_command'], 1)
        run('batch-exit', common + ['-c', 'quit 7'], 7)
        run('native-exit-127', common + ['-c', 'quit 127'], 127)
        # This GDB warns but returns zero for a missing -x file; preserve it.
        assert b'No such file' in run('missing-script', common + ['--script', str(temp/'absent.gdb')])
        # No override: exercise the Make-selected project GDB binary.
        assert b'pc ' in run('default-binary', ['-c', 'info registers pc'])
        with socket.socket() as port_socket:
            port_socket.bind(('127.0.0.1', 0))
            port = port_socket.getsockname()[1]
        assert b'pc ' in run('explicit-port', common + ['--gdb', str(port), '-c', 'info registers pc'])

        marker = temp/'qemu-launched'
        fake_qemu = temp/'fake qemu'
        fake_qemu.write_text(f'#!/bin/sh\ntouch "{marker}"\nexit 1\n')
        fake_qemu.chmod(0o755)
        missing = base + ['--qemu-binary', str(fake_qemu), '--gdb-binary', str(temp/'missing'), '-i']
        assert b'make -C emu gdb' in run('missing-binary', [], 2, missing)
        assert not marker.exists(), 'QEMU launched before validating GDB'
        run('unsupported-limit', [], 2, base+common+['--qemu-binary', str(fake_qemu), '-i', '--limit', '1'])
        assert not marker.exists(), 'unsupported QEMU option reached child creation'
        nonexec = temp/'non executable'
        nonexec.write_text('not executable')
        run('nonexecutable', [], 2, base+['--gdb-binary', str(nonexec), '-i'])
        run('missing-qemu', [], 2, base+common+['--qemu-binary', str(temp/'absent'), '-i'])
        # Exec failure after a successful X_OK check must also clean up.
        badexec = temp/'bad executable'
        badexec.write_text('#!/does/not/exist\n')
        badexec.chmod(0o755)
        run('exec-failure', ['--gdb-binary', str(badexec), '-i'], 2)

        # A small stand-in verifies exact argv, no shell evaluation, and cleanup
        # of both children and private directories on a native nonzero exit.
        fake = temp/'argv gdb'
        received = temp/'received.json'
        fake.write_text(f'#!{sys.executable}\nimport json, os, pathlib, sys\n'
                        f'pathlib.Path({str(received)!r}).write_text(json.dumps(dict(argv=sys.argv, '
                        'pid=os.getpid(), ppid=os.getppid())))\nsys.exit(9)\n')
        fake.chmod(0o755)
        native = 'echo "literal ; $(touch NEVER) `touch NEVER2`"'
        run('exact-argv', ['--gdb-binary', str(fake), '-c', native, '--script', str(script), '-i'], 9)
        received_data = json.loads(received.read_text())
        argv = received_data['argv']
        assert argv[1:6] == ['-nx', '-nh', '-q', '-iex', 'set auto-load off']
        assert argv[-4:] == ['-ex', native, '-x', str(script)]
        assert not (temp/'NEVER').exists() and not (temp/'NEVER2').exists()
        assert not Path(argv[7].removeprefix('target remote ')).parent.exists()
        assert not Path(f"/proc/{received_data['pid']}").exists()
        # Make GDB connect to a nonexistent socket, independently of QEMU's
        # healthy stub. A real connection error must preserve native status.
        failing = temp/'connection failure gdb'
        failing.write_text(f'#!{sys.executable}\nimport os\nos.execv({str(args.gdb)!r}, '
                           f'[{str(args.gdb)!r}, "-nx", "-nh", "--batch", "-ex", '
                           f'"target remote {temp}/absent"])\n')
        failing.chmod(0o755)
        run('connection-failure', ['--gdb-binary', str(failing), '-c', 'info registers'], 1)
        (out/'results.json').write_text(json.dumps(records, indent=2)+'\n')
    print('managed-launch: PASS')


def main():
    args = parse_args(__doc__, needs_gdb=True)
    qualify_managed_launch(args)
    for mode in ("eof", "term", "batch-int", "batch-term", "gdb-crash", "qemu-crash",
                 "stubborn-gdb", "startup-break"):
        qualify_managed_shutdown(args, mode)
    for use_pty in (False, True):
        qualify(args, Transport(args), use_pty)


if __name__ == '__main__':
    main()
