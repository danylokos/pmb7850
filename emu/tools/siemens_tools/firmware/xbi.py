#!/usr/bin/env python3
"""Inspect, unpack, and convert Siemens EGOLD/SGOLD firmware packages.

This is a standalone Python port of the useful behavior in
``@sie-js/siemens-fw-tool`` and ``@sie-js/fw``.  It understands Siemens
service/update executables and the XBI family (XBI, XBZ, XFS, XCI, XBB,
EXCI, and EXBI).

The source behavior ported from siemens-mobile-hacks/node-sie-fw and
siemens-mobile-hacks/siemens-fw-tool is MIT licensed:

Copyright (c) 2024 Kirill Zhumarin <kirill.zhumarin@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Any, Callable


SAG_UDT = b"SAG_UDT"
SAG_JK_WH = b"SAG_JK_WH"
SAG_JK_TRAILER_SIZE = len(SAG_JK_WH) + 4


class FirmwareError(Exception):
    """A malformed or unsupported firmware package."""


ASSEMBLY_SCHEMA = "cemu.siemens-fullflash-assembly"
ASSEMBLY_SCHEMA_VERSION = 2
C55_EEPROM_BANK_SIZE = 0x20000
C55_EEPROM_REGION_SIZE = 0x60000
C55_EEPROM_LINEAR_BASE = 0xFA0000
C55_EEPROM_HEADER_OFFSET = 0x10
C55_EEPROM_DATA_OFFSET = 0x20
C55_FLASH_PROFILE_RANGES = (
    (0x07FE26, 0x07FE2A, "M58 manufacturer/device identification words",
     bytes.fromhex("20 00 17 00"), "profile:c55:m58-identification"),
    (0x07FFFC, 0x080000, "low-flash overlay entry trampoline",
     bytes.fromhex("FA 07 F0 FF"), "profile:c55:low-flash-overlay-entry"),
)


@dataclass(frozen=True)
class LayoutRegion:
    name: str
    offset: int
    length: int

    @property
    def end(self) -> int:
        return self.offset + self.length


@dataclass(frozen=True)
class LayoutChip:
    role: str
    offset: int
    length: int
    owns_reset: bool = False
    owns_metadata: bool = False

    @property
    def end(self) -> int:
        return self.offset + self.length


@dataclass(frozen=True)
class FlashLayout:
    name: str
    base: int
    length: int
    regions: tuple[LayoutRegion, ...]
    reset_offset: int | None = None
    partition_boundaries: tuple[int, ...] = ()
    chips: tuple[LayoutChip, ...] = ()
    statistic_offset: int | None = None
    entry_transfer_offset: int | None = None

    def region(self, name: str) -> LayoutRegion:
        matches = [region for region in self.regions if region.name == name]
        if len(matches) != 1:
            raise FirmwareError(
                f"layout {self.name!r} needs exactly one {name!r} region"
            )
        return matches[0]


@dataclass(frozen=True)
class MaterializedXbi:
    flash: bytes
    write_mask: bytes
    erase_mask: bytes


@dataclass(frozen=True)
class XbiFormat:
    version: int
    signed: bool
    offset: int
    signature_size: int
    key: bytes


@dataclass(frozen=True)
class XbiWrite:
    addr: int
    size: int
    offset: int


@dataclass
class XbiInfo:
    format_version: int
    signed: bool
    signature_size: int
    size: int
    fields: dict[str, Any]
    writes: list[XbiWrite] = field(default_factory=list)

    def get(self, name: str, default: Any = None) -> Any:
        return self.fields.get(name, default)


@dataclass(frozen=True)
class XbiFormatSpec:
    signature_id: bytes
    software_id: bytes
    key: bytes
    version: int


XBI_FORMATS = (
    XbiFormatSpec(
        b"Siemens Mobile Phones:SIGNATURE:01.00",
        b"Siemens Mobile Phones:SOFTWARE:01.00",
        (b"Siemens Mobile Phones:SOFTWARE:01.00\0")[::-1],
        32,
    ),
    XbiFormatSpec(
        b"Siemens Mobile Phones Signature File",
        b"Siemens Mobile Phones Software",
        (b"Siemens Mobile Phones Software\0")[::-1],
        24,
    ),
)

CPU_NAMES = {
    0: "HighGold Vxx ... V3.6",
    1: "HighGold V4 C7-Technology",
    2: "HighGold V4 C9-Technology",
    3: "EGOLD V1 ... 1.2",
    4: "EGOLD V2",
    5: "EGOLD Plus V1.2",
    6: "EGOLD Plus V3",
    7: "SGold-Lite",
    8: "SGold",
    0x80: "TI Hercules-Chipset",
}

UPDATE_TYPES = {
    0: "MobSw",
    1: "Eesimu",
    2: "VoiceMemo",
    3: "CodeOnly",
    4: "LangOnly",
    5: "CodeAndLang",
    6: "DiffFile",
    7: "ExtendedNewSplit",
}

HEADER_FIELDS: dict[int, tuple[str, str]] = {
    0x11: ("min_winswup_version", "version"),
    0x12: ("reconfigure_time", "str"),
    0x13: ("link_time", "str"),
    0x16: ("release_type", "str"),
    0x17: ("product_code", "str"),
    0x1A: ("langpack", "str"),
    0x1D: ("svn", "svn"),
    0x23: ("flash_size", "u32be"),
    0x24: ("ertec_sum", "u16be"),
    0x25: ("statistic_addr", "u32be"),
    0x28: ("model", "str"),
    0x29: ("vendor", "str"),
    0x2A: ("baseline", "str"),
    0x30: ("erase_regions[]", "region"),
    0x31: ("asic_type", "u8"),
    0x32: ("flash_write_type", "u8"),
    0x33: ("ram_size", "ram_size"),
    0x34: ("cpu_type", "u8"),
    0x35: ("ignition_type", "u8"),
    0x37: ("split_info", "split_info"),
    0x38: ("align", "u16be"),
    0x39: ("compression_type", "u8"),
    0x3A: ("compression_info", "compression_info"),
    0x40: ("update_type", "update_type"),
    0x50: ("map_info_size", "u16be"),
    0x51: ("map_info[]", "bytes"),
    0x56: ("hash_area_size", "u16le"),
    0x60: ("t9", "u8"),
    0x61: ("database_name", "str"),
    0x62: ("baseline_version", "str"),
    0x63: ("baseline_release", "str"),
    0x64: ("operator_product_name", "plain_str"),
    0x70: ("dll", "plain_str"),
}

SECONDARY_FIELDS: dict[int, tuple[str, str]] = {
    0x5C: ("data_flash[]", "region"),
}

SERVICE_EXE_VERSIONS = (
    (b"Siemens Mobile Phones Software", 1),
    (b"Siemens Mobile Phones Signature File", 1),
    (b"Siemens Mobile Phones:SOFTWARE:01.00", 2),
    (b"Siemens Mobile Phones:SIGNATURE:01.00", 2),
)


def _need(data: bytes | bytearray | memoryview, offset: int, size: int,
          what: str) -> None:
    if offset < 0 or size < 0 or offset + size > len(data):
        raise FirmwareError(
            f"truncated {what}: need {size} byte(s) at 0x{offset:x}, "
            f"file size is 0x{len(data):x}"
        )


def _u16be(data: bytes, offset: int = 0) -> int:
    _need(data, offset, 2, "uint16")
    return struct.unpack_from(">H", data, offset)[0]


def _u16le(data: bytes, offset: int = 0) -> int:
    _need(data, offset, 2, "uint16")
    return struct.unpack_from("<H", data, offset)[0]


def _u32be(data: bytes, offset: int = 0) -> int:
    _need(data, offset, 4, "uint32")
    return struct.unpack_from(">I", data, offset)[0]


def _u32le(data: bytes, offset: int = 0) -> int:
    _need(data, offset, 4, "uint32")
    return struct.unpack_from("<I", data, offset)[0]


def _xor_checksum(data: bytes | bytearray | memoryview) -> int:
    value = 0
    for byte in data:
        value ^= byte
    return value


def detect_xbi_format(data: bytes) -> XbiFormat | None:
    for spec in XBI_FORMATS:
        if data.startswith(spec.signature_id):
            software_offset = data.find(spec.software_id)
            if software_offset >= 0:
                return XbiFormat(
                    spec.version,
                    True,
                    software_offset + len(spec.software_id) + 1,
                    software_offset,
                    spec.key,
                )
        if data.startswith(spec.software_id):
            return XbiFormat(
                spec.version,
                False,
                len(spec.software_id) + 1,
                0,
                spec.key,
            )
    return None


def is_xbi(data: bytes) -> bool:
    return detect_xbi_format(data) is not None


def _frame_magic(frame_type: int, version: int) -> bytes:
    if version == 24:
        return bytes((0xFF, 0xFF, frame_type))
    if version == 32:
        return bytes((0xFF, 0xFF, 0xFF, frame_type))
    raise FirmwareError(f"unsupported XBI frame version: {version}")


def _decode_info_frame(data: bytes, offset: int, frame_type: int,
                       version: int) -> tuple[int, int, bytes]:
    magic = _frame_magic(frame_type, version)
    _need(data, offset, len(magic), "XBI frame magic")
    if data[offset:offset + len(magic)] != magic:
        raise FirmwareError(
            f"invalid 0x{frame_type:02x} XBI frame at 0x{offset:x}"
        )
    if version == 24:
        _need(data, offset, 5, "24-bit XBI frame header")
        value_size = data[offset + 3]
        if value_size < 1:
            raise FirmwareError(f"invalid XBI frame size at 0x{offset:x}")
        total = 5 + value_size
        checksum_offset = offset + 4 + value_size
        cmd_offset = offset + 4
        value_offset = offset + 5
    else:
        _need(data, offset, 7, "32-bit XBI frame header")
        value_size = _u16be(data, offset + 4)
        if value_size < 1:
            raise FirmwareError(f"invalid XBI frame size at 0x{offset:x}")
        total = 7 + value_size
        checksum_offset = offset + 6 + value_size
        cmd_offset = offset + 6
        value_offset = offset + 7
    _need(data, offset, total, "XBI frame")
    expected = data[checksum_offset]
    actual = _xor_checksum(data[offset:checksum_offset])
    if expected != actual:
        raise FirmwareError(
            f"invalid XBI frame checksum at 0x{offset:x}: "
            f"0x{expected:02x} != 0x{actual:02x}"
        )
    return total, data[cmd_offset], data[value_offset:checksum_offset]


def _is_info_frame(data: bytes, offset: int, frame_type: int,
                   version: int) -> bool:
    magic = _frame_magic(frame_type, version)
    return data[offset:offset + len(magic)] == magic


def _decode_write_frame(data: bytes, offset: int,
                        version: int) -> tuple[int, XbiWrite]:
    if version == 24:
        _need(data, offset, 5, "24-bit XBI write header")
        addr = int.from_bytes(data[offset:offset + 3], "big")
        size = data[offset + 3]
        payload_offset = offset + 4
    elif version == 32:
        _need(data, offset, 7, "32-bit XBI write header")
        addr = _u32be(data, offset)
        size = _u16be(data, offset + 4)
        payload_offset = offset + 6
    else:
        raise FirmwareError(f"unsupported XBI write version: {version}")
    checksum_offset = payload_offset + size
    _need(data, offset, checksum_offset - offset + 1, "XBI write frame")
    expected = data[checksum_offset]
    actual = _xor_checksum(data[offset:checksum_offset])
    if expected != actual:
        raise FirmwareError(
            f"invalid XBI write checksum at 0x{offset:x} "
            f"(address 0x{addr:08x}): 0x{expected:02x} != 0x{actual:02x}"
        )
    total = checksum_offset - offset + 1
    return total, XbiWrite(addr, size, payload_offset)


def _decode_text(value: bytes) -> str:
    return value.decode("utf-8", errors="replace")


def _decode_field(kind: str, key: bytes, value: bytes) -> Any:
    if kind == "str":
        plain = bytes(byte ^ key[i % len(key)] for i, byte in enumerate(value))
        return _decode_text(plain)
    if kind == "plain_str":
        return _decode_text(value)
    if kind == "u8":
        _need(value, 0, 1, "uint8 field")
        return value[0]
    if kind == "u16le":
        return _u16le(value)
    if kind == "u16be":
        return _u16be(value)
    if kind == "u32le":
        return _u32le(value)
    if kind == "u32be":
        return _u32be(value)
    if kind == "svn":
        raw = f"{_u16le(value):04x}"
        try:
            return int(raw, 10) / 100
        except ValueError as exc:
            raise FirmwareError(f"invalid BCD SVN value: {raw}") from exc
    if kind == "update_type":
        _need(value, 0, 1, "update type")
        return UPDATE_TYPES.get(value[0], f"unknown_{value[0]}")
    if kind == "region":
        return {"from": _u32be(value), "to": _u32be(value, 4)}
    if kind == "split_info":
        return {"addr": _u32be(value), "id": _u32be(value, 4)}
    if kind == "bytes":
        return value
    if kind == "ram_size":
        return _u16be(value) * 1024
    if kind == "compression_info":
        _need(value, 0, 12, "compression info")
        return {
            "algorithm": _u16be(value),
            "compression_ratio": _u16be(value, 2),
            "from_format": value[4],
            "to_format": value[5],
            "additional_info": [
                _u16be(value, 6),
                _u16be(value, 8),
                _u16be(value, 10),
            ],
        }
    if kind == "version":
        _need(value, 0, 2, "version field")
        return value[1] + value[0] * 100
    raise FirmwareError(f"unsupported XBI field type: {kind}")


def _store_field(fields: dict[str, Any], spec: tuple[str, str], key: bytes,
                 value: bytes) -> None:
    name, kind = spec
    decoded = _decode_field(kind, key, value)
    if name.endswith("[]"):
        fields.setdefault(name[:-2], []).append(decoded)
    else:
        fields[name] = decoded


def parse_xbi(data: bytes) -> XbiInfo:
    fmt = detect_xbi_format(data)
    if fmt is None:
        raise FirmwareError("unknown XBI format")

    logical_size = len(data)
    if (fmt.version == 24 and data.endswith(SAG_JK_WH)
            and len(data) >= SAG_JK_TRAILER_SIZE):
        trailer_offset = len(data) - SAG_JK_TRAILER_SIZE
        embedded_size = _u32be(data, trailer_offset) + SAG_JK_TRAILER_SIZE
        if embedded_size == len(data):
            logical_size -= SAG_JK_TRAILER_SIZE
    logical = data[:logical_size]
    if fmt.offset > logical_size:
        raise FirmwareError("XBI software header points beyond end of file")

    fields: dict[str, Any] = {"compression_type": 0}
    offset = fmt.offset
    saw_eof = False
    while offset < logical_size:
        total, cmd, value = _decode_info_frame(
            logical, offset, 0xFE, fmt.version
        )
        offset += total
        if cmd == 0x04:
            saw_eof = True
            break
        spec = HEADER_FIELDS.get(cmd)
        if spec:
            _store_field(fields, spec, fmt.key, value)
    if not saw_eof:
        raise FirmwareError("XBI header has no EOF frame")

    hash_area_size = fields.get("hash_area_size", 0)
    if hash_area_size:
        _need(logical, offset, hash_area_size, "XBI hash area")
        offset += hash_area_size

    while offset < logical_size and _is_info_frame(
        logical, offset, 0xFF, fmt.version
    ):
        total, cmd, value = _decode_info_frame(
            logical, offset, 0xFF, fmt.version
        )
        offset += total
        spec = SECONDARY_FIELDS.get(cmd)
        if spec:
            _store_field(fields, spec, fmt.key, value)

    writes: list[XbiWrite] = []
    while offset < logical_size:
        total, write = _decode_write_frame(logical, offset, fmt.version)
        writes.append(write)
        offset += total

    cpu_type = fields.get("cpu_type")
    if cpu_type in CPU_NAMES:
        fields["cpu_type_name"] = CPU_NAMES[cpu_type]
    map_info = fields.get("map_info")
    if map_info and len(map_info[0]) >= 4:
        fields["hwid"] = _u16le(map_info[0], 2)

    return XbiInfo(
        format_version=fmt.version,
        signed=fmt.signed,
        signature_size=fmt.signature_size,
        size=logical_size,
        fields=fields,
        writes=writes,
    )


def firmware_extension(info: XbiInfo) -> str:
    if info.get("update_type") == "ExtendedNewSplit":
        return "xfs"
    if info.get("update_type") == "CodeOnly":
        return "xci"
    if info.get("database_name") == "klf_bootcore":
        return "xbb"
    return "xbi" if info.get("compression_type", 0) == 0 else "xbz"
