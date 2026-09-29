from __future__ import annotations

import hashlib
import re
import struct
from dataclasses import dataclass
from datetime import datetime
from typing import Any

from ..firmware.xbi import FirmwareError


PAGE_SIZE = 0x200
MUTABLE_END = 0x100
TRAMPOLINE_OFFSET = 0x1FC
TRAMPOLINE_SIZE = 4
ERASED = 0xFF
NORMALIZATION_KIND = "pmb7850-statistics-fields-erased"
_TIMESTAMP_RE = re.compile(
    rb"[0-9]{2}\.[0-9]{2}\.[0-9]{2}[0-9]{2}:[0-9]{2}:[0-9]{2}"
)

_NUMERIC_FIELDS = (
    ("xor16", 0x10, 2),
    ("sum16", 0x12, 2),
    ("compact_flash_id", 0x18, 2),
    ("generation", 0x1A, 2),
    ("checksum_failure_counter", 0x20, 2),
    ("format_marker", 0x24, 2),
    ("descriptor_0_manufacturer_id", 0x26, 2),
    ("descriptor_0_device_id", 0x28, 2),
    ("descriptor_0_status", 0x2E, 2),
    ("descriptor_0_generation", 0x30, 2),
    ("descriptor_1_manufacturer_id", 0x32, 2),
    ("descriptor_1_device_id", 0x34, 2),
    ("descriptor_1_status", 0x3A, 2),
    ("descriptor_1_generation", 0x3C, 2),
)
_RAW_FIELDS = (
    ("reserved_prefix", 0x00, 0x10),
    ("pre_record_reserved", 0x14, 0x04),
    ("unknown_1", 0x1C, 0x02),
    ("unknown_2", 0x1E, 0x02),
    ("record_reserved", 0x22, 0x02),
    ("descriptor_0_unknown_1", 0x2A, 0x02),
    ("descriptor_0_unknown_2", 0x2C, 0x02),
    ("descriptor_1_unknown_1", 0x36, 0x02),
    ("descriptor_1_unknown_2", 0x38, 0x02),
    ("descriptor_padding", 0x3E, 0x02),
    ("reserved_1", 0x50, 0x10),
    ("reserved_2", 0x70, 0x10),
)
_TIMESTAMP_FIELDS = (
    ("current_timestamp", 0x40, 0x10),
    ("previous_timestamp", 0x60, 0x10),
)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _raw(offset: int, data: bytes) -> dict[str, Any]:
    return {"offset": offset, "length": len(data), "raw": data.hex()}


def _number(offset: int, size: int, data: bytes) -> dict[str, Any]:
    raw = data[offset:offset + size]
    if raw == bytes([ERASED]) * size:
        return _raw(offset, raw)
    return {
        "offset": offset,
        "length": size,
        "value": int.from_bytes(raw, "little"),
    }


def _timestamp(offset: int, data: bytes) -> dict[str, Any]:
    raw = data[offset:offset + 0x10]
    if _TIMESTAMP_RE.fullmatch(raw):
        try:
            text = raw.decode("ascii")
            datetime.strptime(text, "%d.%m.%y%H:%M:%S")
        except (UnicodeDecodeError, ValueError):
            pass
        else:
            return {"offset": offset, "length": 0x10, "text": text}
    return _raw(offset, raw)


def _parse_ranges(
    data: bytes,
) -> tuple[list[dict[str, Any]], dict[str, Any], dict[str, Any]]:
    ranges = []
    cursor = 0x80
    while cursor + 4 <= MUTABLE_END:
        start_raw = data[cursor:cursor + 4]
        start = struct.unpack_from("<I", start_raw)[0]
        if start == 0xFFFFFFFF:
            terminator = _raw(cursor, start_raw)
            cursor += 4
            padding = _raw(cursor, data[cursor:MUTABLE_END])
            if any(byte != ERASED for byte in data[cursor:MUTABLE_END]):
                raise ValueError("checksum range table padding is not erased")
            if not ranges:
                raise ValueError("checksum range table is empty")
            return ranges, terminator, padding
        if cursor + 8 > MUTABLE_END:
            break
        end = struct.unpack_from("<I", data, cursor + 4)[0]
        if start >= end or start & 1 or end & 1 or end == 0xFFFFFFFF:
            raise ValueError(
                f"invalid checksum range 0x{start:08X}..0x{end:08X}"
            )
        ranges.append({
            "start": {"offset": cursor, "length": 4, "value": start},
            "end": {"offset": cursor + 4, "length": 4, "value": end},
        })
        cursor += 8
    raise ValueError("checksum range table has no terminator")


def _trampoline(page: bytes) -> dict[str, Any]:
    raw = page[TRAMPOLINE_OFFSET:TRAMPOLINE_OFFSET + TRAMPOLINE_SIZE]
    result = _raw(TRAMPOLINE_OFFSET, raw)
    if raw == b"\xFF" * TRAMPOLINE_SIZE:
        result["state"] = "erased"
    elif len(raw) == 4 and raw[0] == 0xFA:
        result["state"] = "jmps"
        result["target"] = (raw[1] << 16) | int.from_bytes(raw[2:4], "little")
    else:
        result["state"] = "unrecognized"
    return result


def inspect_statistics(page: bytes) -> dict[str, Any]:
    if len(page) != PAGE_SIZE:
        return {
            "status": "incomplete",
            "size": len(page),
            "required_size": PAGE_SIZE,
        }
    fields = {
        name: _raw(offset, page[offset:offset + size])
        for name, offset, size in _RAW_FIELDS
    }
    fields.update({
        name: _number(offset, size, page)
        for name, offset, size in _NUMERIC_FIELDS
    })
    fields.update({
        name: _timestamp(offset, page)
        for name, offset, _size in _TIMESTAMP_FIELDS
    })
    template = page[0x18:0x80] == b"\xFF" * 0x68
    marker = struct.unpack_from("<H", page, 0x24)[0]
    record_kind = (
        "package-template" if template
        else "installed" if marker == 0x0010
        else "unrecognized"
    )
    try:
        ranges, terminator, padding = _parse_ranges(page)
    except ValueError as exc:
        return {
            "status": "malformed",
            "reason": str(exc),
            "record_kind": record_kind,
            "fields": fields,
            "trampoline": _trampoline(page),
        }
    result = {
        "record_kind": record_kind,
        "fields": fields,
        "checksum_ranges": ranges,
        "range_terminator": terminator,
        "range_table_padding": padding,
        "trampoline": _trampoline(page),
    }
    if record_kind == "unrecognized":
        return {
            "status": "malformed",
            "reason": f"unrecognized format marker 0x{marker:04X}",
            **result,
        }
    return {"status": "parsed", **result}


@dataclass(frozen=True)
class NormalizedStatistics:
    payload: bytes
    metadata: dict[str, Any]


def normalize_statistics(
    payload: bytes,
    *,
    page_offset: int,
    layout_offset: int,
    full_boundary_present: bool,
) -> NormalizedStatistics | None:
    if (
        not full_boundary_present
        or page_offset < 0
        or page_offset + PAGE_SIZE > len(payload)
    ):
        return None
    page = payload[page_offset:page_offset + PAGE_SIZE]
    parsed = inspect_statistics(page)
    if parsed.get("status") != "parsed":
        return None
    source_sha256 = _sha256(payload)
    normalized = bytearray(payload)
    normalized[page_offset:page_offset + MUTABLE_END] = b"\xFF" * MUTABLE_END
    trampoline = page_offset + TRAMPOLINE_OFFSET
    normalized[trampoline:trampoline + TRAMPOLINE_SIZE] = (
        b"\xFF" * TRAMPOLINE_SIZE
    )
    normalized_payload = bytes(normalized)
    return NormalizedStatistics(normalized_payload, {
        "kind": NORMALIZATION_KIND,
        "statistics_layout_offset": layout_offset,
        "statistics_payload_offset": page_offset,
        "erased_ranges": [
            {"offset": page_offset, "length": MUTABLE_END},
            {"offset": trampoline, "length": TRAMPOLINE_SIZE},
        ],
        "source_sha256": source_sha256,
        "normalized_sha256": _sha256(normalized_payload),
        "restored_sha256": source_sha256,
        "record_kind": parsed["record_kind"],
        "fields": parsed["fields"],
        "checksum_ranges": parsed["checksum_ranges"],
        "range_terminator": parsed["range_terminator"],
        "range_table_padding": parsed["range_table_padding"],
        "trampoline": parsed["trampoline"],
    })


def _decode_field(
    value: object,
    *,
    offset: int,
    length: int,
    kind: str,
    context: str,
    allowed_extra: frozenset[str] = frozenset(),
) -> bytes:
    if (
        not isinstance(value, dict)
        or value.get("offset") != offset
        or value.get("length") != length
    ):
        raise FirmwareError(f"{context} field metadata is invalid")
    forms = [key for key in ("raw", "value", "text") if key in value]
    if len(forms) != 1 or (
        set(value) - {"offset", "length", forms[0]}
    ) - allowed_extra:
        raise FirmwareError(f"{context} field storage is invalid")
    form = forms[0]
    if form == "raw":
        try:
            result = bytes.fromhex(value["raw"])
        except (TypeError, ValueError) as exc:
            raise FirmwareError(f"{context} field raw bytes are invalid") from exc
    elif form == "value" and kind == "number":
        number = value["value"]
        if (
            not isinstance(number, int)
            or number < 0
            or number >= 1 << (length * 8)
        ):
            raise FirmwareError(f"{context} field value is invalid")
        result = number.to_bytes(length, "little")
    elif form == "text" and kind == "timestamp":
        text = value["text"]
        try:
            result = text.encode("ascii")
            datetime.strptime(text, "%d.%m.%y%H:%M:%S")
        except (AttributeError, UnicodeEncodeError, ValueError) as exc:
            raise FirmwareError(f"{context} timestamp is invalid") from exc
    else:
        raise FirmwareError(f"{context} field storage is invalid")
    if len(result) != length:
        raise FirmwareError(f"{context} field length is invalid")
    return result


def restore_statistics(
    payload: bytes,
    metadata: object,
    *,
    context: str = "statistics normalization",
) -> bytes:
    expected_metadata = {
        "kind", "statistics_layout_offset", "statistics_payload_offset",
        "erased_ranges", "source_sha256", "normalized_sha256",
        "restored_sha256", "record_kind", "fields", "checksum_ranges",
        "range_terminator", "range_table_padding", "trampoline",
    }
    if (
        not isinstance(metadata, dict)
        or set(metadata) != expected_metadata
        or metadata.get("kind") != NORMALIZATION_KIND
        or metadata.get("record_kind") not in {"installed", "package-template"}
        or not isinstance(metadata.get("statistics_layout_offset"), int)
        or metadata["statistics_layout_offset"] < 0
    ):
        raise FirmwareError(f"{context} metadata is invalid")
    page_offset = metadata.get("statistics_payload_offset")
    if (
        not isinstance(page_offset, int)
        or page_offset < 0
        or page_offset + PAGE_SIZE > len(payload)
    ):
        raise FirmwareError(f"{context} metadata is invalid")
    expected_ranges = [
        {"offset": page_offset, "length": MUTABLE_END},
        {"offset": page_offset + TRAMPOLINE_OFFSET, "length": TRAMPOLINE_SIZE},
    ]
    if metadata.get("erased_ranges") != expected_ranges:
        raise FirmwareError(f"{context} erased ranges are invalid")
    normalized_sha256 = metadata.get("normalized_sha256")
    source_sha256 = metadata.get("source_sha256")
    if (
        not isinstance(normalized_sha256, str)
        or not isinstance(source_sha256, str)
        or metadata.get("restored_sha256") != source_sha256
        or _sha256(payload) != normalized_sha256
    ):
        raise FirmwareError(f"{context} hash metadata is invalid")
    trampoline_offset = page_offset + TRAMPOLINE_OFFSET
    if (
        payload[page_offset:page_offset + MUTABLE_END] != b"\xFF" * MUTABLE_END
        or payload[trampoline_offset:trampoline_offset + TRAMPOLINE_SIZE]
        != b"\xFF" * TRAMPOLINE_SIZE
    ):
        raise FirmwareError(f"{context} normalized fields are not erased")

    page = bytearray(payload[page_offset:page_offset + PAGE_SIZE])
    fields = metadata.get("fields")
    expected_fields = {
        name
        for name, _offset, _size in (
            *_RAW_FIELDS,
            *_NUMERIC_FIELDS,
            *_TIMESTAMP_FIELDS,
        )
    }
    if not isinstance(fields, dict) or set(fields) != expected_fields:
        raise FirmwareError(f"{context} fields are invalid")
    for name, offset, size in _RAW_FIELDS:
        page[offset:offset + size] = _decode_field(
            fields[name], offset=offset, length=size, kind="raw",
            context=f"{context} {name}",
        )
    for name, offset, size in _NUMERIC_FIELDS:
        page[offset:offset + size] = _decode_field(
            fields[name], offset=offset, length=size, kind="number",
            context=f"{context} {name}",
        )
    for name, offset, size in _TIMESTAMP_FIELDS:
        page[offset:offset + size] = _decode_field(
            fields[name], offset=offset, length=size, kind="timestamp",
            context=f"{context} {name}",
        )

    ranges = metadata.get("checksum_ranges")
    if not isinstance(ranges, list) or not ranges:
        raise FirmwareError(f"{context} checksum ranges are invalid")
    cursor = 0x80
    for index, pair in enumerate(ranges):
        if (
            not isinstance(pair, dict)
            or set(pair) != {"start", "end"}
            or cursor + 8 > MUTABLE_END
        ):
            raise FirmwareError(f"{context} checksum range {index} is invalid")
        page[cursor:cursor + 4] = _decode_field(
            pair["start"], offset=cursor, length=4, kind="number",
            context=f"{context} checksum range {index} start",
        )
        page[cursor + 4:cursor + 8] = _decode_field(
            pair["end"], offset=cursor + 4, length=4, kind="number",
            context=f"{context} checksum range {index} end",
        )
        cursor += 8
    terminator = _decode_field(
        metadata.get("range_terminator"),
        offset=cursor,
        length=4,
        kind="raw",
        context=f"{context} range terminator",
    )
    if terminator != b"\xFF" * 4:
        raise FirmwareError(f"{context} range terminator is invalid")
    page[cursor:cursor + 4] = terminator
    cursor += 4
    page[cursor:MUTABLE_END] = _decode_field(
        metadata.get("range_table_padding"),
        offset=cursor,
        length=MUTABLE_END - cursor,
        kind="raw",
        context=f"{context} range table padding",
    )
    trampoline = metadata.get("trampoline")
    raw_trampoline = _decode_field(
        trampoline,
        offset=TRAMPOLINE_OFFSET,
        length=TRAMPOLINE_SIZE,
        kind="raw",
        context=f"{context} trampoline",
        allowed_extra=frozenset({"state", "target"}),
    )
    expected_trampoline = _trampoline(
        b"\xFF" * TRAMPOLINE_OFFSET + raw_trampoline
    )
    if trampoline != expected_trampoline:
        raise FirmwareError(f"{context} trampoline state is invalid")
    page[TRAMPOLINE_OFFSET:TRAMPOLINE_OFFSET + TRAMPOLINE_SIZE] = raw_trampoline

    parsed = inspect_statistics(bytes(page))
    if (
        parsed.get("status") != "parsed"
        or parsed.get("record_kind") != metadata.get("record_kind")
    ):
        raise FirmwareError(f"{context} restored structure is invalid")
    restored = bytearray(payload)
    restored[page_offset:page_offset + PAGE_SIZE] = page
    result = bytes(restored)
    if _sha256(result) != source_sha256:
        raise FirmwareError(f"{context} restored hash mismatch")
    return result
