#!/usr/bin/env python3
"""Unit and CLI tests for tools.siemens_tools.firmware."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path
from typing import Any

from Crypto.Cipher import AES
from Crypto.Util.Padding import pad

from tools.siemens_tools import eeprom
from tools.siemens_tools import firmware as fw
from tools.siemens_tools import fullflash as ff
from tools.siemens_tools import layout as lt
from tools.siemens_tools.firmware.cli import main as firmware_main
from tools.siemens_tools.fullflash.cli import main as fullflash_main
from tools.siemens_tools.firmware.inspection import (
    _select_convert_payload as select_convert_payload,
)
from tools.siemens_tools.fullflash.ranges import _true_ranges as true_ranges


SPECS = {
    24: {
        "signature": b"Siemens Mobile Phones Signature File",
        "software": b"Siemens Mobile Phones Software",
        "key": b"Siemens Mobile Phones Software\0"[::-1],
    },
    32: {
        "signature": b"Siemens Mobile Phones:SIGNATURE:01.00",
        "software": b"Siemens Mobile Phones:SOFTWARE:01.00",
        "key": b"Siemens Mobile Phones:SOFTWARE:01.00\0"[::-1],
    },
}


def xor_checksum(data: bytes) -> int:
    value = 0
    for byte in data:
        value ^= byte
    return value


def encrypt_field(version: int, value: bytes) -> bytes:
    key = SPECS[version]["key"]
    return bytes(byte ^ key[index % len(key)]
                 for index, byte in enumerate(value))


def info_frame(version: int, frame_type: int, command: int,
               value: bytes = b"") -> bytes:
    size = len(value) + 1
    if version == 24:
        prefix = bytes((0xFF, 0xFF, frame_type, size, command)) + value
    else:
        prefix = (bytes((0xFF, 0xFF, 0xFF, frame_type))
                  + struct.pack(">H", size) + bytes((command,)) + value)
    return prefix + bytes((xor_checksum(prefix),))


def write_frame(version: int, address: int, payload: bytes) -> bytes:
    if version == 24:
        if len(payload) > 255:
            raise ValueError("24-bit test frame is too large")
        prefix = address.to_bytes(3, "big") + bytes((len(payload),)) + payload
    else:
        prefix = struct.pack(">IH", address, len(payload)) + payload
    return prefix + bytes((xor_checksum(prefix),))


def bit_pack(bits: list[int]) -> bytes:
    while len(bits) % 8:
        bits.append(0)
    output = bytearray()
    for offset in range(0, len(bits), 8):
        value = 0
        for bit in bits[offset:offset + 8]:
            value = (value << 1) | bit
        output.append(value)
    return bytes(output)


def literal_lzss(data: bytes, add_partial_backref: bool = False) -> bytes:
    bits: list[int] = []
    for byte in data:
        bits.append(1)
        bits.extend((byte >> shift) & 1 for shift in range(7, -1, -1))
    if add_partial_backref:
        # Selector + 12-bit pointer. For a 15-byte transport stream this leaves
        # four zero pad bits, completing a two-byte back-reference that must be
        # rejected as final-byte padding rather than as a real transport frame.
        bits.extend([0] * 13)
    return bit_pack(bits)


def transport_stream(address: int, payload: bytes) -> bytes:
    address_prefix = bytes((0x80, 0xFF)) + struct.pack(">I", address)
    address_frame = address_prefix + bytes((xor_checksum(address_prefix),))
    data_prefix = bytes((len(payload),)) + payload
    data_frame = data_prefix + bytes((xor_checksum(data_prefix),))
    return address_frame + data_frame


def build_xbi(*, version: int = 24, signed: bool = False,
              compression_type: int = 0, flash_size: int = 0x100,
              writes: list[tuple[int, bytes]] | None = None,
              compressed_stream: bytes | None = None,
              model: bytes = b"T55", langpack: bytes = b"lg1",
              svn: int = 24, t9: int | None = 1,
              update_type: int = 0,
              erase_regions: list[tuple[int, int]] | None = None) -> bytes:
    spec = SPECS[version]
    if signed:
        result = bytearray(spec["signature"] + b"\0signature\0"
                           + spec["software"] + b"\0")
    else:
        result = bytearray(spec["software"] + b"\0")
    erase_regions = erase_regions or [(0x10, 0x7F)]
    fields = [
        (0x23, struct.pack(">I", flash_size)),
        (0x39, bytes((compression_type,))),
        (0x28, encrypt_field(version, model)),
        (0x1A, encrypt_field(version, langpack)),
        (0x1D, bytes((svn // 100, ((svn // 10) % 10) << 4 | svn % 10))),
        *((0x30, struct.pack(">II", start, end))
          for start, end in erase_regions),
        (0x37, struct.pack(">II", 0x20, 12345)),
        (0x51, b"\x00\x00\x82\x00"),
        *(((0x60, bytes((t9,))),) if t9 is not None else ()),
        (0x34, b"\x06"),
        (0x40, bytes((update_type,))),
    ]
    for command, value in fields:
        result += info_frame(version, 0xFE, command, value)
    result += info_frame(version, 0xFE, 0x04)
    result += info_frame(
        version, 0xFF, 0x5C, struct.pack(">II", 0, flash_size - 1)
    )
    if compressed_stream is not None:
        cursor = 0
        chunk_size = 255 if version == 24 else 1024
        while cursor < len(compressed_stream):
            chunk = compressed_stream[cursor:cursor + chunk_size]
            result += write_frame(version, cursor, chunk)
            cursor += len(chunk)
    else:
        for address, payload in writes or []:
            result += write_frame(version, address, payload)
    return bytes(result)


def size_bits(size: int) -> bytes:
    return bytes(0x80 if size & (1 << bit) else 0 for bit in range(32))


def xor_payload(payload: bytes, key: bytes) -> bytes:
    return bytes(byte ^ key[index % len(key)]
                 for index, byte in enumerate(payload))


def service_v1(payload: bytes) -> bytes:
    key = bytearray(b"MZ" + bytes(range(2, 64)))
    encrypted = xor_payload(payload, key)
    metadata = bytearray(108)
    metadata[:32] = size_bits(len(payload))
    marker = b"Siemens Mobile Phones Software"
    marker_end = len(metadata) - len(fw.SAG_JK_WH) - 5
    metadata[marker_end - len(marker):marker_end] = marker
    metadata[-len(fw.SAG_JK_WH):] = fw.SAG_JK_WH
    return bytes(key + encrypted + metadata)


def legacy_service(payload: bytes) -> bytes:
    trailer = struct.pack(">I", len(payload)) + fw.SAG_JK_WH
    return b"MZ-updater" + payload + trailer


def service_v2(payloads: list[bytes]) -> bytes:
    if len(payloads) != 4:
        raise ValueError("service v2 fixture needs four payloads")
    key = bytes(b"MZ" + bytes(range(2, 80)))
    encrypted = [xor_payload(payload, key) for payload in payloads]
    metadata = bytearray(114)
    metadata[:32] = size_bits(len(payloads[0]))
    marker = b"Siemens Mobile Phones:SOFTWARE:01.00"
    marker_end = len(metadata) - len(fw.SAG_JK_WH) - 5
    metadata[marker_end - len(marker):marker_end] = marker
    metadata[-len(fw.SAG_JK_WH):] = fw.SAG_JK_WH
    return b"".join((
        key,
        encrypted[3], size_bits(len(payloads[3])),
        encrypted[1], size_bits(len(payloads[1])),
        encrypted[2], size_bits(len(payloads[2])),
        encrypted[0], bytes(metadata),
    ))


def update_exe(payload: bytes, encrypted: bool) -> bytes:
    payload_offset = 0x300
    prefix = bytearray(b"\0" * payload_offset)
    prefix[:2] = b"MZ"
    key = bytes(range(16))
    base_iv = bytes(range(16, 32))
    key_offset = 0x100
    iv_offset = 0x120
    prefix[key_offset:key_offset + 16] = key
    prefix[iv_offset:iv_offset + 16] = base_iv
    pattern_offset = 0x200
    prefix[pattern_offset:pattern_offset + 15] = b"".join((
        b"\x68", struct.pack("<I", 0x400000 + iv_offset),
        b"\x68\x80\x00\x00\x00",
        b"\x68", struct.pack("<I", 0x400000 + key_offset),
    ))

    if encrypted:
        sign_time = 0x12345678
        iv = bytearray(base_iv)
        struct.pack_into(">I", iv, 0, sign_time)
        body = AES.new(key, AES.MODE_CBC, bytes(iv)).encrypt(
            pad(payload, AES.block_size)
        )
        inner = struct.pack(">II", sign_time, 128) + bytes(128) + body
        encryption_type = 1
    else:
        inner = payload
        encryption_type = 0

    trailer = bytearray(34)
    trailer[-7:] = fw.SAG_UDT
    full = bytearray(prefix + struct.pack(">H", encryption_type) + inner
                     + trailer)
    full[-29] = payload_offset & 0xFF
    full[-22] = (payload_offset >> 8) & 0xFF
    full[-17] = (payload_offset >> 16) & 0xFF
    full[-26] = (payload_offset >> 24) & 0xFF
    return bytes(full)
