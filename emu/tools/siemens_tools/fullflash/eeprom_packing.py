from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Any

from .. import eeprom
from ..firmware.xbi import (
    C55_EEPROM_BANK_SIZE,
    C55_EEPROM_DATA_OFFSET,
    C55_EEPROM_HEADER_OFFSET,
    C55_EEPROM_LINEAR_BASE,
    C55_EEPROM_REGION_SIZE,
    FirmwareError,
)


@dataclass(frozen=True)
class EepromPackingPolicy:
    name: str
    bank_size: int
    region_size: int
    linear_base: int
    header_offset: int
    data_offset: int
    lite_marker: int
    full_marker: int
    empty_full_marker: int


C55_EEPROM_POLICY = EepromPackingPolicy(
    name="C55 EELITE/EEFULL",
    bank_size=C55_EEPROM_BANK_SIZE,
    region_size=C55_EEPROM_REGION_SIZE,
    linear_base=C55_EEPROM_LINEAR_BASE,
    header_offset=C55_EEPROM_HEADER_OFFSET,
    data_offset=C55_EEPROM_DATA_OFFSET,
    lite_marker=0xA0,
    full_marker=0xAA,
    empty_full_marker=0xAA,
)

A55_EEPROM_POLICY = EepromPackingPolicy(
    name="A55 EELITE/EEFULL",
    bank_size=C55_EEPROM_BANK_SIZE,
    region_size=C55_EEPROM_REGION_SIZE,
    linear_base=C55_EEPROM_LINEAR_BASE,
    header_offset=C55_EEPROM_HEADER_OFFSET,
    data_offset=C55_EEPROM_DATA_OFFSET,
    lite_marker=0xA0,
    full_marker=0xA3,
    empty_full_marker=0xA3,
)

A52_EEPROM_POLICY = EepromPackingPolicy(
    name="A52 EELITE/EEFULL",
    bank_size=C55_EEPROM_BANK_SIZE,
    region_size=C55_EEPROM_REGION_SIZE,
    linear_base=C55_EEPROM_LINEAR_BASE,
    header_offset=C55_EEPROM_HEADER_OFFSET,
    data_offset=C55_EEPROM_DATA_OFFSET,
    lite_marker=0xA0,
    full_marker=0xA0,
    empty_full_marker=0xA0,
)

M55_EEPROM_POLICY = EepromPackingPolicy(
    name="M55 EELITE/EEFULL",
    bank_size=0x10000,
    region_size=0x30000,
    linear_base=0xFC0000,
    header_offset=0x80,
    data_offset=0x90,
    lite_marker=0xA1,
    full_marker=0xAA,
    empty_full_marker=0xA9,
)

MC60_EEPROM_POLICY = EepromPackingPolicy(
    name="MC60 EELITE/EEFULL",
    bank_size=0x10000,
    region_size=0x30000,
    linear_base=0xFC0000,
    header_offset=0x80,
    data_offset=0x90,
    lite_marker=0xA1,
    full_marker=0xA8,
    empty_full_marker=0xA8,
)

CF62_EEPROM_POLICY = EepromPackingPolicy(
    name="CF62 EELITE/EEFULL",
    bank_size=0x10000,
    region_size=0x30000,
    linear_base=0xFC0000,
    header_offset=0x80,
    data_offset=0x90,
    lite_marker=0xA0,
    full_marker=0xA4,
    empty_full_marker=0xA4,
)

A60_EEPROM_POLICY = EepromPackingPolicy(
    name="A60 EELITE/EEFULL",
    bank_size=0x10000,
    region_size=0x30000,
    linear_base=0x7C0000,
    header_offset=0x80,
    data_offset=0x90,
    lite_marker=0xA0,
    full_marker=0xA4,
    empty_full_marker=0xA3,
)

A62_EEPROM_POLICY = EepromPackingPolicy(
    name="A62 EELITE/EEFULL",
    bank_size=0x10000,
    region_size=0x30000,
    linear_base=0x7C0000,
    header_offset=0x80,
    data_offset=0x90,
    lite_marker=0xA0,
    full_marker=0xA9,
    empty_full_marker=0xA8,
)

A65_EEPROM_POLICY = EepromPackingPolicy(
    name="A65 EELITE/EEFULL",
    bank_size=0x10000,
    region_size=0x30000,
    linear_base=0x7C0000,
    header_offset=0x80,
    data_offset=0x90,
    lite_marker=0xA0,
    full_marker=0xA8,
    empty_full_marker=0xA7,
)

C60_EEPROM_POLICY = EepromPackingPolicy(
    name="C60 EELITE/EEFULL",
    bank_size=0x10000,
    region_size=0x30000,
    linear_base=0x7C0000,
    header_offset=0x80,
    data_offset=0x90,
    lite_marker=0xA0,
    full_marker=0xA2,
    empty_full_marker=0xA2,
)

SL55_EEPROM_POLICY = EepromPackingPolicy(
    name="SL55 EELITE/EEFULL",
    bank_size=0x10000,
    region_size=0x20000,
    linear_base=0xFE0000,
    header_offset=0x80,
    data_offset=0x90,
    lite_marker=0xA0,
    full_marker=0xA4,
    empty_full_marker=0xA4,
)

S55_EEPROM_POLICY = EepromPackingPolicy(
    name="S55 EELITE/EEFULL",
    bank_size=0x10000,
    region_size=0x20000,
    linear_base=0xFE0000,
    header_offset=0x80,
    data_offset=0x90,
    lite_marker=0xA0,
    full_marker=0xAC,
    empty_full_marker=0xAB,
)


def _build_eeprom_bank(
    policy: EepromPackingPolicy,
    name: bytes,
    sequence: int,
    marker: int,
    bank_linear_base: int,
    blocks: dict[int, bytes],
    versions: dict[int, int],
) -> bytes:
    if name not in (b"EELITE", b"EEFULL"):
        raise FirmwareError("invalid EEPROM bank name")
    bank = bytearray(b"\xFF") * policy.bank_size
    header = (
        b"\xFE\xFE"
        + name
        + struct.pack("<HH", 1, sequence)
        + bytes((marker, 0xFF, 0xFE, 0xFE))
    )
    bank[policy.header_offset:policy.data_offset] = header
    payload_cursor = policy.data_offset
    directory_cursor = policy.bank_size
    for block_id in sorted(blocks):
        payload = blocks[block_id]
        if not 0 <= block_id <= 0xFFFF or not 0 < len(payload) <= 0xFFFF:
            raise FirmwareError(f"invalid EEPROM block {block_id}/{len(payload)}")
        directory_cursor -= eeprom.BLOCKHEADER_LEN
        if payload_cursor + len(payload) > directory_cursor:
            raise FirmwareError(
                f"{name.decode()} bank does not fit block {block_id}"
            )
        linear = bank_linear_base + payload_cursor
        bank[payload_cursor:payload_cursor + len(payload)] = payload
        struct.pack_into(
            "<HHHHHH", bank, directory_cursor,
            (versions[block_id] << 8) | 0xFC, len(payload),
            linear & 0xFFFF, linear >> 16, block_id, 0xFC00,
        )
        payload_cursor += len(payload)
    return bytes(bank)


def pack_eeprom(
    policy: EepromPackingPolicy,
    blocks: dict[int, bytes],
    memory_classes: dict[int, int],
    versions: dict[int, int],
) -> tuple[bytes, dict[str, Any]]:
    """Pack deterministic EELITE, active EEFULL, and empty EEFULL banks."""
    if set(blocks) != set(memory_classes):
        raise FirmwareError("every EEPROM block needs exactly one Memory class")
    if set(blocks) != set(versions):
        raise FirmwareError("every EEPROM block needs exactly one Version")
    invalid_versions = sorted(
        block_id for block_id, version in versions.items()
        if not isinstance(version, int) or not 0 <= version <= 0xFF
    )
    if invalid_versions:
        raise FirmwareError(
            f"EEPROM Versions must be bytes: {invalid_versions[:8]}"
        )
    lite = {
        block_id: blocks[block_id] for block_id in blocks
        if memory_classes[block_id] == 2
    }
    full = {
        block_id: blocks[block_id] for block_id in blocks
        if memory_classes[block_id] == 8
    }
    if len(lite) + len(full) != len(blocks):
        raise FirmwareError("EEPROM Memory classes must be 2 (lite) or 8 (full)")
    bank0 = _build_eeprom_bank(
        policy, b"EELITE", 0, policy.lite_marker,
        policy.linear_base, lite, versions,
    )
    bank1 = _build_eeprom_bank(
        policy, b"EEFULL", 0, policy.full_marker,
        policy.linear_base + policy.bank_size, full, versions,
    )
    banks = [bank0, bank1]
    bank_descriptions = [
        {"name": "EELITE", "sequence": 0, "records": len(lite)},
        {"name": "EEFULL", "sequence": 0, "records": len(full)},
    ]
    if policy.region_size == 3 * policy.bank_size:
        banks.append(_build_eeprom_bank(
            policy, b"EEFULL", 1, policy.empty_full_marker,
            policy.linear_base + 2 * policy.bank_size, {}, {},
        ))
        bank_descriptions.append(
            {"name": "EEFULL", "sequence": 1, "records": 0},
        )
    elif policy.region_size != 2 * policy.bank_size:
        raise FirmwareError(f"unsupported EEPROM bank count for {policy.name}")
    return b"".join(banks), {
        "format": policy.name,
        "linear_base": f"0x{policy.linear_base:06X}",
        "bank_size": policy.bank_size,
        "descriptor_order": "numeric block id",
        "history_records": 0,
        "banks": bank_descriptions,
    }

def pack_c55_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                     versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(C55_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_a55_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                     versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(A55_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_a52_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                     versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(A52_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_m55_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                     versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(M55_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_mc60_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                      versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(MC60_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_cf62_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                      versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(CF62_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_a60_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                     versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(A60_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_a62_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                     versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(A62_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_a65_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                     versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(A65_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_c60_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                     versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return pack_eeprom(C60_EEPROM_POLICY, blocks, memory_classes, versions)


def pack_sl55_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                      versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return _pack_x55_8k_full(
        SL55_EEPROM_POLICY, blocks, memory_classes, versions,
        (0xA4, 0xA3, 0xA3, 0xA6, 0xA5, 0xA3, 0xA4, 0xA3),
    )


def _pack_x55_8k_full(
    policy: EepromPackingPolicy,
    blocks: dict[int, bytes],
    memory_classes: dict[int, int],
    versions: dict[int, int],
    full_markers: tuple[int, ...],
) -> tuple[bytes, dict[str, Any]]:
    """Pack the S55/SL55 64 KiB EELITE plus eight 8 KiB EEFULL logs."""
    if set(blocks) != set(memory_classes) or set(blocks) != set(versions):
        raise FirmwareError("every EEPROM block needs Memory and Version metadata")
    lite = {block_id: blocks[block_id] for block_id in blocks
            if memory_classes[block_id] == 2}
    full = {block_id: blocks[block_id] for block_id in blocks
            if memory_classes[block_id] == 8}
    if len(lite) + len(full) != len(blocks):
        raise FirmwareError("EEPROM Memory classes must be 2 (lite) or 8 (full)")
    bank0 = _build_eeprom_bank(
        policy, b"EELITE", 0, policy.lite_marker, policy.linear_base,
        lite, versions,
    )
    full_policy = EepromPackingPolicy(
        name=policy.name, bank_size=0x2000, region_size=0x10000,
        linear_base=policy.linear_base + 0x10000,
        header_offset=policy.header_offset, data_offset=policy.data_offset,
        lite_marker=policy.lite_marker, full_marker=policy.full_marker,
        empty_full_marker=policy.empty_full_marker,
    )
    chunks: list[dict[int, bytes]] = [{}]
    payload_bytes = 0
    for block_id in sorted(full):
        payload = full[block_id]
        record_count = len(chunks[-1]) + 1
        if full_policy.data_offset + payload_bytes + len(payload) > (
            full_policy.bank_size - eeprom.BLOCKHEADER_LEN * record_count
        ):
            chunks.append({})
            payload_bytes = 0
        chunks[-1][block_id] = payload
        payload_bytes += len(payload)
    if len(chunks) > len(full_markers):
        raise FirmwareError(f"{policy.name} EEFULL records need too many log banks")
    chunks.extend({} for _ in range(len(full_markers) - len(chunks)))
    full_banks = [
        _build_eeprom_bank(
            full_policy, b"EEFULL", sequence, marker,
            full_policy.linear_base + sequence * full_policy.bank_size,
            chunk, versions,
        )
        for sequence, (marker, chunk) in enumerate(zip(full_markers, chunks))
    ]
    descriptions = [
        {"name": "EELITE", "sequence": 0, "records": len(lite)},
        *(
            {"name": "EEFULL", "sequence": sequence, "records": len(chunk)}
            for sequence, chunk in enumerate(chunks)
        ),
    ]
    return bank0 + b"".join(full_banks), {
        "format": policy.name,
        "linear_base": f"0x{policy.linear_base:06X}",
        "bank_size": policy.bank_size,
        "full_bank_size": full_policy.bank_size,
        "descriptor_order": "numeric block id",
        "history_records": 0,
        "banks": descriptions,
    }


def pack_s55_eeprom(blocks: dict[int, bytes], memory_classes: dict[int, int],
                     versions: dict[int, int]) -> tuple[bytes, dict[str, Any]]:
    return _pack_x55_8k_full(
        S55_EEPROM_POLICY, blocks, memory_classes, versions,
        (0xAC, 0xAB, 0xAB, 0xAE, 0xAB, 0xAB, 0xAB, 0xAC),
    )
