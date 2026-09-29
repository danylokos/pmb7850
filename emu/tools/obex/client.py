"""Stateful Siemens Line55 OBEX filesystem service."""

from __future__ import annotations

import asyncio
import secrets
from contextlib import asynccontextmanager
from collections.abc import Awaitable, Callable

from . import codec
from .errors import (ProtocolError, RemoteError, TransportError, UnsupportedFilesystemRootError,
                     SessionUnavailable, PathConflict, FileNotFound, OverwriteRequired)
from .listing import parse_folder_listing
from .models import (
    ClientConfig, ConnectionInfo, FileEntry, RemotePath, TransferProgress, TransferResult,
    ConnectionState, EntryKind, MutationEvent,
)
from .streams import DownloadSink, UploadSource
from .transport import SerialTransport

SIEMENS_FFS_TARGET = bytes.fromhex("6b01cb31410611d49a770050da3f471f")
FOLDER_LISTING_TYPE = b"x-obex/folder-listing\0"
SIEMENS_FFS_ROOT = "A:"
ProgressCallback = Callable[[TransferProgress], Awaitable[None]]


class ObexFilesystemClient:
    """One reusable, serialized OBEX filesystem session."""

    def __init__(self, config: ClientConfig, *, transport=None, on_mutation=None) -> None:
        self.config = config
        self.transport = transport or SerialTransport(config)
        self.info: ConnectionInfo | None = None
        self._cwd: tuple[str, ...] = ()
        self._operation_lock = asyncio.Lock()
        self._opened = False
        self._invalid = False
        self.state = ConnectionState.DISCONNECTED
        self.error: str | None = None
        self.on_mutation = on_mutation
        self._generation = 0
        self._active = None
        self._overwrite_checks = {}

    async def __aenter__(self) -> "ObexFilesystemClient":
        await self.connect()
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        await self.close()

    async def _read_at_response(self) -> bytes:
        deadline = asyncio.get_running_loop().time() + self.config.timeout
        data = bytearray()
        while asyncio.get_running_loop().time() < deadline:
            try:
                data.extend(await self.transport.read_exactly(1, "at", timeout=0.5))
            except TimeoutError:
                continue
            upper = bytes(data).upper()
            if b"OK\r\n" in upper or b"ERROR\r\n" in upper:
                return bytes(data)
            if len(data) > 4096:
                raise ProtocolError("AT response exceeds 4096 bytes")
        raise TimeoutError("timed out waiting for AT response")

    async def _enter_obex_mode(self) -> None:
        last = b""
        for _ in range(self.config.at_retries):
            await self.transport.write_all(b"AT^SQWE=0\r\n", "at-sqwe-0")
            last = await self._read_at_response()
            if b"OK\r\n" in last.upper():
                break
        else:
            raise ProtocolError(f"AT^SQWE=0 rejected: {last!r}")
        # The C55 task acknowledges mode 0 before its provider handoff is ready
        # to parse the next command.  A measured one-second quiet interval keeps
        # the subsequent SQWE3 bytes on the normal command path. Keep this
        # calibration configurable and leave the MPM-required post-write wait
        # intact.
        if self.config.sqwe0_settle > 0:
            await asyncio.sleep(self.config.sqwe0_settle)
        await self.transport.write_all(b"AT^SQWE=3\r\n", "at-sqwe-3")
        await asyncio.sleep(1.0)
        last = await self._read_at_response()
        if b"OK\r\n" not in last.upper():
            raise ProtocolError(f"AT^SQWE=3 rejected: {last!r}")

    async def _exchange(self, packet: bytes, phase: str) -> codec.Packet:
        await self.transport.write_all(packet, phase)
        prefix = await self.transport.read_exactly(3, phase)
        code, length = codec.decode_prefix(prefix)
        payload = await self.transport.read_exactly(length - 3, phase) if length > 3 else b""
        return codec.Packet(code, payload)

    def _connection_header(self) -> bytes:
        if self.info is None or self.info.connection_id is None:
            return b""
        return codec.uint32_header(codec.CONNECTION_ID, self.info.connection_id)

    @asynccontextmanager
    async def _operation(self, *, connecting=False):
        generation = self._generation
        # Cancellation here affects only this waiter, never the current owner.
        async with self._operation_lock:
            if generation != self._generation:
                raise SessionUnavailable("operation belongs to a retired session")
            if not connecting:
                await self._ensure_connected()
            self._active = asyncio.current_task()
            try:
                yield
            except (asyncio.CancelledError, ConnectionError, TransportError,
                    TimeoutError, ProtocolError, OSError) as exc:
                self._retire(f"operation interrupted: {exc}")
                # The interrupted exchange may have an unread response. Closing
                # without ABORT avoids injecting another packet into that stream.
                cleanup = asyncio.create_task(self._close_transport())
                while not cleanup.done():
                    try:
                        await asyncio.shield(cleanup)
                    except asyncio.CancelledError:
                        # Repeated Cancel/close requests must not release the
                        # operation lock while transport cleanup is still active.
                        continue
                cleanup.result()
                raise
            finally:
                self._active = None

    def _retire(self, reason):
        self._generation += 1
        self._overwrite_checks.clear()
        self._invalid = True
        self.state = ConnectionState.ERROR
        self.error = reason

    async def _close_transport(self):
        try:
            await self.transport.close()
        finally:
            self.info = None
            self._opened = False
            self._cwd = ()

    async def invalidate(self, reason="session invalidated"):
        self._retire(reason)
        active = self._active
        if active is not None and active is not asyncio.current_task():
            active.cancel()
        async with self._operation_lock:
            await self._close_transport()

    async def connect(self, *, retry=False) -> ConnectionInfo:
        async with self._operation(connecting=True):
            if self.info is not None and not self._invalid and not retry:
                return self.info
            if self._invalid and not retry:
                raise SessionUnavailable("session is invalid; reconnect explicitly with retry=True")
            if retry:
                self._retire("reconnecting")
                await self._close_transport()
                self._invalid = False
            self.state = ConnectionState.CONNECTING
            self.error = None
            if not self._opened:
                await self.transport.open()
                self._opened = True
            try:
                await self._enter_obex_mode()
                request = codec.encode_packet(
                    codec.CONNECT,
                    b"\x10\x00\x40\x06",
                    codec.bytes_header(codec.TARGET, SIEMENS_FFS_TARGET),
                )
                response = await self._exchange(request, "obex-connect")
                if response.code != codec.SUCCESS:
                    raise RemoteError(response.code, "OBEX CONNECT rejected")
                if len(response.payload) < 4:
                    raise ProtocolError("truncated OBEX CONNECT response")
                peer_max = int.from_bytes(response.payload[2:4], "big")
                if peer_max < 255:
                    raise ProtocolError(f"invalid peer maximum packet size {peer_max}")
                connection_id = None
                who = None
                for header in codec.parse_headers(response.payload[4:]):
                    if header.identifier == codec.CONNECTION_ID and isinstance(header.value, int):
                        connection_id = header.value
                    elif header.identifier == codec.WHO and isinstance(header.value, bytes):
                        who = header.value
                if who is not None and who != SIEMENS_FFS_TARGET:
                    raise ProtocolError(f"unexpected OBEX Who header: {who.hex()}")
                self.info = ConnectionInfo(peer_max, connection_id, who, SIEMENS_FFS_TARGET)
                self._cwd = ()
                self.state = ConnectionState.READY
                return self.info
            except BaseException as exc:
                self._retire(str(exc))
                await self._close_transport()
                raise

    async def _ensure_connected(self) -> None:
        if self.info is None:
            raise SessionUnavailable("OBEX session is not connected; connect explicitly")
        if self._invalid:
            raise SessionUnavailable("OBEX session is invalid; reconnect explicitly")

    async def _setpath(self, name: str | None, *, backup: bool, create: bool) -> None:
        flags = (0x01 if backup else 0) | (0 if create else 0x02)
        headers = [self._connection_header()]
        if name is not None:
            headers.append(codec.unicode_header(codec.NAME, name))
        response = await self._exchange(
            codec.encode_packet(codec.SETPATH, bytes((flags, 0)), *headers), "obex-setpath"
        )
        if response.code != codec.SUCCESS:
            raise RemoteError(response.code, f"SETPATH rejected for {name!r}")

    async def _navigate(self, path: RemotePath) -> None:
        drive, *components = path.parts
        if drive.upper() != SIEMENS_FFS_ROOT:
            raise UnsupportedFilesystemRootError(drive)

        # The Siemens filesystem OBEX target opens directly at A:\.  "A:" is
        # an API-side absolute-path designator, not a child directory to send
        # in a SETPATH request.
        target = tuple(components)
        common = 0
        while common < len(self._cwd) and common < len(target) and self._cwd[common] == target[common]:
            common += 1
        for _ in range(len(self._cwd) - common):
            await self._setpath(None, backup=True, create=False)
            self._cwd = self._cwd[:-1]
        current = list(self._cwd[:common])
        for component in target[common:]:
            await self._setpath(component, backup=False, create=False)
            current.append(component)
            self._cwd = tuple(current)
        self._cwd = tuple(current)

    async def _emit(self, callback: ProgressCallback | None, progress: TransferProgress) -> None:
        if callback is not None:
            await callback(progress)

    async def _get(self, path: RemotePath, sink: DownloadSink, *, listing: bool,
                   progress: ProgressCallback | None = None) -> TransferResult:
        if listing:
            await self._navigate(path)
            headers = [self._connection_header(), codec.bytes_header(codec.TYPE, FOLDER_LISTING_TYPE)]
        else:
            parent = path.parent
            if parent is None:
                raise ValueError("cannot download a drive root")
            await self._navigate(parent)
            headers = [self._connection_header(), codec.unicode_header(codec.NAME, path.name)]
        total = None
        completed = 0
        await self._emit(progress, TransferProgress("download", path, 0, None, "started"))
        while True:
            response = await self._exchange(codec.encode_packet(codec.GET_FINAL, *headers), "obex-get")
            headers = [self._connection_header()]
            if response.code not in (codec.CONTINUE, codec.SUCCESS):
                raise RemoteError(response.code, f"GET rejected for {path}")
            final_body = False
            for header in codec.parse_headers(response.payload):
                if header.identifier == codec.LENGTH and isinstance(header.value, int):
                    total = header.value
                elif header.identifier in (codec.BODY, codec.END_BODY) and isinstance(header.value, bytes):
                    await sink.write(header.value)
                    completed += len(header.value)
                    final_body |= header.identifier == codec.END_BODY
                    await self._emit(progress, TransferProgress("download", path, completed, total, "running"))
            if response.code == codec.SUCCESS:
                break
            if final_body:
                raise ProtocolError("peer continued after End-of-Body")
        await self._emit(progress, TransferProgress("download", path, completed, total, "completed"))
        return TransferResult(path, completed)

    async def _list(self, path):
        from .streams import MemorySink
        sink = MemorySink()
        await self._get(path, sink, listing=True)
        return parse_folder_listing(bytes(sink.data), path)

    async def _find_entry(self, path):
        if path.parent is None:
            raise ValueError("operation requires a file or directory below the root")
        return next((entry for entry in await self._list(path.parent)
                     if entry.name == path.name), None)

    async def _changed(self, operation, path):
        if self.on_mutation is not None:
            await self.on_mutation(MutationEvent(operation, path))

    async def list(self, path: RemotePath) -> list[FileEntry]:
        async with self._operation():
            return await self._list(path)

    async def download(self, path: RemotePath, sink: DownloadSink, *,
                       progress: ProgressCallback | None = None,
                       metadata: Callable[[FileEntry], Awaitable[None]] | None = None) -> TransferResult:
        async with self._operation():
            entry = await self._find_entry(path)
            if entry is None or entry.kind != EntryKind.FILE:
                raise FileNotFound(f"file not found: {path}")
            if metadata is not None:
                await metadata(entry)
            return await self._get(path, sink, listing=False, progress=progress)

    async def upload(self, path: RemotePath, source: UploadSource, *,
                     progress: ProgressCallback | None = None,
                     confirmation_token: str | None = None) -> TransferResult:
        async with self._operation():
            check = self._overwrite_checks.pop(confirmation_token, None)
            existing = await self._find_entry(path)
            if existing is not None:
                if existing.kind != EntryKind.FILE:
                    raise PathConflict(path)
                signature = (self._generation, path, existing.kind, existing.size, existing.modified)
                if check != signature:
                    token = secrets.token_urlsafe(24)
                    # Bound abandoned confirmations in a long-lived session.
                    if len(self._overwrite_checks) >= 128:
                        self._overwrite_checks.pop(next(iter(self._overwrite_checks)))
                    self._overwrite_checks[token] = signature
                    raise OverwriteRequired(path, token)
                await self._delete(path)
                await self._changed("delete", path)
            result = await self._upload(path, source, progress=progress)
            await self._changed("upload", path)
            return result

    async def _upload(self, path, source, *, progress=None):
        assert self.info is not None
        parent = path.parent
        if parent is None:
            raise ValueError("cannot upload over a drive root")
        await self._navigate(parent)
        chunk_size = self.info.peer_max_packet - 64
        if chunk_size < 1:
            raise ProtocolError("negotiated OBEX packet is too small")
        completed = 0
        first = True
        pending = await source.read(chunk_size)
        await self._emit(progress, TransferProgress("upload", path, 0, source.size, "started"))
        while True:
            following = await source.read(chunk_size) if pending else b""
            final = not following
            headers = [self._connection_header()]
            if first:
                headers.append(codec.unicode_header(codec.NAME, path.name))
                if source.size is not None:
                    headers.append(codec.uint32_header(codec.LENGTH, source.size))
            headers.append(codec.bytes_header(codec.END_BODY if final else codec.BODY, pending))
            response = await self._exchange(
                codec.encode_packet(codec.PUT_FINAL if final else codec.PUT, *headers), "obex-put"
            )
            expected = codec.SUCCESS if final else codec.CONTINUE
            if response.code != expected:
                raise RemoteError(response.code, f"PUT rejected for {path}")
            completed += len(pending)
            await self._emit(progress, TransferProgress("upload", path, completed, source.size, "running"))
            if final:
                break
            pending, first = following, False
        await self._emit(progress, TransferProgress("upload", path, completed, source.size, "completed"))
        return TransferResult(path, completed)

    async def _delete(self, path: RemotePath) -> None:
        parent = path.parent
        if parent is None:
            raise ValueError("cannot delete a drive root")
        await self._navigate(parent)
        response = await self._exchange(codec.encode_packet(
            codec.PUT_FINAL, self._connection_header(), codec.unicode_header(codec.NAME, path.name)
        ), "obex-delete")
        if response.code != codec.SUCCESS:
            raise RemoteError(response.code, f"delete rejected for {path}")

    async def delete(self, path: RemotePath) -> None:
        async with self._operation():
            await self._delete(path)
            await self._changed("delete", path)

    async def rmdir(self, path: RemotePath) -> None:
        await self.delete(path)

    async def mkdir(self, path: RemotePath) -> None:
        async with self._operation():
            parent = path.parent
            if parent is None:
                raise ValueError("cannot create a drive root")
            await self._navigate(parent)
            await self._setpath(path.name, backup=False, create=True)
            self._cwd = path.parts[1:]
            await self._changed("mkdir", path)

    async def abort(self) -> None:
        await self.invalidate("session aborted")

    async def close(self) -> None:
        # Retire queued operations before waiting for the active transaction.
        healthy = not self._invalid
        self._retire("session closed")
        active = self._active
        if active is not None and active is not asyncio.current_task():
            healthy = False
            active.cancel()
        async with self._operation_lock:
            try:
                if self.info is not None and healthy:
                    try:
                        await self._exchange(codec.encode_packet(
                            codec.DISCONNECT, self._connection_header()), "obex-disconnect")
                        await asyncio.sleep(1.0)
                        await self.transport.write_all(b"+++", "serial-escape")
                    except Exception:
                        pass
            finally:
                await self._close_transport()
                self._invalid = False
                self.state = ConnectionState.DISCONNECTED
                self.error = None
