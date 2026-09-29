from __future__ import annotations

import struct

from Crypto.Cipher import AES
from Crypto.Util.Padding import unpad

from .xbi import (
    FirmwareError,
    SAG_JK_TRAILER_SIZE,
    SAG_JK_WH,
    SAG_UDT,
    SERVICE_EXE_VERSIONS,
    _need,
    _u16be,
    _u32be,
    _u32le,
)


def detect_exe_type(data: bytes) -> str | None:
    if data.endswith(SAG_UDT):
        return "update"
    if data.endswith(SAG_JK_WH):
        return "service"
    return None


def detect_service_exe_version(data: bytes) -> int:
    if not data.endswith(SAG_JK_WH):
        raise FirmwareError("not a Siemens service executable")
    offset = len(data) - len(SAG_JK_WH) - 5
    for marker, version in SERVICE_EXE_VERSIONS:
        start = offset - len(marker)
        if start >= 0 and data[start:offset] == marker:
            return version
    trailer_offset = len(data) - SAG_JK_TRAILER_SIZE
    _need(data, trailer_offset, 4, "legacy service trailer")
    size = _u32be(data, trailer_offset)
    return 0 if size < len(data) else -1


def _read_size_bits(data: bytes, offset: int) -> int:
    _need(data, offset, 32, "service block-size bitfield")
    result = 0
    for bit in range(32):
        if data[offset + bit] & 0x80:
            result |= 1 << bit
    return result


def _xor_payload(payload: bytes, key: bytes) -> bytes:
    if not key:
        raise FirmwareError("empty service executable XOR key")
    return bytes(byte ^ key[index % len(key)]
                 for index, byte in enumerate(payload))


def _extract_service(data: bytes, version: int) -> list[bytes]:
    blocks: dict[int, tuple[int, int]] = {}
    if version == 0:
        trailer_offset = len(data) - SAG_JK_TRAILER_SIZE
        size = _u32be(data, trailer_offset) + SAG_JK_TRAILER_SIZE
        blocks[0] = (len(data) - size, size)
        cipher_key: bytes | None = None
    elif version in (1, 2):
        metadata_offset = 108 if version == 1 else 114
        cursor = len(data) - metadata_offset
        size = _read_size_bits(data, cursor)
        blocks[0] = (cursor - size, size)
        if version == 2:
            cursor = blocks[0][0] - 32
            size = _read_size_bits(data, cursor)
            blocks[2] = (cursor - size, size)
            cursor = blocks[2][0] - 32
            size = _read_size_bits(data, cursor)
            blocks[1] = (cursor - size, size)
            cursor = blocks[1][0] - 32
            size = _read_size_bits(data, cursor)
            blocks[3] = (cursor - size, size)
        offsets = [offset for offset, size in blocks.values() if size > 0]
        if not offsets:
            raise FirmwareError("service executable contains no payload blocks")
        cipher_key = data[:min(offsets)]
    else:
        raise FirmwareError(f"unsupported service executable version: {version}")

    payloads = []
    for index in sorted(blocks):
        offset, size = blocks[index]
        if size == 0:
            continue
        _need(data, offset, size, f"service payload {index}")
        payload = data[offset:offset + size]
        if cipher_key is not None:
            payload = _xor_payload(payload, cipher_key)
        payloads.append(payload)
    return payloads


def _find_aes_material(data: bytes) -> tuple[bytes, bytes] | None:
    patterns = (
        (bytes.fromhex("688000000068"), -5, 5),
        (bytes.fromhex("2BFA5268"), -5, 3),
    )
    for pattern, iv_delta, key_delta in patterns:
        start = 0
        while True:
            index = data.find(pattern, start)
            if index < 0:
                break
            start = index + 1
            iv_instruction = index + iv_delta
            key_instruction = index + key_delta
            if iv_instruction < 0 or key_instruction < 0:
                continue
            if data[iv_instruction:iv_instruction + 1] != b"\x68":
                continue
            if data[key_instruction:key_instruction + 1] != b"\x68":
                continue
            _need(data, iv_instruction, 5, "AES IV pointer")
            _need(data, key_instruction, 5, "AES key pointer")
            iv_offset = _u32le(data, iv_instruction + 1) - 0x400000
            key_offset = _u32le(data, key_instruction + 1) - 0x400000
            if iv_offset < 0 or key_offset < 0:
                continue
            if iv_offset + 16 > len(data) or key_offset + 16 > len(data):
                continue
            return data[key_offset:key_offset + 16], data[iv_offset:iv_offset + 16]
    return None


def _extract_update(data: bytes) -> list[bytes]:
    _need(data, len(data) - 34, 34, "update executable trailer")
    payload_offset = (
        data[-29]
        | (data[-26] << 24)
        | (data[-22] << 8)
        | (data[-17] << 16)
    )
    payload_size = len(data) - payload_offset - 34
    if payload_size < 2:
        raise FirmwareError("invalid update executable payload offset")
    encryption_type = _u16be(data, payload_offset)
    if encryption_type not in (0, 1):
        raise FirmwareError(
            f"unsupported update encryption type: 0x{encryption_type:04x}"
        )
    payload_offset += 2
    payload_size -= 2
    _need(data, payload_offset, payload_size, "update executable payload")
    payload = data[payload_offset:payload_offset + payload_size]
    if encryption_type == 0:
        return [payload]

    material = _find_aes_material(data)
    if material is None:
        raise FirmwareError("cannot locate AES key and IV in update executable")
    _need(payload, 0, 8, "encrypted update header")
    sign_time = _u32be(payload)
    sign_size = _u32be(payload, 4)
    if sign_size != 128:
        raise FirmwareError(f"invalid encrypted update signature size: {sign_size}")
    _need(payload, 8, sign_size, "encrypted update signature")
    encrypted_body = payload[8 + sign_size:]
    if not encrypted_body or len(encrypted_body) % AES.block_size:
        raise FirmwareError("encrypted update body is not AES block aligned")
    key, base_iv = material
    iv = bytearray(base_iv)
    struct.pack_into(">I", iv, 0, sign_time)
    try:
        decrypted = AES.new(key, AES.MODE_CBC, bytes(iv)).decrypt(encrypted_body)
        return [unpad(decrypted, AES.block_size)]
    except ValueError as exc:
        raise FirmwareError(f"AES update decryption failed: {exc}") from exc


def extract_exe(data: bytes) -> list[bytes]:
    exe_type = detect_exe_type(data)
    if exe_type == "service":
        return _extract_service(data, detect_service_exe_version(data))
    if exe_type == "update":
        return _extract_update(data)
    raise FirmwareError("unknown Siemens executable format")
