"""Stage 1 identity, bootstrap, and bounded delivery contracts."""
import asyncio
import json
import unittest
from unittest import mock
from tools import ui_protocol

from .. import app
from .test_server import C55_BUTTONS, hello_payload, subscribe, live


class Socket:
    def __init__(self):
        self.sent = []
        self.closed = None
        self.client = None

    async def close(self, *, code, message):
        self.closed = code
        if self.client:
            self.client.close()

    async def send_str(self, value):
        self.sent.append(json.loads(value))

    async def send_bytes(self, value):
        self.sent.append(value)


async def attach(bridge, run_id, *, width=101, height=64):
    bridge.awaiting_hello = True
    await bridge.handle_cemu_packet(app.Packet(app.HELLO,
        hello_payload(width, height, "c55", C55_BUTTONS), 1, 100, run_id))
    await bridge.handle_cemu_packet(app.Packet(app.LIFECYCLE, bytes((1, 0, 0, 0)), 2, 100, run_id))
    await bridge.handle_cemu_packet(app.Packet(app.STATS,
        app.STATS_PAYLOAD.pack(10, 100, 90, 0x1234, 1, 123456, 0, 0.0, 0.0, 0), 3, 100, run_id))
    await subscribe(bridge, run_id)


class RuntimeDeliveryTests(unittest.IsolatedAsyncioTestCase):
    async def test_atomic_replacement_and_same_run_reattachment(self):
        bridge = app.CemuBridge("unused")
        client = app.BrowserClient(Socket())
        bridge.clients.add(client)
        first, second = bytes(range(16)), bytes(range(16, 32))
        await attach(bridge, first)
        await bridge.handle_cemu_packet(app.Packet(app.FRAME, bytes([7]) * (101 * 64 * 3), 4, 100, first))
        await live(bridge, b"old serial", first)
        old_frame, old_stats = bridge.frame, bridge.stats.copy()
        old_generation = bridge.generation
        bridge.connected = False
        self.assertEqual(bridge.status()["stats"], old_stats)
        self.assertEqual(bridge.frame, old_frame)
        await attach(bridge, first)
        self.assertEqual(bridge.run_id, first.hex())
        self.assertGreater(bridge.generation, old_generation)
        self.assertEqual(bridge.frame, old_frame)
        self.assertEqual(bridge.asc0_tx, b"")
        bridge.awaiting_hello = True
        await bridge.handle_cemu_packet(app.Packet(app.HELLO,
            hello_payload(101, 64, "c55", C55_BUTTONS), 1, 0, second))
        self.assertEqual(bridge.run_id, first.hex())
        self.assertEqual(bridge.frame, old_frame)
        await bridge.handle_cemu_packet(app.Packet(app.LIFECYCLE, bytes((0, 0, 0, 0)), 2, 0, second))
        self.assertEqual(bridge.status()["run_id"], first.hex())
        await bridge.handle_cemu_packet(app.Packet(app.STATS,
            app.STATS_PAYLOAD.pack(0, 0, 0, 0, 1, 123457, 0, 0.0, 0.0, 0), 3, 0, second))
        await subscribe(bridge, second)
        state = json.loads(client.bootstrap)
        self.assertEqual((state["run_id"], state["stats"]["ticks"]), (second.hex(), "0"))
        self.assertIsNone(bridge.frame)
        self.assertIsNone(client.latest_frame)
        self.assertEqual(state["asc0_tx"], "")
        with self.assertRaises(app.ProtocolError):
            await bridge.handle_cemu_packet(app.Packet(app.FRAME, old_frame, 6, 100, first))
        with self.assertRaises(app.ProtocolError):
            await bridge.handle_browser_message(client, json.dumps({"type": "key", "key": "1", "pressed": True,
                "run_id": first.hex(), "generation": old_generation}))
        await attach(bridge, bytes([3]) * 16, width=101, height=80)
        self.assertEqual((bridge.width, bridge.height), (101, 80))

    async def test_bootstrap_requires_lifecycle_and_snapshot_before_streams(self):
        bridge = app.CemuBridge("unused")
        await bridge.handle_cemu_packet(app.Packet(app.HELLO,
            hello_payload(101, 64, "c55", C55_BUTTONS), 1, 0))
        with self.assertRaises(app.ProtocolError):
            await bridge.handle_cemu_packet(app.Packet(app.FRAME, bytes(101 * 64 * 3), 2, 0))
        self.assertFalse(bridge.connected)
        self.assertIsNone(bridge.run_id)

    async def test_lifecycle_can_advance_while_bootstrap_is_partially_written(self):
        bridge = app.CemuBridge("unused")
        await bridge.handle_cemu_packet(app.Packet(app.HELLO,
            hello_payload(101, 64, "c55", C55_BUTTONS), 1, 0))
        await bridge.handle_cemu_packet(app.Packet(app.LIFECYCLE, bytes((1, 0, 0, 0)), 2, 0))
        await bridge.handle_cemu_packet(app.Packet(app.LIFECYCLE,
            bytes((2, 5, 0, 0)) + b"limit", 3, 100))
        await bridge.handle_cemu_packet(app.Packet(app.STATS,
            app.STATS_PAYLOAD.pack(10, 100, 90, 0, 2, 200, 0, 0.0, 0.0, 0), 4, 100))
        await subscribe(bridge)
        self.assertTrue(bridge.connected)
        self.assertEqual(bridge.lifecycle["phase"], "stopped")
        self.assertEqual(bridge.stats["ticks"], "100")

    async def test_metrics_coalesce_while_ordered_events_survive(self):
        socket = Socket()
        client = app.BrowserClient(socket)
        client.enqueue_asc0_tx_snapshot({"type": "state", "run_id": "12" * 16, "generation": 9})
        for number in range(1000):
            client.enqueue_json({"type": "metrics", "stats": {"ticks": str(number)}})
            if number % 100 == 0:
                client.enqueue_json({"type": "lifecycle", "index": number})
        client.enqueue_json({"type": "files_changed", "index": 1001})
        self.assertEqual(len(client.messages), 11)
        self.assertEqual(json.loads(client.metrics)["stats"]["ticks"], "999")
        task = asyncio.create_task(client.send_loop())
        await asyncio.sleep(0)
        client.close()
        await task
        self.assertEqual(socket.sent[0]["type"], "state")
        self.assertEqual([event["index"] for event in socket.sent if "index" in event], list(range(0, 1000, 100)) + [1001])
        self.assertEqual(socket.sent[-1]["stats"]["ticks"], "999")
        self.assertIsNone(socket.closed)

    async def test_ordered_overflow_bounds_and_only_affected_client(self):
        for count, size in ((129, 1), (2, 140000)):
            slow, fast = Socket(), Socket()
            slow_client, fast_client = app.BrowserClient(slow), app.BrowserClient(fast)
            slow.client = slow_client
            for number in range(count):
                slow_client.enqueue_json({"type": "error", "index": number, "message": "x" * size})
                fast_client.enqueue_json({"type": "metrics", "stats": {"ticks": str(number)}})
            await slow_client.overflow_task
            self.assertEqual(slow.closed, 1013)
            self.assertIsNone(fast.closed)
            self.assertEqual(json.loads(slow_client.messages[0])["index"], 0)
            self.assertLessEqual(len(slow_client.messages), 128)
            self.assertLessEqual(slow_client.message_bytes, 256 * 1024)

    async def test_binary_envelopes_bind_both_identifiers(self):
        client = app.BrowserClient(Socket())
        client.enqueue_asc0_tx_snapshot({"type": "state", "run_id": "ab" * 16, "generation": 123})
        client.enqueue_frame(b"RGB")
        self.assertEqual(client.latest_frame[:17], b"\x01" + bytes.fromhex("ab" * 16))
        self.assertEqual(int.from_bytes(client.latest_frame[17:25], "little"), 123)
        self.assertEqual(client.latest_frame[25:], b"RGB")


class MeasurementCodecTests(unittest.IsolatedAsyncioTestCase):
    async def test_binary64_precision_and_malformed_rates(self):
        fields = (2**64-1, 2**53+3, 2**53+7, 0x123456, 2**64-1, 2**64-1)
        rates = (1_234_567_890, 1234567.890123, 0., 1)
        self.assertEqual(ui_protocol.decode_stats(app.STATS_PAYLOAD.pack(*fields, *rates)), fields + rates)
        for tail in ((1_000_000_000, float('nan'), 0., 1),
                     (1_000_000_000, 0., float('inf'), 1),
                     (1_000_000_000, -1., 0., 1), (0, 1., 0., 0),
                     (1, 0., 0., 0), (999999999, 1., 0., 1), (0, 0., 0., 2)):
            with self.subTest(tail=tail), self.assertRaises(app.ProtocolError):
                ui_protocol.decode_stats(app.STATS_PAYLOAD.pack(*fields, *tail))

    async def test_age_is_stamped_at_send_after_queue_delay(self):
        socket = Socket()
        client = app.BrowserClient(socket)
        stats = {"measured_ns": "9007199254740999", "ticks": "18446744073709551615"}
        for kind in ('state', 'metrics'):
            client.enqueue_json({"type": kind, "stats": stats})
        with mock.patch.object(app.time, 'monotonic_ns', return_value=9007202254741000):
            task = asyncio.create_task(client.send_loop())
            await asyncio.sleep(0)
            client.close()
            await task
        self.assertEqual(len(socket.sent), 2)
        for value in socket.sent:
            self.assertEqual(value['stats']['age_ns'], '3000000001')
            self.assertEqual(value['stats']['ticks'], stats['ticks'])
        self.assertNotIn('age_ns', stats)
