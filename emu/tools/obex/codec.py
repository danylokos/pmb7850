"""Small, strict OBEX packet and header codec."""

from __future__ import annotations

from dataclasses import dataclass

from .errors import ProtocolError

CONNECT = 0x80
DISCONNECT = 0x81
PUT = 0x02
PUT_FINAL = 0x82
GET = 0x03
GET_FINAL = 0x83
SETPATH = 0x85
ABORT = 0xFF
CONTINUE = 0x90
SUCCESS = 0xA0

NAME = 0x01
TYPE = 0x42
TARGET = 0x46
WHO = 0x4A
BODY = 0x48
END_BODY = 0x49
LENGTH = 0xC3
CONNECTION_ID = 0xCB


@dataclass(frozen=True)
class Header:
    identifier: int
    value: bytes | int


@dataclass(frozen=True)
class Packet:
    code: int
    payload: bytes


def unicode_header(identifier: int, value: str) -> bytes:
    return bytes((identifier,)) + _sized(value.encode("utf-16-be") + b"\0\0")


def bytes_header(identifier: int, value: bytes) -> bytes:
    return bytes((identifier,)) + _sized(value)


def uint32_header(identifier: int, value: int) -> bytes:
    if not 0 <= value <= 0xFFFFFFFF:
        raise ValueError("32-bit OBEX header out of range")
    return bytes((identifier,)) + value.to_bytes(4, "big")


def _sized(value: bytes) -> bytes:
    length = len(value) + 3
    if length > 0xFFFF:
        raise ValueError("OBEX header too large")
    return length.to_bytes(2, "big") + value


def encode_packet(code: int, *parts: bytes) -> bytes:
    payload = b"".join(parts)
    length = len(payload) + 3
    if length > 0xFFFF:
        raise ValueError("OBEX packet too large")
    return bytes((code,)) + length.to_bytes(2, "big") + payload


def decode_prefix(prefix: bytes) -> tuple[int, int]:
    if len(prefix) != 3:
        raise ProtocolError("OBEX prefix must contain three bytes")
    length = int.from_bytes(prefix[1:3], "big")
    if length < 3:
        raise ProtocolError("invalid OBEX packet length")
    return prefix[0], length


def parse_headers(data: bytes) -> list[Header]:
    result: list[Header] = []
    offset = 0
    while offset < len(data):
        identifier = data[offset]
        kind = identifier & 0xC0
        if kind in (0x00, 0x40):
            if offset + 3 > len(data):
                raise ProtocolError("truncated OBEX header")
            length = int.from_bytes(data[offset + 1:offset + 3], "big")
            if length < 3 or offset + length > len(data):
                raise ProtocolError("invalid OBEX header length")
            result.append(Header(identifier, data[offset + 3:offset + length]))
            offset += length
        elif kind == 0x80:
            if offset + 2 > len(data):
                raise ProtocolError("truncated one-byte OBEX header")
            result.append(Header(identifier, data[offset + 1]))
            offset += 2
        else:
            if offset + 5 > len(data):
                raise ProtocolError("truncated four-byte OBEX header")
            result.append(Header(identifier, int.from_bytes(data[offset + 1:offset + 5], "big")))
            offset += 5
    return result
