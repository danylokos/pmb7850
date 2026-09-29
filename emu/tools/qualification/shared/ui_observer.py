"""Bounded UI protocol observer shared by debugger transport qualification tools."""
import hashlib
import json
import socket
import threading
import time

from tools import ui_protocol as wire


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def remaining(deadline, maximum):
    value = min(maximum, deadline - time.monotonic())
    if value <= 0:
        raise TimeoutError('UI observer deadline expired')
    return value


def connect_ui(process, path, deadline):
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise EOFError(f'EMU exited: {process.returncode}')
        sock = socket.socket(socket.AF_UNIX)
        try:
            sock.connect(str(path))
            sock.settimeout(.1)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            time.sleep(.01)
    raise TimeoutError('UI connection deadline expired')


class Observer:
    """Continuously drain and validate host packets, including during MI calls."""
    def __init__(self, process, path, out, deadline, *, serial=True):
        self.process, self.out, self.deadline = process, out, deadline
        self.sock = connect_ui(process, path, time.monotonic() + remaining(deadline, 5))
        self.state, self.packets, self.frames = {}, [], []
        self.condition = threading.Condition()
        self.error = None
        self.closed = False
        self.request = 0
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()
        try:
            self.fresh(wire.ASC0_SUBSCRIBED if serial else wire.HELLO, after=0)
        except Exception:
            self.close()
            raise

    def read(self):
        buffer = b''
        try:
            with (self.out / 'ui-packets.jsonl').open('w') as log:
                while not self.closed:
                    try:
                        chunk = self.sock.recv(1024 * 1024)
                    except socket.timeout:
                        continue
                    if not chunk:
                        raise EOFError('EMU UI closed')
                    buffer += chunk
                    packets, buffer = wire.decode_packets(buffer)
                    with self.condition:
                        for packet in packets:
                            wire.observe_packet(packet, self.state)
                            record = dict(type=packet.message_type, sequence=packet.sequence,
                                          icount=packet.icount, run_id=packet.run_id.hex(),
                                          received=time.monotonic(), payload=packet.payload.hex())
                            log.write(json.dumps(record) + '\n')
                            self.packets.append(packet)
                            if packet.message_type == wire.FRAME:
                                raw = packet.payload
                                width, height = self.state['hello'][:2]
                                if len(raw) != width * height * 3:
                                    raise AssertionError('wrong RGB frame size')
                                name = f'frame-{packet.sequence}.ppm'
                                (self.out / name).write_bytes(f'P6\n{width} {height}\n255\n'.encode() + raw)
                                self.frames.append(dict(file=name, sequence=packet.sequence,
                                                        icount=packet.icount,
                                                        sha256=hashlib.sha256(raw).hexdigest(),
                                                        nonblank=any(raw[i:i+3] != raw[:3]
                                                                     for i in range(0, len(raw), 3))))
                        log.flush()
                        self.condition.notify_all()
        except Exception as error:
            with self.condition:
                self.error = error
                self.condition.notify_all()

    def marker(self):
        with self.condition:
            return self.state.get('sequence', 0)

    def fresh(self, kind, predicate=lambda p: True, *, after=None, timeout=5):
        deadline = time.monotonic() + remaining(self.deadline, timeout)
        with self.condition:
            after = self.marker() if after is None else after
            while True:
                remaining(deadline, timeout)
                if self.process.poll() is not None:
                    raise EOFError(f'EMU exited unexpectedly: {self.process.returncode}')
                if self.error:
                    raise self.error
                for packet in self.packets:
                    if packet.sequence > after and packet.message_type == kind and predicate(packet):
                        return packet
                self.condition.wait(min(0.1, remaining(deadline, timeout)))

    def stats(self, count=None):
        packet = self.fresh(wire.STATS, lambda p: count is None or
                            wire.decode_stats(p.payload)[2] == count)
        elapsed, ticks, guest, pc = wire.decode_stats(packet.payload)[:4]
        return dict(sequence=packet.sequence, elapsed=elapsed, ticks=ticks,
                    guest_icount=guest, pc=pc)

    def serial(self, label):
        # Opening the link has no guest RX payload; history can legitimately be empty.
        before = self.marker()
        self.sock.sendall(wire.encode_packet(wire.ASC0_OPEN, run_id=self.state['run_id']))
        self.fresh(wire.ASC0_READY, after=before)
        with self.condition:
            mirror = self.state['serial']
            subscription, start, end = mirror.subscription, mirror.start, mirror.next
        position, history, responses = start, bytearray(), []
        while True:
            self.request += 1
            before = self.marker()
            self.sock.sendall(wire.encode_packet(wire.ASC0_HISTORY_READ,
                              wire.SERIAL_RANGE.pack(subscription, self.request, position, end),
                              run_id=self.state['run_id']))
            packet = self.fresh(wire.ASC0_HISTORY_DATA, after=before)
            fields = wire.SERIAL_RANGE.unpack_from(packet.payload)
            actual_subscription, request, actual_start, next_offset = fields
            assert (actual_subscription, request, actual_start) == (subscription, self.request, position), fields
            assert position <= next_offset <= end and len(packet.payload) - 32 == next_offset - position, fields
            assert next_offset > position or position == end, fields
            history.extend(packet.payload[32:])
            responses.append(dict(request=request, start=position, next=next_offset, sequence=packet.sequence))
            position = next_offset
            if position == end:
                break
        (self.out / f'{label}-serial.bin').write_bytes(history)
        return dict(subscription=subscription, start=start, tail=end, responses=responses)

    def close(self):
        self.closed = True
        self.sock.close()
        self.thread.join(timeout=5)
        save(self.out / 'frames.json', self.frames)
