from __future__ import annotations

import copy
from dataclasses import dataclass, field
from typing import Callable, Iterable

from .xbi import FirmwareError, MaterializedXbi, XbiInfo, XbiWrite, _need, parse_xbi


@dataclass
class _LzssDecoder:
    ring: bytearray = field(default_factory=lambda: bytearray(4096))
    ring_pos: int = 1
    state: int = 0
    temp: int = 0
    temp_bits: int = 0
    copy_from: int = 0

    def feed(self, byte: int, bit_count: int = 8) -> bytes:
        output = bytearray()
        for bit_number in range(bit_count):
            bit = 1 if byte & (1 << (7 - bit_number)) else 0
            if self.state == 0:
                self.temp = 0
                self.temp_bits = 0
                self.state = 1 if bit else 2
            elif self.state == 1:
                self.temp = (self.temp << 1) | bit
                self.temp_bits += 1
                if self.temp_bits == 8:
                    output.append(self.temp)
                    self.ring[self.ring_pos] = self.temp
                    self.ring_pos = (self.ring_pos + 1) & 0xFFF
                    self.state = 0
            elif self.state == 2:
                self.temp = (self.temp << 1) | bit
                self.temp_bits += 1
                if self.temp_bits == 12:
                    self.copy_from = self.temp
                    self.temp = 0
                    self.temp_bits = 0
                    self.state = 3
            elif self.state == 3:
                self.temp = (self.temp << 1) | bit
                self.temp_bits += 1
                if self.temp_bits == 4:
                    for index in range(self.temp + 2):
                        value = self.ring[(self.copy_from + index) & 0xFFF]
                        output.append(value)
                        self.ring[self.ring_pos] = value
                        self.ring_pos = (self.ring_pos + 1) & 0xFFF
                    self.state = 0
            else:
                raise FirmwareError(f"invalid LZSS state: {self.state}")
        return bytes(output)


@dataclass
class _TransportParser:
    on_write: Callable[[int, bytes], None]
    state: int = 0
    checksum: int = 0
    block_addr: int = 0
    remaining: int = 0
    block_data: bytearray = field(default_factory=bytearray)

    def clone(self, on_write: Callable[[int, bytes], None]) -> _TransportParser:
        return _TransportParser(
            on_write=on_write,
            state=self.state,
            checksum=self.checksum,
            block_addr=self.block_addr,
            remaining=self.remaining,
            block_data=bytearray(self.block_data),
        )

    def feed(self, data: bytes) -> None:
        for byte in data:
            self.checksum ^= byte
            if self.state == 0:
                self.checksum = byte
                if byte & 0x80:
                    self.state = 1
                else:
                    self.remaining = byte & 0x7F
                    if self.remaining == 0:
                        raise FirmwareError("invalid compressed data chunk size 0")
                    self.block_data.clear()
                    self.state = 7
            elif self.state == 1:
                if byte != 0xFF:
                    raise FirmwareError("invalid compressed address frame")
                self.state = 2
            elif self.state == 2:
                self.block_addr = byte
                self.state = 3
            elif self.state in (3, 4, 5):
                self.block_addr = ((self.block_addr << 8) + byte) & 0xFFFFFFFF
                self.state += 1
            elif self.state == 6:
                if self.checksum != 0:
                    raise FirmwareError("invalid compressed address checksum")
                self.state = 0
            elif self.state == 7:
                self.block_data.append(byte)
                self.remaining -= 1
                if self.remaining == 0:
                    self.state = 8
            elif self.state == 8:
                if self.checksum != 0:
                    raise FirmwareError("invalid compressed data checksum")
                payload = bytes(self.block_data)
                self.on_write(self.block_addr, payload)
                self.block_addr = (self.block_addr + len(payload)) & 0xFFFFFFFF
                self.state = 0
            else:
                raise FirmwareError(f"invalid compressed transport state: {self.state}")


def _write_flash(flash: bytearray, addr: int, payload: bytes,
                 coverage: bytearray | None = None) -> None:
    offset = addr & 0x0FFFFFFF
    if offset + len(payload) > len(flash):
        raise FirmwareError(
            f"flash write 0x{addr:08x}+0x{len(payload):x} exceeds "
            f"flash size 0x{len(flash):x}"
        )
    flash[offset:offset + len(payload)] = payload
    if coverage is not None:
        coverage[offset:offset + len(payload)] = b"\x01" * len(payload)


def _compressed_bytes(data: bytes, writes: Iterable[XbiWrite]) -> list[memoryview]:
    result = []
    view = memoryview(data)
    for write in writes:
        _need(data, write.offset, write.size, "compressed XBI payload")
        if write.size:
            result.append(view[write.offset:write.offset + write.size])
    return result


def _convert_compressed(data: bytes, info: XbiInfo,
                        flash: bytearray,
                        coverage: bytearray | None = None) -> None:
    chunks = _compressed_bytes(data, info.writes)
    if not chunks:
        raise FirmwareError("compressed XBI has no payload bytes")

    decoder = _LzssDecoder()
    parser = _TransportParser(lambda addr, payload: _write_flash(
        flash, addr, payload, coverage
    ))

    last_chunk = chunks[-1]
    for chunk in chunks[:-1]:
        for byte in chunk:
            parser.feed(decoder.feed(byte))
    for byte in last_chunk[:-1]:
        parser.feed(decoder.feed(byte))

    final_byte = last_chunk[-1]
    candidates: list[tuple[int, _LzssDecoder, _TransportParser,
                           list[tuple[int, bytes]]]] = []
    errors: list[str] = []
    for bit_count in range(8, 0, -1):
        candidate_decoder = copy.deepcopy(decoder)
        candidate_writes: list[tuple[int, bytes]] = []
        candidate_parser = parser.clone(
            lambda addr, payload: candidate_writes.append((addr, payload))
        )
        try:
            candidate_parser.feed(candidate_decoder.feed(final_byte, bit_count))
            if candidate_parser.state == 0:
                candidates.append((
                    bit_count,
                    candidate_decoder,
                    candidate_parser,
                    candidate_writes,
                ))
        except FirmwareError as exc:
            errors.append(str(exc))

    if not candidates:
        detail = errors[0] if errors else (
            f"unexpected end of compressed stream in transport state "
            f"{parser.state}"
        )
        raise FirmwareError(detail)

    write_variants = {
        tuple((addr, payload) for addr, payload in candidate[3])
        for candidate in candidates
    }
    if len(write_variants) != 1:
        bits = ", ".join(str(candidate[0]) for candidate in candidates)
        raise FirmwareError(
            f"ambiguous final LZSS padding; valid bit counts {bits} produce "
            "different flash writes"
        )

    chosen = candidates[0]
    for addr, payload in chosen[3]:
        _write_flash(flash, addr, payload, coverage)


def materialize_xbi(data: bytes,
                    info: XbiInfo | None = None) -> MaterializedXbi:
    """Expand one package while preserving explicit write and erase coverage.

    An erased output byte (0xFF) is ambiguous by itself: it may be a real package
    write, part of a declared erase range, or an untouched sparse hole.  Assembly
    needs those cases separated so later packages only override bytes they own.
    """
    parsed = info or parse_xbi(data)
    flash_size = parsed.get("flash_size")
    if not isinstance(flash_size, int) or flash_size <= 0:
        raise FirmwareError("XBI header has no valid flash size")
    flash = bytearray(b"\xFF") * flash_size
    write_mask = bytearray(flash_size)
    erase_mask = bytearray(flash_size)
    for region in parsed.get("erase_regions", []):
        start = region.get("from")
        inclusive_end = region.get("to")
        if (not isinstance(start, int) or not isinstance(inclusive_end, int)
                or start < 0 or inclusive_end < start
                or inclusive_end >= flash_size):
            raise FirmwareError(f"invalid XBI erase region: {region!r}")
        erase_mask[start:inclusive_end + 1] = b"\x01" * (
            inclusive_end - start + 1
        )

    compression_type = parsed.get("compression_type", 0)
    if compression_type == 0:
        for write in parsed.writes:
            _need(data, write.offset, write.size, "XBI payload")
            _write_flash(
                flash,
                write.addr,
                data[write.offset:write.offset + write.size],
                write_mask,
            )
    elif compression_type == 3:
        _convert_compressed(data, parsed, flash, write_mask)
    else:
        raise FirmwareError(f"unsupported XBI compression type: {compression_type}")
    return MaterializedXbi(bytes(flash), bytes(write_mask), bytes(erase_mask))


def convert_xbi_to_flash(data: bytes, info: XbiInfo | None = None) -> bytes:
    return materialize_xbi(data, info).flash
