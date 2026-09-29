"""Siemens OBEX folder-listing XML decoding."""

from __future__ import annotations

import xml.etree.ElementTree as ET

from .errors import ProtocolError
from .models import EntryKind, FileEntry, RemotePath

_SIEMENS_UTF8_MARKER = b'name="\x1f'
_MARKER_TOKEN = "__SIEMENS_UTF8__"


def _decode_name(name: str) -> str:
    if not name.startswith(_MARKER_TOKEN):
        return name
    encoded = name[len(_MARKER_TOKEN):]
    try:
        return encoded.encode("latin-1").decode("utf-8")
    except (UnicodeEncodeError, UnicodeDecodeError) as exc:
        raise ProtocolError(f"invalid Siemens encoded folder-listing name: {encoded!r}") from exc


def parse_folder_listing(data: bytes, parent: RemotePath) -> list[FileEntry]:
    # C55 SW24 prefixes selected non-ASCII names with the otherwise illegal XML
    # control byte 0x1f, then represents the UTF-8 bytes through an additional
    # Latin-1-to-UTF-8 pass. Replace only that measured attribute prefix before
    # handing bytes to ElementTree, which remains responsible for BOM and XML
    # encoding-declaration handling.
    normalized = data.replace(
        _SIEMENS_UTF8_MARKER,
        b'name="' + _MARKER_TOKEN.encode("ascii"),
    )
    try:
        root = ET.fromstring(normalized)
    except ET.ParseError as exc:
        raise ProtocolError(f"invalid folder listing XML: {exc}") from exc
    result: list[FileEntry] = []
    for element in root:
        tag = element.tag.rsplit("}", 1)[-1].lower()
        if tag not in ("file", "folder", "parent-folder"):
            continue
        attrs = {str(key): str(value) for key, value in element.attrib.items()}
        name = _decode_name(attrs.get("name", ".." if tag == "parent-folder" else ""))
        if not name:
            continue
        if "name" in attrs:
            attrs["name"] = name
        kind = {
            "file": EntryKind.FILE,
            "folder": EntryKind.FOLDER,
            "parent-folder": EntryKind.PARENT,
        }[tag]
        if kind is EntryKind.PARENT:
            path = parent.parent or parent
        else:
            path = parent.child(name)
        raw_size = attrs.get("size")
        try:
            size = int(raw_size) if raw_size is not None else None
        except ValueError:
            size = None
        modified = attrs.get("modified") or attrs.get("created")
        result.append(FileEntry(name, path, kind, size, modified, attrs))
    return result
