#!/usr/bin/env python3

import asyncio
import contextlib
import json
import os
import signal
import struct
import sys
import tempfile
import unittest

from aiohttp import ClientSession
from tools.obex import ClientConfig
from tools.obex.errors import TransportError
from websockets.asyncio.client import connect

from .. import app as web_ui
from ..asc0 import UiAsc0Transport

C55_BUTTONS = (
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "star", "hash", "up", "down", "soft-left", "soft-right", "send", "power",
)
M55_BUTTONS = (
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "star", "hash", "up", "down", "left", "right",
    "soft-left", "soft-right", "send", "power",
)


def hello_payload(width, height, model, keys, flags=web_ui.CAP_ASC0):
    model_bytes = model.encode()
    payload = bytearray(struct.pack("<HHHHB", width, height, len(keys), flags,
                                    len(model_bytes)))
    payload.extend(model_bytes)
    for key in keys:
        encoded = key.encode("ascii")
        payload.append(len(encoded))
        payload.extend(encoded)
    return bytes(payload)


async def wait_until(predicate, timeout=3.0):
    async with asyncio.timeout(timeout):
        while not predicate():
            await asyncio.sleep(0.01)


async def finish_bootstrap(bridge):
    await bridge.handle_cemu_packet(web_ui.Packet(web_ui.LIFECYCLE, bytes((1, 0, 0, 0)), 2, 0))
    await bridge.handle_cemu_packet(web_ui.Packet(web_ui.STATS,
        web_ui.STATS_PAYLOAD.pack(0, 0, 0, 0, 1, 100, 0, 0.0, 0.0, 0), 3, 0))
    await subscribe(bridge)

async def subscribe(bridge, run_id=bytes(16), start=0, handle=1):
    if bridge.attaching and bridge.pending_descriptor[4]:
        await bridge.handle_cemu_packet(web_ui.Packet(web_ui.ASC0_SUBSCRIBED,
            struct.pack("<QQ", handle, start), 4, 0, run_id))


async def live(bridge, data, run_id=bytes(16)):
    await bridge.handle_cemu_packet(web_ui.Packet(web_ui.ASC0_TX,
        struct.pack("<QQ", bridge.serial_mirror.subscription, bridge.serial_mirror.next) + data,
        5, 0, run_id))


async def bootstrap(bridge):
    await bridge.handle_cemu_packet(web_ui.Packet(web_ui.HELLO,
        hello_payload(101, 64, "c55", C55_BUTTONS), 1, 0))
    await finish_bootstrap(bridge)


class FakeCemu:
    def __init__(self, path, frame=None, model="c55", width=101, height=64,
                 keys=C55_BUTTONS, asc0_tx=b"", asc0_available=True,
                 audio_available=False, auto_ready=True):
        self.path = path
        self.model = model
        self.width = width
        self.height = height
        self.keys = tuple(keys)
        self.frame = frame if frame is not None else bytes([12, 34, 56]) * (width * height)
        self.asc0_tx = asc0_tx
        self.asc0_available = asc0_available
        self.audio_available = audio_available
        self.auto_ready = auto_ready
        self.server = None
        self.writer = None
        self.connected = asyncio.Event()
        self.received = asyncio.Queue()
        self.owner_packets = asyncio.Queue()
        self.serial_next = 0

    async def start(self):
        with contextlib.suppress(FileNotFoundError):
            os.unlink(self.path)
        self.server = await asyncio.start_unix_server(self.handle, path=self.path)

    async def handle(self, reader, writer):
        self.writer = writer
        writer.write(web_ui.encode_packet(
            web_ui.HELLO,
            hello_payload(self.width, self.height, self.model, self.keys,
                          (web_ui.CAP_ASC0 if self.asc0_available else 0) |
                          (web_ui.CAP_AUDIO if self.audio_available else 0)),
            sequence=1, icount=100,
        ))
        writer.write(web_ui.encode_packet(web_ui.LIFECYCLE, bytes((1, 0, 0, 0))))
        stats = web_ui.STATS_PAYLOAD.pack(61_140_000_000, 42_500_000, 42_500_000, 0xEF4B98, 1, 100, 0, 0.0, 0.0, 0)
        writer.write(web_ui.encode_packet(web_ui.STATS, stats, sequence=2, icount=42_500_000))
        self.serial_next = 0
        if self.asc0_available:
            writer.write(web_ui.encode_packet(web_ui.ASC0_SUBSCRIBED, struct.pack("<QQ", 1, 0)))
        sequence = 4
        if self.asc0_tx:
            writer.write(web_ui.encode_packet(web_ui.ASC0_TX, struct.pack("<QQ", 1, 0) + self.asc0_tx, sequence=sequence, icount=100))
            sequence += 1
            self.serial_next = len(self.asc0_tx)
        writer.write(web_ui.encode_packet(web_ui.FRAME, self.frame, sequence=sequence, icount=100))
        await writer.drain()
        self.connected.set()
        try:
            while True:
                packet = await web_ui.read_packet(reader)
                await self.owner_packets.put(packet)
                # Presentation regressions inspect key intent, never aggregation.
                # Keep exact owner packets separately for protocol assertions.
                if packet.message_type == web_ui.OWNER_OPEN:
                    continue
                if packet.message_type == web_ui.OWNER_KEY:
                    token, key, action = web_ui.OWNER_KEY_PAYLOAD.unpack(packet.payload)
                    packet = web_ui.Packet(
                        web_ui.KEY_RELEASE_AFTER_SAMPLE if action == 2 else web_ui.KEY,
                        bytes((key,)) if action == 2 else bytes((key, action)),
                        packet.sequence, packet.icount, packet.run_id)
                await self.received.put(packet)
                if packet.message_type == web_ui.ASC0_OPEN and self.auto_ready:
                    await self.send_ready()
        except (asyncio.IncompleteReadError, ConnectionError):
            pass

    async def send_asc0_tx(self, payload):
        self.asc0_tx += payload
        if self.writer is None:
            raise RuntimeError("fake cemu is not connected")
        self.writer.write(web_ui.encode_packet(
            web_ui.ASC0_TX, struct.pack("<QQ", 1, self.serial_next) + payload, sequence=100, icount=42_500_001
        ))
        await self.writer.drain()

        self.serial_next += len(payload)

    async def send_ready(self):
        self.writer.write(web_ui.encode_packet(
            web_ui.ASC0_READY, struct.pack("<QQ", 1, self.serial_next), sequence=1000, icount=42_500_001))
        await self.writer.drain()

    async def stop(self):
        if self.writer is not None:
            self.writer.close()
            with contextlib.suppress(Exception):
                await self.writer.wait_closed()
            self.writer = None
        if self.server is not None:
            self.server.close()
            await self.server.wait_closed()
            self.server = None
        with contextlib.suppress(FileNotFoundError):
            os.unlink(self.path)


class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def test_packet_round_trip_and_partial_decode(self):
        encoded = web_ui.encode_packet(web_ui.KEY, b"\x0f\x01", sequence=7, icount=1234)
        reader = asyncio.StreamReader()
        reader.feed_data(encoded[:9])
        task = asyncio.create_task(web_ui.read_packet(reader))
        await asyncio.sleep(0)
        self.assertFalse(task.done())
        reader.feed_data(encoded[9:])
        packet = await task
        self.assertEqual(packet, web_ui.Packet(web_ui.KEY, b"\x0f\x01", 7, 1234))

    async def test_rejects_bad_header_and_payload_limit(self):
        reader = asyncio.StreamReader()
        bad = bytearray(web_ui.encode_packet(
            web_ui.STATS, web_ui.STATS_PAYLOAD.pack(1, 2, 3, 4, 1, 100, 0, 0.0, 0.0, 0)
        ))
        bad[0] ^= 0xFF
        reader.feed_data(bad)
        with self.assertRaises(web_ui.ProtocolError):
            await web_ui.read_packet(reader)
        with self.assertRaises(web_ui.ProtocolError):
            web_ui.encode_packet(web_ui.FRAME, bytes(web_ui.MAX_PAYLOAD + 1))

    async def test_hello_controls_frame_geometry(self):
        bridge = web_ui.CemuBridge("/tmp/not-used")
        hello = web_ui.Packet(
            web_ui.HELLO, hello_payload(101, 80, "m55", M55_BUTTONS), 1, 0
        )
        await bridge.handle_cemu_packet(hello)
        await finish_bootstrap(bridge)
        self.assertEqual((bridge.width, bridge.height), (101, 80))
        self.assertEqual(bridge.model, "m55")
        self.assertEqual(bridge.buttons, M55_BUTTONS)
        self.assertFalse(bridge.audio_available)
        frame = bytes(101 * 80 * 3)
        await bridge.handle_cemu_packet(web_ui.Packet(web_ui.FRAME, frame, 2, 0))
        self.assertEqual(bridge.frame, frame)
        with self.assertRaises(web_ui.ProtocolError):
            await bridge.handle_cemu_packet(
                web_ui.Packet(web_ui.FRAME, frame[:-3], 3, 0)
            )
        with self.assertRaises(web_ui.ProtocolError):
            await bridge.handle_cemu_packet(web_ui.Packet(
                web_ui.HELLO, struct.pack("<HHHH", 101, 80, 20, 0), 4, 0
            ))

    async def test_asc0_escape_and_raw_retention(self):
        self.assertEqual(
            web_ui.escape_asc0(b"<tag>\x00\x1f\x7f\x80&"),
            "<tag>\\x00\n\\x1F\n\\x7F\n\\x80&",
        )
        self.assertEqual(
            web_ui.escape_asc0(b"\x09\x12\x1bEXIT: 740E 08 008A\x09\x0a\x030000255000"),
            "\\x09\n\\x12\n\\x1B\nEXIT: 740E 08 008A\\x09\n"
            "\\x0A\n\\x03\n0000255000",
        )
        bridge = web_ui.CemuBridge("/tmp/not-used")
        await bootstrap(bridge)
        await live(bridge, b"A\x00B")
        self.assertEqual(bridge.asc0_tx, b"A\x00B")
        self.assertEqual(bridge.status()["asc0_tx"], "A\\x00\nB")
        self.assertNotIn("asc0_tx", bridge.status(include_asc0_tx=False))

        client = web_ui.BrowserClient(None)
        client.enqueue_asc0_tx("stale")
        client.enqueue_asc0_tx_snapshot({"type": "state", "asc0_tx": "fresh"})
        self.assertEqual(client.asc0_tx_pending, "")
        self.assertEqual(json.loads(client.bootstrap)["asc0_tx"], "fresh")

    async def test_audio_validation_and_typed_browser_envelope(self):
        bridge = web_ui.CemuBridge("/tmp/not-used")
        await bridge.handle_cemu_packet(web_ui.Packet(
            web_ui.HELLO,
            hello_payload(101, 64, "c55", C55_BUTTONS, web_ui.CAP_AUDIO),
            1, 10,
        ))
        await finish_bootstrap(bridge)
        client = web_ui.BrowserClient(None)
        bridge.clients.add(client)
        pcm = struct.pack("<IHHhh", 48_000, 1, 0, -32768, 32767)
        await bridge.handle_cemu_packet(web_ui.Packet(
            web_ui.SPEAKER_PCM, pcm, 3, 1235
        ))
        self.assertEqual(client.audio[-1],
                         client.envelope(web_ui.WS_SPEAKER_PCM, pcm[8:]))
        await bridge.handle_cemu_packet(web_ui.Packet(
            web_ui.AUDIO_RESET, b"", 4, 1236
        ))
        self.assertEqual(list(client.audio), [client.envelope(web_ui.WS_AUDIO_RESET, b"")])
        client.audio.clear()
        client.audio_bytes = 0
        large = bytes(700_000)
        for _ in range(3):
            client.enqueue_audio(web_ui.WS_SPEAKER_PCM, large)
        self.assertEqual(len(client.audio), 2)
        self.assertLessEqual(client.audio_bytes,
                             web_ui.MAX_BROWSER_AUDIO_BYTES)
        client.enqueue_audio(web_ui.WS_SPEAKER_PCM,
                             bytes(web_ui.MAX_BROWSER_AUDIO_BYTES + 1))
        self.assertEqual((len(client.audio), client.audio_bytes), (0, 0))
        client.enqueue_audio(web_ui.WS_AUDIO_RESET)
        self.assertEqual(list(client.audio),
                         [client.envelope(web_ui.WS_AUDIO_RESET, b"")])
        with self.assertRaises(web_ui.ProtocolError):
            await bridge.handle_cemu_packet(web_ui.Packet(
                web_ui.SPEAKER_PCM,
                struct.pack("<IHHh", 16_000, 1, 0, 1), 5, 1237
            ))

    async def test_rejects_duplicate_or_malformed_key_descriptors(self):
        bridge = web_ui.CemuBridge("/tmp/not-used")
        duplicate = hello_payload(102, 68, "c55", ("up", "up"))
        with self.assertRaises(web_ui.ProtocolError):
            await bridge.handle_cemu_packet(web_ui.Packet(web_ui.HELLO, duplicate, 1, 0))
        malformed = bytearray(hello_payload(102, 68, "c55", ("up",)))
        malformed[-3] = 10
        with self.assertRaises(web_ui.ProtocolError):
            await bridge.handle_cemu_packet(
                web_ui.Packet(web_ui.HELLO, bytes(malformed), 2, 0)
            )


class StaticTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.bridge = web_ui.CemuBridge("/tmp/not-used")
        self.server = web_ui.WebServer(self.bridge, "127.0.0.1", 0)
        await self.server.start()
        self.base = f"http://127.0.0.1:{self.server.port}"
        self.client = ClientSession()

    async def asyncTearDown(self):
        await self.client.close()
        await self.server.stop()

    async def test_static_responses_and_traversal(self):
        response = await self.client.get(self.base + "/")
        body = await response.read()
        self.assertEqual(response.status, 200)
        self.assertIn(b"x55", body)
        self.assertNotIn(b">C55<", body)
        self.assertNotIn(b'class="product"', body)
        self.assertIn(b"<dt>STATUS</dt>", body)
        self.assertEqual(body.count(b'<div class="runtime-metric'), 5)
        self.assertIn(b'<body class="asc0-visible">', body)
        self.assertIn(b'id="asc0-panel" class="asc0-panel"', body)
        self.assertIn(b'id="files-panel"', body)
        self.assertIn(b'>ASC0 TX</span>', body)
        self.assertIn(b'<script type="module" src="/app.js"></script>', body)
        self.assertEqual(response.headers["X-Content-Type-Options"], "nosniff")

        for path in ("/missing", "/index.html", "/assets/phone.js",
                     "/app.js.map", "/%2e%2e/app.py"):
            with self.subTest(path=path):
                response = await self.client.get(self.base + path)
                self.assertEqual(response.status, 404)

    async def test_javascript_assets_and_security_headers(self):
        for filename in ("app.js", "connection.js", "phone.js", "capture.js", "keypad.js",
                         "status.js", "audio.js", "asc0.js", "files.js",
                         "audio-worklet.js"):
            with self.subTest(filename=filename):
                response = await self.client.get(self.base + "/" + filename)
                self.assertEqual(response.status, 200)
                self.assertEqual(response.headers["Content-Type"],
                                 "text/javascript; charset=utf-8")
                self.assertEqual(await response.read(),
                                 (web_ui.ASSET_DIR / filename).read_bytes())
                self.assertEqual(response.headers["Cache-Control"], "no-store")
                self.assertEqual(response.headers["X-Content-Type-Options"], "nosniff")
                self.assertEqual(response.headers["Content-Security-Policy"],
                                 "default-src 'self'; connect-src 'self'; img-src 'self' data:; "
                                 "style-src 'self'; script-src 'self'; frame-ancestors 'none'")

    async def test_websocket_origin_is_required_and_validated(self):
        response = await self.client.get(self.base + "/ws")
        self.assertEqual(response.status, 403)
        response = await self.client.get(
            self.base + "/ws", headers={"Origin": "http://evil.invalid"})
        self.assertEqual(response.status, 403)

    def test_non_loopback_requires_unsafe_flag(self):
        with self.assertRaises(SystemExit):
            web_ui.parse_args(["--connect", "/tmp/cemu.sock", "--host", "0.0.0.0"])
        args = web_ui.parse_args([
            "--connect", "/tmp/cemu.sock", "--host", "0.0.0.0", "--unsafe-bind"
        ])
        self.assertEqual(args.host, "0.0.0.0")


class BridgeIntegrationTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cemu-web-test-")
        self.path = os.path.join(self.temp.name, "cemu.sock")
        self.fake = FakeCemu(self.path)
        await self.fake.start()
        self.bridge = web_ui.CemuBridge(self.path, reconnect_delay=0.03)
        self.bridge_task = asyncio.create_task(self.bridge.run())
        await asyncio.wait_for(self.fake.connected.wait(), 2)
        await wait_until(lambda: self.bridge.frame is not None)
        self.web = web_ui.WebServer(self.bridge, "127.0.0.1", 0)
        await self.web.start()
        self.uri = f"ws://127.0.0.1:{self.web.port}/ws"
        self.origin = f"http://127.0.0.1:{self.web.port}"

    async def asyncTearDown(self):
        await self.web.stop()
        await self.bridge.stop()
        self.bridge_task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await self.bridge_task
        await self.fake.stop()
        self.temp.cleanup()

    async def recv_frame(self, websocket):
        async with asyncio.timeout(2):
            while True:
                message = await websocket.recv()
                if (isinstance(message, bytes) and message and
                        message[0] == web_ui.WS_FRAME):
                    return message[25:]

    async def test_overflow_disconnect_releases_only_slow_owner(self):
        async with connect(self.uri, origin=self.origin, proxy=None) as slow, connect(self.uri, origin=self.origin, proxy=None) as fast:
            await self.recv_frame(slow)
            await self.recv_frame(fast)
            await slow.send(json.dumps({**self.bridge.identifiers(), "type": "key", "key": "1", "pressed": True}))
            await fast.send(json.dumps({**self.bridge.identifiers(), "type": "key", "key": "2", "pressed": True}))
            await wait_until(lambda: all(any(k in c.held for c in self.bridge.clients) for k in ("1", "2")))
            slow_client = next(c for c in self.bridge.clients if "1" in c.held)
            for index in range(129):
                slow_client.enqueue_json({"type": "error", "index": index})
            await wait_until(lambda: slow_client not in self.bridge.clients)
            await asyncio.wait_for(slow.wait_closed(), 2)
            self.assertEqual(slow.close_code, 1013)
            self.assertFalse(any("1" in c.held for c in self.bridge.clients))
            self.assertTrue(any("2" in c.held for c in self.bridge.clients))
            self.assertEqual(len(self.bridge.clients), 1)
            releases = []
            while not self.fake.received.empty():
                releases.append((await self.fake.received.get()).payload)
            self.assertIn(web_ui.OWNER_TOKEN.pack(slow_client.owner_token), releases)
            fast_client = next(iter(self.bridge.clients))
            self.assertNotIn(web_ui.OWNER_TOKEN.pack(fast_client.owner_token), releases)

    async def test_delayed_filesystem_result_is_rejected_after_reattachment(self):
        entered, finish = asyncio.Event(), asyncio.Event()
        async def delayed_list(path):
            entered.set()
            await finish.wait()
            return []
        self.web.files.list = delayed_list
        async with ClientSession() as client:
            task = asyncio.create_task(client.get(self.origin + "/api/files/list?path=A:"))
            await entered.wait()
            self.bridge.generation += 1
            finish.set()
            response = await task
            self.assertEqual(response.status, 409)
            self.assertIn("retired", await response.text())

    async def test_frame_broadcast_and_stats(self):
        async with connect(self.uri, origin=self.origin, proxy=None) as websocket:
            self.assertEqual(await self.recv_frame(websocket), self.fake.frame)
            self.assertEqual(self.bridge.stats, {
                "icount": "42500000", "sample_sequence": "1", "measured_ns": "100",
                "elapsed_ns": "61140000000",
                "ticks": "42500000",
                "guest_instructions": "42500000",
                "pc": 0xEF4B98,
                "window_ns": "0", "rates_valid": False,
                "ticks_per_s": None, "guest_instructions_per_s": None,
            })
            await websocket.send(json.dumps({**self.bridge.identifiers(), "type": "set_running", "running": False}))
            message = json.loads(await websocket.recv())
            self.assertEqual(message, {**self.bridge.identifiers(), "type": "error", "message": "unknown browser control"})
            with self.assertRaises(TimeoutError):
                await asyncio.wait_for(self.fake.received.get(), 0.1)

    async def test_large_counters_remain_exact_decimal_strings_on_websocket(self):
        async with connect(self.uri, origin=self.origin, proxy=None) as websocket:
            await self.recv_frame(websocket)
            values = (2**63 + 1, 2**53 + 3, 2**53 + 7)
            self.fake.writer.write(web_ui.encode_packet(
                web_ui.STATS, web_ui.STATS_PAYLOAD.pack(*values, 0x123456, 1, 100, 0, 0.0, 0.0, 0)))
            await self.fake.writer.drain()
            async with asyncio.timeout(2):
                while True:
                    raw = await websocket.recv()
                    if isinstance(raw, str):
                        state = json.loads(raw)
                        if state.get("stats", {}).get("pc") == 0x123456:
                            break
            self.assertEqual({k: state["stats"][k] for k in ("elapsed_ns", "ticks", "guest_instructions", "pc")}, dict(
                zip(("elapsed_ns", "ticks", "guest_instructions", "pc"),
                    (*map(str, values), 0x123456))))

    async def test_duplicate_key_transitions_emit_one_press_and_release(self):
        async with connect(self.uri, origin=self.origin, proxy=None) as websocket:
            for pressed, expected in ((True, web_ui.KEY),
                                      (False, web_ui.KEY_RELEASE_AFTER_SAMPLE)):
                control = json.dumps({**self.bridge.identifiers(), "type": "key", "key": "soft-left", "pressed": pressed})
                await websocket.send(control)
                await websocket.send(control)
                packet = await asyncio.wait_for(self.fake.received.get(), 2)
                self.assertEqual(packet.message_type, expected)
                with self.assertRaises(TimeoutError):
                    await asyncio.wait_for(self.fake.received.get(), 0.1)

    async def test_capability_zero_phone_surface_and_controls(self):
        await self.fake.stop()
        self.fake = FakeCemu(self.path, asc0_available=False)
        await self.fake.start()
        await wait_until(lambda: self.bridge.connection_count >= 2)

        async with connect(self.uri, origin=self.origin, proxy=None) as websocket:
            state = None
            frame = None
            async with asyncio.timeout(2):
                while state is None or frame is None:
                    raw = await websocket.recv()
                    if isinstance(raw, str):
                        candidate = json.loads(raw)
                        if candidate.get("type") == "state":
                            state = candidate
                    else:
                        if raw and raw[0] == web_ui.WS_FRAME:
                            frame = raw[25:]
            self.assertFalse(state["asc0_available"])
            self.assertEqual(state["model"], "c55")
            self.assertEqual(state["keys"], list(C55_BUTTONS))
            self.assertEqual(frame, self.fake.frame)
            await websocket.send(json.dumps(
                {**self.bridge.identifiers(), "type": "key", "key": "soft-left", "pressed": True}))
            packet = await asyncio.wait_for(self.fake.received.get(), 2)
            self.assertEqual((packet.message_type, packet.payload),
                             (web_ui.KEY, bytes((14, 1))))

        transport = UiAsc0Transport(ClientConfig("embedded", timeout=1),
                                    self.bridge)
        with self.assertRaisesRegex(TransportError, "unavailable"):
            await transport.open()

    async def test_asc0_ready_barrier_and_single_fanout(self):
        self.fake.auto_ready = False
        transport = UiAsc0Transport(
            ClientConfig("embedded", timeout=1), self.bridge)
        opening = asyncio.create_task(transport.open())
        packet = await asyncio.wait_for(self.fake.received.get(), 2)
        self.assertEqual(packet.message_type, web_ui.ASC0_OPEN)
        write = asyncio.create_task(transport.write_all(b"AT\r\n", "at"))
        with self.assertRaises(TransportError):
            await write
        self.assertFalse(opening.done())
        await self.fake.send_ready()
        await opening
        await transport.write_all(b"AT\r\n", "at")
        packet = await asyncio.wait_for(self.fake.received.get(), 2)
        self.assertEqual((packet.message_type, packet.payload),
                         (web_ui.ASC0_RX, b"AT\r\n"))
        browser = web_ui.BrowserClient(None)
        self.bridge.clients.add(browser)
        await self.fake.send_asc0_tx(b"OK\r\n")
        self.assertEqual(await transport.read_exactly(4, "at"), b"OK\r\n")
        self.assertTrue(self.bridge.asc0_tx.endswith(b"OK\r\n"))
        self.assertEqual(browser.asc0_tx_pending, "OK\\x0D\n\\x0A\n")
        self.bridge.clients.remove(browser)
        await transport.close()
        packet = await asyncio.wait_for(self.fake.received.get(), 2)
        self.assertEqual(packet.message_type, web_ui.ASC0_CLOSE)

    async def test_asc0_history_for_late_client_and_live_delta(self):
        await self.fake.send_asc0_tx(b"<tag>\x00")
        await wait_until(lambda: self.bridge.asc0_tx == b"<tag>\x00")
        async with connect(self.uri, origin=self.origin, proxy=None) as websocket:
            async with asyncio.timeout(2):
                while True:
                    raw = await websocket.recv()
                    if isinstance(raw, str):
                        message = json.loads(raw)
                        if message.get("type") == "state":
                            break
            self.assertEqual(message["asc0_tx"], "<tag>\\x00\n")
            await self.fake.send_asc0_tx(b"&")
            async with asyncio.timeout(2):
                while True:
                    raw = await websocket.recv()
                    if isinstance(raw, str):
                        message = json.loads(raw)
                        if message.get("type") == "asc0_tx":
                            break
            self.assertEqual(message, {**self.bridge.identifiers(), "type": "asc0_tx", "text": "&"})
            await self.fake.send_asc0_tx(b"\xff\"")
            async with asyncio.timeout(2):
                while True:
                    raw = await websocket.recv()
                    if isinstance(raw, str):
                        message = json.loads(raw)
                        if message.get("type") == "asc0_tx":
                            break
            self.assertEqual(message, {**self.bridge.identifiers(), "type": "asc0_tx", "text": "\\xFF\""})
            self.assertEqual(self.bridge.asc0_tx, b"<tag>\x00&\xff\"")

            self.bridge.awaiting_hello = True
            await self.bridge.handle_cemu_packet(web_ui.Packet(
                web_ui.HELLO,
                hello_payload(self.fake.width, self.fake.height,
                              self.fake.model, self.fake.keys), 101, 0))
            await finish_bootstrap(self.bridge)
            self.assertEqual(self.bridge.asc0_tx, b"")

            stats = web_ui.STATS_PAYLOAD.pack(2, 3, 4, 5, 1, 100, 0, 0.0, 0.0, 0)
            self.fake.writer.write(web_ui.encode_packet(
                web_ui.STATS, stats, sequence=101, icount=42_500_002
            ))
            await self.fake.writer.drain()
            async with asyncio.timeout(2):
                while True:
                    raw = await websocket.recv()
                    if isinstance(raw, str):
                        message = json.loads(raw)
                        if message.get("type") == "metrics":
                            break
            self.assertNotIn("asc0_tx", message)

    async def test_multi_client_owner_registration_and_routing(self):
        first = await connect(self.uri, origin=self.origin, proxy=None)
        second = await connect(self.uri, origin=self.origin, proxy=None)
        try:
            opens = [await asyncio.wait_for(self.fake.owner_packets.get(), 2) for _ in range(2)]
            self.assertTrue(all(p.message_type == web_ui.OWNER_OPEN for p in opens))
            tokens = [web_ui.OWNER_TOKEN.unpack(p.payload)[0] for p in opens]
            self.assertNotEqual(*tokens)
            for ws, token in zip((first, second), tokens):
                await ws.send(json.dumps({**self.bridge.identifiers(), "type": "key", "key": "soft-left", "pressed": True, "owner": 999}))
                packet = await asyncio.wait_for(self.fake.owner_packets.get(), 2)
                self.assertEqual(packet.message_type, web_ui.OWNER_KEY)
                self.assertEqual(web_ui.OWNER_KEY_PAYLOAD.unpack(packet.payload),
                                 (token, C55_BUTTONS.index("soft-left"), 1))
            for ws, token in zip((first, second), tokens):
                await ws.close()
                packet = await asyncio.wait_for(self.fake.owner_packets.get(), 2)
                self.assertEqual((packet.message_type, packet.payload),
                                 (web_ui.OWNER_CLOSE, web_ui.OWNER_TOKEN.pack(token)))
        finally:
            await first.close()
            await second.close()

    async def test_server_stop_closes_browser_and_releases_keys(self):
        websocket = await connect(self.uri, origin=self.origin, proxy=None)
        index = C55_BUTTONS.index("soft-left")
        await websocket.send(json.dumps(
            {**self.bridge.identifiers(), "type": "key", "key": "soft-left", "pressed": True}
        ))
        packet = await asyncio.wait_for(self.fake.received.get(), 2)
        self.assertEqual((packet.message_type, packet.payload),
                         (web_ui.KEY, bytes((index, 1))))

        await asyncio.wait_for(self.web.stop(), 1)
        await asyncio.wait_for(websocket.wait_closed(), 1)
        self.assertEqual(websocket.close_code, 1001)
        self.assertEqual(self.bridge.clients, set())
        packet = await asyncio.wait_for(self.fake.received.get(), 2)
        self.assertIn(packet.message_type, (web_ui.OWNER_CLOSE, web_ui.OWNER_RELEASE_ALL))
        self.assertIsNone(self.web.site)
        self.assertIsNone(self.web.runner)

    async def test_browser_key_up_is_deferred_but_release_all_is_immediate(self):
        async with connect(self.uri, origin=self.origin, proxy=None) as websocket:
            index = C55_BUTTONS.index("down")
            await websocket.send(json.dumps(
                {**self.bridge.identifiers(), "type": "key", "key": "down", "pressed": True}
            ))
            packet = await asyncio.wait_for(self.fake.received.get(), 2)
            self.assertEqual((packet.message_type, packet.payload),
                             (web_ui.KEY, bytes((index, 1))))
            await websocket.send(json.dumps(
                {**self.bridge.identifiers(), "type": "key", "key": "down", "pressed": False}
            ))
            packet = await asyncio.wait_for(self.fake.received.get(), 2)
            self.assertEqual((packet.message_type, packet.payload),
                             (web_ui.KEY_RELEASE_AFTER_SAMPLE, bytes((index,))))

            await websocket.send(json.dumps(
                {**self.bridge.identifiers(), "type": "key", "key": "down", "pressed": True}
            ))
            await asyncio.wait_for(self.fake.received.get(), 2)
            await websocket.send(json.dumps({**self.bridge.identifiers(), "type": "release_all"}))
            packet = await asyncio.wait_for(self.fake.received.get(), 2)
            self.assertEqual(packet.message_type, web_ui.OWNER_RELEASE_ALL)

    async def test_bridge_reconnects(self):
        self.assertEqual(self.bridge.connection_count, 1)
        await self.fake.stop()
        await wait_until(lambda: not self.bridge.connected)
        self.fake = FakeCemu(self.path, frame=bytes([90, 80, 70]) * (101 * 64))
        await self.fake.start()
        await wait_until(lambda: self.bridge.connection_count >= 2)
        await wait_until(lambda: self.bridge.frame == self.fake.frame)
        self.assertTrue(self.bridge.connected)


class ChildLifecycleTests(unittest.IsolatedAsyncioTestCase):
    async def test_owned_child_is_terminated(self):
        owner = web_ui.ChildOwner([
            sys.executable, "-c", "import time; time.sleep(60)"
        ], "/tmp/unused-cemu-test.sock")
        await owner.start()
        self.assertIsNone(owner.process.returncode)
        await owner.stop()
        self.assertIsNotNone(owner.process.returncode)

    async def test_repeated_sigint_forces_owned_child(self):
        with tempfile.TemporaryDirectory(prefix="cemu-child-test-") as directory:
            ready = os.path.join(directory, "ready")
            owner = web_ui.ChildOwner([
                sys.executable, "-c",
                "import signal,sys,time; "
                "signal.signal(signal.SIGTERM, lambda *_: None); "
                "open(sys.argv[1], 'w').close(); time.sleep(60)",
                ready,
            ], "/tmp/unused-cemu-test.sock")
            await owner.start()
            await wait_until(lambda: os.path.exists(ready))
            shutdown = web_ui.ShutdownController(owner)

            shutdown.request(signal.SIGINT)
            stopping = asyncio.create_task(web_ui.finish_unless_forced(
                owner.stop(), shutdown.force_event
            ))
            await asyncio.sleep(0.05)
            self.assertFalse(stopping.done())
            shutdown.request(signal.SIGINT)
            await asyncio.wait_for(stopping, 1)
            await owner.force_stop()

            self.assertTrue(shutdown.stop_event.is_set())
            self.assertTrue(shutdown.force_event.is_set())
            self.assertEqual(owner.process.returncode, -signal.SIGKILL)

    async def test_attach_shutdown_force_and_repeated_sigterm(self):
        shutdown = web_ui.ShutdownController(None)
        shutdown.request(signal.SIGTERM)
        shutdown.request(signal.SIGTERM)
        self.assertTrue(shutdown.stop_event.is_set())
        self.assertFalse(shutdown.force_event.is_set())

        operation = asyncio.create_task(web_ui.finish_unless_forced(
            asyncio.Event().wait(), shutdown.force_event
        ))
        await asyncio.sleep(0)
        shutdown.request(signal.SIGINT)
        self.assertFalse(await asyncio.wait_for(operation, 1))
        self.assertTrue(shutdown.force_event.is_set())


if __name__ == "__main__":
    unittest.main()
