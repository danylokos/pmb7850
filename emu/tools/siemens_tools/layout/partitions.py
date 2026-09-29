from __future__ import annotations

from dataclasses import dataclass

from ..firmware.xbi import FirmwareError, FlashLayout


@dataclass(frozen=True)
class LayoutPartition:
    label: str
    kind: str
    start: int
    end: int


def partition_layout(layout: FlashLayout) -> tuple[LayoutPartition, ...]:
    partitions: list[LayoutPartition] = []
    cursor = 0
    unknown_number = 1
    boundaries = iter(layout.partition_boundaries)
    boundary = next(boundaries, None)

    def append_unknown(start: int, end: int) -> None:
        nonlocal unknown_number, boundary
        while boundary is not None and boundary <= start:
            boundary = next(boundaries, None)
        while boundary is not None and boundary < end:
            partitions.append(LayoutPartition(
                f"UNKNOWN_{unknown_number}", "unknown-gap", start, boundary
            ))
            unknown_number += 1
            start = boundary
            boundary = next(boundaries, None)
        if start < end:
            partitions.append(LayoutPartition(
                f"UNKNOWN_{unknown_number}", "unknown-gap", start, end
            ))
            unknown_number += 1

    for region in sorted(layout.regions, key=lambda item: item.offset):
        if region.offset < cursor:
            raise FirmwareError(f"layout {layout.name!r} has overlapping regions")
        if cursor < region.offset:
            append_unknown(cursor, region.offset)
        partitions.append(LayoutPartition(
            region.name, "known-region", region.offset, region.end
        ))
        cursor = region.end
    if cursor < layout.length:
        append_unknown(cursor, layout.length)
    if (
        not partitions
        or partitions[0].start != 0
        or partitions[-1].end != layout.length
    ):
        raise FirmwareError(f"layout {layout.name!r} does not partition its flash")
    represented = {item.start for item in partitions} | {
        item.end for item in partitions
    }
    missing = set(layout.partition_boundaries) - represented
    if missing:
        rendered = ", ".join(f"0x{value:X}" for value in sorted(missing))
        raise FirmwareError(
            f"layout {layout.name!r} partition boundaries fall inside named "
            f"regions: {rendered}"
        )
    return tuple(partitions)
