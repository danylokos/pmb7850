"""Shared Siemens flash layout loading, partitioning, and rendering."""

from .catalog import (
    BUILTIN_LAYOUT_PATH,
    LoadedLayout,
    LoadedLayoutCatalog,
    StatisticsLocations,
    load_flash_layout,
    load_layout,
    load_layout_catalog,
    resolve_statistics_locations,
)
from .partitions import LayoutPartition, partition_layout
from .render import (
    LayoutRow,
    layout_rows,
    render_layout_markdown,
    render_layout_reference,
    render_layout_text,
    render_layouts_text,
)
from ..firmware.xbi import FirmwareError, FlashLayout, LayoutChip, LayoutRegion

__all__ = [
    "BUILTIN_LAYOUT_PATH",
    "FirmwareError",
    "FlashLayout",
    "LayoutChip",
    "LayoutPartition",
    "LayoutRegion",
    "LayoutRow",
    "LoadedLayout",
    "LoadedLayoutCatalog",
    "StatisticsLocations",
    "layout_rows",
    "load_flash_layout",
    "load_layout",
    "load_layout_catalog",
    "partition_layout",
    "render_layout_markdown",
    "resolve_statistics_locations",
    "render_layout_reference",
    "render_layout_text",
    "render_layouts_text",
]
