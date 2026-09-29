"""Dependency-free CEMU v13 codec. QEMU v7 is explicitly selected by consumers."""
from __future__ import annotations
import asyncio
import re
import struct
import math
from dataclasses import dataclass

MAGIC = 0x55353543
VERSION = 13
HEADER = struct.Struct("<IHHIQQI16s")
MAX_PAYLOAD = 128 * 162 * 3
MAX_MODEL_BYTES = 31
MAX_KEY_NAME_BYTES = 31
MAX_KEYS = 32
DEFAULT_WIDTH = 101
DEFAULT_HEIGHT = 64
HELLO_PREFIX = struct.Struct("<HHHH")
STATS_PAYLOAD = struct.Struct("<QQQIQQQddI")

HELLO = 1
FRAME = 2
STATS = 3
KEY = 4
RELEASE_ALL = 5
KEY_RELEASE_AFTER_SAMPLE = 6
ERROR = 7
ASC0_OPEN = 8
ASC0_READY = 9
ASC0_RX = 10
ASC0_TX = 11
ASC0_CLOSE = 12
AUDIO_RESET = 13
SPEAKER_PCM = 14
CAP_ASC0 = 0x0001
CAP_AUDIO = 0x0002

LIFECYCLE = 15
OWNER_OPEN = 16
OWNER_CLOSE = 17
OWNER_RELEASE_ALL = 18
OWNER_KEY = 19
OWNER_TOKEN = struct.Struct("<Q")
OWNER_KEY_PAYLOAD = struct.Struct("<QBB")
INPUT_RELEASE = 0
INPUT_PRESS = 1
INPUT_RELEASE_SAMPLED = 2

KEY_NAME_RE = re.compile(r"[a-z0-9-]+\Z")

class ProtocolError(ValueError):
    pass


def decode_hello(
    payload: bytes,
) -> tuple[int, int, str, tuple[str, ...], bool, bool]:
    if len(payload) < HELLO_PREFIX.size + 1:
        raise ProtocolError("invalid HELLO payload")
    width, height, key_count, flags = HELLO_PREFIX.unpack_from(payload)
    if not width or not height or width * height * 3 > MAX_PAYLOAD:
        raise ProtocolError("unsupported cemu UI geometry")
    if flags & ~(CAP_ASC0 | CAP_AUDIO) or not (1 <= key_count <= MAX_KEYS):
        raise ProtocolError("unsupported cemu UI capabilities")
    model_length = payload[HELLO_PREFIX.size]
    offset = HELLO_PREFIX.size + 1
    if not (1 <= model_length <= MAX_MODEL_BYTES) or offset + model_length > len(payload):
        raise ProtocolError("invalid HELLO model")
    try:
        model = payload[offset:offset + model_length].decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ProtocolError("invalid HELLO model") from exc
    if any(ord(char) < 0x21 or ord(char) == 0x7F for char in model):
        raise ProtocolError("invalid HELLO model")
    offset += model_length
    keys: list[str] = []
    for _ in range(key_count):
        if offset >= len(payload):
            raise ProtocolError("truncated HELLO key list")
        name_length = payload[offset]
        offset += 1
        if not (1 <= name_length <= MAX_KEY_NAME_BYTES) or offset + name_length > len(payload):
            raise ProtocolError("invalid HELLO key name")
        try:
            name = payload[offset:offset + name_length].decode("ascii")
        except UnicodeDecodeError as exc:
            raise ProtocolError("invalid HELLO key name") from exc
        if not KEY_NAME_RE.fullmatch(name):
            raise ProtocolError("invalid HELLO key name")
        keys.append(name)
        offset += name_length
    if offset != len(payload) or len(set(keys)) != len(keys):
        raise ProtocolError("invalid HELLO key list")
    return (width, height, model, tuple(keys), bool(flags & CAP_ASC0),
            bool(flags & CAP_AUDIO))


@dataclass(frozen=True)
class Packet:
    message_type: int
    payload: bytes
    sequence: int
    icount: int
    run_id: bytes = bytes(16)


def encode_packet(message_type: int, payload: bytes = b"", *,
                  sequence: int = 0, icount: int = 0, run_id: bytes = bytes(16)) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError("payload exceeds protocol limit")
    return HEADER.pack(MAGIC, VERSION, message_type, len(payload), sequence,
                       icount, 0, run_id) + payload


async def read_packet(reader: asyncio.StreamReader) -> Packet:
    raw = await reader.readexactly(HEADER.size)
    magic, version, message_type, length, sequence, icount, reserved, run_id = HEADER.unpack(raw)
    if magic != MAGIC or version != VERSION or reserved != 0:
        raise ProtocolError("invalid protocol header")
    if length > MAX_PAYLOAD:
        raise ProtocolError("payload exceeds protocol limit")
    payload = await reader.readexactly(length)
    return Packet(message_type, payload, sequence, icount, run_id)


def decode_lifecycle(payload: bytes) -> dict:
    if len(payload) < 4:
        raise ProtocolError("invalid LIFECYCLE payload")
    phase, status_len, reason_len = struct.unpack_from("<BBH", payload)
    if phase > 2 or status_len > 31 or reason_len > 703 or len(payload) != 4 + status_len + reason_len:
        raise ProtocolError("invalid LIFECYCLE payload")
    try:
        return {"phase": ("initialized", "running", "stopped")[phase],
                "status": payload[4:4 + status_len].decode("utf-8"),
                "reason": payload[4 + status_len:].decode("utf-8")}
    except UnicodeDecodeError as exc:
        raise ProtocolError("invalid lifecycle text") from exc


def decode_packets(buffer: bytes):
    if len(buffer) >= 6 and (struct.unpack_from("<I", buffer)[0] != MAGIC or
                             struct.unpack_from("<H", buffer, 4)[0] != VERSION):
        raise ProtocolError("invalid UI protocol header")
    packets = []
    offset = 0
    while len(buffer) - offset >= HEADER.size:
        magic, version, kind, length, sequence, icount, reserved, run_id = HEADER.unpack_from(buffer, offset)
        if magic != MAGIC or version != VERSION or reserved or length > MAX_PAYLOAD:
            raise ProtocolError("invalid UI protocol header")
        end = offset + HEADER.size + length
        if end > len(buffer):
            break
        packets.append(Packet(kind, buffer[offset + HEADER.size:end], sequence, icount, run_id))
        offset = end
    return packets, buffer[offset:]


def decode_stats(payload):
    """Validate v13 measurement fields, including canonical unavailable rates."""
    if len(payload) != STATS_PAYLOAD.size:
        raise ProtocolError("invalid STATS payload")
    values = STATS_PAYLOAD.unpack(payload)
    window, ticks_rate, guest_rate, valid = values[6:]
    if (valid not in (0, 1) or
            not all(math.isfinite(rate) and rate >= 0 for rate in (ticks_rate, guest_rate)) or
            (valid and window < 1_000_000_000) or
            (not valid and (window or ticks_rate or guest_rate))):
        raise ProtocolError("invalid STATS rate fields")
    return values

# Serial offsets count raw bytes from this invocation's retained history.
ASC0_SUBSCRIBED = 20
ASC0_HISTORY_READ = 21
ASC0_HISTORY_DATA = 22
SERIAL_POSITION = struct.Struct("<QQ")
SERIAL_RANGE = struct.Struct("<QQQQ")


def decode_serial_position(payload: bytes, *, live: bool = False):
    if len(payload) < 16 or (live and len(payload) == 16) or (not live and len(payload) != 16):
        raise ProtocolError("invalid ASC0 position payload")
    subscription, offset = SERIAL_POSITION.unpack_from(payload)
    if not subscription or (live and len(payload) - 16 > (2**64 - 1) - offset):
        raise ProtocolError("invalid ASC0 position")
    return subscription, offset


class SerialMirror:
    """Validate a native subscription before consumers accept raw live bytes."""
    def __init__(self):
        self.subscription = None
        self.start = self.next = 0

    def subscribe(self, payload):
        if self.subscription is not None:
            raise ProtocolError("duplicate ASC0_SUBSCRIBED")
        self.subscription, self.start = decode_serial_position(payload)
        self.next = self.start

    def live(self, payload):
        subscription, offset = decode_serial_position(payload, live=True)
        if subscription != self.subscription or offset != self.next:
            raise ProtocolError("noncontiguous ASC0 live data")
        data = payload[16:]
        self.next += len(data)
        return data

    def ready(self, payload):
        subscription, boundary = decode_serial_position(payload)
        if subscription != self.subscription or boundary != self.next:
            raise ProtocolError("invalid ASC0_READY boundary")


def observe_packet(packet: Packet, state: dict) -> None:
    """Validate an automation consumer's attachment before using its data."""
    first = "run_id" not in state
    if first and packet.message_type != HELLO:
        raise ProtocolError("expected HELLO before observations")
    if not first and packet.run_id != state["run_id"]:
        raise ProtocolError("runtime identity changed on connection")
    if packet.sequence <= state.get("sequence", 0):
        raise ProtocolError("non-increasing packet sequence")
    if packet.message_type == HELLO:
        if not first:
            raise ProtocolError("duplicate HELLO on connection")
        state["hello"] = decode_hello(packet.payload)
    elif packet.message_type == STATS:
        decode_stats(packet.payload)
    elif packet.message_type == LIFECYCLE:
        decode_lifecycle(packet.payload)
    mirror = state.setdefault("serial", SerialMirror())
    if packet.message_type == ASC0_SUBSCRIBED:
        mirror.subscribe(packet.payload)
    elif packet.message_type == ASC0_TX:
        mirror.live(packet.payload)
    elif packet.message_type == ASC0_READY:
        mirror.ready(packet.payload)
    state["run_id"], state["sequence"] = packet.run_id, packet.sequence
