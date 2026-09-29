"""Complete-operation policy, independent of HTTP and native attachments."""
import asyncio
import io
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from tools.obex import (RemotePath, MemorySource, MemorySink, OverwriteRequired,
                        PathConflict, SessionUnavailable, FileNotFound, resolve_remote_path)
from tools.obex.cli import run_shell, _upload
from tools.obex.models import FileEntry, EntryKind
from .fakes import MemoryClient


class PolicyTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.path = RemotePath("a:/test.bin")
        self.entry = FileEntry(self.path.name, self.path, EntryKind.FILE, 3, "now",
                               {"data": b"old".hex()})
        self.client = MemoryClient({str(self.path): self.entry})
        self.events = []

        async def changed(event):
            self.events.append(event)
        self.client.on_mutation = changed
        await self.client.connect()

    async def asyncTearDown(self):
        await self.client.invalidate("test complete")
        await self.client.close()

    async def token(self, path=None):
        with self.assertRaises(OverwriteRequired) as error:
            await self.client.upload(path or self.path, MemorySource(b"new"))
        return error.exception.token

    async def test_overwrite_race_single_use_and_session_binding(self):
        token = await self.token()
        self.client.files[str(self.path)] = replace(self.entry, modified="later")
        with self.assertRaises(OverwriteRequired) as changed:
            await self.client.upload(self.path, MemorySource(b"new"), confirmation_token=token)
        fresh = changed.exception.token
        await self.client.upload(self.path, MemorySource(b"new"), confirmation_token=fresh)
        self.assertEqual([event.operation for event in self.events], ["delete", "upload"])
        self.assertEqual(self.client.files[str(self.path)].attributes["data"], b"new".hex())
        with self.assertRaises(OverwriteRequired):
            await self.client.upload(self.path, MemorySource(b"bad"), confirmation_token=fresh)
        token = await self.token()
        await self.client.invalidate("disconnected")
        with self.assertRaises(SessionUnavailable):
            await self.client.connect()
        await self.client.connect(retry=True)
        with self.assertRaises(OverwriteRequired):
            await self.client.upload(self.path, MemorySource(b"bad"), confirmation_token=token)

    async def test_token_wrong_path_directory_and_disappeared_target(self):
        token = await self.token()
        other = RemotePath("A:/other")
        self.client.files[str(other)] = replace(self.entry, path=other, name=other.name)
        with self.assertRaises(OverwriteRequired):
            await self.client.upload(other, MemorySource(b"x"), confirmation_token=token)
        with self.assertRaises(OverwriteRequired):
            await self.client.upload(self.path, MemorySource(b"x"), confirmation_token=token)
        self.client.files[str(self.path)] = replace(self.entry, kind=EntryKind.FOLDER)
        with self.assertRaises(PathConflict):
            await self.client.upload(self.path, MemorySource(b"x"))
        self.client.files[str(self.path)] = self.entry
        token = await self.token()
        del self.client.files[str(self.path)]
        await self.client.upload(self.path, MemorySource(b"new"), confirmation_token=token)
        self.assertEqual([event.operation for event in self.events], ["upload"])

    async def test_listing_replacement_and_put_share_one_lock(self):
        token = await self.token()
        entered, release = asyncio.Event(), asyncio.Event()
        original = self.client._delete

        async def blocked(path):
            entered.set()
            await release.wait()
            await original(path)
        self.client._delete = blocked
        upload = asyncio.create_task(self.client.upload(
            self.path, MemorySource(b"new"), confirmation_token=token))
        await entered.wait()
        listing = asyncio.create_task(self.client.list(RemotePath("A:")))
        await asyncio.sleep(0)
        self.assertFalse(listing.done())
        release.set()
        await upload
        entries = await listing
        self.assertEqual(entries[0].attributes["data"], b"new".hex())

    async def blocked_listing(self):
        entered = asyncio.Event()
        original = self.client._list

        async def blocked(path):
            entered.set()
            await asyncio.Future()
        self.client._list = blocked
        task = asyncio.create_task(self.client.list(RemotePath("A:")))
        await entered.wait()
        return task, original

    async def test_queued_cancel_does_not_invalidate_active_session(self):
        task, original = await self.blocked_listing()
        queued = asyncio.create_task(self.client.mkdir(RemotePath("A:/queued")))
        await asyncio.sleep(0)
        queued.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await queued
        self.assertEqual(self.client.state, "ready")
        self.assertFalse(task.done())
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task

    async def test_active_cancel_retires_queued_calls_and_clears_tokens(self):
        await self.token()
        task, original = await self.blocked_listing()
        queued = asyncio.create_task(self.client.mkdir(RemotePath("A:/queued")))
        await asyncio.sleep(0)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertEqual(self.client.state, "error")
        self.assertFalse(self.client.transport.opened)
        self.assertFalse(self.client._overwrite_checks)
        self.client._list = original
        await self.client.connect(retry=True)
        with self.assertRaises(SessionUnavailable):
            await queued
        self.assertEqual(len(await self.client.list(RemotePath("A:"))), 1)
        self.assertFalse(any(phase == "obex-abort" for phase, _ in self.client.transport.writes))

    async def test_repeated_cancel_keeps_lock_until_transport_cleanup_finishes(self):
        task, original_list = await self.blocked_listing()
        entered, release = asyncio.Event(), asyncio.Event()
        original_close = self.client.transport.close

        async def blocked_close():
            entered.set()
            await release.wait()
            await original_close()
        self.client.transport.close = blocked_close
        task.cancel()
        await entered.wait()
        for _ in range(3):
            task.cancel()
            await asyncio.sleep(0)
        self.assertFalse(task.done())
        self.assertTrue(self.client._operation_lock.locked())
        release.set()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertFalse(self.client.transport.opened)
        self.client.transport.close = original_close
        self.client._list = original_list

    async def test_external_invalidation_and_close_cancel_active_transaction(self):
        for method in (self.client.invalidate, self.client.close):
            task, original = await self.blocked_listing()
            await method()
            with self.assertRaises(asyncio.CancelledError):
                await task
            self.assertFalse(self.client.transport.opened)
            self.client._list = original
            await self.client.connect(retry=True)

    async def test_download_metadata_precedes_body_and_holds_lock(self):
        entered, release = asyncio.Event(), asyncio.Event()
        sink, counts = MemorySink(), []

        async def metadata(entry):
            self.assertEqual(entry, self.entry)
            self.assertFalse(sink.data)
            entered.set()
            await release.wait()

        async def progress(value):
            counts.append(value.completed)
        task = asyncio.create_task(self.client.download(self.path, sink, metadata=metadata, progress=progress))
        await entered.wait()
        deletion = asyncio.create_task(self.client.delete(self.path))
        await asyncio.sleep(0)
        self.assertFalse(deletion.done())
        release.set()
        await task
        await deletion
        self.assertEqual(sink.data, b"old")
        self.assertEqual(counts, [2, 3, 3])
        with self.assertRaises(FileNotFound):
            await self.client.download(self.path, MemorySink())

    async def test_failed_replacement_reports_successful_delete_only(self):
        token = await self.token()
        self.client.fail_upload = True
        with self.assertRaises(ConnectionError):
            await self.client.upload(self.path, MemorySource(b"new"), confirmation_token=token)
        self.assertEqual([event.operation for event in self.events], ["delete"])
        self.assertNotIn(str(self.path), self.client.files)

    async def test_cli_overwrite_and_interactive_confirmation(self):
        with tempfile.TemporaryDirectory() as tmp:
            local = Path(tmp) / "source"
            local.write_bytes(b"cli")
            errors = io.StringIO()
            await run_shell(self.client, input_stream=io.StringIO(
                f"put {local} A:/test.bin\nput --overwrite {local} A:/test.bin\nquit\n"),
                output_stream=io.StringIO(), error_stream=errors)
            self.assertIn("overwrite confirmation required", errors.getvalue())
            self.assertEqual(self.client.files[str(self.path)].attributes["data"], b"cli".hex())
            class Terminal(io.StringIO):
                def isatty(self):
                    return True
            output = io.StringIO()
            await _upload(self.client, str(local), self.path,
                          input_stream=Terminal("yes\n"), output_stream=output)
            self.assertIn("Overwrite", output.getvalue())

    def test_paths_reject_controls_colons_and_unsupported_roots(self):
        for path in (None, "A:/bad\x00", "A:/bad\x7f", "A:/bad\x85", "A:/bad:name", "A:/..", "B:", "A:/" + "x" * 513):
            with self.subTest(path=path), self.assertRaises(ValueError):
                RemotePath(path)
        for path in ("bad\x00/..", "bad\x85/..", "bad:name/.."):
            with self.subTest(relative=path), self.assertRaises(ValueError):
                resolve_remote_path(path, RemotePath("A:"))
        self.assertEqual(RemotePath("a://dir\\file"), RemotePath("A:/dir/file"))
