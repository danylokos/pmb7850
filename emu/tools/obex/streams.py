"""Async streaming interfaces and file adapters for CLI and HTTP transfers."""

from __future__ import annotations

import asyncio
from pathlib import Path
from typing import Protocol


class UploadSource(Protocol):
    size: int | None

    async def read(self, maximum: int) -> bytes: ...


class DownloadSink(Protocol):
    async def write(self, data: bytes) -> None: ...


class FileSource:
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.size = self.path.stat().st_size
        self._file = self.path.open("rb")

    async def read(self, maximum: int) -> bytes:
        return await asyncio.to_thread(self._file.read, maximum)

    def close(self) -> None:
        self._file.close()


class FileSink:
    def __init__(self, path: str | Path, *, force: bool = False) -> None:
        self.path = Path(path)
        mode = "wb" if force else "xb"
        self._file = self.path.open(mode)

    async def write(self, data: bytes) -> None:
        await asyncio.to_thread(self._file.write, data)

    def close(self) -> None:
        self._file.close()


class MemorySource:
    def __init__(self, data: bytes) -> None:
        self._data = memoryview(data)
        self._offset = 0
        self.size = len(data)

    async def read(self, maximum: int) -> bytes:
        data = bytes(self._data[self._offset:self._offset + maximum])
        self._offset += len(data)
        return data


class MemorySink:
    def __init__(self) -> None:
        self.data = bytearray()

    async def write(self, data: bytes) -> None:
        self.data.extend(data)
