#!/usr/bin/env python3
"""Browser bridge for cemu's versioned Unix-socket UI protocol."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import ipaddress
import json
import os
import signal
import struct
import tempfile
import time
from collections import deque
from http import HTTPStatus
from pathlib import Path
from typing import Any, Awaitable
from urllib.parse import unquote

from aiohttp import WSCloseCode, WSMsgType, web

from tools.obex.errors import (RemoteError, TransportError, ProtocolError as ObexProtocolError, SessionUnavailable,
                               OverwriteRequired, PathConflict, FileNotFound)
from .asc0 import UiAsc0Transport
from .filesystem import FilesystemService, remote_path

from tools.ui_protocol import (MAGIC, VERSION, HEADER, MAX_PAYLOAD, DEFAULT_WIDTH,
    DEFAULT_HEIGHT, HELLO_PREFIX, STATS_PAYLOAD, HELLO, FRAME, STATS, KEY,
    RELEASE_ALL, KEY_RELEASE_AFTER_SAMPLE, ERROR, ASC0_OPEN, ASC0_READY,
    ASC0_SUBSCRIBED, ASC0_HISTORY_READ, ASC0_HISTORY_DATA, SERIAL_RANGE, SerialMirror,
    OWNER_OPEN, OWNER_CLOSE, OWNER_RELEASE_ALL, OWNER_KEY, OWNER_TOKEN, OWNER_KEY_PAYLOAD,
    ASC0_RX, ASC0_TX, ASC0_CLOSE, AUDIO_RESET, SPEAKER_PCM, CAP_ASC0, CAP_AUDIO,
    LIFECYCLE, Packet, ProtocolError, encode_packet, read_packet, decode_hello, decode_lifecycle, decode_stats)

WS_FRAME = 1
WS_AUDIO_RESET = 2
WS_SPEAKER_PCM = 3
MAX_BROWSER_AUDIO_BYTES = 2 * 1024 * 1024

ASSET_DIR = Path(__file__).with_name("assets")
STATIC_FILES = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/app.css": ("app.css", "text/css; charset=utf-8"),
    "/app.js": ("app.js", "text/javascript; charset=utf-8"),
    "/connection.js": ("connection.js", "text/javascript; charset=utf-8"),
    "/capture.js": ("capture.js", "text/javascript; charset=utf-8"),
    "/phone.js": ("phone.js", "text/javascript; charset=utf-8"),
    "/keypad.js": ("keypad.js", "text/javascript; charset=utf-8"),
    "/status.js": ("status.js", "text/javascript; charset=utf-8"),
    "/audio.js": ("audio.js", "text/javascript; charset=utf-8"),
    "/asc0.js": ("asc0.js", "text/javascript; charset=utf-8"),
    "/files.js": ("files.js", "text/javascript; charset=utf-8"),
    "/audio-worklet.js": ("audio-worklet.js", "text/javascript; charset=utf-8"),
}



def asc0_control(byte: int) -> bool:
    return byte < 0x20 or byte == 0x7F


def escape_asc0(data: bytes | bytearray) -> str:
    escaped: list[str] = []
    for byte in data:
        escaped.append(chr(byte) if 0x20 <= byte <= 0x7E else f"\\x{byte:02X}")
        if asc0_control(byte):
            escaped.append("\n")
    return "".join(escaped)


class BrowserClient:
    """One browser's key ownership and non-blocking latest-frame outbox."""

    def __init__(self, websocket: web.WebSocketResponse) -> None:
        self.websocket = websocket
        self.held: set[str] = set()
        self.owner_token: int | None = None
        self.owner_generation = -1
        self.messages: deque[str] = deque()
        self.message_bytes = 0
        self.bootstrap: str | None = None
        self.metrics: str | None = None
        self.run_id: str | None = None
        self.generation = 0
        self.overflow_task: asyncio.Task | None = None
        self.latest_frame: bytes | None = None
        self.audio: deque[bytes] = deque()
        self.audio_bytes = 0
        self.asc0_tx_pending = ""
        self.wake = asyncio.Event()
        self.closed = False

    def identifiers(self) -> dict[str, Any]:
        return {"run_id": self.run_id, "generation": self.generation}

    def envelope(self, kind: int, payload: bytes) -> bytes:
        return bytes((kind,)) + bytes.fromhex(self.run_id or "00" * 16) + struct.pack("<Q", self.generation) + payload

    def enqueue_json(self, value: dict[str, Any]) -> None:
        if self.closed or self.overflow_task is not None:
            return
        item = json.dumps({**self.identifiers(), **value}, separators=(",", ":"))
        if value.get("type") == "state":
            self.bootstrap = item
        elif value.get("type") == "metrics":
            self.metrics = item
        else:
            size = len(item.encode("utf-8"))
            if len(self.messages) >= 128 or self.message_bytes + size > 256 * 1024:
                self.closed = True
                self.wake.set()
                self.overflow_task = asyncio.create_task(self.websocket.close(
                    code=1013, message=b"ordered event queue overflow"))
                return
            self.messages.append(item)
            self.message_bytes += size
        self.wake.set()

    def enqueue_frame(self, frame: bytes) -> None:
        self.latest_frame = self.envelope(WS_FRAME, frame)
        self.wake.set()

    def enqueue_audio(self, kind: int, payload: bytes = b"") -> None:
        if kind == WS_AUDIO_RESET:
            self.audio.clear()
            self.audio_bytes = 0
        item = self.envelope(kind, payload)
        while self.audio and self.audio_bytes + len(item) > MAX_BROWSER_AUDIO_BYTES:
            self.audio_bytes -= len(self.audio.popleft())
        if len(item) <= MAX_BROWSER_AUDIO_BYTES:
            self.audio.append(item)
            self.audio_bytes += len(item)
            self.wake.set()

    def enqueue_asc0_tx(self, text: str) -> None:
        self.asc0_tx_pending += text
        self.wake.set()

    def enqueue_asc0_tx_snapshot(self, value: dict[str, Any]) -> None:
        self.asc0_tx_pending = ""
        self.run_id = value.get("run_id")
        self.generation = value.get("generation", 0)
        self.latest_frame = None
        self.metrics = None
        self.audio.clear()
        self.audio_bytes = 0
        self.enqueue_json(value)

    async def send_loop(self) -> None:
        while not self.closed:
            await self.wake.wait()
            while not self.closed and (
                    self.bootstrap or self.messages or self.metrics or self.asc0_tx_pending or
                    self.audio or self.latest_frame is not None):
                if self.bootstrap:
                    item: str | bytes = self.bootstrap
                    self.bootstrap = None
                elif self.messages:
                    item = self.messages.popleft()
                    self.message_bytes -= len(item.encode("utf-8"))
                elif self.metrics:
                    item = self.metrics
                    self.metrics = None
                elif self.asc0_tx_pending:
                    text = self.asc0_tx_pending
                    self.asc0_tx_pending = ""
                    item = json.dumps(
                        {**self.identifiers(), "type": "asc0_tx", "text": text}, separators=(",", ":")
                    )
                elif self.audio:
                    item = self.audio.popleft()
                    self.audio_bytes -= len(item)
                else:
                    item = self.latest_frame or b""
                    self.latest_frame = None
                if isinstance(item, bytes):
                    await self.websocket.send_bytes(item)
                else:
                    value = json.loads(item)
                    stats = value.get("stats")
                    if stats and "measured_ns" in stats:
                        stats["age_ns"] = str(max(0, time.monotonic_ns() - int(stats["measured_ns"])))
                        item = json.dumps(value, separators=(",", ":"))
                    await self.websocket.send_str(item)
            self.wake.clear()
            if (self.bootstrap or self.messages or self.metrics or self.asc0_tx_pending or self.audio or
                    self.latest_frame is not None):
                self.wake.set()

    def close(self) -> None:
        self.closed = True
        self.wake.set()


class CemuBridge:
    def __init__(self, socket_path: str, reconnect_delay: float = 0.2) -> None:
        self.socket_path = socket_path
        self.reconnect_delay = reconnect_delay
        self.clients: set[BrowserClient] = set()
        self.writer: asyncio.StreamWriter | None = None
        self.write_lock = asyncio.Lock()
        self.stop_event = asyncio.Event()
        self.connected = False
        self.icount = 0
        self.stats: dict[str, str | int] | None = None
        self.frame: bytes | None = None
        self.asc0_tx = bytearray()
        self.serial_mirror = SerialMirror()
        self.serial_identity = None
        self.history_lock = asyncio.Lock()
        self.history_request = 0
        self.history_pending = None
        self.asc0_available = False
        self.audio_available = False
        self.asc0_transport: UiAsc0Transport | None = None
        self.asc0_ready_waiter: asyncio.Future[None] | None = None
        self.sequence = 1
        self.buttons: tuple[str, ...] = ()
        self.button_index: dict[str, int] = {}
        self.next_owner_token = 1
        self.last_error: str | None = None
        self.connection_count = 0
        self.run_id: str | None = None
        self.generation = 0
        self.lifecycle: dict | None = None
        self.attaching = False
        self.awaiting_hello = True
        self.bootstrap_phase = 0
        self.width = DEFAULT_WIDTH
        self.height = DEFAULT_HEIGHT
        self.model: str | None = None
        self.disconnect_callbacks: list[Any] = []

    def status(self, include_asc0_tx: bool = True) -> dict[str, Any]:
        value: dict[str, Any] = {
            "type": "state",
            **self.identifiers(),
            "lifecycle": self.lifecycle,
            "bridge": "connected" if self.connected else "disconnected",
            "stats": self.stats,
            "width": self.width,
            "height": self.height,
            "model": self.model,
            "keys": list(self.buttons),
        }
        if include_asc0_tx:
            value["asc0_tx"] = escape_asc0(self.asc0_tx)
        value["asc0_available"] = self.asc0_available
        value["audio_available"] = self.audio_available
        if self.last_error:
            value["error"] = self.last_error
        return value

    def identifiers(self) -> dict[str, Any]:
        return {"run_id": self.run_id, "generation": self.generation}

    def broadcast_event(self, value: dict[str, Any]) -> None:
        for client in tuple(self.clients):
            client.enqueue_json({**self.identifiers(), **value})

    def broadcast_transport(self) -> None:
        self.broadcast_event({"type": "transport", "bridge": "connected" if self.connected else "disconnected", "error": self.last_error})

    def broadcast_status(self, include_asc0_tx: bool = False) -> None:
        status = self.status(include_asc0_tx=include_asc0_tx)
        for client in tuple(self.clients):
            if include_asc0_tx:
                client.enqueue_asc0_tx_snapshot(status)
            else:
                client.enqueue_json(status)

    def broadcast_frame(self, frame: bytes) -> None:
        for client in tuple(self.clients):
            client.enqueue_frame(frame)

    def broadcast_audio(self, kind: int, payload: bytes = b"") -> None:
        for client in tuple(self.clients):
            client.enqueue_audio(kind, payload)

    def broadcast_asc0_tx(self, text: str) -> None:
        for client in tuple(self.clients):
            client.enqueue_asc0_tx(text)

    async def _write_locked(self, message_type: int, payload: bytes = b"") -> bool:
        writer = self.writer
        if not self.connected or writer is None or writer.is_closing():
            return False
        packet = encode_packet(message_type, payload,
                               sequence=self.sequence, icount=self.icount,
                               run_id=bytes.fromhex(self.run_id or "00" * 16))
        self.sequence += 1
        try:
            writer.write(packet)
            await writer.drain()
        except (ConnectionError, OSError):
            return False
        return True

    async def send_packet(self, message_type: int, payload: bytes = b"") -> bool:
        generation = self.generation
        async with self.write_lock:
            if generation != self.generation:
                return False
            return await self._write_locked(message_type, payload)

    async def _register_locked(self, client: BrowserClient) -> bool:
        if client.closed or client not in self.clients or not self.connected:
            return False
        if client.owner_token is None:
            client.owner_token = self.next_owner_token
            self.next_owner_token += 1
        if client.owner_generation == self.generation:
            return True
        if not await self._write_locked(OWNER_OPEN, OWNER_TOKEN.pack(client.owner_token)):
            return False
        client.owner_generation = self.generation
        return True

    async def open_asc0(self, transport: UiAsc0Transport) -> None:
        if not self.connected or not self.asc0_available:
            raise TransportError("embedded ASC0 is unavailable")
        if self.asc0_transport is not None and self.asc0_transport is not transport:
            raise TransportError("ASC0 is already in use")
        self.asc0_transport = transport
        waiter = asyncio.get_running_loop().create_future()
        self.asc0_ready_waiter = waiter
        if not await self.send_packet(ASC0_OPEN):
            self.asc0_transport = None
            self.asc0_ready_waiter = None
            raise TransportError("cannot open ASC0 session")
        try:
            async with asyncio.timeout(transport.config.timeout):
                await waiter
        except BaseException:
            if self.asc0_transport is transport:
                self.asc0_transport = None
            if self.asc0_ready_waiter is waiter:
                self.asc0_ready_waiter = None
            raise

    async def write_asc0(self, data: bytes) -> bool:
        return await self.send_packet(ASC0_RX, data)

    async def close_asc0(self, transport: UiAsc0Transport) -> None:
        generation = self.generation
        async with self.write_lock:
            if generation != self.generation or self.asc0_transport is not transport:
                return
            self.asc0_transport = None
            waiter, self.asc0_ready_waiter = self.asc0_ready_waiter, None
            if waiter is not None and not waiter.done():
                waiter.set_exception(TransportError("ASC0 session closed"))
            if self.connected:
                await self._write_locked(ASC0_CLOSE)

    async def invalidate_asc0(self, reason: str) -> None:
        transport, self.asc0_transport = self.asc0_transport, None
        waiter, self.asc0_ready_waiter = self.asc0_ready_waiter, None
        if waiter is not None and not waiter.done():
            waiter.set_exception(TransportError(reason))
        if transport is not None:
            await transport.invalidate(reason)

    async def set_key(self, client: BrowserClient, name: str, pressed: bool, *,
                      deferred_release: bool = True) -> None:
        generation = self.generation
        async with self.write_lock:
            if client.closed or client not in self.clients or generation != self.generation:
                return
            if name not in self.button_index:
                raise ProtocolError(f"unknown key: {name}")
            if pressed == (name in client.held):
                return
            # Desired intent survives transport loss; native owns aggregation.
            if pressed:
                client.held.add(name)
            else:
                client.held.remove(name)
            if await self._register_locked(client) and not client.closed:
                action = 1 if pressed else (2 if deferred_release else 0)
                await self._write_locked(OWNER_KEY, OWNER_KEY_PAYLOAD.pack(
                    client.owner_token, self.button_index[name], action))

    async def release_client(self, client: BrowserClient, *, close: bool = False) -> None:
        generation = self.generation
        if close:
            client.close()
        async with self.write_lock:
            if generation != self.generation:
                return
            client.held.clear()
            if client.owner_generation == generation and client.owner_token is not None:
                await self._write_locked(OWNER_CLOSE if close else OWNER_RELEASE_ALL,
                                         OWNER_TOKEN.pack(client.owner_token))
                if close:
                    client.owner_generation = -1

    async def release_all_clients(self) -> None:
        for client in tuple(self.clients):
            await self.release_client(client)

    async def handle_browser_message(self, client: BrowserClient, raw: str | bytes) -> None:
        if not isinstance(raw, str):
            raise ProtocolError("browser control messages must be JSON text")
        try:
            message = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise ProtocolError("invalid JSON control message") from exc
        if not isinstance(message, dict):
            raise ProtocolError("control message must be an object")
        if (self.run_id is None or message.get("run_id") != self.run_id or
                message.get("generation") != self.generation):
            raise ProtocolError("stale runtime attachment")
        kind = message.get("type")
        if kind == "key":
            name = message.get("key")
            pressed = message.get("pressed")
            if not isinstance(name, str) or not isinstance(pressed, bool):
                raise ProtocolError("invalid key control")
            await self.set_key(client, name, pressed)
        elif kind == "release_all":
            await self.release_client(client)
        else:
            raise ProtocolError("unknown browser control")

    async def browser_handler(self, websocket: web.WebSocketResponse) -> None:
        client = BrowserClient(websocket)
        self.clients.add(client)
        client.enqueue_asc0_tx_snapshot(self.status(include_asc0_tx=True))
        if self.frame is not None:
            client.enqueue_frame(self.frame)
        if self.audio_available:
            client.enqueue_audio(WS_AUDIO_RESET)
        sender = asyncio.create_task(client.send_loop())
        try:
            async with self.write_lock:
                await self._register_locked(client)
            async for message in websocket:
                if message.type not in (WSMsgType.TEXT, WSMsgType.BINARY):
                    if message.type in (WSMsgType.CLOSE, WSMsgType.CLOSED,
                                        WSMsgType.ERROR):
                        break
                    continue
                raw = message.data
                try:
                    await self.handle_browser_message(client, raw)
                except ProtocolError as exc:
                    client.enqueue_json({"type": "error", "message": str(exc)})
        finally:
            await self.release_client(client, close=True)
            self.clients.discard(client)
            client.close()
            sender.cancel()
            with contextlib.suppress(asyncio.CancelledError, ConnectionError):
                await sender

    async def replay_controls(self) -> None:
        generation = self.generation
        async with self.write_lock:
            if generation != self.generation or not self.connected:
                return
            for client in tuple(self.clients):
                if not await self._register_locked(client):
                    continue
                for name in sorted(client.held):
                    if client.closed:
                        break
                    await self._write_locked(OWNER_KEY, OWNER_KEY_PAYLOAD.pack(
                        client.owner_token, self.button_index[name], 1))

    async def read_asc0_history(self, start: int, end: int) -> bytes:
        """Non-consuming native replay; never feed replay into OBEX or the cache."""
        generation = self.generation
        async with self.history_lock:
            result = bytearray()
            while True:
                async with self.write_lock:
                    if (generation != self.generation or not self.connected or
                            self.serial_mirror.subscription is None):
                        raise TransportError("serial attachment retired")
                    if not (self.serial_mirror.start <= start <= end <= self.serial_mirror.next):
                        raise ProtocolError("invalid serial replay range")
                    if self.history_pending is not None or self.history_request == 2**64 - 1:
                        raise TransportError("serial replay unavailable")
                    self.history_request += 1
                    waiter = asyncio.get_running_loop().create_future()
                    request = self.history_request
                    self.history_pending = (generation, request, start, end, waiter)
                    if not await self._write_locked(ASC0_HISTORY_READ, SERIAL_RANGE.pack(
                            self.serial_mirror.subscription, request, start, end)):
                        self.history_pending = None
                        raise TransportError("serial replay write failed")
                # Cancellation leaves the pending response reserved until consumed
                # or disconnect cleanup. It cannot be mistaken for another request.
                data, start = await waiter
                result.extend(data)
                if start == end:
                    return bytes(result)

    def _retire_history_locked(self):
        pending, self.history_pending = self.history_pending, None
        if pending is not None and not pending[4].done():
            pending[4].set_exception(TransportError("serial attachment retired"))

    async def handle_cemu_packet(self, packet: Packet, writer=None) -> None:
        async with self.write_lock:
            if writer is not None and self.writer is not writer:
                return
            was_attaching = self.attaching
            await self._handle_cemu_packet(packet)
            committed = was_attaching and not self.attaching
        if committed:
            await self.replay_controls()

    async def _commit_attachment(self):
        await self.invalidate_asc0("serial attachment replaced")
        width, height, model, buttons, asc0, audio = self.pending_descriptor
        self._retire_history_locked()
        self.history_request = 0
        self.generation += 1
        if self.run_id != self.pending_run:
            self.frame = None
        self.run_id = self.pending_run
        self.width, self.height, self.model = width, height, model
        self.buttons = buttons
        self.asc0_available, self.audio_available = asc0, audio
        self.button_index = {name: index for index, name in enumerate(buttons)}
        for client in self.clients:
            client.held.intersection_update(buttons)
        self.serial_mirror = self.pending_serial
        self.serial_identity = (self.run_id, self.generation,
                                self.serial_mirror.subscription, self.serial_mirror.start)
        self.asc0_tx.clear()
        self.lifecycle, self.stats = self.pending_lifecycle, self.pending_stats
        self.icount = int(self.stats["icount"])
        self.attaching = False
        self.connected = True
        self.broadcast_status(include_asc0_tx=True)
        if self.frame is not None:
            self.broadcast_frame(self.frame)
        self.broadcast_audio(WS_AUDIO_RESET)

    async def _handle_cemu_packet(self, packet: Packet) -> None:
        if packet.message_type != HELLO:
            if self.awaiting_hello or packet.run_id.hex() != (self.pending_run if self.attaching else self.run_id):
                raise ProtocolError("stale runtime packet")
            if self.attaching:
                allowed = ((LIFECYCLE,) if self.bootstrap_phase == 1 else
                           (LIFECYCLE, STATS) if self.bootstrap_phase == 2 else
                           (LIFECYCLE, STATS, ASC0_SUBSCRIBED))
                if packet.message_type not in allowed:
                    raise ProtocolError("incomplete runtime bootstrap")
        elif not self.awaiting_hello:
            raise ProtocolError("duplicate HELLO")
        if not self.attaching and packet.message_type != HELLO:
            self.icount = packet.icount
        if packet.message_type == HELLO:
            self.pending_descriptor = decode_hello(packet.payload)
            self.pending_serial = SerialMirror()
            self.pending_run = packet.run_id.hex()
            self.awaiting_hello = False
            self.attaching = True
            self.bootstrap_phase = 1
        elif packet.message_type == LIFECYCLE:
            lifecycle = decode_lifecycle(packet.payload)
            if self.attaching:
                self.pending_lifecycle = lifecycle
                self.bootstrap_phase = max(self.bootstrap_phase, 2)
            else:
                self.lifecycle = lifecycle
                self.broadcast_event({"type": "lifecycle", "lifecycle": self.lifecycle})
        elif packet.message_type == FRAME:
            if len(packet.payload) != self.width * self.height * 3:
                raise ProtocolError("invalid FRAME payload")
            self.frame = packet.payload
            self.broadcast_frame(packet.payload)
        elif packet.message_type == STATS:
            if len(packet.payload) != STATS_PAYLOAD.size:
                raise ProtocolError("invalid STATS payload")
            (elapsed_ns, ticks, guest_instructions, pc, sample_sequence, measured_ns,
             window_ns, tick_rate, guest_rate, valid) = decode_stats(packet.payload)
            stats = {
                "icount": str(packet.icount),
                "sample_sequence": str(sample_sequence),
                "measured_ns": str(measured_ns),
                "elapsed_ns": str(elapsed_ns),
                "ticks": str(ticks),
                "guest_instructions": str(guest_instructions),
                "pc": pc,
                "window_ns": str(window_ns), "rates_valid": bool(valid),
                "ticks_per_s": tick_rate if valid else None,
                "guest_instructions_per_s": guest_rate if valid else None,
            }
            if self.attaching:
                self.pending_stats = stats
                self.bootstrap_phase = 3
                if not self.pending_descriptor[4]:
                    await self._commit_attachment()
            else:
                self.stats = stats
                self.broadcast_event({"type": "metrics", "stats": self.stats})
        elif packet.message_type == ASC0_SUBSCRIBED:
            if not self.attaching or not self.pending_descriptor[4]:
                raise ProtocolError("unexpected ASC0_SUBSCRIBED")
            self.pending_serial.subscribe(packet.payload)
            await self._commit_attachment()
        elif packet.message_type == ASC0_TX:
            data = self.serial_mirror.live(packet.payload)
            self.asc0_tx.extend(data)
            self.broadcast_asc0_tx(escape_asc0(data))
            if self.asc0_transport is not None:
                await self.asc0_transport.feed(data)
        elif packet.message_type == ASC0_READY:
            self.serial_mirror.ready(packet.payload)
            waiter = self.asc0_ready_waiter
            if self.asc0_transport is None or waiter is None or waiter.done():
                raise ProtocolError("unexpected ASC0_READY")
            await self.asc0_transport.mark_ready()
            self.asc0_ready_waiter = None
            waiter.set_result(None)
        elif packet.message_type == ASC0_HISTORY_DATA:
            pending = self.history_pending
            if len(packet.payload) < 32 or pending is None:
                raise ProtocolError("unexpected ASC0_HISTORY_DATA")
            subscription, request, start, next_offset = SERIAL_RANGE.unpack_from(packet.payload)
            generation, expected_request, expected_start, end, waiter = pending
            data = packet.payload[32:]
            if (generation != self.generation or subscription != self.serial_mirror.subscription or
                    request != expected_request or start != expected_start or
                    next_offset != start + len(data) or next_offset > end or
                    (start < end and not data)):
                raise ProtocolError("invalid ASC0_HISTORY_DATA")
            self.history_pending = None
            if not waiter.done():
                waiter.set_result((data, next_offset))
        elif packet.message_type == AUDIO_RESET:
            if packet.payload or not self.audio_available:
                raise ProtocolError("unexpected AUDIO_RESET")
            self.broadcast_audio(WS_AUDIO_RESET)
        elif packet.message_type == SPEAKER_PCM:
            if not self.audio_available or len(packet.payload) < 10:
                raise ProtocolError("unexpected SPEAKER_PCM")
            sample_rate, channels, reserved = struct.unpack_from(
                "<IHH", packet.payload
            )
            sample_bytes = len(packet.payload) - 8
            if (sample_rate != 48_000 or channels != 1 or reserved or
                    not sample_bytes or sample_bytes % 2):
                raise ProtocolError("invalid SPEAKER_PCM payload")
            self.broadcast_audio(WS_SPEAKER_PCM, packet.payload[8:])
        elif packet.message_type == ERROR:
            self.last_error = packet.payload.decode("utf-8", "replace")
            self.broadcast_event({"type": "error", "message": self.last_error})
        else:
            raise ProtocolError(f"unexpected cemu message type {packet.message_type}")

    async def run(self) -> None:
        while not self.stop_event.is_set():
            try:
                reader, writer = await asyncio.open_unix_connection(self.socket_path)
            except (FileNotFoundError, ConnectionRefusedError, OSError) as exc:
                self.last_error = str(exc)
                self.broadcast_transport()
                try:
                    await asyncio.wait_for(self.stop_event.wait(), self.reconnect_delay)
                except TimeoutError:
                    pass
                continue

            self.writer = writer
            self.connected = False
            self.awaiting_hello = True
            self.last_error = None
            self.connection_count += 1
            try:
                while not self.stop_event.is_set():
                    packet = await read_packet(reader)
                    if self.writer is not writer:
                        break
                    await self.handle_cemu_packet(packet, writer)
            except (asyncio.IncompleteReadError, ConnectionError, OSError, ProtocolError) as exc:
                self.last_error = str(exc)
            finally:
                async with self.write_lock:
                    active = self.writer is writer
                    if active:
                        self.writer = None
                        self.connected = False
                        self._retire_history_locked()
                        await self.invalidate_asc0("CEMU UI socket disconnected")
                writer.close()
                with contextlib.suppress(Exception):
                    await writer.wait_closed()
                if active:
                    self.broadcast_audio(WS_AUDIO_RESET)
                    for callback in tuple(self.disconnect_callbacks):
                        await callback("CEMU UI socket disconnected")
                    self.broadcast_transport()

            if not self.stop_event.is_set():
                try:
                    await asyncio.wait_for(self.stop_event.wait(), self.reconnect_delay)
                except TimeoutError:
                    pass

    async def stop(self) -> None:
        self.stop_event.set()
        for client in tuple(self.clients):
            await self.release_client(client, close=True)
        async with self.write_lock:
            writer = self.writer
            self.writer = None
            self.connected = False
            self._retire_history_locked()
            await self.invalidate_asc0("bridge stopped")
        if writer is not None:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()


class WebServer:
    def __init__(self, bridge: CemuBridge, host: str, port: int) -> None:
        self.bridge = bridge
        self.host = host
        self.port = port
        self.runner: web.AppRunner | None = None
        self.site: web.TCPSite | None = None
        self.allowed_origins = self._allowed_origins(host, port)
        self.files = FilesystemService(bridge)
        self.bridge.disconnect_callbacks.append(self.files.invalidate)
        self.app = web.Application(middlewares=[self.error_responses,
                                                 self.attachment_guard,
                                                 self.security_headers])
        self.app.router.add_get("/ws", self.websocket_handler)
        self.app.router.add_get("/favicon.ico", self.favicon_handler)
        self.app.router.add_get("/api/files/transfers/{transfer}", self.files.transfer_status)
        self.app.router.add_post("/api/files/transfers/{transfer}/cancel", self.files_cancel_handler)
        self.app.router.add_get("/api/files/status", self.files_status_handler)
        self.app.router.add_post("/api/files/connect", self.files_connect_handler)
        self.app.router.add_get("/api/files/list", self.files_list_handler)
        self.app.router.add_get("/api/files/download", self.files_download_handler)
        self.app.router.add_put("/api/files/upload", self.files_upload_handler)
        self.app.router.add_post("/api/files/mkdir", self.files_mkdir_handler)
        self.app.router.add_delete("/api/files/delete", self.files_delete_handler)
        for path in STATIC_FILES:
            self.app.router.add_get(path, self.asset_handler)

    @staticmethod
    def _allowed_origins(host: str, port: int) -> set[str]:
        origins = {f"http://127.0.0.1:{port}", f"http://localhost:{port}"}
        shown = f"[{host}]" if ":" in host and not host.startswith("[") else host
        origins.add(f"http://{shown}:{port}")
        return origins

    def check_attachment(self, request: web.Request) -> None:
        if request.get("attachment") != self.bridge.identifiers():
            raise web.HTTPConflict(text="retired runtime attachment\n")

    @web.middleware
    async def attachment_guard(self, request: web.Request, handler):
        if not request.path.startswith("/api/files/"):
            return await handler(request)
        identity = self.bridge.identifiers()
        request["attachment"] = identity
        run = request.headers.get("X-CEMU-Run", request.query.get("run_id"))
        generation = request.headers.get("X-CEMU-Generation", request.query.get("generation"))
        if run is not None and (run != identity["run_id"] or generation != str(identity["generation"])):
            raise web.HTTPConflict(text="retired runtime attachment\n")
        response = await handler(request)
        self.check_attachment(request)
        response.headers["X-CEMU-Run"] = identity["run_id"] or ""
        response.headers["X-CEMU-Generation"] = str(identity["generation"])
        return response

    @web.middleware
    async def error_responses(self, request: web.Request, handler):
        try:
            return await handler(request)
        except web.HTTPException:
            raise
        except OverwriteRequired as exc:
            return web.json_response({"error": "overwrite_confirmation_required",
                                      "path": str(exc.path), "token": exc.token}, status=409)
        except PathConflict as exc:
            return web.json_response({"error": "target_is_folder", "path": str(exc.path)}, status=409)
        except FileNotFound as exc:
            raise web.HTTPNotFound(text=f"{exc}\n") from None
        except SessionUnavailable as exc:
            raise web.HTTPConflict(text=f"{exc}\n") from None
        except (RemoteError, ObexProtocolError) as exc:
            return web.json_response({"error": "remote_error", "message": str(exc)},
                                     status=HTTPStatus.BAD_GATEWAY)
        except (ConnectionError, TransportError, TimeoutError) as exc:
            return web.json_response({"error": "transport_error", "message": str(exc)},
                                     status=HTTPStatus.SERVICE_UNAVAILABLE)
        except ValueError as exc:
            return web.json_response({"error": "invalid_request", "message": str(exc)},
                                     status=HTTPStatus.BAD_REQUEST)

    @web.middleware
    async def security_headers(self, request: web.Request, handler):
        response = await handler(request)
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Content-Security-Policy"] = (
            "default-src 'self'; connect-src 'self'; img-src 'self' data:; "
            "style-src 'self'; script-src 'self'; frame-ancestors 'none'"
        )
        return response

    @staticmethod
    def require_mutation_header(request: web.Request) -> None:
        if request.headers.get("X-CEMU-Request") != "1":
            raise web.HTTPForbidden(text="missing same-origin request header\n")

    async def websocket_handler(self, request: web.Request) -> web.StreamResponse:
        origin = request.headers.get("Origin")
        host = request.headers.get("Host")
        same_origin = bool(host) and origin == f"http://{host}"
        if origin not in self.allowed_origins and not same_origin:
            raise web.HTTPForbidden(text="Forbidden\n")
        websocket = web.WebSocketResponse(compress=False, max_msg_size=64 * 1024)
        await websocket.prepare(request)
        await self.bridge.browser_handler(websocket)
        return websocket

    async def favicon_handler(self, request: web.Request) -> web.Response:
        del request
        return web.Response(status=HTTPStatus.NO_CONTENT)

    async def files_cancel_handler(self, request: web.Request) -> web.Response:
        self.require_mutation_header(request)
        return await self.files.cancel_transfer(request)

    async def files_status_handler(self, request: web.Request) -> web.Response:
        del request
        return web.json_response(self.files.status())

    async def files_connect_handler(self, request: web.Request) -> web.Response:
        self.require_mutation_header(request)
        retry = request.query.get("retry") == "1"
        return web.json_response(await self.files.connect(retry=retry))

    async def files_list_handler(self, request: web.Request) -> web.Response:
        path = remote_path(request.query.get("path", "A:"))
        entries = await self.files.list(path)
        return web.json_response({"path": str(path),
                                  "entries": [entry.to_dict() for entry in entries]})

    async def files_download_handler(self, request: web.Request) -> web.StreamResponse:
        return await self.files.download(request, remote_path(request.query.get("path")))

    async def files_upload_handler(self, request: web.Request) -> web.Response:
        self.require_mutation_header(request)
        path = remote_path(request.query.get("path"))
        try:
            result = await self.files.upload(request, path)
        except asyncio.CancelledError:
            return web.json_response({"error": "transfer_cancelled"}, status=HTTPStatus.CONFLICT)
        self.check_attachment(request)
        return web.json_response(result, status=HTTPStatus.CREATED)

    async def files_mkdir_handler(self, request: web.Request) -> web.Response:
        self.require_mutation_header(request)
        data = await request.json()
        path = remote_path(data.get("path") if isinstance(data, dict) else None)
        await self.files.mkdir(path)
        self.check_attachment(request)
        return web.json_response({"path": str(path)}, status=HTTPStatus.CREATED)

    async def files_delete_handler(self, request: web.Request) -> web.Response:
        self.require_mutation_header(request)
        if request.headers.get("X-CEMU-Confirm") != "delete":
            raise web.HTTPPreconditionRequired(text="deletion requires confirmation\n")
        path = remote_path(request.query.get("path"))
        await self.files.delete(path)
        self.check_attachment(request)
        return web.json_response({"path": str(path)})

    async def asset_handler(self, request: web.Request) -> web.Response:
        decoded = unquote(request.path)
        if ".." in decoded.split("/"):
            raise web.HTTPForbidden(text="Forbidden\n")
        filename, content_type = STATIC_FILES[request.path]
        try:
            body = (ASSET_DIR / filename).read_bytes()
        except OSError:
            raise web.HTTPInternalServerError(text="Asset unavailable\n") from None
        return web.Response(body=body, content_type=content_type.split(";", 1)[0],
                            charset="utf-8")

    async def start(self) -> None:
        self.runner = web.AppRunner(self.app, access_log=None)
        await self.runner.setup()
        self.site = web.TCPSite(self.runner, self.host, self.port)
        await self.site.start()
        if self.port == 0:
            assert self.site._server is not None
            self.port = self.site._server.sockets[0].getsockname()[1]
            self.allowed_origins = self._allowed_origins(self.host, self.port)

    async def stop(self) -> None:
        if self.files.invalidate in self.bridge.disconnect_callbacks:
            self.bridge.disconnect_callbacks.remove(self.files.invalidate)
        if self.site is not None:
            await self.site.stop()
            self.site = None
        await asyncio.gather(*(
            client.websocket.close(
                code=WSCloseCode.GOING_AWAY, message=b"server shutdown"
            )
            for client in tuple(self.bridge.clients)
        ), return_exceptions=True)
        await self.files.close()
        if self.runner is not None:
            await self.runner.cleanup()
            self.runner = None


class ChildOwner:
    def __init__(self, command: list[str], socket_path: str) -> None:
        self.command = command
        self.socket_path = socket_path
        self.process: asyncio.subprocess.Process | None = None

    async def start(self) -> None:
        if "--ui-socket" in self.command:
            raise ValueError("spawn command must not contain --ui-socket")
        self.process = await asyncio.create_subprocess_exec(
            *self.command, "--ui-socket", self.socket_path
        )

    async def stop(self) -> None:
        process = self.process
        if process is None or process.returncode is not None:
            return
        process.terminate()
        try:
            await asyncio.wait_for(process.wait(), 3)
        except TimeoutError:
            process.kill()
            await process.wait()

    def force(self) -> None:
        process = self.process
        if process is not None and process.returncode is None:
            with contextlib.suppress(ProcessLookupError):
                process.kill()

    async def force_stop(self) -> None:
        process = self.process
        if process is None or process.returncode is not None:
            return
        self.force()
        await process.wait()


class ShutdownController:
    def __init__(self, owner: ChildOwner | None) -> None:
        self.owner = owner
        self.stop_event = asyncio.Event()
        self.force_event = asyncio.Event()

    def request(self, signal_number: int) -> None:
        if signal_number == signal.SIGINT and self.stop_event.is_set():
            self.force_event.set()
            if self.owner is not None:
                self.owner.force()
            return
        self.stop_event.set()


async def finish_unless_forced(operation: Awaitable[Any],
                               force_event: asyncio.Event) -> bool:
    task = asyncio.ensure_future(operation)
    force_waiter = asyncio.create_task(force_event.wait())
    try:
        done, _ = await asyncio.wait(
            (task, force_waiter), return_when=asyncio.FIRST_COMPLETED
        )
        if task in done:
            await task
            return True
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task
        return False
    finally:
        force_waiter.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await force_waiter


def is_loopback_host(host: str) -> bool:
    if host.lower() == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Serve the live cemu browser UI")
    parser.add_argument("--connect", metavar="PATH", help="attach to an existing cemu UI socket")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--unsafe-bind", action="store_true",
                        help="allow binding the web server outside loopback")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="spawn command after --")
    args = parser.parse_args(argv)
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if bool(args.connect) == bool(args.command):
        parser.error("use either --connect PATH or -- CEMU FLASH [options]")
    if not (0 <= args.port <= 65535):
        parser.error("--port must be between 0 and 65535")
    if not is_loopback_host(args.host) and not args.unsafe_bind:
        parser.error("non-loopback --host requires --unsafe-bind")
    return args


async def run_application(args: argparse.Namespace) -> None:
    runtime: tempfile.TemporaryDirectory[str] | None = None
    owner: ChildOwner | None = None
    if args.connect:
        socket_path = args.connect
    else:
        runtime = tempfile.TemporaryDirectory(prefix="cemu-ui-")
        socket_path = os.path.join(runtime.name, "cemu.sock")
        owner = ChildOwner(args.command, socket_path)
        await owner.start()

    bridge = CemuBridge(socket_path)
    bridge_task = asyncio.create_task(bridge.run())
    web = WebServer(bridge, args.host, args.port)
    shutdown = ShutdownController(owner)
    loop = asyncio.get_running_loop()
    installed_signals: list[signal.Signals] = []
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, shutdown.request, sig)
            installed_signals.append(sig)
        except NotImplementedError:
            pass

    try:
        await web.start()
        shown_host = f"[{args.host}]" if ":" in args.host else args.host
        print(f"x55 web UI: http://{shown_host}:{web.port}/", flush=True)
        await shutdown.stop_event.wait()
    finally:
        graceful = await finish_unless_forced(web.stop(), shutdown.force_event)
        if graceful:
            graceful = await finish_unless_forced(
                bridge.stop(), shutdown.force_event
            )
        bridge_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await bridge_task
        if owner is not None:
            if graceful and not shutdown.force_event.is_set():
                graceful = await finish_unless_forced(
                    owner.stop(), shutdown.force_event
                )
            if not graceful or shutdown.force_event.is_set():
                await owner.force_stop()
        if runtime is not None:
            runtime.cleanup()
        for sig in installed_signals:
            loop.remove_signal_handler(sig)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        asyncio.run(run_application(args))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
