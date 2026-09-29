"""Native serial identity, replay isolation, and attachment retirement."""
import asyncio
import struct
import unittest

from tools import ui_protocol as wire
from tools.obex import ClientConfig
from tools.obex.errors import TransportError
from .. import app
from ..asc0 import UiAsc0Transport
from .test_input_ownership import Writer
from .test_runtime_delivery import Socket, attach
from .test_server import C55_BUTTONS, hello_payload, live, subscribe


class SerialHistoryTests(unittest.IsolatedAsyncioTestCase):
    async def setup_bridge(self):
        bridge = app.CemuBridge('unused')
        bridge.writer = Writer()
        await attach(bridge, bytes(16))
        return bridge

    async def test_subscription_is_atomic_commit_and_offline_cache_survives(self):
        b = await self.setup_bridge()
        await live(b, b'old\x00history')
        identity, cache, stats = b.serial_identity, bytes(b.asc0_tx), b.stats
        b.connected = False
        # Offline browser gets a coherent cache snapshot without native replay.
        client = app.BrowserClient(Socket())
        client.enqueue_asc0_tx_snapshot(b.status())
        self.assertIn('old', client.bootstrap)
        b.awaiting_hello = True
        await b.handle_cemu_packet(wire.Packet(wire.HELLO,
            hello_payload(101, 64, 'c55', C55_BUTTONS), 1, 100))
        await b.handle_cemu_packet(wire.Packet(wire.LIFECYCLE, bytes((1, 0, 0, 0)), 2, 100))
        await b.handle_cemu_packet(wire.Packet(wire.STATS,
            wire.STATS_PAYLOAD.pack(1, 2, 3, 4, 1, 100, 0, 0., 0., 0), 3, 100))
        self.assertFalse(b.connected)
        self.assertEqual((b.serial_identity, bytes(b.asc0_tx), b.stats), (identity, cache, stats))
        with self.assertRaises(wire.ProtocolError):
            await b.handle_cemu_packet(wire.Packet(wire.ASC0_SUBSCRIBED, bytes(16), 4, 100))
        self.assertEqual((b.serial_identity, bytes(b.asc0_tx)), (identity, cache))
        await subscribe(b, start=123, handle=2)
        self.assertEqual(b.serial_mirror.next, 123)
        self.assertEqual(b.asc0_tx, b'')
        self.assertGreater(b.generation, identity[1])
        await live(b, b'new')
        self.assertEqual(b.serial_mirror.next, 126)

    async def test_bad_live_offsets_never_reach_cache_browser_or_obex(self):
        b = await self.setup_bridge()
        client = app.BrowserClient(Socket())
        b.clients.add(client)
        transport = UiAsc0Transport(ClientConfig(port='ui'), b)
        transport._opened = transport._ready = True
        b.asc0_transport = transport
        await live(b, b'ok')
        for payload in (b'', struct.pack('<QQ', 1, 2),
                        struct.pack('<QQ', 2, 2) + b'bad',
                        struct.pack('<QQ', 1, 1) + b'duplicate',
                        struct.pack('<QQ', 1, 3) + b'gap',
                        struct.pack('<QQ', 1, 2**64 - 1) + b'overflow'):
            with self.subTest(payload=payload), self.assertRaises(wire.ProtocolError):
                await b.handle_cemu_packet(wire.Packet(wire.ASC0_TX, payload, 5, 0))
            self.assertEqual(b.asc0_tx, b'ok')
            self.assertEqual(client.asc0_tx_pending, 'ok')
            self.assertEqual(transport._buffer, b'ok')
            self.assertEqual(b.serial_mirror.next, 2)

    async def test_concurrent_chunked_replay_live_and_obex_byte_identity(self):
        b = await self.setup_bridge()
        data = bytes(range(256)) * 600
        await live(b, data[:50000])
        await live(b, data[50000:100000])
        await live(b, data[100000:])
        transport = UiAsc0Transport(ClientConfig(port='ui'), b)
        transport._opened = transport._ready = True
        b.asc0_transport = transport
        replay = asyncio.create_task(b.read_asc0_history(0, len(data)))
        second = asyncio.create_task(b.read_asc0_history(0, 0))
        live_data = b'\x00\xffOBEX live reply\x81'
        await asyncio.sleep(0)
        await live(b, live_data)
        replies = 0
        while not replay.done() or not second.done():
            await asyncio.sleep(0)
            pending = b.history_pending
            if pending is None:
                continue
            _, request, start, end, _ = pending
            next_offset = min(end, start + wire.MAX_PAYLOAD - 32)
            payload = wire.SERIAL_RANGE.pack(1, request, start, next_offset) + data[start:next_offset]
            await b.handle_cemu_packet(wire.Packet(wire.ASC0_HISTORY_DATA, payload, 10, 0))
            replies += 1
        self.assertEqual(await replay, data)
        self.assertEqual(await second, b'')
        self.assertGreaterEqual(replies, 4)
        self.assertEqual(b.asc0_tx, data + live_data)
        self.assertEqual(await transport.read_exactly(len(live_data), 'reply'), live_data)
        self.assertFalse(transport._buffer)

    async def test_pending_replay_and_retired_callbacks_cannot_cross_generation(self):
        b = await self.setup_bridge()
        await live(b, b'old')
        old_writer = b.writer
        task = asyncio.create_task(b.read_asc0_history(0, 3))
        await asyncio.sleep(0)
        b.writer = Writer()
        await attach(b, bytes(16))
        with self.assertRaises(TransportError):
            await task
        self.assertEqual(b.asc0_tx, b'')
        await b.handle_cemu_packet(wire.Packet(wire.ASC0_TX,
            struct.pack('<QQ', 1, 0) + b'retired', 10, 0), old_writer)
        self.assertEqual(b.asc0_tx, b'')
        await b.write_lock.acquire()
        task = asyncio.create_task(b.read_asc0_history(0, 0))
        await asyncio.sleep(0)
        b.generation += 1
        b.write_lock.release()
        with self.assertRaises(TransportError):
            await task
        self.assertFalse(b.history_pending)

    async def test_malformed_replay_response_and_cancelled_request(self):
        b = await self.setup_bridge()
        await live(b, b'abc')
        task = asyncio.create_task(b.read_asc0_history(0, 3))
        await asyncio.sleep(0)
        for fields, data in (((2, 1, 0, 3), b'abc'), ((1, 2, 0, 3), b'abc'),
                             ((1, 1, 1, 3), b'bc'), ((1, 1, 0, 4), b'abcd'),
                             ((1, 1, 0, 0), b''), ((1, 1, 0, 3), b'ab')):
            with self.assertRaises(wire.ProtocolError):
                await b.handle_cemu_packet(wire.Packet(wire.ASC0_HISTORY_DATA,
                    wire.SERIAL_RANGE.pack(*fields) + data, 1, 0))
            self.assertFalse(task.done())
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        with self.assertRaises(TransportError):
            await b.read_asc0_history(0, 0)
        await b.handle_cemu_packet(wire.Packet(wire.ASC0_HISTORY_DATA,
            wire.SERIAL_RANGE.pack(1, 1, 0, 3) + b'abc', 1, 0))
        self.assertIsNone(b.history_pending)
        self.assertEqual(b.asc0_tx, b'abc')

    async def test_ready_validates_boundary_before_changing_transport(self):
        b = await self.setup_bridge()
        transport = UiAsc0Transport(ClientConfig(port='ui'), b)
        transport._opened = True
        b.asc0_transport = transport
        b.asc0_ready_waiter = asyncio.get_running_loop().create_future()
        for payload in (b'', struct.pack('<QQ', 2, 0), struct.pack('<QQ', 1, 1)):
            with self.assertRaises(wire.ProtocolError):
                await b.handle_cemu_packet(wire.Packet(wire.ASC0_READY, payload, 1, 0))
            self.assertFalse(transport._ready)
        await b.handle_cemu_packet(wire.Packet(wire.ASC0_READY, struct.pack('<QQ', 1, 0), 1, 0))
        self.assertTrue(transport._ready)
