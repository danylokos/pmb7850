"""Bounded CEMU native debugger transport and shutdown qualification."""
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import tempfile
import time

from tools.qualification.shared.transport import (
    Observer, Terminal, hold_key, parse_args, paused,
    qualify, stopped, wire,
)


class Transport:
    name = 'cemu'
    native = True
    managed = False
    expected_status = 0

    def __init__(self, args):
        self.args = args

    def configure_command(self, command):
        command += ['--interactive']

    def initial_prompt(self, terminal, process, master):
        terminal.prompt()

    def break_and_step(self, terminal, out):
        terminal.command('break 0x800102')
        assert '[break]' in terminal.command('cont')
        assert 'pc=0x00800104' in terminal.command('step')
        terminal.command('delete 0x800102')

    def configure_serial(self, terminal):
        terminal.command('setr pc 0x800200')
        terminal.command('step 2')  # configure ASC0 before accepting RX

    def resume(self, master):
        os.write(master, b'cont\n')

    def interrupt(self, terminal, process, master):
        process.send_signal(signal.SIGINT)
        assert '[interrupted]' in terminal.prompt()
        assert '0x005a' in terminal.command('sfr 0xfeb2')

    def finish(self, terminal, process, master, observer, current,
               use_pty, serial_fd, out, original_termios):
        hold_key(observer)
        terminal.command('checkpoint')
        before = paused(observer)
        terminal.command('step 7')
        terminal.command('setr pc 0x800208')
        terminal.command('cont 1000')
        terminal.command('restore')
        after = paused(observer)
        assert after.icount == before.icount
        assert wire.decode_stats(after.payload)[1] == wire.decode_stats(before.payload)[1] + 1007
        assert after.sequence > before.sequence
        observer.fresh(wire.FRAME, after=before.sequence)
        observer.fresh(wire.AUDIO_RESET, after=before.sequence)
        # Replay only new TX; restoring must not append old output again.
        terminal.command('setr pc 0x800208')
        terminal.command('cont 1000')
        if use_pty:
            assert select.select([serial_fd], [], [], 3)[0]
            assert os.read(serial_fd, 4096) == b'AA'
        else:
            observer.serial('restored')
            assert (out/'restored-serial.bin').read_bytes() == b'AAA'
        os.write(master, b'pending command')
        process.send_signal(signal.SIGINT)
        terminal.prompt()
        assert 'pc=' in terminal.command('regs')
        os.write(master, b'quit\n')

    def close(self):
        pass


def qualify_shutdown(args, mode):
    out = args.output / ('shutdown-' + mode)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='debugger-stop-') as temp:
        temp = Path(temp)
        ui, serial = temp/'ui.sock', temp/'asc0'
        # Write a local-RAM spin, preserving the supplied fixture on disk.
        script = 'set 0xf600 0xff0d; setr pc 0xf600; cont'
        command = [str(args.emu), 'run', str(args.fixture), '--device', 'c55',
                   '--ui-socket', str(ui), '--serial-pty', str(serial)]
        if mode == 'command-int':
            command += ['--command', script]
        elif mode == 'script-int':
            commands = temp/'commands.txt'
            commands.write_text(script)
            command += ['--script', str(commands)]
        else:
            command += ['--interactive']
        master, slave = pty.openpty()
        process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                                   start_new_session=True)
        os.close(slave)
        terminal, observer = Terminal(process, master), None
        try:
            if mode in ('eof', 'term'):
                terminal.prompt()
            observer = Observer(process, ui, out, time.monotonic()+15, serial=False)
            if mode in ('eof', 'term'):
                hold_key(observer)
            else:
                observer.fresh(wire.STATS, lambda p: p.icount > 0)
            if mode == 'eof':
                os.write(master, b'\x04')
                expected = 0
            else:
                sig = signal.SIGTERM if mode == 'term' else signal.SIGINT
                process.send_signal(sig)
                expected = 128 + sig
            process.wait(timeout=5)
            stopped(observer)
            assert process.returncode == expected
            assert not ui.exists() and not serial.exists()
        finally:
            if observer:
                observer.close()
            if process.poll() is None:
                process.send_signal(signal.SIGCONT)
                process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            os.close(master)
            (out/'terminal.log').write_bytes(terminal.log)
    print('shutdown-' + mode + ': PASS')


def main():
    args = parse_args(__doc__, needs_gdb=False)
    for use_pty in (False, True):
        qualify(args, Transport(args), use_pty)
    for mode in ('eof', 'term', 'command-int', 'script-int'):
        qualify_shutdown(args, mode)


if __name__ == '__main__':
    main()
