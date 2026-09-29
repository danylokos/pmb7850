"""OBEX byte transport fed only validated live bytes by the CEMU v13 bridge."""

from __future__ import annotations

import asyncio
from typing import Protocol

from tools.obex.errors import TransportError
from tools.obex.models import ClientConfig


class Asc0Bridge(Protocol):
    async def open_asc0(self, transport: "UiAsc0Transport") -> None: ...
    async def write_asc0(self, data: bytes) -> bool: ...
    async def close_asc0(self, transport: "UiAsc0Transport") -> None: ...


class UiAsc0Transport:
    """Serialized transport that does not expose RX until CEMU's READY barrier."""

    def __init__(self, config: ClientConfig, bridge: Asc0Bridge) -> None:
        self.config = config
        self.bridge = bridge
        self._buffer = bytearray()
        self._condition = asyncio.Condition()
        self._opened = False
        self._ready = False
        self._error: BaseException | None = None

    async def open(self) -> None:
        if self._opened:
            return
        self._opened = True
        self._ready = False
        self._error = None
        self._buffer.clear()
        try:
            await self.bridge.open_asc0(self)
        except BaseException:
            self._opened = False
            raise

    async def mark_ready(self) -> None:
        async with self._condition:
            self._ready = True
            self._condition.notify_all()

    async def feed(self, data: bytes) -> None:
        if not self._opened or not self._ready:
            return
        async with self._condition:
            self._buffer.extend(data)
            self._condition.notify_all()

    async def invalidate(self, reason: str) -> None:
        async with self._condition:
            self._ready = False
            self._error = TransportError(reason)
            self._condition.notify_all()

    async def write_all(self, data: bytes, phase: str) -> None:
        del phase
        if not self._opened or not self._ready:
            raise TransportError("ASC0 transport is not ready")
        if data and not await self.bridge.write_asc0(data):
            raise TransportError("CEMU disconnected during ASC0 write")

    async def read_exactly(self, length: int, phase: str,
                           timeout: float | None = None) -> bytes:
        deadline = self.config.timeout if timeout is None else timeout
        try:
            async with asyncio.timeout(deadline):
                async with self._condition:
                    while len(self._buffer) < length:
                        if self._error is not None:
                            raise self._error
                        if not self._opened or not self._ready:
                            raise TransportError("ASC0 transport is not ready")
                        await self._condition.wait()
                    result = bytes(self._buffer[:length])
                    del self._buffer[:length]
                    return result
        except TimeoutError:
            raise TimeoutError(f"timed out reading {phase}") from None

    async def close(self) -> None:
        if not self._opened:
            return
        self._opened = False
        self._ready = False
        await self.bridge.close_asc0(self)
        async with self._condition:
            self._buffer.clear()
            self._condition.notify_all()
