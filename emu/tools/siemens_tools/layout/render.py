from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from .catalog import load_layout_catalog
from .partitions import partition_layout
from ..firmware.xbi import FlashLayout


HEADER = """# Siemens Phone Flash Layouts

This reference is generated from
[`siemens_flash_layouts.yaml`](../../../emu/tools/siemens_tools/data/siemens_flash_layouts.yaml),
the authoritative operational layout catalog. Named ranges retain their
independently recovered semantics; `partition_boundaries` preserve additional
official updater endpoints inside otherwise unnamed space.

Exact version-dependent updater coverage remains in
[Official Firmware Data-Flash Boundaries](official-firmware-data-flash.md).
The operational map uses the union of those endpoints rather than creating
software-version-specific layouts.

- Addresses and file offsets are inclusive.
- Hexadecimal values are uppercase and omit the `0x` prefix.
- File offsets are relative to the FullFlash base.
- Human-readable sizes use binary units: 1 KB = 1024 bytes and 1 MB = 1024 KB.
- `UNKNOWN_n` rows are the ordered pieces left after named ranges and explicit
  partition boundaries are applied.
- Optional regions with a zero start and zero length are omitted.
"""


@dataclass(frozen=True)
class LayoutRow:
    label: str
    start: int
    end: int
    size: int
    file_start: int
    file_end: int


def _size(value: int) -> str:
    if value % 0x100000 == 0:
        amount = value / 0x100000
        rendered = str(int(amount))
        unit = "MB"
    else:
        amount = value / 0x100000
        if value >= 0x100000 and amount * 16 == int(amount * 16):
            rendered = f"{amount:g}"
            unit = "MB"
        else:
            rendered = str(value // 1024)
            unit = "KB"
    return f"{value:06X} ({rendered} {unit})"


def layout_rows(layout: FlashLayout) -> tuple[LayoutRow, ...]:
    rows = [LayoutRow(
        "FullFlash",
        layout.base,
        layout.base + layout.length - 1,
        layout.length,
        0,
        layout.length - 1,
    )]
    rows.extend(
        LayoutRow(
            partition.label,
            layout.base + partition.start,
            layout.base + partition.end - 1,
            partition.end - partition.start,
            partition.start,
            partition.end - 1,
        )
        for partition in partition_layout(layout)
    )
    return tuple(rows)


def _markdown_lines(layout: FlashLayout) -> list[str]:
    lines = [
        f"## {layout.name}",
        "",
        "| Region Name | Region Start | Region End | Size | "
        "File Offset Start | File Offset End |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for row in layout_rows(layout):
        lines.append(
            f"| {row.label} | `{row.start:06X}` | `{row.end:06X}` | "
            f"`{_size(row.size)}` | `{row.file_start:06X}` | "
            f"`{row.file_end:06X}` |"
        )
    lines.append("")
    return lines


def render_layout_markdown(layout: FlashLayout) -> str:
    return "\n".join(_markdown_lines(layout))


def render_layout_reference(path: Path | None = None) -> str:
    catalog = load_layout_catalog(path)
    lines = [HEADER.rstrip(), ""]
    for layout in catalog.layouts:
        lines.extend(_markdown_lines(layout))
    return "\n".join(lines)


def _text_lines(layout: FlashLayout) -> list[str]:
    headers = ("Region", "Start", "End", "Size", "File Start", "File End")
    values = [
        (
            row.label,
            f"{row.start:06X}",
            f"{row.end:06X}",
            _size(row.size),
            f"{row.file_start:06X}",
            f"{row.file_end:06X}",
        )
        for row in layout_rows(layout)
    ]
    widths = [
        max(len(headers[index]), *(len(row[index]) for row in values))
        for index in range(len(headers))
    ]

    def format_row(row: tuple[str, ...]) -> str:
        cells = [f"{row[0]:<{widths[0]}}"]
        cells.extend(
            f"{row[index]:>{widths[index]}}"
            for index in range(1, len(row))
        )
        return "  ".join(cells)

    return [
        f"{layout.name} flash layout",
        "",
        format_row(headers),
        *(format_row(row) for row in values),
    ]


def render_layout_text(layout: FlashLayout) -> str:
    return "\n".join(_text_lines(layout)) + "\n"


def render_layouts_text(layouts: Iterable[FlashLayout]) -> str:
    sections = ["\n".join(_text_lines(layout)) for layout in layouts]
    return "\n\n".join(sections) + "\n"
