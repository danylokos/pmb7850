"""The same fragmented OBEX peer over scripted bytes and a real serial PTY."""
import asyncio
import os
import pty
import unittest
from contextlib import suppress

from tools.obex import (ClientConfig, ObexFilesystemClient, RemotePath,
                        MemorySource, MemorySink, OverwriteRequired)
from tools.obex import codec
from tools.obex.client import SIEMENS_FFS_TARGET
from tools.obex.transport import SerialTransport


class Peer:
    def __init__(self):
        self.files = {}
        self.pending = None
        self.put_name = None
        self.writes = []
        self.times = []

    def response(self, packet):
        self.writes.append(packet)
        self.times.append(asyncio.get_running_loop().time())
        if packet.startswith(b"AT"):
            return b"\r\nOK\r\n"
        if packet == b"+++":
            return b""
        code = packet[0]
        if code == codec.CONNECT:
            return codec.encode_packet(codec.SUCCESS, b"\x10\x00\x01\x00",
                                       codec.bytes_header(codec.WHO, SIEMENS_FFS_TARGET))
        headers = codec.parse_headers(packet[3:])
        name = next((h.value.decode("utf-16-be").rstrip("\0") for h in headers if h.identifier == codec.NAME), None)
        if code == codec.GET_FINAL:
            if self.pending is None:
                if any(h.identifier == codec.TYPE for h in headers):
                    self.pending = ('<?xml version="1.0" encoding="iso-8859-1"?><folder-listing>' +
                        ''.join(f'<file name="{key}" size="{len(value)}"/>' for key, value in self.files.items()) +
                        '</folder-listing>').encode('latin-1')
                else:
                    self.pending = self.files[name]
            body, self.pending = self.pending[:37], self.pending[37:]
            final = not self.pending
            if final:
                self.pending = None
            return codec.encode_packet(codec.SUCCESS if final else codec.CONTINUE,
                                       codec.bytes_header(codec.END_BODY if final else codec.BODY, body))
        if code in (codec.PUT, codec.PUT_FINAL):
            bodies = [h.value for h in headers if h.identifier in (codec.BODY, codec.END_BODY)]
            if not bodies:
                del self.files[name]
            else:
                if name is not None:
                    self.put_name = name
                # Emulate the measured Siemens append behavior.
                self.files[self.put_name] = self.files.get(self.put_name, b"") + b"".join(bodies)
            return codec.encode_packet(codec.SUCCESS if code == codec.PUT_FINAL else codec.CONTINUE)
        return codec.encode_packet(codec.SUCCESS)


class Scripted:
    def __init__(self, peer):
        self.peer = peer
        self.buffer = bytearray()

    async def open(self):
        pass

    async def close(self):
        pass

    async def write_all(self, data, phase):
        self.buffer.extend(self.peer.response(data))

    async def read_exactly(self, length, phase, timeout=None):
        assert len(self.buffer) >= length
        result = bytes(self.buffer[:length])
        del self.buffer[:length]
        return result


class StreamingTests(unittest.IsolatedAsyncioTestCase):
    async def roundtrip(self, transport, peer):
        client = ObexFilesystemClient(ClientConfig("fixture"), transport=transport)
        data = bytes(range(256)) * 12 + b"\0\xfftail"
        path = RemotePath("A:/caf\xe9.bin")
        counts = []
        async def progress(value):
            counts.append(value)
        try:
            await client.connect()
            await client.upload(path, MemorySource(data), progress=progress)
            self.assertEqual(peer.files[path.name], data)
            with self.assertRaises(OverwriteRequired) as error:
                await client.upload(path, MemorySource(b"replace"))
            await client.upload(path, MemorySource(data[::-1]), confirmation_token=error.exception.token)
            sink = MemorySink()
            metadata = []
            async def prepared(entry):
                metadata.append(entry)
                self.assertFalse(sink.data)
            await client.download(path, sink, metadata=prepared, progress=progress)
            self.assertEqual(sink.data, data[::-1])
            self.assertEqual(metadata[0].size, len(data))
            self.assertEqual(counts[-1].completed, len(data))
            await client.delete(path)
            self.assertEqual(await client.list(RemotePath("A:")), [])
            self.assertGreaterEqual(peer.times[1] - peer.times[0], 1.0)
            self.assertGreaterEqual(peer.times[2] - peer.times[1], 1.0)
        finally:
            await client.close()

    async def test_scripted_fragmented_binary_roundtrip(self):
        peer = Peer()
        await self.roundtrip(Scripted(peer), peer)

    @unittest.skipUnless(os.name == "posix", "requires POSIX PTY")
    async def test_real_pty_stalled_connect_is_cancellable(self):
        from tools.obex import SessionUnavailable
        master, slave = pty.openpty()
        transport = SerialTransport(ClientConfig(os.ttyname(slave), timeout=3))
        client = ObexFilesystemClient(transport.config, transport=transport)
        task = asyncio.create_task(client.connect())
        try:
            async with asyncio.timeout(2):
                while transport._serial is None:
                    await asyncio.sleep(0.001)
                queued = asyncio.create_task(client.list(RemotePath("A:")))
                await asyncio.sleep(0)
                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await task
                with self.assertRaises(SessionUnavailable):
                    await queued
            self.assertEqual(client.state, "error")
            self.assertIsNone(transport._serial)
        finally:
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task
            await client.close()
            os.close(master)
            os.close(slave)

    @unittest.skipUnless(os.name == "posix", "requires POSIX PTY")
    async def test_real_pty_fragmented_protocol_roundtrip(self):
        peer = Peer()
        master, slave = pty.openpty()
        os.set_blocking(master, False)
        queue = asyncio.Queue()
        loop = asyncio.get_running_loop()
        def readable():
            with suppress(BlockingIOError):
                queue.put_nowait(os.read(master, 65536))
        loop.add_reader(master, readable)
        async def serve():
            buffer = bytearray()
            while True:
                buffer.extend(await queue.get())
                while buffer:
                    if buffer.startswith(b"AT"):
                        end = buffer.find(b"\r\n")
                        if end < 0:
                            break
                        size = end + 2
                    elif buffer.startswith(b"+++"):
                        size = 3
                    else:
                        if len(buffer) < 3:
                            break
                        size = int.from_bytes(buffer[1:3], "big")
                    if len(buffer) < size:
                        break
                    packet = bytes(buffer[:size])
                    del buffer[:size]
                    response = peer.response(packet)
                    for offset in range(0, len(response), 7):
                        os.write(master, response[offset:offset + 7])
                        await asyncio.sleep(0.001)
        server = asyncio.create_task(serve())
        try:
            async with asyncio.timeout(20):
                await self.roundtrip(SerialTransport(ClientConfig(os.ttyname(slave), timeout=3)), peer)
        finally:
            server.cancel()
            with suppress(asyncio.CancelledError):
                await server
            loop.remove_reader(master)
            os.close(slave)
            os.close(master)
