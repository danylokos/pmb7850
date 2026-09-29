"""Owner transport ordering and desired intent across native generations."""
import asyncio
import unittest
from tools import ui_protocol as wire
from .. import app
from .test_runtime_delivery import Socket, attach
from .test_server import C55_BUTTONS, hello_payload, subscribe


class Writer:
    def __init__(self):
        self.packets = []
        self.block = None
        self.entered = asyncio.Event()

    def is_closing(self):
        return False

    def write(self, raw):
        packets, rest = wire.decode_packets(raw)
        assert not rest
        self.packets.extend(packets)

    async def drain(self):
        self.entered.set()
        if self.block:
            await self.block.wait()


class InputTransportTests(unittest.IsolatedAsyncioTestCase):
    async def setup_bridge(self):
        bridge = app.CemuBridge('unused')
        client = app.BrowserClient(Socket())
        bridge.clients.add(client)
        bridge.writer = writer = Writer()
        await attach(bridge, bytes(16))
        writer.packets.clear()
        return bridge, client, writer

    async def test_disconnected_release_and_replay_uses_new_indices(self):
        bridge, client, writer = await self.setup_bridge()
        await bridge.set_key(client, '1', True)
        await bridge.set_key(client, '2', True)
        bridge.connected = False
        await bridge.set_key(client, '1', False)
        await bridge.set_key(client, '3', True)
        self.assertEqual(client.held, {'2', '3'})
        writer.packets.clear()
        bridge.awaiting_hello = True
        keys = tuple(reversed(C55_BUTTONS))
        await bridge.handle_cemu_packet(app.Packet(app.HELLO,
            hello_payload(101, 64, 'c55', keys), 1, 0))
        await bridge.handle_cemu_packet(app.Packet(app.LIFECYCLE, bytes((1, 0, 0, 0)), 2, 0))
        self.assertEqual(writer.packets, [])
        await bridge.handle_cemu_packet(app.Packet(app.STATS,
            app.STATS_PAYLOAD.pack(0, 0, 0, 0, 1, 100, 0, 0., 0., 0), 3, 0))
        await subscribe(bridge)
        self.assertEqual([p.message_type for p in writer.packets], [wire.OWNER_OPEN, wire.OWNER_KEY, wire.OWNER_KEY])
        self.assertEqual([wire.OWNER_KEY_PAYLOAD.unpack(p.payload) for p in writer.packets[1:]],
                         [(client.owner_token, keys.index(k), 1) for k in ('2', '3')])

    async def test_retired_control_and_cleanup_cannot_modify_new_attachment(self):
        bridge, client, writer = await self.setup_bridge()
        await bridge.write_lock.acquire()
        press = asyncio.create_task(bridge.set_key(client, '1', True))
        release = asyncio.create_task(bridge.release_client(client))
        await asyncio.sleep(0)
        bridge.generation += 1
        client.held = {'2'}
        bridge.write_lock.release()
        await asyncio.gather(press, release)
        self.assertEqual(writer.packets, [])
        self.assertEqual(client.held, {'2'})

    async def test_close_during_registration_never_replays_closed_owner(self):
        bridge, client, writer = await self.setup_bridge()
        client.owner_generation = -1
        writer.entered.clear()
        writer.block = asyncio.Event()
        press = asyncio.create_task(bridge.set_key(client, '1', True))
        await writer.entered.wait()
        close = asyncio.create_task(bridge.release_client(client, close=True))
        await asyncio.sleep(0)
        writer.block.set()
        await asyncio.gather(press, close)
        self.assertEqual([p.message_type for p in writer.packets], [wire.OWNER_OPEN, wire.OWNER_CLOSE])
        writer.packets.clear()
        await bridge.replay_controls()
        await bridge.set_key(client, '1', True)
        self.assertEqual(writer.packets, [])
        self.assertFalse(client.held)

    async def test_release_all_after_sampled_up_still_targets_owner(self):
        bridge, client, writer = await self.setup_bridge()
        await bridge.set_key(client, '1', True)
        await bridge.set_key(client, '1', False)
        self.assertFalse(client.held)
        await bridge.release_client(client)
        self.assertEqual(writer.packets[-1].message_type, wire.OWNER_RELEASE_ALL)
        self.assertEqual(writer.packets[-1].payload, wire.OWNER_TOKEN.pack(client.owner_token))

    async def test_cancelled_browser_registration_runs_cleanup(self):
        bridge = app.CemuBridge('unused')
        bridge.writer = writer = Writer()
        await attach(bridge, bytes(16))
        await bridge.write_lock.acquire()
        task = asyncio.create_task(bridge.browser_handler(Socket()))
        await asyncio.sleep(0)
        self.assertEqual(len(bridge.clients), 1)
        client = next(iter(bridge.clients))
        task.cancel()
        await asyncio.sleep(0)
        self.assertTrue(client.closed)
        bridge.write_lock.release()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(bridge.clients, set())
        self.assertEqual(writer.packets, [])
