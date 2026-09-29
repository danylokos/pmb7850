"""Shared fixture, terminal, observer, and serial lifecycle for transport probes."""
import argparse
import hashlib
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import tempfile
import termios
import time

from tools import ui_protocol as wire
from .ui_observer import Observer

from .support import ROOT


class Terminal:
    def __init__(self, process, fd, gdb=False):
        self.process, self.fd, self.log = process, fd, bytearray()
        self.gdb = gdb

    def prompt(self):
        data = bytearray()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError(f'runner exited: {self.process.returncode}: {data!r}')
            if select.select([self.fd], [], [], .05)[0]:
                chunk = os.read(self.fd, 65536)
                self.log.extend(chunk)
                data.extend(chunk)
                if (self.gdb and data.endswith(b'(gdb) ')) or (
                        not self.gdb and b'(dbg pc=' in data and data.endswith(b') ')):
                    return data.decode(errors='replace')
        raise TimeoutError(f'debugger prompt: {data!r}')

    def command(self, command):
        os.write(self.fd, command.encode() + b'\n')
        return self.prompt()


def controlling_terminal():
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


def paused(observer):
    first = observer.fresh(wire.STATS)
    second = observer.fresh(wire.STATS)
    a, b = wire.decode_stats(first.payload), wire.decode_stats(second.payload)
    assert second.sequence > first.sequence and b[4] > a[4]
    assert first.icount == second.icount and a[1:3] == b[1:3], (a, b)
    return second


def stopped(observer):
    deadline = time.monotonic() + 3
    with observer.condition:
        while time.monotonic() < deadline:
            for packet in observer.packets:
                if packet.message_type == wire.LIFECYCLE and wire.decode_lifecycle(packet.payload)['phase'] == 'stopped':
                    return
            observer.condition.wait(.01)
    raise AssertionError('final lifecycle was not drained')


def hold_key(observer):
    identity = observer.state['run_id']
    key = observer.state['hello'][3].index('1')
    observer.sock.sendall(wire.encode_packet(wire.OWNER_OPEN, wire.OWNER_TOKEN.pack(42), run_id=identity))
    observer.sock.sendall(wire.encode_packet(wire.OWNER_KEY,
        wire.OWNER_KEY_PAYLOAD.pack(42, key, wire.INPUT_PRESS), run_id=identity))
    paused(observer)


def qualify(args, transport, use_pty):
    """Run the common UI/PTY lifecycle with debugger-specific transport actions.

    Each probe supplies its command setup, debugger interaction, and final
    shutdown actions. Fixture bytes, event ordering, deadlines, and artifacts
    stay shared so both serial paths exercise the same observer contract.
    """
    name = transport.name + ('-pty' if use_pty else '-ui')
    out = args.output / name
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='debugger-transports-') as temp:
        temp = Path(temp)
        image = bytearray(args.fixture.read_bytes())
        image[0x100:0x106] = bytes.fromhex('cc00 cc00 0dfd')
        # MOV S0BG,#0; MOV S0CON,#8011; MOV S0TBUF,#41; JMPR cc_UC,self.
        image[0x200:0x20e] = bytes.fromhex('e65a0000 e6d81180 e6584100 0dff')
        flash, ui, serial = temp/'fixture.bin', temp/'ui.sock', temp/'asc0'
        flash.write_bytes(image)
        command = [str(args.emu), 'run', str(flash), '--device', 'c55', '--ui-socket', str(ui)]
        if use_pty:
            command += ['--serial-pty', str(serial)]
        transport.configure_command(command)
        (out/'input.json').write_text(json.dumps(dict(command=command,
            sha256=hashlib.sha256(image).hexdigest(), synthetic=True), indent=2)+'\n')
        master, slave = pty.openpty()
        original_termios = termios.tcgetattr(slave)
        process = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave,
                                   preexec_fn=controlling_terminal)
        os.close(slave)
        terminal = Terminal(process, master, gdb=transport.managed)
        observer = None
        serial_fd = None
        try:
            transport.initial_prompt(terminal, process, master)
            initial_out = out/'initial'
            initial_out.mkdir(exist_ok=True)
            observer = Observer(process, ui, initial_out, time.monotonic()+45, serial=not use_pty)
            initial = paused(observer)
            identity = initial.run_id
            assert observer.state['hello'][4] == (not use_pty)
            if transport.native:
                observer.fresh(wire.FRAME, after=0)
            if use_pty:
                serial_fd = os.open(serial, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            transport.break_and_step(terminal, out)
            hold_key(observer)
            observer.close()
            time.sleep(.05)  # let the host observe EOF before reconnecting
            observer = Observer(process, ui, out, time.monotonic()+45, serial=not use_pty)
            assert paused(observer).run_id == identity
            transport.configure_serial(terminal)
            if not use_pty:
                observer.serial('before')
                observer.sock.sendall(wire.encode_packet(wire.ASC0_RX, b'Z', run_id=identity))
            else:
                os.write(serial_fd, b'Z')
            paused(observer)  # input accepted without firmware execution
            transport.resume(master)
            observer.fresh(wire.STATS, lambda p: p.icount > 1000)
            transport.interrupt(terminal, process, master)
            current = paused(observer)
            assert current.run_id == identity
            if use_pty:
                assert select.select([serial_fd], [], [], 3)[0]
                assert os.read(serial_fd, 4096) == b'A'
            else:
                observer.serial('after')
                assert (out/'after-serial.bin').read_bytes() == b'A'
            transport.finish(terminal, process, master, observer, current,
                             use_pty, serial_fd, out, original_termios)
            process.wait(timeout=5)
            stopped(observer)
            assert process.returncode == transport.expected_status, process.returncode
            assert not ui.exists() and not serial.exists()
            if transport.managed:
                assert termios.tcgetattr(master) == original_termios
            (out/'result.json').write_text(json.dumps(dict(passed=True, run_id=identity.hex(), frames=len(observer.frames),
                initial_frame_required=transport.native))+'\n')
        finally:
            if observer:
                observer.close()
            transport.close()
            if process.poll() is None:
                process.send_signal(signal.SIGCONT)
                process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            if serial_fd is not None:
                os.close(serial_fd)
            os.close(master)
            (out/'terminal.log').write_bytes(terminal.log)
    print(name + ': PASS')


def parse_args(description, *, needs_gdb):
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('--emu', type=Path, default=ROOT/'emu/bin/emu')
    parser.add_argument('--fixture', type=Path, default=ROOT/'emu/build/fixtures/generated-c55.bin')
    parser.add_argument('--output', type=Path, required=True)
    if needs_gdb:
        parser.add_argument('--qemu', type=Path, required=True)
        parser.add_argument('--gdb', type=Path, required=True)
    args = parser.parse_args()
    args.emu = args.emu.resolve()
    args.fixture = args.fixture.resolve()
    if needs_gdb:
        args.qemu, args.gdb = args.qemu.resolve(), args.gdb.resolve()
    return args
