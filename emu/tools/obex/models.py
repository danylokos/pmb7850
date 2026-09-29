"""Protocol-neutral filesystem value objects."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any

from .errors import UnsupportedFilesystemRootError


@dataclass(frozen=True)
class ClientConfig:
    port: str
    baud: int = 19200
    timeout: float = 40.0
    at_retries: int = 3
    wire_trace: str | None = None
    sqwe0_settle: float = 1.0


@dataclass(frozen=True)
class ConnectionInfo:
    peer_max_packet: int
    connection_id: int | None
    who: bytes | None
    target: bytes

    def to_dict(self) -> dict[str, Any]:
        return {
            "peer_max_packet": self.peer_max_packet,
            "connection_id": self.connection_id,
            "who": self.who.hex() if self.who is not None else None,
            "target": self.target.hex(),
        }


class EntryKind(str, Enum):
    FILE = "file"
    FOLDER = "folder"
    PARENT = "parent"


def _validate_path_text(value: str) -> None:
    if not isinstance(value, str) or len(value) > 512 or any(
            ord(char) < 32 or 0x7f <= ord(char) < 0xa0 for char in value):
        raise ValueError("invalid remote path")


@dataclass(frozen=True)
class RemotePath:
    value: str

    def __post_init__(self) -> None:
        _validate_path_text(self.value)
        normalized = self.value.replace("/", "\\")
        parts = [part for part in normalized.split("\\") if part]
        if not parts or not parts[0].endswith(":"):
            raise ValueError("remote path must be absolute, for example A:\\\\Pictures")
        if any(part in (".", "..") for part in parts):
            raise ValueError("remote path cannot contain . or .. components")
        if parts[0].upper() != "A:":
            raise UnsupportedFilesystemRootError(parts[0])
        if any(":" in part for part in parts[1:]):
            raise ValueError("invalid remote path component")
        parts[0] = "A:"
        object.__setattr__(self, "value", "\\".join(parts))

    @property
    def parts(self) -> tuple[str, ...]:
        return tuple(self.value.split("\\"))

    @property
    def name(self) -> str:
        return self.parts[-1]

    @property
    def parent(self) -> "RemotePath | None":
        parts = self.parts
        return RemotePath("\\".join(parts[:-1])) if len(parts) > 1 else None

    def child(self, name: str) -> "RemotePath":
        if not name or "\\" in name or "/" in name or name in (".", ".."):
            raise ValueError("invalid remote child name")
        return RemotePath(f"{self.value}\\{name}")

    def __str__(self) -> str:
        return self.value


@dataclass(frozen=True)
class FileEntry:
    name: str
    path: RemotePath
    kind: EntryKind
    size: int | None = None
    modified: str | None = None
    attributes: dict[str, str] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "path": str(self.path),
            "kind": self.kind.value,
            "size": self.size,
            "modified": self.modified,
            "attributes": dict(self.attributes),
        }


@dataclass(frozen=True)
class TransferProgress:
    operation: str
    path: RemotePath
    completed: int
    total: int | None
    state: str


@dataclass(frozen=True)
class TransferResult:
    path: RemotePath
    bytes_transferred: int


def resolve_remote_path(value: str, cwd: RemotePath) -> RemotePath:
    _validate_path_text(value)
    normalized = value.replace("/", "\\")
    incoming = [part for part in normalized.split("\\") if part]
    if incoming and incoming[0].endswith(":"):
        parts = [incoming.pop(0)]
    elif normalized.startswith("\\"):
        parts = [cwd.parts[0]]
    else:
        parts = list(cwd.parts)

    for part in incoming:
        if part == ".":
            continue
        if part == "..":
            if len(parts) == 1:
                raise ValueError("remote path cannot traverse above the drive root")
            parts.pop()
            continue
        if ":" in part:
            raise ValueError(f"invalid remote path component {part!r}")
        parts.append(part)
    return RemotePath("\\".join(parts))



class ConnectionState(str, Enum):
    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    READY = "ready"
    ERROR = "error"


@dataclass(frozen=True)
class MutationEvent:
    operation: str
    path: RemotePath
