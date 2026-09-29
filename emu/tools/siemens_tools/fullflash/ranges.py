from __future__ import annotations

from typing import Any, Iterable


def source_layout_intersection(
        source_length: int, layout_length: int,
        source_to_layout_shift: int) -> tuple[int, int, int, int]:
    """Return corresponding half-open source and layout coverage ranges."""
    source_start = max(0, -source_to_layout_shift)
    source_end = min(
        source_length, layout_length - source_to_layout_shift
    )
    if source_end < source_start:
        source_end = source_start
    return (
        source_start,
        source_end,
        source_start + source_to_layout_shift,
        source_end + source_to_layout_shift,
    )


def _true_ranges(mask: bytes | bytearray) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    start: int | None = None
    for offset, value in enumerate(mask):
        if value and start is None:
            start = offset
        elif not value and start is not None:
            ranges.append((start, offset))
            start = None
    if start is not None:
        ranges.append((start, len(mask)))
    return ranges


def _merge_ranges(ranges: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[list[int]] = []
    for start, end in sorted(ranges):
        if start >= end:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [(start, end) for start, end in merged]


def _range_json(ranges: Iterable[tuple[int, int]]) -> list[dict[str, Any]]:
    return [{
        "from": f"0x{start:06X}",
        "to_exclusive": f"0x{end:06X}",
        "length": end - start,
    } for start, end in ranges]


def _covered_intersections(ranges: Iterable[tuple[int, int]],
                           coverage: bytearray) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    for start, end in ranges:
        cursor = start
        while cursor < end:
            while cursor < end and not coverage[cursor]:
                cursor += 1
            overlap_start = cursor
            while cursor < end and coverage[cursor]:
                cursor += 1
            if overlap_start < cursor:
                result.append((overlap_start, cursor))
    return _merge_ranges(result)


def _ownership_json(ownership: bytearray,
                    names: dict[int, str]) -> list[dict[str, Any]]:
    result = []
    for owner, name in names.items():
        ranges: list[tuple[int, int]] = []
        count = 0
        start: int | None = None
        for offset, value in enumerate(ownership):
            if value == owner:
                count += 1
                if start is None:
                    start = offset
            elif start is not None:
                ranges.append((start, offset))
                start = None
        if start is not None:
            ranges.append((start, len(ownership)))
        if count:
            result.append({
                "source": name,
                "bytes": count,
                "ranges": _range_json(ranges),
            })
    return result


def _reject_duplicate_json_keys(
        pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise FirmwareError(f"duplicate overlay JSON field {key!r}")
        result[key] = value
    return result
