#!/usr/bin/env python3

import asyncio
import json
import unittest

from aiohttp import ClientSession, web

from tools.obex import RemotePath
from tools.obex.models import EntryKind, FileEntry

from .. import app as web_ui


from tools.obex.tests.fakes import MemoryClient as FakeObexClient


class FilesystemHttpTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.files = {
            "A:\\hello.txt": FileEntry(
                "hello.txt", RemotePath("A:\\hello.txt"), EntryKind.FILE, 5,
                attributes={"data": b"hello".hex()}),
            "A:\\Pictures": FileEntry(
                "Pictures", RemotePath("A:\\Pictures"), EntryKind.FOLDER),
        }
        self.created = []

        def factory():
            client = FakeObexClient(self.files)
            self.created.append(client)
            return client

        self.bridge = web_ui.CemuBridge("/tmp/not-used")
        self.server = web_ui.WebServer(self.bridge, "127.0.0.1", 0)
        self.server.files.client_factory = factory
        await self.server.start()
        self.base = f"http://127.0.0.1:{self.server.port}"
        self.origin = self.base
        self.client = ClientSession()
        self.mutation = {"X-CEMU-Request": "1"}

    async def asyncTearDown(self):
        await self.client.close()
        await self.server.stop()

    async def connect_files(self):
        response = await self.client.post(self.base + "/api/files/connect",
                                          headers=self.mutation)
        self.assertEqual(response.status, 200, await response.text())

    async def test_connect_list_stream_upload_download_and_overwrite_recheck(self):
        forbidden = await self.client.post(self.base + "/api/files/connect")
        self.assertEqual(forbidden.status, 403)
        await self.connect_files()

        response = await self.client.get(
            self.base + "/api/files/list", params={"path": "A:"})
        listing = await response.json()
        self.assertEqual({entry["name"] for entry in listing["entries"]},
                         {"hello.txt", "Pictures"})

        response = await self.client.put(
            self.base + "/api/files/upload", params={"path": "A:\\new.bin"},
            headers=self.mutation, data=b"abcdefgh")
        self.assertEqual(response.status, 201, await response.text())
        self.assertEqual(self.files["A:\\new.bin"].attributes["data"], b"abcdefgh".hex())
        self.assertEqual(self.created[0].read_sizes, [3, 3, 2, 0])

        response = await self.client.put(
            self.base + "/api/files/upload", params={"path": "A:\\hello.txt"},
            headers=self.mutation, data=b"first")
        self.assertEqual(response.status, 409)
        conflict = await response.json()
        self.files["A:\\hello.txt"] = FileEntry(
            "hello.txt", RemotePath("A:\\hello.txt"), EntryKind.FILE, 7,
            attributes={"data": b"changed".hex()})
        headers = dict(self.mutation, **{"X-CEMU-Overwrite": conflict["token"]})
        response = await self.client.put(
            self.base + "/api/files/upload", params={"path": "A:\\hello.txt"},
            headers=headers, data=b"second")
        self.assertEqual(response.status, 409)
        fresh = await response.json()
        headers["X-CEMU-Overwrite"] = fresh["token"]
        response = await self.client.put(
            self.base + "/api/files/upload", params={"path": "A:\\hello.txt"},
            headers=headers, data=b"final")
        self.assertEqual(response.status, 201, await response.text())
        self.assertEqual(self.files["A:\\hello.txt"].size, 5)

        response = await self.client.get(
            self.base + "/api/files/download", params={"path": "A:\\hello.txt"})
        self.assertEqual(await response.read(), b"final")
        self.assertIn("attachment", response.headers["Content-Disposition"])

    async def test_mkdir_confirmed_delete_and_multi_tab_refresh(self):
        await self.connect_files()
        ws1 = await self.client.ws_connect(self.base + "/ws", origin=self.origin)
        ws2 = await self.client.ws_connect(self.base + "/ws", origin=self.origin)
        await ws1.receive_json()
        await ws2.receive_json()
        response = await self.client.post(
            self.base + "/api/files/mkdir", headers={**self.mutation,
                                                      "Content-Type": "application/json"},
            data=json.dumps({"path": "A:\\Sounds"}))
        self.assertEqual(response.status, 201, await response.text())
        for websocket in (ws1, ws2):
            event = await websocket.receive_json(timeout=2)
            self.assertEqual(event["type"], "files_changed")

        response = await self.client.delete(
            self.base + "/api/files/delete", params={"path": "A:\\Sounds"},
            headers=self.mutation)
        self.assertEqual(response.status, 428)
        response = await self.client.delete(
            self.base + "/api/files/delete", params={"path": "A:\\Sounds"},
            headers={**self.mutation, "X-CEMU-Confirm": "delete"})
        self.assertEqual(response.status, 200, await response.text())
        await ws1.close()
        await ws2.close()

    async def test_malformed_obex_maps_to_502_and_retires_session(self):
        from tools.obex import codec
        from tools.obex.client import ObexFilesystemClient
        await self.connect_files()
        phone = self.created[0]
        original_list = phone._list
        original_get = phone._get
        original_write = phone.transport.write_all
        # Exercise the actual listing exchange/decoder and operation cleanup.
        phone._list = ObexFilesystemClient._list.__get__(phone)
        phone._get = ObexFilesystemClient._get.__get__(phone)

        async def malformed_packet(data, phase):
            if phase == "obex-get":
                phone.transport.incoming.extend(bytes((codec.SUCCESS, 0, 2)))
            else:
                await original_write(data, phase)
        phone.transport.write_all = malformed_packet
        response = await self.client.get(self.base + "/api/files/list?path=A:")
        self.assertEqual(response.status, 502)
        error = await response.json()
        self.assertEqual(error["error"], "remote_error")
        self.assertIn("length", error["message"])
        self.assertEqual(phone.state, "error")
        self.assertFalse(phone.transport.opened)
        response = await self.client.get(self.base + "/api/files/list?path=A:")
        self.assertEqual(response.status, 409)
        response = await self.client.post(self.base + "/api/files/connect", headers=self.mutation)
        self.assertEqual(response.status, 409)
        phone._list = original_list
        phone._get = original_get
        phone.transport.write_all = original_write
        response = await self.client.post(self.base + "/api/files/connect?retry=1", headers=self.mutation)
        self.assertEqual(response.status, 200)
        response = await self.client.get(self.base + "/api/files/list?path=A:")
        self.assertEqual(response.status, 200)

    async def test_interruption_invalidates_session_and_requires_retry(self):
        def failing_factory():
            client = FakeObexClient(self.files, fail_upload=True)
            self.created.append(client)
            return client

        self.server.files.client_factory = failing_factory
        await self.connect_files()
        response = await self.client.put(
            self.base + "/api/files/upload", params={"path": "A:\\broken.bin"},
            headers=self.mutation, data=b"broken")
        self.assertEqual(response.status, 503)
        self.assertFalse(self.created[0].transport.opened)
        status = await (await self.client.get(self.base + "/api/files/status")).json()
        self.assertEqual(status["state"], "error")
        response = await self.client.get(
            self.base + "/api/files/list", params={"path": "A:"})
        self.assertEqual(response.status, 409)
        self.created[0].fail_upload = False
        response = await self.client.post(
            self.base + "/api/files/connect?retry=1", headers=self.mutation)
        self.assertEqual(response.status, 200, await response.text())

    async def test_progress_owner_authorization_and_active_cancel(self):
        await self.connect_files()
        entered = asyncio.Event()
        original = self.created[0]._upload

        async def blocked(path, source, *, progress=None):
            from tools.obex.models import TransferProgress
            await progress(TransferProgress("upload", path, 2, 100, "running"))
            entered.set()
            await asyncio.Future()
        self.created[0]._upload = blocked
        request = asyncio.create_task(self.client.put(
            self.base + "/api/files/upload?path=A:/cancel.bin&transfer=active",
            headers={**self.mutation, "X-CEMU-Tab": "owner"}, data=b"payload"))
        await asyncio.wait_for(entered.wait(), 2)
        url = self.base + "/api/files/transfers/active"
        response = await self.client.get(url, headers={"X-CEMU-Tab": "owner"})
        self.assertEqual((await response.json())["completed"], 2)
        response = await self.client.post(url + "/cancel", headers={**self.mutation, "X-CEMU-Tab": "other"})
        self.assertEqual(response.status, 404)
        response = await self.client.post(url + "/cancel", headers={"X-CEMU-Tab": "owner"})
        self.assertEqual(response.status, 403)
        response = await self.client.post(url + "/cancel", headers={**self.mutation, "X-CEMU-Tab": "owner"})
        self.assertEqual(response.status, 200)
        from aiohttp import ClientConnectionError
        try:
            response = await asyncio.wait_for(request, 2)
            self.assertEqual(response.status, 409)  # an idempotent HTTP retry cannot reuse the ID
        except ClientConnectionError:
            pass
        self.assertEqual(self.server.files.status()["state"], "error")
        response = await self.client.get(url, headers={"X-CEMU-Tab": "owner"})
        self.assertEqual((await response.json())["state"], "cancelled")
        self.created[0]._upload = original

    async def test_queued_upload_cancel_preserves_active_streamed_download(self):
        from dataclasses import replace
        from aiohttp import ClientPayloadError
        await self.connect_files()
        path = "A:\\hello.txt"
        self.files[path] = replace(self.files[path], size=100)
        entered = asyncio.Event()

        async def blocked_get(path, sink, *, listing, progress=None):
            await sink.write(b"he")
            entered.set()
            await asyncio.Future()
        self.created[0]._get = blocked_get
        download = await self.client.get(
            self.base + "/api/files/download?path=A:/hello.txt&transfer=download&tab=first")
        await asyncio.wait_for(entered.wait(), 2)
        self.assertEqual(await download.content.readexactly(2), b"he")
        queued = asyncio.create_task(self.client.put(
            self.base + "/api/files/upload?path=A:/queued.bin&transfer=queued",
            headers={**self.mutation, "X-CEMU-Tab": "second"}, data=b"queued"))
        async with asyncio.timeout(2):
            while "queued" not in self.server.files._transfers:
                await asyncio.sleep(0.001)
        response = await self.client.post(self.base + "/api/files/transfers/queued/cancel",
            headers={**self.mutation, "X-CEMU-Tab": "second"})
        self.assertEqual(response.status, 200)
        self.assertEqual((await queued).status, 409)
        self.assertEqual(self.created[0].state, "ready")
        self.assertFalse(download.content.at_eof())
        response = await self.client.post(self.base + "/api/files/transfers/download/cancel",
            headers={**self.mutation, "X-CEMU-Tab": "first"})
        self.assertEqual(response.status, 200)
        with self.assertRaises(ClientPayloadError):
            await asyncio.wait_for(download.read(), 2)
        self.assertEqual(self.created[0].state, "error")

    async def test_download_missing_folder_unicode_and_security_headers(self):
        await self.connect_files()
        for path in ("A:/missing", "A:/Pictures"):
            response = await self.client.get(self.base + "/api/files/download", params={"path": path})
            self.assertEqual(response.status, 404)
        path = RemotePath('A:/café".bin')
        self.files[str(path)] = FileEntry(path.name, path, EntryKind.FILE, 3,
                                         attributes={"data": "00ff80"})
        response = await self.client.get(self.base + "/api/files/download", params={"path": str(path)})
        self.assertEqual(await response.read(), b"\x00\xff\x80")
        self.assertIn("caf%C3%A9%22.bin", response.headers["Content-Disposition"])
        self.assertEqual(response.headers["Cache-Control"], "no-store")
        self.assertEqual(response.headers["X-Content-Type-Options"], "nosniff")

    async def test_remote_path_security(self):
        await self.connect_files()
        for drive in ("B:", "C:", "D:"):
            response = await self.client.get(
                self.base + "/api/files/list", params={"path": drive + "\\private"})
            self.assertEqual(response.status, 400)
            body = await response.text()
            self.assertIn(f"filesystem root '{drive}'", body)
            self.assertIn("no host-side drive selector is proven", body)

        for value in ("A:\\..\\secret", "A:\\bad\x00name"):
            response = await self.client.get(
                self.base + "/api/files/list", params={"path": value})
            self.assertEqual(response.status, 400)


if __name__ == "__main__":
    unittest.main()
