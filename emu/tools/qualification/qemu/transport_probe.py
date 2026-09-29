"""Bounded externally driven QEMU/GDB transport qualification."""
import signal
import socket
import time

from tools.qualification.gdb.gdb_execution import MI

from tools.qualification.shared.transport import parse_args, qualify


class Transport:
    name = 'qemu'
    native = False
    managed = False
    expected_status = 143

    def __init__(self, args):
        self.args = args
        self.gdb = None

    def configure_command(self, command):
        with socket.socket() as port_socket:
            port_socket.bind(('127.0.0.1', 0))
            self.port = port_socket.getsockname()[1]
        command += ['--engine', 'qemu', '--qemu-binary', str(self.args.qemu)]
        command += ['--gdb', str(self.port)]

    def initial_prompt(self, terminal, process, master):
        pass

    def break_and_step(self, terminal, out):
        self.gdb = MI(self.args.gdb, out, 'transport')
        self.gdb.cli(f'target remote 127.0.0.1:{self.port}', timeout=5)
        self.gdb.records.clear()
        self.gdb.cli('break *0x800102')
        deadline = self.gdb.resume('continue')
        self.gdb.stop(deadline, 'breakpoint-hit', pc=0x800102)
        self.gdb.stop(self.gdb.resume('stepi'), 'end-stepping-range', pc=0x800104)
        self.gdb.cli('delete breakpoints')

    def configure_serial(self, terminal):
        self.gdb.cli('set $pc=0x800200')
        self.gdb.stop(self.gdb.resume('stepi 2'), 'end-stepping-range', pc=0x800208)

    def resume(self, master):
        self.gdb.resume('continue')

    def interrupt(self, terminal, process, master):
        self.gdb.command('-exec-interrupt', timeout=5)
        self.gdb.event('stopped', time.monotonic()+5)
        value = self.gdb.command('-data-evaluate-expression "*(unsigned short *)0xfeb2"')
        assert int(value['value'], 0) == 90, value

    def finish(self, terminal, process, master, observer, current,
               use_pty, serial_fd, out, original_termios):
        process.send_signal(signal.SIGTERM)

    def close(self):
        if self.gdb:
            self.gdb.close()


def main():
    args = parse_args(__doc__, needs_gdb=True)
    for use_pty in (False, True):
        qualify(args, Transport(args), use_pty)


if __name__ == '__main__':
    main()
