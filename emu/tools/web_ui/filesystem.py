"""HTTP streams, transfer controls and browser delivery for tools.obex."""

from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from dataclasses import asdict
from typing import Any
from urllib.parse import quote

from aiohttp import web

from tools.obex import ClientConfig, ObexFilesystemClient, RemotePath
from tools.obex.models import MutationEvent

from .asc0 import UiAsc0Transport


def remote_path(value: str | None) -> RemotePath:
    try:
        return RemotePath(value)
    except ValueError as exc:
        raise web.HTTPBadRequest(text=f"{exc}\n") from None


class RequestSource:
    def __init__(self, request: web.Request) -> None:
        self.request = request
        self.size = request.content_length

    async def read(self, maximum: int) -> bytes:
        return await self.request.content.read(maximum)


class ResponseSink:
    def __init__(self, response: web.StreamResponse) -> None:
        self.response = response

    async def write(self, data: bytes) -> None:
        await self.response.write(data)


class FilesystemService:
    """Adapts a shared client; native attachment identity stays in this layer."""

    def __init__(self, bridge: Any, *, timeout: float = 40.0,
                 client_factory=None) -> None:
        self.bridge = bridge
        self.config = ClientConfig("embedded-asc0", timeout=timeout)
        self.client_factory = client_factory
        self.client: ObexFilesystemClient | None = None
        self._transfers = {}

    def status(self) -> dict[str, Any]:
        return {"state": self.client.state.value if self.client else "disconnected",
                "error": self.client.error if self.client else None}

    def _get_client(self) -> ObexFilesystemClient:
        if self.client is None:
            self.client = (self.client_factory() if self.client_factory else
                           ObexFilesystemClient(self.config, transport=UiAsc0Transport(
                               self.config, self.bridge)))
            self.client.on_mutation = self.changed
        return self.client

    async def connect(self, *, retry: bool) -> dict[str, Any]:
        await self._get_client().connect(retry=retry)
        return self.status()

    async def invalidate(self, reason: str) -> None:
        if self.client is not None:
            await self.client.invalidate(reason)
        self._transfers.clear()

    async def close(self) -> None:
        if self.client is not None:
            await self.client.close()
        self._transfers.clear()

    async def list(self, path: RemotePath):
        return await self._get_client().list(path)

    @asynccontextmanager
    async def transfer(self, request, path, operation):
        identifier = request.query.get("transfer")
        owner = request.headers.get("X-CEMU-Tab", request.query.get("tab"))
        record = None
        if identifier:
            if not owner or len(owner) > 128 or len(identifier) > 128:
                raise web.HTTPBadRequest(text="invalid transfer identity\n")
            if identifier in self._transfers:
                raise web.HTTPConflict(text="transfer identifier already used\n")
            if len(self._transfers) >= 128:
                retired = next((key for key, value in self._transfers.items()
                                if value["task"] is None), None)
                if retired is None:
                    raise web.HTTPServiceUnavailable(text="too many transfers\n")
                del self._transfers[retired]
            record = {"owner": owner, "attachment": self.bridge.identifiers(),
                      "task": asyncio.current_task(), "progress": {
                          "operation": operation, "path": str(path), "completed": 0,
                          "total": None, "state": "queued"}}
            self._transfers[identifier] = record

        async def progress(value):
            if record is not None:
                # One latest value, sampled by the initiating tab at 4 Hz.
                record["progress"] = {**asdict(value), "path": str(value.path)}

        try:
            yield progress
        except asyncio.CancelledError:
            if record is not None:
                record["progress"]["state"] = "cancelled"
            raise
        except BaseException:
            if record is not None:
                record["progress"]["state"] = "error"
            raise
        finally:
            if record is not None:
                record["task"] = None

    def transfer_record(self, request):
        record = self._transfers.get(request.match_info["transfer"])
        if (record is None or record["owner"] != request.headers.get("X-CEMU-Tab") or
                record["attachment"] != self.bridge.identifiers()):
            raise web.HTTPNotFound(text="transfer not found\n")
        return record

    async def transfer_status(self, request):
        return web.json_response(self.transfer_record(request)["progress"])

    async def cancel_transfer(self, request):
        record = self.transfer_record(request)
        if record["task"] is not None:
            record["task"].cancel()
        return web.json_response({"cancelled": True})

    async def upload(self, request: web.Request, path: RemotePath) -> dict[str, Any]:
        async with self.transfer(request, path, "upload") as progress:
            result = await self._get_client().upload(
                path, RequestSource(request), progress=progress,
                confirmation_token=request.headers.get("X-CEMU-Overwrite"))
        return {"path": str(result.path), "bytes_transferred": result.bytes_transferred}

    async def download(self, request: web.Request, path: RemotePath) -> web.StreamResponse:
        response = web.StreamResponse(headers={
            "Content-Disposition": "attachment; filename*=UTF-8''" + quote(path.name, safe=""),
            "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff",
            "Content-Security-Policy": "default-src 'none'; frame-ancestors 'none'",
        })
        response.content_type = "application/octet-stream"

        async def metadata(entry):
            if request.get("attachment") != self.bridge.identifiers():
                raise web.HTTPConflict(text="retired runtime attachment\n")
            if entry.size is not None:
                response.content_length = entry.size
            await response.prepare(request)

        async with self.transfer(request, path, "download") as progress:
            await self._get_client().download(path, ResponseSink(response),
                                              metadata=metadata, progress=progress)
            await response.write_eof()
        return response

    async def mkdir(self, path: RemotePath) -> None:
        await self._get_client().mkdir(path)

    async def delete(self, path: RemotePath) -> None:
        await self._get_client().delete(path)

    async def changed(self, event: MutationEvent) -> None:
        message = {"type": "files_changed", "operation": event.operation, "path": str(event.path)}
        for browser in tuple(self.bridge.clients):
            browser.enqueue_json(message)
