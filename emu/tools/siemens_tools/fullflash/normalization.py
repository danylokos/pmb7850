from __future__ import annotations

from typing import Any

from .bcore_normalization import (
    NORMALIZATION_KIND as BCORE_NORMALIZATION_KIND,
    normalize_bcore,
    restore_bcore,
)
from .statistics import (
    NORMALIZATION_KIND as STATISTICS_NORMALIZATION_KIND,
    normalize_statistics,
    restore_statistics,
)
from ..firmware.xbi import FirmwareError, FlashLayout


def normalize_partition(
    payload: bytes,
    layout: FlashLayout,
    *,
    role: str,
    partition_start: int,
    partition_end: int,
    full_boundary_present: bool,
    bcore_metadata: dict[str, Any] | None = None,
) -> tuple[bytes, dict[str, Any] | None]:
    """Normalize one complete canonical partition when its structure proves it."""
    if role == "BCORE":
        normalized = normalize_bcore(
            payload,
            layout,
            bcore_metadata,
            full_boundary_present=full_boundary_present,
        )
        if normalized is not None:
            return normalized.payload, normalized.metadata

    statistic_offset = layout.statistic_offset
    if (
        statistic_offset is not None
        and partition_start <= statistic_offset
        and statistic_offset + 0x200 <= partition_end
    ):
        normalized_statistics = normalize_statistics(
            payload,
            page_offset=statistic_offset - partition_start,
            layout_offset=statistic_offset,
            full_boundary_present=full_boundary_present,
        )
        if normalized_statistics is not None:
            return (
                normalized_statistics.payload,
                normalized_statistics.metadata,
            )
    return payload, None


def restore_normalization(
    payload: bytes,
    metadata: object,
    *,
    context: str = "partition normalization",
) -> bytes:
    if not isinstance(metadata, dict):
        raise FirmwareError(f"{context} metadata is invalid")
    kind = metadata.get("kind")
    if kind == BCORE_NORMALIZATION_KIND:
        return restore_bcore(payload, metadata, context=context)
    if kind == STATISTICS_NORMALIZATION_KIND:
        return restore_statistics(payload, metadata, context=context)
    raise FirmwareError(f"{context} kind is unsupported")
