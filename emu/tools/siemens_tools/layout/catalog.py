from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import yaml

from ..firmware.xbi import FirmwareError, FlashLayout, LayoutChip, LayoutRegion


BUILTIN_LAYOUT_PATH = (
    Path(__file__).resolve().parents[1] / "data" / "siemens_flash_layouts.yaml"
)


@dataclass(frozen=True)
class LoadedLayout:
    layout: FlashLayout
    source: Path
    catalog_bytes: bytes


@dataclass(frozen=True)
class LoadedLayoutCatalog:
    layouts: tuple[FlashLayout, ...]
    source: Path
    catalog_bytes: bytes


@dataclass(frozen=True)
class StatisticsLocations:
    layout_name: str
    statistic_offset: int
    entry_transfer_offset: int


def _load_document(path: Path) -> tuple[bytes, list[dict]]:
    catalog_bytes = path.read_bytes()
    try:
        document = yaml.safe_load(catalog_bytes)
    except yaml.YAMLError as exc:
        raise FirmwareError(f"invalid layout YAML {path}: {exc}") from exc
    phones = document.get("phones") if isinstance(document, dict) else None
    if not isinstance(phones, list):
        raise FirmwareError("layout YAML must contain a phones list")
    return catalog_bytes, [
        phone for phone in phones
        if isinstance(phone, dict) and isinstance(phone.get("name"), str)
    ]


def _parse_layout(raw: dict) -> FlashLayout:
    name = raw["name"]
    base, length = raw.get("base"), raw.get("length")
    if not isinstance(base, int) or not isinstance(length, int) or length <= 0:
        raise FirmwareError(f"layout {name!r} has invalid base/length")
    reset_offset = raw.get("reset_offset")
    if (reset_offset is not None
            and (not isinstance(reset_offset, int)
                 or reset_offset < 0
                 or reset_offset + 4 > length)):
        raise FirmwareError(f"layout {name!r} has invalid reset_offset")
    statistic_offset = raw.get("statistic_offset")
    if (statistic_offset is not None
            and (not isinstance(statistic_offset, int)
                 or statistic_offset < 0
                 or statistic_offset + 0x200 > length)):
        raise FirmwareError(f"layout {name!r} has invalid statistic_offset")
    entry_transfer_offset = raw.get("entry_transfer_offset")
    if (entry_transfer_offset is not None
            and (not isinstance(entry_transfer_offset, int)
                 or entry_transfer_offset < 0
                 or entry_transfer_offset + 4 > length)):
        raise FirmwareError(
            f"layout {name!r} has invalid entry_transfer_offset"
        )
    partition_boundaries = raw.get("partition_boundaries", [])
    if (not isinstance(partition_boundaries, list)
            or any(not isinstance(value, int)
                   or value <= 0 or value >= length
                   or value % 0x10000
                   for value in partition_boundaries)
            or len(partition_boundaries) != len(set(partition_boundaries))):
        raise FirmwareError(
            f"layout {name!r} has invalid partition_boundaries"
        )
    regions: list[LayoutRegion] = []
    for item in raw.get("regions", []):
        if not isinstance(item, dict):
            raise FirmwareError(f"layout {name!r} contains a non-mapping region")
        region_name = item.get("name")
        offset, region_length = item.get("offset"), item.get("length")
        if (not isinstance(region_name, str) or not isinstance(offset, int)
                or not isinstance(region_length, int) or offset < 0
                or region_length <= 0 or offset + region_length > length):
            raise FirmwareError(f"layout {name!r} has invalid region {item!r}")
        regions.append(LayoutRegion(region_name, offset, region_length))
    ordered_regions = sorted(regions, key=lambda value: value.offset)
    for index, region in enumerate(ordered_regions):
        if index and region.offset < ordered_regions[index - 1].end:
            raise FirmwareError(f"layout {name!r} has overlapping regions")
    interior_boundaries = [
        boundary
        for boundary in partition_boundaries
        if any(region.offset < boundary < region.end
               for region in ordered_regions)
    ]
    if interior_boundaries:
        rendered = ", ".join(
            f"0x{boundary:X}" for boundary in sorted(interior_boundaries)
        )
        raise FirmwareError(
            f"layout {name!r} partition_boundaries fall inside named "
            f"regions: {rendered}"
        )
    chips: list[LayoutChip] = []
    for item in raw.get("chips", []):
        if not isinstance(item, dict):
            raise FirmwareError(f"layout {name!r} contains a non-mapping chip")
        role = item.get("role")
        offset, chip_length = item.get("offset"), item.get("length")
        if (not isinstance(role, str) or not role
                or not isinstance(offset, int) or offset < 0
                or not isinstance(chip_length, int) or chip_length <= 0
                or offset + chip_length > length
                or not isinstance(item.get("owns_reset", False), bool)
                or not isinstance(item.get("owns_metadata", False), bool)):
            raise FirmwareError(f"layout {name!r} has invalid chip {item!r}")
        chips.append(LayoutChip(
            role, offset, chip_length,
            item.get("owns_reset", False),
            item.get("owns_metadata", False),
        ))
    ordered_chips = sorted(chips, key=lambda value: value.offset)
    if ordered_chips:
        cursor = 0
        for chip in ordered_chips:
            if chip.offset != cursor:
                raise FirmwareError(
                    f"layout {name!r} chips do not partition its flash"
                )
            cursor = chip.end
        if cursor != length or len({chip.role for chip in chips}) != len(chips):
            raise FirmwareError(
                f"layout {name!r} chips do not partition its flash"
            )
        reset_owners = [chip for chip in chips if chip.owns_reset]
        metadata_owners = [chip for chip in chips if chip.owns_metadata]
        if len(reset_owners) != 1 or len(metadata_owners) != 1:
            raise FirmwareError(
                f"layout {name!r} needs one reset and metadata owning chip"
            )
        if (reset_offset is None
                or not reset_owners[0].offset <= reset_offset
                < reset_owners[0].end):
            raise FirmwareError(
                f"layout {name!r} reset is outside its owning chip"
            )
    return FlashLayout(
        name, base, length, tuple(regions), reset_offset,
        tuple(sorted(partition_boundaries)), tuple(chips), statistic_offset,
        entry_transfer_offset,
    )


def load_flash_layout(path: Path, name: str) -> FlashLayout:
    """Load and validate one named layout from a YAML catalog."""
    _catalog_bytes, named_phones = _load_document(path)
    folded_name = name.casefold()
    matches = [
        phone for phone in named_phones
        if phone["name"].casefold() == folded_name
    ]
    if not matches:
        matches = [
            phone for phone in named_phones
            if folded_name in {
                alias.casefold() for alias in phone["name"].split("/")
            }
        ]
    if len(matches) > 1:
        candidates = ", ".join(repr(phone["name"]) for phone in matches)
        raise FirmwareError(
            f"layout model {name!r} matches multiple entries in {path}: "
            f"{candidates}"
        )
    if not matches:
        raise FirmwareError(
            f"layout {name!r} not found exactly once in {path}"
        )
    return _parse_layout(matches[0])


def load_layout_catalog(path: Path | None = None) -> LoadedLayoutCatalog:
    source = path or BUILTIN_LAYOUT_PATH
    catalog_bytes, phones = _load_document(source)
    return LoadedLayoutCatalog(
        layouts=tuple(_parse_layout(phone) for phone in phones),
        source=source,
        catalog_bytes=catalog_bytes,
    )


def load_layout(name: str, path: Path | None = None) -> LoadedLayout:
    source = path or BUILTIN_LAYOUT_PATH
    catalog_bytes = source.read_bytes()
    return LoadedLayout(
        layout=load_flash_layout(source, name),
        source=source,
        catalog_bytes=catalog_bytes,
    )


def resolve_statistics_locations(
    model: str,
    flash_size: int,
    statistic_offset: int,
    path: Path | None = None,
) -> StatisticsLocations:
    layout = load_layout(model, path).layout
    if layout.length != flash_size:
        raise FirmwareError(
            f"layout {layout.name!r} length 0x{layout.length:x} does not "
            f"match package flash size 0x{flash_size:x}"
        )
    if layout.statistic_offset != statistic_offset:
        rendered = (
            "none" if layout.statistic_offset is None
            else f"0x{layout.statistic_offset:x}"
        )
        raise FirmwareError(
            f"layout {layout.name!r} statistics offset {rendered} does not "
            f"match package value 0x{statistic_offset:x}"
        )
    if layout.entry_transfer_offset is None:
        raise FirmwareError(
            f"layout {layout.name!r} has no entry transfer offset"
        )
    return StatisticsLocations(
        layout.name, statistic_offset, layout.entry_transfer_offset
    )
