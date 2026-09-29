"""In-memory protocol hooks exercising the real client's filesystem policy."""

from tools.obex.client import ObexFilesystemClient
from tools.obex.models import ClientConfig, EntryKind, FileEntry, TransferProgress, TransferResult
from .test_obex import ScriptedTransport


class MemoryClient(ObexFilesystemClient):
    def __init__(self, files=None, *, fail_upload=False):
        super().__init__(ClientConfig("fake"), transport=ScriptedTransport())
        self.files = files if files is not None else {}
        self.fail_upload = fail_upload
        self.read_sizes = []
        self.effects = []

    async def _enter_obex_mode(self):
        pass

    async def _list(self, path):
        self.effects.append(("list", path))
        return [entry for entry in self.files.values() if entry.path.parent == path]

    async def _upload(self, path, source, *, progress=None):
        self.effects.append(("put", path))
        data = bytearray()
        await self._emit(progress, TransferProgress("upload", path, 0, source.size, "started"))
        while True:
            chunk = await source.read(3)
            self.read_sizes.append(len(chunk))
            if not chunk:
                break
            data.extend(chunk)
            if self.fail_upload:
                raise ConnectionResetError("browser upload disconnected")
            await self._emit(progress, TransferProgress("upload", path, len(data), source.size, "running"))
        self.files[str(path)] = FileEntry(
            path.name, path, EntryKind.FILE, len(data), attributes={"data": bytes(data).hex()})
        await self._emit(progress, TransferProgress("upload", path, len(data), source.size, "completed"))
        return TransferResult(path, len(data))

    async def _get(self, path, sink, *, listing, progress=None):
        data = bytes.fromhex(self.files[str(path)].attributes["data"])
        for offset in range(0, len(data), 2):
            await sink.write(data[offset:offset + 2])
            await self._emit(progress, TransferProgress("download", path, min(offset + 2, len(data)), len(data), "running"))
        await self._emit(progress, TransferProgress("download", path, len(data), len(data), "completed"))
        return TransferResult(path, len(data))

    async def _setpath(self, name, *, backup, create):
        if create:
            from tools.obex.models import RemotePath
            path = RemotePath("\\".join(("A:", *self._cwd, name)))
            self.files[str(path)] = FileEntry(path.name, path, EntryKind.FOLDER)

    async def _delete(self, path):
        self.effects.append(("delete", path))
        del self.files[str(path)]
