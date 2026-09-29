from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .normalization import normalize_partition, restore_normalization
from .mining import mine_fullflash
from .ranges import source_layout_intersection
from ..layout.catalog import LoadedLayout, load_layout
from ..layout.partitions import LayoutPartition, partition_layout
from ..firmware.xbi import FirmwareError, FlashLayout


SCHEMA = "siemens-fullflash-split"
SCHEMA_VERSION = 6
MANIFEST_NAME = "split-manifest.json"


@dataclass(frozen=True)
class SplitSlice:
    order: int
    filename: str
    label: str
    kind: str
    source_start: int
    source_end: int
    layout_start: int | None
    layout_end: int | None
    canonical_start: int | None
    canonical_end: int | None

    @property
    def size(self) -> int:
        return self.source_end - self.source_start

    @property
    def full_boundary_present(self) -> bool:
        return (
            self.layout_start is not None
            and self.layout_start == self.canonical_start
            and self.layout_end == self.canonical_end
        )


@dataclass(frozen=True)
class SplitPlan:
    layout: FlashLayout
    source_to_layout_shift: int
    slices: tuple[SplitSlice, ...]

    @property
    def mapped_bytes(self) -> int:
        return sum(item.size for item in self.slices
                   if item.kind != "outside-layout")

    @property
    def outside_layout_bytes(self) -> int:
        return sum(item.size for item in self.slices
                   if item.kind == "outside-layout")


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _is_erased(data: bytes) -> bool:
    return bool(data) and data.count(0xFF) == len(data)


def _range(start: int, end: int) -> dict[str, int]:
    return {"from": start, "to_exclusive": end, "length": end - start}


def _region_label(label: str) -> str:
    unknown = re.fullmatch(r"UNKNOWN_(\d+)", label)
    if unknown:
        return f"unknown{unknown.group(1)}"
    return re.sub(r"-+", "-", re.sub(r"[^a-z0-9]+", "-", label.lower())).strip("-")


def _mapped_filename(layout: FlashLayout, partition: LayoutPartition) -> str:
    start_page = (layout.base + partition.start) >> 16
    end_page = (layout.base + partition.end - 1) >> 16
    return (
        f"{start_page:02X}-{end_page:02X}_FF-"
        f"{_region_label(partition.label)}.bin"
    )


def _outside_filename(start: int, end: int) -> str:
    return (
        f"SOURCE-{start:08X}-{end - 1:08X}_FF-outside-layout.bin"
    )


def plan_split(source_length: int, layout: FlashLayout,
               source_to_layout_shift: int) -> SplitPlan:
    if source_length <= 0:
        raise FirmwareError("input is empty")
    source_start, source_end, layout_start, layout_end = (
        source_layout_intersection(
            source_length, layout.length, source_to_layout_shift
        )
    )
    if source_start == source_end:
        raise FirmwareError("input does not intersect the selected layout")

    slices: list[SplitSlice] = []
    if source_start:
        slices.append(SplitSlice(
            len(slices), _outside_filename(0, source_start),
            "outside-layout", "outside-layout",
            0, source_start, None, None, None, None,
        ))
    for partition in partition_layout(layout):
        start = max(layout_start, partition.start)
        end = min(layout_end, partition.end)
        if start >= end:
            continue
        slices.append(SplitSlice(
            len(slices), _mapped_filename(layout, partition),
            partition.label, partition.kind,
            start - source_to_layout_shift,
            end - source_to_layout_shift,
            start, end, partition.start, partition.end,
        ))
    if source_end < source_length:
        slices.append(SplitSlice(
            len(slices), _outside_filename(source_end, source_length),
            "outside-layout", "outside-layout",
            source_end, source_length, None, None, None, None,
        ))

    filenames = [item.filename for item in slices]
    if len(filenames) != len(set(filenames)):
        raise FirmwareError("split plan contains duplicate output filenames")
    cursor = 0
    for item in slices:
        if item.size <= 0 or item.source_start != cursor:
            raise FirmwareError("split plan does not cover the input exactly once")
        cursor = item.source_end
    if cursor != source_length:
        raise FirmwareError("split plan does not cover the input exactly once")
    return SplitPlan(layout, source_to_layout_shift, tuple(slices))


def _atomic_write(path: Path, data: bytes) -> None:
    fd, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".part", dir=path.parent
    )
    try:
        with os.fdopen(fd, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def _old_generated_files(manifest_path: Path) -> set[str]:
    if not manifest_path.exists():
        return set()
    if not manifest_path.is_file():
        raise FirmwareError(f"output path is not a file: {manifest_path}")
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid existing split manifest: {exc}") from exc
    if (
        not isinstance(document, dict)
        or document.get("schema") != SCHEMA
        or document.get("schema_version") != SCHEMA_VERSION
        or not isinstance(document.get("slices"), list)
    ):
        raise FirmwareError("existing split manifest has an unsupported schema")
    generated = list(document["slices"])
    result = set()
    for item in generated:
        if isinstance(item, dict):
            has_filename = "filename" in item
            has_erased = "erased" in item
            if has_filename == has_erased:
                raise FirmwareError(
                    "existing split slice must have exactly one storage form"
                )
            if has_erased:
                if item["erased"] is not True:
                    raise FirmwareError(
                        "existing split erased marker must be true"
                    )
                continue
        filename = item.get("filename") if isinstance(item, dict) else None
        if (
            not isinstance(filename, str)
            or Path(filename).name != filename
            or filename in ("", ".", "..")
        ):
            raise FirmwareError("existing split manifest has an unsafe filename")
        result.add(filename)
    return result


def _slice_manifest(
    data: bytes,
    item: SplitSlice,
    layout: FlashLayout,
    audit: dict[str, Any],
) -> tuple[dict[str, Any], bytes]:
    source_payload = data[item.source_start:item.source_end]
    payload = source_payload
    normalization = None
    if item.canonical_start is not None and item.canonical_end is not None:
        payload, normalization = normalize_partition(
            payload,
            layout,
            role=item.label,
            partition_start=item.canonical_start,
            partition_end=item.canonical_end,
            full_boundary_present=item.full_boundary_present,
            bcore_metadata=audit.get("bcore", {}).get("selected"),
        )
    result: dict[str, Any] = {
        "order": item.order,
        "logical_label": item.label,
        "kind": item.kind,
        "size": item.size,
        "sha256": _sha256(payload),
        "source_sha256": _sha256(source_payload),
        "source_range": _range(item.source_start, item.source_end),
        "full_boundary_present": item.full_boundary_present,
    }
    erased = item.kind != "outside-layout" and _is_erased(source_payload)
    if erased:
        result["erased"] = True
    else:
        result["filename"] = item.filename
        result["normalization"] = normalization
    if item.layout_start is not None and item.layout_end is not None:
        assert item.canonical_start is not None
        assert item.canonical_end is not None
        result.update({
            "actual_layout_range": _range(
                item.layout_start, item.layout_end
            ),
            "actual_native_range": _range(
                layout.base + item.layout_start,
                layout.base + item.layout_end,
            ),
            "canonical_layout_range": _range(
                item.canonical_start, item.canonical_end
            ),
            "canonical_native_range": _range(
                layout.base + item.canonical_start,
                layout.base + item.canonical_end,
            ),
        })
    else:
        result.update({
            "actual_layout_range": None,
            "actual_native_range": None,
            "canonical_layout_range": None,
            "canonical_native_range": None,
        })
    return result, payload


def _canonical_input(
        data: bytes, audit: dict[str, Any], layout: FlashLayout,
) -> tuple[bytes, str]:
    chip_mapping = (
        audit.get("layout_resolution", {}).get("selected", {})
        .get("chip_mapping")
    )
    if chip_mapping is None:
        return data, "source-order"
    canonical = bytearray(b"\xFF") * layout.length
    covered = bytearray(layout.length)
    for segment in chip_mapping["segments"]:
        source = segment["source"]
        logical = segment["layout"]
        payload = data[source["from"]:source["to"]]
        if len(payload) != logical["to"] - logical["from"]:
            raise FirmwareError("dual-chip segment length mismatch")
        canonical[logical["from"]:logical["to"]] = payload
        covered[logical["from"]:logical["to"]] = b"\x01" * len(payload)
    if not all(covered):
        raise FirmwareError("dual-chip mapping does not cover the layout")
    return bytes(canonical), "logical-address-order"


def _reset_manifest(
        layout: FlashLayout, partitions: tuple[LayoutPartition, ...],
) -> dict[str, Any] | None:
    if layout.reset_offset is None:
        return None
    owner = next(
        partition for partition in partitions
        if partition.start <= layout.reset_offset < partition.end
    )
    return {
        "storage": (
            "native" if layout.base + layout.reset_offset == 0
            else "low-alias"
        ),
        "layout_offset": layout.reset_offset,
        "native_address": layout.base + layout.reset_offset,
        "owning_slice": _mapped_filename(layout, owner),
        "duplicates_slice_bytes": False,
    }


def write_split(
        data: bytes, output_dir: Path,
        loaded_layout: LoadedLayout, audit: dict[str, Any],
        plan: SplitPlan, force: bool = False,
        normalization: str = "source-order") -> Path:
    manifest_path = output_dir / MANIFEST_NAME
    if output_dir.exists() and not force:
        raise FirmwareError(
            f"output directory already exists: {output_dir} (use --force)"
        )
    if output_dir.exists() and not output_dir.is_dir():
        raise FirmwareError(f"output path is not a directory: {output_dir}")

    rendered = [
        _slice_manifest(data, item, plan.layout, audit)
        for item in plan.slices
    ]
    old_generated = (
        _old_generated_files(manifest_path)
        if output_dir.exists() and force else set()
    )
    planned = {
        document["filename"] for document, _payload in rendered
        if "filename" in document
    }
    for filename in planned | old_generated | {MANIFEST_NAME}:
        path = output_dir / filename
        if path.exists() and not path.is_file():
            raise FirmwareError(f"output path is not a file: {path}")

    slice_documents = [document for document, _payload in rendered]
    materialized_payloads = [
        payload for document, payload in rendered
        if "filename" in document
    ]
    restored_payloads = [
        (
            restore_normalization(
                payload,
                document.get("normalization"),
                context=(
                    f"split slice "
                    f"{document.get('filename', document['logical_label'])}"
                ),
            )
            if document.get("normalization") is not None else payload
        )
        for document, payload in rendered
    ]
    round_trip = b"".join(restored_payloads)
    if round_trip != data:
        raise FirmwareError("split plan failed byte-exact round-trip validation")
    selected = audit["layout_resolution"]["selected"]
    partitions = partition_layout(plan.layout)
    manifest = {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "input": {
            "size": len(data),
            "sha256": _sha256(data),
            "content_type": audit["content_type"],
        },
        "layout": {
            "catalog_path": loaded_layout.source.name,
            "catalog_sha256": _sha256(loaded_layout.catalog_bytes),
            "name": plan.layout.name,
            "base": plan.layout.base,
            "length": plan.layout.length,
            "partition_boundaries": list(plan.layout.partition_boundaries),
        },
        "placement": {
            "selection_method": selected["selection"],
            "source_to_layout_shift": plan.source_to_layout_shift,
            "normalization": normalization,
        },
        "output": {
            "slice_count": len(plan.slices),
            "materialized_file_count": len(materialized_payloads),
            "mapped_bytes": plan.mapped_bytes,
            "outside_layout_bytes": plan.outside_layout_bytes,
            "round_trip_sha256": _sha256(round_trip),
            "materialized_payloads_sha256": _sha256(
                b"".join(materialized_payloads)
            ),
        },
        "reset": _reset_manifest(plan.layout, partitions),
        "slices": slice_documents,
    }
    manifest_data = (
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")

    output_dir.mkdir(parents=True, exist_ok=True)
    for document, payload in rendered:
        if "filename" not in document:
            continue
        _atomic_write(
            output_dir / document["filename"],
            payload,
        )
    for filename in old_generated - planned:
        stale = output_dir / filename
        if stale.exists():
            stale.unlink()
    _atomic_write(manifest_path, manifest_data)
    return manifest_path


def command_split(args: argparse.Namespace) -> None:
    if args.layout_file and not args.layout:
        raise FirmwareError("--layout-file requires --layout")
    if args.file_offset and args.flash_address is None:
        raise FirmwareError("--file-offset requires --flash-address")
    data = args.input.read_bytes()
    if not data:
        raise FirmwareError("input is empty")
    requested_layout = (
        load_layout(args.layout, args.layout_file) if args.layout else None
    )
    audit = mine_fullflash(
        data,
        layout=requested_layout.layout if requested_layout else None,
        layout_file=args.layout_file,
        flash_address=args.flash_address,
        file_offset=args.file_offset,
    )
    if audit["content_type"] != "bin":
        raise FirmwareError(
            f"input is not a raw firmware capture: {audit['content_type']}"
        )
    selected = audit["layout_resolution"]["selected"]
    if selected is None:
        raise FirmwareError("input placement is unresolved or ambiguous")
    loaded_layout = requested_layout or load_layout(
        selected["layout"], args.layout_file
    )
    split_data, normalization = _canonical_input(
        data, audit, loaded_layout.layout
    )
    plan = plan_split(
        len(split_data), loaded_layout.layout,
        0 if normalization == "logical-address-order" else selected["shift"],
    )
    output_dir = args.output or Path(
        str(args.input.with_suffix("")) + ".split"
    )
    manifest_path = write_split(
        split_data, output_dir, loaded_layout, audit, plan, args.force,
        normalization,
    )
    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    for item in document["slices"]:
        if "filename" in item:
            print(output_dir / item["filename"])
    print(manifest_path)
