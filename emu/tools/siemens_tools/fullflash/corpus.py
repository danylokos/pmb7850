from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

from .normalization import normalize_partition, restore_normalization
from .corpus_scope import (
    T9_HEADER_SIZE,
    atomic_relative_symlink,
    embedded_t9_version,
    lg_collapse_evidence,
    normalize_lg,
    payload_paths,
    resolve_scope_references,
    scope_key,
    scoped_payload_plan,
    select_lg_scope,
    select_payload_path,
    software_version,
    t9_version,
)
from ..layout.catalog import LoadedLayout, load_layout
from .mining import mine_fullflash, scan_firmware_metadata
from .split import (
    _canonical_input,
    plan_split,
)
from ..layout.partitions import partition_layout
from .reconstruct import SCHEMA as RECIPE_SCHEMA
from .reconstruct import SCHEMA_VERSION as RECIPE_SCHEMA_VERSION
from ..firmware.compression import materialize_xbi
from ..firmware.inspection import detect_content_type
from ..firmware.xbi import FirmwareError, is_xbi, parse_xbi


SCHEMA = "siemens-community-fullflash-corpus"
SCHEMA_VERSION = 14
LANGUAGE_ROLES = {"LangPack", "T9"}
SOURCE_SUFFIXES = {".bin", ".fls"}
OFFICIAL_SUFFIXES = {
    ".xbb", ".xbi", ".xbz", ".xfs", ".xci", ".exbi", ".exci",
}
STANDALONE_ROLES = {"BCORE", "T9", "EEPROM"}
LANGPACK_LG_OFFSET = 0x15
LANGPACK_LG_FIELD_END = 0x20
FIRMWARE_METADATA_OFFSET = 0x7FF50
FIRMWARE_METADATA_SIZE = 0x38
QUARANTINED_RESET_STATUSES = {
    "invalid-opcode",
    "erased",
    "target-erased",
    "target-outside-layout",
}


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _is_erased(data: bytes) -> bool:
    return bool(data) and data.count(0xFF) == len(data)


def _range(start: int, end: int) -> dict[str, int]:
    return {"from": start, "to_exclusive": end, "length": end - start}


def _display_path(path: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(Path.cwd().resolve()).as_posix()
    except ValueError:
        return resolved.as_posix()


def _inside_generated_tree(root: Path, path: Path) -> bool:
    relative = path.relative_to(root)
    return any(
        part.endswith(".split") or part in ("corpus", "slice-corpus")
        for part in relative.parts[:-1]
    )


def discover_sources(roots: Iterable[Path]) -> list[Path]:
    """Recursively discover raw candidates without re-ingesting generated trees."""
    roots = list(roots)
    if not roots:
        raise FirmwareError("at least one corpus input is required")
    found: dict[Path, Path] = {}
    for root in roots:
        if not root.exists():
            raise FirmwareError(f"corpus input does not exist: {root}")
        if root.is_file():
            candidates = (
                [root] if root.suffix.lower() in SOURCE_SUFFIXES else []
            )
        elif root.is_dir():
            candidates = (
                path for path in root.rglob("*")
                if path.is_file()
                and path.suffix.lower() in SOURCE_SUFFIXES
                and not _inside_generated_tree(root, path)
            )
        else:
            candidates = []
        for path in candidates:
            found.setdefault(path.resolve(), path)
    if not found:
        raise FirmwareError("corpus inputs contain no .bin or .fls candidates")
    return sorted(found.values(), key=lambda path: path.as_posix())


def _find_collection_manifest(roots: Iterable[Path]) -> Path | None:
    candidates: set[Path] = set()
    for root in roots:
        start = root if root.is_dir() else root.parent
        for ancestor in (start, *start.parents):
            candidate = ancestor / "FULLFLASH_COLLECTION.json"
            if candidate.is_file():
                candidates.add(candidate.resolve())
    if len(candidates) > 1:
        rendered = ", ".join(path.as_posix() for path in sorted(candidates))
        raise FirmwareError(f"multiple collection manifests found: {rendered}")
    return next(iter(candidates), None)


def _load_collection_provenance(
    roots: Iterable[Path], explicit: Path | None,
) -> tuple[dict[Path, dict[str, Any]], str | None]:
    manifest_path = explicit or _find_collection_manifest(roots)
    if manifest_path is None:
        return {}, None
    try:
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(
            f"invalid collection manifest {manifest_path}: {exc}"
        ) from exc
    artifacts = document.get("artifacts")
    if not isinstance(artifacts, list):
        raise FirmwareError("collection manifest has no artifacts array")
    base = manifest_path.parent / "Fullflash"
    result: dict[Path, dict[str, Any]] = {}
    retained = (
        "role", "software_version", "version_evidence", "langpack",
        "associated_capture", "provenance", "duplicate_count",
        "technical_hints", "warnings",
    )
    for artifact in artifacts:
        canonical = artifact.get("canonical_path")
        if not isinstance(canonical, str):
            continue
        result[(base / canonical).resolve()] = {
            key: artifact.get(key) for key in retained if key in artifact
        }
    return result, _display_path(manifest_path)


def _model_matches(layout_name: str, model: str | None) -> bool:
    aliases = {alias.upper() for alias in layout_name.split("/")}
    return model is not None and model.upper() in aliases


def _standalone_role(
    data: bytes, audit: dict[str, Any], loaded_layout: LoadedLayout,
) -> tuple[str | None, str]:
    layout = loaded_layout.layout
    matching = [
        part for part in partition_layout(layout)
        if part.label in STANDALONE_ROLES and part.end - part.start == len(data)
    ]
    selected = audit["layout_resolution"]["selected"]
    if selected is None:
        return None, "placement-unresolved"
    matching = [part for part in matching if selected["shift"] == part.start]
    if len(matching) != 1:
        sizes = sorted({
            part.end - part.start for part in partition_layout(layout)
            if part.label in STANDALONE_ROLES
        })
        if len(data) not in sizes:
            return None, "non-canonical-region-size"
        return None, "placement-does-not-identify-one-canonical-region"

    role = matching[0].label
    if role == "BCORE":
        bcore = audit.get("bcore", {}).get("selected")
        reset = audit.get("reset", {}).get("status")
        if bcore is None or not _model_matches(layout.name, bcore.get("model")):
            return None, "bcore-structure-invalid"
        owns_reset = (
            layout.reset_offset is not None
            and matching[0].start <= layout.reset_offset < matching[0].end
        )
        if owns_reset and reset != "valid":
            return None, f"bcore-reset-{reset or 'missing'}"
    elif role == "T9":
        if not data.startswith(b"T9"):
            return None, "t9-marker-missing-at-region-start"
    elif role == "EEPROM":
        eeprom = audit.get("eeprom", {})
        if eeprom.get("status") != "parsed":
            return None, "eeprom-parser-validation-failed"
        if eeprom.get("region_size") != len(data):
            return None, "eeprom-geometry-is-not-canonical"
    return role, "accepted"


def _placement_is_evidenced(
    audit: dict[str, Any], role: str | None = None,
) -> bool:
    selected = audit.get("layout_resolution", {}).get("selected")
    if not isinstance(selected, dict):
        return False
    if selected.get("model_bearing") is True:
        return True
    families = set(selected.get("families", []))
    if len(families) >= 2:
        return True
    if role == "BCORE":
        return audit.get("bcore", {}).get("selected") is not None
    if role == "EEPROM":
        return audit.get("eeprom", {}).get("status") == "parsed"
    return bool({"bcore", "eeprom"} & families)


def _reset_quarantine(
    audit: dict[str, Any], loaded_layout: LoadedLayout,
) -> list[dict[str, Any]]:
    reset = audit.get("reset", {})
    reason = reset.get("status")
    offset = loaded_layout.layout.reset_offset
    if reason not in QUARANTINED_RESET_STATUSES or offset is None:
        return []
    owner = next(
        part for part in partition_layout(loaded_layout.layout)
        if part.start <= offset < part.end
    )
    return [{
        "role": owner.label,
        "layout_range": _range(owner.start, owner.end),
        "native_range": _range(
            loaded_layout.layout.base + owner.start,
            loaded_layout.layout.base + owner.end,
        ),
        "reset_reason": reason,
    }]


def _source_metadata(audit: dict[str, Any]) -> dict[str, Any]:
    metadata = audit.get("metadata")
    eeprom_findings = [
        finding.get("evidence", [])
        for finding in audit.get("findings", [])
        if finding.get("kind") == "eeprom"
    ]
    return {
        "firmware": metadata,
        "reset": audit.get("reset"),
        "cold_boot": audit.get("cold_boot"),
        "bcore": audit.get("bcore", {}).get("selected"),
        "statistics": audit.get("statistics"),
        "eeprom": {
            key: value for key, value in audit.get("eeprom", {}).items()
            if key not in ("findings", "selected", "blocks")
        },
        "eeprom_parser_evidence": eeprom_findings,
    }


def _eeprom_snr_label(metadata: object) -> str | None:
    if not isinstance(metadata, dict):
        return None
    eeprom = metadata.get("eeprom")
    if not isinstance(eeprom, dict):
        return None
    imeis = eeprom.get("imeis")
    if not isinstance(imeis, dict) or not imeis:
        return None
    values = list(imeis.values())
    if any(
        not isinstance(value, str)
        or len(value) != 14
        or not value.isascii()
        or not value.isdigit()
        for value in values
    ):
        return None
    unique = set(values)
    if len(unique) != 1:
        return None
    return next(iter(unique))[-6:]


def _eeprom_labels_by_scope(
    occurrences: Iterable[dict[str, Any]],
    artifacts_by_id: dict[str, dict[str, Any]],
    reference_scopes: dict[str, dict[str, Any]] | None = None,
) -> dict[tuple[int | None, str | None, int | None], set[str]]:
    labels: dict[
        tuple[int | None, str | None, int | None], set[str]
    ] = defaultdict(set)
    for occurrence in occurrences:
        artifact = artifacts_by_id.get(occurrence.get("source_id"))
        label = _eeprom_snr_label(
            artifact.get("metadata") if artifact is not None else None
        )
        if label is None:
            continue
        scope = (reference_scopes or {}).get(
            occurrence["id"], occurrence["scope"]
        )
        labels[scope_key(
            scope.get("software_version"),
            scope.get("langpack"),
            scope.get("t9_version"),
            "eeprom",
        )].add(label)
    return dict(labels)


def _classify_source(
    path: Path, loaded_layout: LoadedLayout,
    collection: dict[Path, dict[str, Any]],
) -> tuple[dict[str, Any] | None, dict[str, Any] | None, bytes | None,
           dict[str, Any] | None]:
    data = path.read_bytes()
    provenance = collection.get(path.resolve(), {})
    excluded = {
        "path": _display_path(path),
        "size": len(data),
        "sha256": _sha256(data),
        "collection": provenance or None,
    }
    if data.startswith(b"MRT"):
        excluded["reason"] = "proprietary-martech-backup"
        return None, excluded, None, None
    try:
        content_type = detect_content_type(data)
    except FirmwareError:
        content_type = "bin"
    if content_type != "bin":
        excluded["reason"] = f"content-type-{content_type}"
        return None, excluded, None, None

    audit = mine_fullflash(
        data,
        layout=loaded_layout.layout,
        layout_file=loaded_layout.source,
    )
    selected = audit["layout_resolution"]["selected"]
    if len(data) == loaded_layout.layout.length:
        metadata = audit.get("metadata")
        representation = audit.get("representation", {})
        if (
            selected is None
            or representation.get("kind") != "exact-fullflash"
            or representation.get("status") != "mapped"
        ):
            excluded["reason"] = "complete-image-placement-invalid"
            return None, excluded, None, audit
        if metadata is None or not _model_matches(
            loaded_layout.layout.name, metadata.get("model")
        ):
            excluded["reason"] = "complete-image-metadata-invalid"
            return None, excluded, None, audit
        quarantined = _reset_quarantine(audit, loaded_layout)
        artifact = {
            "kind": (
                "partial-fullflash" if quarantined
                else "complete-fullflash"
            ),
            "path": _display_path(path),
            "size": len(data),
            "sha256": _sha256(data),
            "collection": provenance or None,
            "metadata": _source_metadata(audit),
            "coverage": audit.get("coverage"),
            "quarantined_regions": quarantined,
        }
        return artifact, None, data, audit

    role, reason = _standalone_role(data, audit, loaded_layout)
    if role is not None:
        artifact = {
            "kind": "standalone-region",
            "role": role,
            "path": _display_path(path),
            "size": len(data),
            "sha256": _sha256(data),
            "collection": provenance or None,
            "metadata": _source_metadata(audit),
            "coverage": audit.get("coverage"),
            "quarantined_regions": [],
        }
        return artifact, None, data, audit

    if selected is None:
        excluded["reason"] = reason
        return None, excluded, None, audit
    slices, _payloads = _source_slices(data, audit, loaded_layout)
    complete_slices = [
        item for item in slices
        if item["kind"] != "outside-layout"
        and item["full_boundary_present"]
    ]
    if not complete_slices:
        excluded["reason"] = "partial-capture-has-no-complete-region"
        return None, excluded, None, audit
    if not _placement_is_evidenced(audit):
        excluded["reason"] = "partial-placement-insufficient-evidence"
        return None, excluded, None, audit
    artifact = {
        "kind": "partial-fullflash",
        "path": _display_path(path),
        "size": len(data),
        "sha256": _sha256(data),
        "collection": provenance or None,
        "metadata": _source_metadata(audit),
        "coverage": audit.get("coverage"),
        "quarantined_regions": _reset_quarantine(audit, loaded_layout),
    }
    return artifact, None, data, audit


def _source_slices(
    data: bytes, audit: dict[str, Any], loaded_layout: LoadedLayout,
) -> tuple[list[dict[str, Any]], dict[tuple[str, str], bytes]]:
    split_data, normalization = _canonical_input(
        data, audit, loaded_layout.layout
    )
    plan = plan_split(
        len(split_data), loaded_layout.layout,
        0 if normalization == "logical-address-order"
        else audit["layout_resolution"]["selected"]["shift"],
    )
    expected: list[dict[str, Any]] = []
    payloads: dict[tuple[str, str], bytes] = {}
    chip_mapping = (
        audit.get("layout_resolution", {}).get("selected", {})
        .get("chip_mapping")
    )
    for item in plan.slices:
        source_start, source_end = item.source_start, item.source_end
        if chip_mapping is not None and item.layout_start is not None:
            segment = next(
                segment for segment in chip_mapping["segments"]
                if (
                    segment["layout"]["from"] <= item.layout_start
                    and item.layout_end <= segment["layout"]["to"]
                )
            )
            source_start = (
                segment["source"]["from"]
                + item.layout_start - segment["layout"]["from"]
            )
            source_end = source_start + item.size
            source_payload = data[source_start:source_end]
        else:
            source_payload = split_data[item.source_start:item.source_end]
        payload = source_payload
        normalization = None
        if item.canonical_start is not None and item.canonical_end is not None:
            payload, normalization = normalize_partition(
                source_payload,
                loaded_layout.layout,
                role=item.label,
                partition_start=item.canonical_start,
                partition_end=item.canonical_end,
                full_boundary_present=item.full_boundary_present,
                bcore_metadata=audit.get("bcore", {}).get("selected"),
            )
        digest = _sha256(payload)
        row: dict[str, Any] = {
            "order": item.order,
            "logical_label": item.label,
            "kind": item.kind,
            "size": item.size,
            "sha256": digest,
            "source_sha256": _sha256(source_payload),
            "normalization": normalization,
            "source_range": _range(source_start, source_end),
            "full_boundary_present": item.full_boundary_present,
            "actual_layout_range": (
                _range(item.layout_start, item.layout_end)
                if item.layout_start is not None and item.layout_end is not None
                else None
            ),
            "actual_native_range": (
                _range(
                    loaded_layout.layout.base + item.layout_start,
                    loaded_layout.layout.base + item.layout_end,
                )
                if item.layout_start is not None and item.layout_end is not None
                else None
            ),
            "canonical_layout_range": (
                _range(item.canonical_start, item.canonical_end)
                if item.canonical_start is not None and item.canonical_end is not None
                else None
            ),
            "canonical_native_range": (
                _range(
                    loaded_layout.layout.base + item.canonical_start,
                    loaded_layout.layout.base + item.canonical_end,
                )
                if item.canonical_start is not None and item.canonical_end is not None
                else None
            ),
        }
        if _is_erased(source_payload):
            row["erased"] = True
        expected.append(row)
        payloads[(item.label, digest)] = payload
    restored = b"".join(
        (
            restore_normalization(
                payloads[(item["logical_label"], item["sha256"])],
                item["normalization"],
                context=f"corpus slice {item['logical_label']}",
            )
            if item["normalization"] is not None
            else payloads[(item["logical_label"], item["sha256"])]
        )
        for item in sorted(
            expected, key=lambda value: value["source_range"]["from"]
        )
    )
    if restored != data:
        raise FirmwareError("corpus slice plan failed byte-exact reconstruction")
    return expected, payloads


def _role_slug(role: str) -> str:
    if match := re.fullmatch(r"UNKNOWN_(\d+)", role):
        return f"unknown_{match.group(1)}"
    if role == "FFS(A)":
        return "ffs_a"
    return re.sub(r"[^a-z0-9]+", "_", role.lower()).strip("_")


def _short_hashes(hashes: Iterable[str], minimum: int = 12) -> dict[str, str]:
    values = sorted(set(hashes))
    result: dict[str, str] = {}
    for digest in values:
        length = minimum
        while any(
            other != digest and other.startswith(digest[:length])
            for other in values
        ):
            length += 1
        result[digest] = digest[:length]
    return result


def _software_version(value: object) -> int | None:
    return software_version(value)


def _path_software_version(value: object) -> int | None:
    if not isinstance(value, str):
        return None
    path = Path(value)
    for part in reversed(path.parts[:-1]):
        match = re.fullmatch(r"(?:v\.)?(\d{1,3})", part, re.IGNORECASE)
        if match is not None:
            return software_version(match.group(1))
    for pattern in (
        r"(?:^|[_-])SW(\d{1,3})(?:[_-]|$)",
        r"(?:^|[_-])(\d{1,3})(?:[_-]|$)",
    ):
        match = re.search(pattern, path.stem, re.IGNORECASE)
        if match is not None:
            return software_version(match.group(1))
    return None


def _bcore_software_scope(artifact: dict[str, Any]) -> tuple[int | None, str]:
    bcore = artifact["metadata"].get("bcore") or {}
    embedded = _software_version(bcore.get("software_version"))
    if embedded is not None:
        return embedded, "embedded-bcore"

    collection = artifact.get("collection") or {}
    declared = _software_version(collection.get("software_version"))
    if declared is not None:
        return declared, "collection-metadata"

    firmware = artifact["metadata"].get("firmware") or {}
    parent = _software_version(firmware.get("software_version"))
    if parent is not None:
        return parent, "fullflash-metadata"

    for provenance in collection.get("provenance", []):
        if not isinstance(provenance, dict):
            continue
        for key in ("member_name", "source_archive"):
            hinted = _path_software_version(provenance.get(key))
            if hinted is not None:
                return hinted, f"collection-{key}"

    hinted = _path_software_version(artifact.get("path"))
    if hinted is not None:
        return hinted, "source-path"
    return None, "unknown"


def _langpack_lg(data: bytes) -> str | None:
    """Return the fixed LangPack-header LG, rejecting scans and loose matches."""
    if len(data) < LANGPACK_LG_FIELD_END or data[:2] != b"\xBB\xBB":
        return None
    match = re.fullmatch(
        rb"@(lg[0-9]+)\x00\xFF*",
        data[LANGPACK_LG_OFFSET:LANGPACK_LG_FIELD_END],
    )
    return match.group(1).decode("ascii") if match is not None else None


def _materialized_firmware_lg(
    flash: bytes, ownership: bytes,
) -> str | None:
    end = FIRMWARE_METADATA_OFFSET + FIRMWARE_METADATA_SIZE
    if len(flash) < end or not all(ownership[FIRMWARE_METADATA_OFFSET:end]):
        return None
    for finding in scan_firmware_metadata(flash):
        if int(finding["file_range"]["from"], 16) != FIRMWARE_METADATA_OFFSET:
            continue
        return normalize_lg(finding["metadata"].get("langpack"))
    return None


def _official_payloads(root: Path) -> list[tuple[Path, int | None, bytes]]:
    found: list[tuple[Path, int | None, bytes]] = []
    if not root.exists():
        raise FirmwareError(f"official package root does not exist: {root}")
    paths = [root] if root.is_file() else sorted(
        (
            path for path in root.rglob("*")
            if path.is_file() and path.suffix.lower() in OFFICIAL_SUFFIXES
        ),
        key=lambda path: path.as_posix(),
    )
    for path in paths:
        raw = path.read_bytes()
        if is_xbi(raw):
            found.append((path, None, raw))
    return found


def _official_analysis(
    roots: Iterable[Path], loaded_layout: LoadedLayout,
) -> tuple[
    dict[tuple[str, str], list[dict[str, Any]]],
    dict[str, int],
    dict[str, list[dict[str, Any]]],
]:
    matches: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    coverage_grouped: dict[
        tuple[str, str, str], dict[str, Any]
    ] = {}
    seen_payloads: set[str] = set()
    parsed = 0
    for root in roots:
        for path, payload_index, payload in _official_payloads(root):
            payload_sha = _sha256(payload)
            if payload_sha in seen_payloads:
                continue
            seen_payloads.add(payload_sha)
            info = parse_xbi(payload)
            if info.get("flash_size") != loaded_layout.layout.length:
                continue
            model = info.get("model")
            normal_model = (
                model is not None
                and _model_matches(loaded_layout.layout.name, str(model))
            )
            boot_model = (
                model is not None and str(model).startswith("BOOTL55")
            )
            if not normal_model and not boot_model:
                continue
            materialized = materialize_xbi(payload, info)
            action = bytes(
                write or erase for write, erase
                in zip(materialized.write_mask, materialized.erase_mask)
            )
            langpack_part = next(
                (
                    part for part in partition_layout(loaded_layout.layout)
                    if part.label == "LangPack"
                ),
                None,
            )
            embedded_langpack = None
            if langpack_part is not None and all(
                action[
                    langpack_part.start:
                    langpack_part.start + LANGPACK_LG_FIELD_END
                ]
            ):
                embedded_langpack = _langpack_lg(
                    materialized.flash[
                        langpack_part.start:langpack_part.end
                    ]
                )
            declared_langpack = normalize_lg(info.get("langpack"))
            materialized_langpack = _materialized_firmware_lg(
                materialized.flash, action
            )
            primary_langpack = materialized_langpack or declared_langpack
            primary_langpack_source = (
                "fullflash-metadata" if materialized_langpack is not None
                else "package-metadata" if declared_langpack is not None
                else "unknown"
            )
            t9_part = next(
                (
                    part for part in partition_layout(loaded_layout.layout)
                    if part.label == "T9"
                ),
                None,
            )
            embedded_t9 = None
            if t9_part is not None and all(
                action[
                    t9_part.start:
                    t9_part.start + T9_HEADER_SIZE
                ]
            ):
                embedded_t9 = embedded_t9_version(
                    materialized.flash[t9_part.start:t9_part.end]
                )
            declared_t9 = t9_version(info.get("t9"))
            package = {
                "path": _display_path(path),
                "payload_index": payload_index,
                "payload_sha256": payload_sha,
                "package_kind": path.suffix.lower().removeprefix("."),
                "software_version": info.get("svn"),
                "langpack": info.get("langpack"),
                "embedded_langpack": embedded_langpack,
                "primary_langpack": primary_langpack,
                "primary_langpack_source": primary_langpack_source,
                "embedded_t9_version": embedded_t9,
                "declared_t9_version": declared_t9,
                "scope_t9_version": (
                    embedded_t9 if embedded_t9 is not None else declared_t9
                ),
                "format_version": info.format_version,
            }
            for partition in partition_layout(loaded_layout.layout):
                partition_action = action[partition.start:partition.end]
                covered = partition_action.count(1)
                if covered:
                    coverage = (
                        "full"
                        if covered == partition.end - partition.start
                        else "partial"
                    )
                    kind = path.suffix.lower().removeprefix(".")
                    key = (partition.label, kind, coverage)
                    row = coverage_grouped.setdefault(key, {
                        "package_kind": kind,
                        "coverage": coverage,
                        "package_count": 0,
                        "covered_bytes_min": covered,
                        "covered_bytes_max": covered,
                        "declared_software_versions": set(),
                        "declared_langpacks": set(),
                    })
                    row["package_count"] += 1
                    row["covered_bytes_min"] = min(
                        row["covered_bytes_min"], covered
                    )
                    row["covered_bytes_max"] = max(
                        row["covered_bytes_max"], covered
                    )
                    if info.get("svn") is not None:
                        row["declared_software_versions"].add(info.get("svn"))
                    if info.get("langpack") is not None:
                        row["declared_langpacks"].add(
                            str(info.get("langpack"))
                        )
                if normal_model and covered == partition.end - partition.start:
                    region_payload = materialized.flash[
                        partition.start:partition.end
                    ]
                    source_digest = _sha256(region_payload)
                    normalization = None
                    bcore_metadata = None
                    if partition.label == "BCORE":
                        region_audit = mine_fullflash(
                            region_payload,
                            layout=loaded_layout.layout,
                            layout_file=loaded_layout.source,
                        )
                        bcore_metadata = (
                            region_audit.get("bcore", {}).get("selected")
                        )
                    region_payload, normalization = normalize_partition(
                        region_payload,
                        loaded_layout.layout,
                        role=partition.label,
                        partition_start=partition.start,
                        partition_end=partition.end,
                        full_boundary_present=True,
                        bcore_metadata=bcore_metadata,
                    )
                    digest = _sha256(region_payload)
                    match = dict(package)
                    (
                        match["scope_langpack"],
                        match["scope_langpack_source"],
                    ) = select_lg_scope(
                        partition.label,
                        primary_langpack,
                        embedded_langpack,
                        primary_source=primary_langpack_source,
                    )
                    if partition.label == "BCORE":
                        bcore = region_audit.get("bcore", {}).get("selected")
                        match["embedded_bcore_software_version"] = (
                            (bcore or {}).get("software_version")
                        )
                    match["source_region_sha256"] = source_digest
                    match["stored_region_sha256"] = digest
                    match["normalization"] = normalization
                    matches[(partition.label, digest)].append(match)
            if normal_model:
                parsed += 1
    for packages in matches.values():
        packages.sort(key=lambda item: (
            item["path"], item["payload_index"] if item["payload_index"] is not None else -1
        ))
    summary = {
        "unique_payloads_parsed": parsed,
        "exact_region_payloads": len(matches),
    }
    result: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for (role, _kind, _coverage), row in coverage_grouped.items():
        row["declared_software_versions"] = sorted(
            row["declared_software_versions"]
        )
        row["declared_langpacks"] = sorted(row["declared_langpacks"])
        result[role].append(row)
    for rows in result.values():
        rows.sort(key=lambda item: (
            item["package_kind"], item["coverage"],
        ))
    return matches, summary, dict(result)


def _official_attribution(
    roots: Iterable[Path], loaded_layout: LoadedLayout,
) -> tuple[dict[tuple[str, str], list[dict[str, Any]]], dict[str, int]]:
    matches, summary, _coverage = _official_analysis(roots, loaded_layout)
    return matches, summary


def _official_coverage(
    roots: Iterable[Path], loaded_layout: LoadedLayout,
) -> dict[str, list[dict[str, Any]]]:
    _matches, _summary, coverage = _official_analysis(roots, loaded_layout)
    return coverage


def _official_catalog_analysis(
    catalog_path: Path | None, loaded_layout: LoadedLayout,
) -> tuple[
    dict[tuple[str, str], list[dict[str, Any]]],
    dict[str, int],
    dict[str, list[dict[str, Any]]],
    dict[tuple[str, str], set[str]],
]:
    if catalog_path is None:
        return {}, {"unique_payloads_parsed": 0, "exact_region_payloads": 0}, {}, {}
    try:
        document = json.loads(catalog_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid official catalog {catalog_path}: {exc}") from exc
    if (
        document.get("schema") != "siemens-official-corpus"
        or document.get("schema_version") != 9
    ):
        raise FirmwareError(f"official catalog is not schema 9: {catalog_path}")
    expected_layout = {
        "name": loaded_layout.layout.name,
        "base": loaded_layout.layout.base,
        "length": loaded_layout.layout.length,
        "catalog_sha256": _sha256(loaded_layout.catalog_bytes),
    }
    if any(
        document.get("layout", {}).get(field) != value
        for field, value in expected_layout.items()
    ):
        raise FirmwareError("community and official catalog layouts differ")

    packages = {
        package.get("sha256"): package
        for package in document.get("packages", [])
        if isinstance(package, dict) and isinstance(package.get("sha256"), str)
    }
    matches: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for region in document.get("regions", []):
        role = region.get("role")
        if not isinstance(role, str):
            continue
        for variant in region.get("variants", []):
            digest = variant.get("sha256")
            if not isinstance(digest, str) or variant.get("erased") is True:
                continue
            for occurrence in variant.get("occurrences", []):
                if not isinstance(occurrence, dict):
                    continue
                package_digest = occurrence.get("package_sha256")
                package = packages.get(package_digest, {})
                scope = occurrence.get("scope")
                if not isinstance(scope, dict):
                    scope = {}
                sources = package.get("sources", [])
                source = next(
                    (item for item in sources if isinstance(item, dict)), {}
                )
                match = {
                    "path": source.get("path", str(package_digest)),
                    "payload_index": None,
                    "payload_sha256": package_digest,
                    "package_kind": source.get("container_type", "catalog"),
                    "software_version": scope.get("software_version"),
                    "langpack": scope.get("langpack"),
                    "embedded_langpack": None,
                    "primary_langpack": scope.get("langpack"),
                    "primary_langpack_source": scope.get(
                        "langpack_source", "official-catalog"
                    ),
                    "embedded_t9_version": scope.get("t9_version"),
                    "declared_t9_version": scope.get("t9_version"),
                    "scope_t9_version": scope.get("t9_version"),
                    "scope_langpack": scope.get("langpack"),
                    "scope_langpack_source": scope.get(
                        "langpack_source", "official-catalog"
                    ),
                    "source_region_sha256": occurrence.get("source_sha256"),
                    "stored_region_sha256": digest,
                    "normalization": occurrence.get("normalization"),
                    "scope": scope,
                }
                if role == "BCORE":
                    match["embedded_bcore_software_version"] = scope.get(
                        "software_version"
                    )
                matches[(role, digest)].append(match)
    for values in matches.values():
        values.sort(key=lambda item: (
            str(item.get("path", "")), str(item.get("payload_sha256", ""))
        ))

    coverage_grouped: dict[tuple[str, str, str], dict[str, Any]] = {}
    for package in packages.values():
        source_kinds = sorted({
            str(item.get("container_type", "catalog"))
            for item in package.get("sources", []) if isinstance(item, dict)
        }) or ["catalog"]
        package_kind = ",".join(source_kinds)
        scope = package.get("scope", {})
        if not isinstance(scope, dict):
            scope = {}
        for region in package.get("regions", []):
            role = region.get("role")
            if not isinstance(role, str):
                continue
            coverage_name = str(region.get("coverage", "unknown"))
            owned_bytes = region.get("owned_bytes", 0)
            if not isinstance(owned_bytes, int):
                owned_bytes = 0
            key = role, package_kind, coverage_name
            row = coverage_grouped.setdefault(key, {
                "package_kind": package_kind,
                "coverage": coverage_name,
                "package_count": 0,
                "covered_bytes_min": owned_bytes,
                "covered_bytes_max": owned_bytes,
                "declared_software_versions": set(),
                "declared_langpacks": set(),
            })
            row["package_count"] += 1
            row["covered_bytes_min"] = min(
                row["covered_bytes_min"], owned_bytes
            )
            row["covered_bytes_max"] = max(
                row["covered_bytes_max"], owned_bytes
            )
            if scope.get("software_version") is not None:
                row["declared_software_versions"].add(
                    scope["software_version"]
                )
            if scope.get("langpack") is not None:
                row["declared_langpacks"].add(scope["langpack"])

    coverage: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for (role, _kind, _coverage), row in coverage_grouped.items():
        row["declared_software_versions"] = sorted(
            row["declared_software_versions"]
        )
        row["declared_langpacks"] = sorted(row["declared_langpacks"])
        coverage[role].append(row)
    for rows in coverage.values():
        rows.sort(key=lambda item: (item["package_kind"], item["coverage"]))

    lg_evidence: dict[tuple[str, str], set[str]] = defaultdict(set)
    for row in document.get("lg_evidence", []):
        if not isinstance(row, dict):
            continue
        role, digest = row.get("role"), row.get("sha256")
        if not isinstance(role, str) or not isinstance(digest, str):
            continue
        for value in row.get("known_lgs", []):
            lg = normalize_lg(value)
            if lg is not None:
                lg_evidence[(role, digest)].add(lg)
    return (
        dict(matches),
        {
            "unique_payloads_parsed": len(packages),
            "exact_region_payloads": len(matches),
        },
        dict(coverage),
        dict(lg_evidence),
    )


def _official_reference_scope(
    occurrence: dict[str, Any],
) -> dict[str, Any] | None:
    observed = occurrence["scope"]
    role = occurrence["role"]
    candidates: list[dict[str, Any]] = []
    for match in occurrence.get("official_package_matches", []):
        scope = match.get("scope")
        if not isinstance(scope, dict):
            continue
        candidate = {
            "software_version": software_version(scope.get("software_version")),
            "langpack": normalize_lg(scope.get("langpack")),
            "t9_version": t9_version(scope.get("t9_version")),
        }
        fields = (
            ("software_version",)
            if role in ("BCORE", "FFS(A)")
            else ("software_version", "langpack", "t9_version")
        )
        if any(candidate[field] is None for field in fields):
            continue
        if any(
            observed.get(field) is not None and (
                normalize_lg(observed.get(field))
                if field == "langpack"
                else t9_version(observed.get(field))
                if field == "t9_version"
                else software_version(observed.get(field))
            ) != candidate[field]
            for field in fields
        ):
            continue
        candidates.append(candidate)
    if not candidates:
        return None
    selected = min(candidates, key=lambda item: (
        item["software_version"],
        int(item["langpack"][2:]) if item["langpack"] is not None else -1,
        item["t9_version"] if item["t9_version"] is not None else -1,
    ))
    resolved = dict(observed)
    resolved.update(selected)
    resolved["evidence_source"] = "exact-official-catalog"
    return resolved


def _atomic_output(path: Path, data: bytes, force: bool) -> str:
    path.parent.mkdir(parents=True, exist_ok=True)
    if os.path.lexists(path):
        if (
            not path.is_symlink()
            and path.is_file()
            and path.read_bytes() == data
        ):
            return "reused"
        if not force:
            raise FirmwareError(f"output already exists: {path} (use --force)")
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
    return "written"


def build_corpus(
    roots: Iterable[Path], loaded_layout: LoadedLayout, *,
    official_catalog: Path | None = None,
    collection_manifest: Path | None = None,
    write_splits: bool = False,
    corpus_root: Path | None = None,
    force: bool = False,
) -> dict[str, Any]:
    roots = list(roots)
    if write_splits and corpus_root is None:
        raise FirmwareError("materialized corpus requires corpus_root")
    collection, collection_path = _load_collection_provenance(
        roots, collection_manifest
    )
    artifacts: list[dict[str, Any]] = []
    excluded: list[dict[str, Any]] = []
    source_data: dict[str, bytes] = {}
    source_audits: dict[str, dict[str, Any]] = {}

    for path in discover_sources(roots):
        artifact, rejection, data, audit = _classify_source(
            path, loaded_layout, collection
        )
        if rejection is not None:
            excluded.append(rejection)
            continue
        assert artifact is not None and data is not None and audit is not None
        artifact["id"] = f"source-{len(artifacts) + 1:03d}"
        artifacts.append(artifact)
        source_data[artifact["id"]] = data
        source_audits[artifact["id"]] = audit

    complete = [
        artifact for artifact in artifacts
        if artifact["kind"] == "complete-fullflash"
    ]
    partial = [
        artifact for artifact in artifacts
        if artifact["kind"] == "partial-fullflash"
    ]
    standalone = [
        artifact for artifact in artifacts
        if artifact["kind"] == "standalone-region"
    ]
    if not complete:
        raise FirmwareError("corpus contains no complete fullflash images")

    (
        official_matches, official_summary, official_coverage,
        official_lg_evidence,
    ) = _official_catalog_analysis(official_catalog, loaded_layout)
    occurrences: list[dict[str, Any]] = []
    payload_data: dict[tuple[str, str], bytes] = {}
    for artifact in [*complete, *partial]:
        source_id = artifact["id"]
        slices, source_payloads = _source_slices(
            source_data[source_id], source_audits[source_id], loaded_layout
        )
        quarantined = {
            item["role"] for item in artifact.get("quarantined_regions", [])
        }
        accepted = [
            item for item in slices
            if item["kind"] != "outside-layout"
            and item["full_boundary_present"]
            and item["logical_label"] not in quarantined
        ]
        artifact["harvested_ranges"] = [{
            "role": item["logical_label"],
            "source_range": item["source_range"],
            "layout_range": item["actual_layout_range"],
            "native_range": item["actual_native_range"],
        } for item in accepted]
        for item in accepted:
            key = (item["logical_label"], item["sha256"])
            payload = source_payloads[key]
            existing = payload_data.setdefault(key, payload)
            if existing != payload:
                raise FirmwareError(
                    f"payload hash collision for {key[0]} {key[1]}"
                )
            occurrence = {
                "source_id": source_id,
                "source_kind": artifact["kind"],
                "role": item["logical_label"],
                "sha256": item["sha256"],
                "source_sha256": item["source_sha256"],
                "normalization": item["normalization"],
                "size": item["size"],
                "source_range": item["source_range"],
                "layout_range": item["actual_layout_range"],
                "native_range": item["actual_native_range"],
                "erased": item.get("erased") is True,
            }
            if item["logical_label"] == "LangPack":
                occurrence["embedded_langpack"] = _langpack_lg(payload)
            occurrence["official_package_matches"] = official_matches.get(
                (occurrence["role"], occurrence["sha256"]), []
            )
            occurrences.append(occurrence)

    metadata_bearing_sources = {
        artifact["id"] for artifact in [*complete, *partial]
        if artifact.get("metadata", {}).get("firmware") is not None
    }
    embedded_langpack_by_source = {
        occurrence["source_id"]: occurrence["embedded_langpack"]
        for occurrence in occurrences
        if (
            occurrence["source_id"] in metadata_bearing_sources
            and occurrence["role"] == "LangPack"
        )
    }
    embedded_t9_by_source = {
        occurrence["source_id"]: embedded_t9_version(
            payload_data[(occurrence["role"], occurrence["sha256"])]
        )
        for occurrence in occurrences
        if (
            occurrence["source_id"] in metadata_bearing_sources
            and occurrence["role"] == "T9"
        )
    }
    for occurrence in occurrences:
        if (
            occurrence["source_id"] in metadata_bearing_sources
            and occurrence["role"] == "T9"
        ):
            occurrence["paired_langpack"] = (
                embedded_langpack_by_source.get(occurrence["source_id"])
            )
            occurrence["embedded_t9_version"] = (
                embedded_t9_by_source.get(occurrence["source_id"])
            )

    parts_by_label = {
        part.label: part for part in partition_layout(loaded_layout.layout)
    }
    for artifact in standalone:
        part = parts_by_label[artifact["role"]]
        source_payload = source_data[artifact["id"]]
        payload, normalization = normalize_partition(
            source_payload,
            loaded_layout.layout,
            role=artifact["role"],
            partition_start=part.start,
            partition_end=part.end,
            full_boundary_present=True,
            bcore_metadata=(
                source_audits[artifact["id"]]
                .get("bcore", {}).get("selected")
            ),
        )
        digest = _sha256(payload)
        key = (artifact["role"], digest)
        existing = payload_data.setdefault(key, payload)
        if existing != payload:
            raise FirmwareError(
                f"payload hash collision for {key[0]} {key[1]}"
            )
        occurrence = {
            "source_id": artifact["id"],
            "source_kind": "standalone-region",
            "role": artifact["role"],
            "sha256": digest,
            "source_sha256": artifact["sha256"],
            "normalization": normalization,
            "size": artifact["size"],
            "source_range": _range(0, artifact["size"]),
            "layout_range": _range(part.start, part.end),
            "native_range": _range(
                loaded_layout.layout.base + part.start,
                loaded_layout.layout.base + part.end,
            ),
            "erased": _is_erased(source_payload),
        }
        occurrence["official_package_matches"] = official_matches.get(key, [])
        occurrences.append(occurrence)
        artifact["harvested_ranges"] = [{
            "role": artifact["role"],
            "source_range": occurrence["source_range"],
            "layout_range": occurrence["layout_range"],
            "native_range": occurrence["native_range"],
        }]

    fullflash_by_hash = {artifact["sha256"]: artifact for artifact in complete}
    complete_occurrences: dict[tuple[str, str], list[dict[str, Any]]] = defaultdict(list)
    for occurrence in occurrences:
        if occurrence["source_kind"] == "complete-fullflash":
            complete_occurrences[
                (occurrence["role"], occurrence["sha256"])
            ].append(occurrence)
    artifact_by_id = {artifact["id"]: artifact for artifact in artifacts}
    for occurrence in occurrences:
        source = artifact_by_id[occurrence["source_id"]]
        metadata_bearing = occurrence["source_id"] in metadata_bearing_sources
        if metadata_bearing:
            firmware = source["metadata"].get("firmware") or {}
            langpack, langpack_source = select_lg_scope(
                occurrence["role"],
                firmware.get("langpack"),
                embedded_langpack_by_source.get(occurrence["source_id"]),
                primary_source="fullflash-metadata",
            )
        else:
            firmware = {}
            langpack, langpack_source = select_lg_scope(
                occurrence["role"],
                None,
                primary_source="unknown",
            )
        if occurrence["role"] == "BCORE" and (
            metadata_bearing or occurrence["source_kind"] == "standalone-region"
        ):
            version, version_source = _bcore_software_scope(source)
        else:
            version = _software_version(firmware.get("software_version"))
            version_source = (
                "fullflash-metadata" if version is not None else "unknown"
            )
        occurrence["scope"] = {
            "software_version": version,
            "software_version_source": version_source,
            "langpack": langpack,
            "langpack_source": langpack_source,
            "t9_version": (
                embedded_t9_by_source.get(occurrence["source_id"])
                if metadata_bearing
                else embedded_t9_version(
                    payload_data[(occurrence["role"], occurrence["sha256"])]
                )
                if (
                    occurrence["source_kind"] == "standalone-region"
                    and occurrence["role"] == "T9"
                )
                else None
            ),
            "evidence_source": occurrence["source_kind"],
        }

    for occurrence in occurrences:
        if occurrence["source_kind"] != "standalone-region":
            continue
        exact = complete_occurrences[(occurrence["role"], occurrence["sha256"])]
        occurrence["exact_fullflash_matches"] = [
            {
                "source_id": item["source_id"],
                "path": artifact_by_id[item["source_id"]]["path"],
            }
            for item in exact
        ]
        source = artifact_by_id[occurrence["source_id"]]
        parent_hash = (source.get("collection") or {}).get("associated_capture")
        parent = fullflash_by_hash.get(parent_hash)
        occurrence["associated_parent"] = {
            "sha256": parent_hash,
            "cataloged_source_id": parent["id"] if parent else None,
            "exact_region_match": (
                parent is not None
                and any(item["source_id"] == parent["id"] for item in exact)
            ),
        } if parent_hash else None

    grouped: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
    for index, occurrence in enumerate(occurrences):
        key = occurrence["sha256"]
        variant = grouped[occurrence["role"]].setdefault(key, {
            "sha256": key,
            "size": occurrence["size"],
            "erased": occurrence["erased"],
            "occurrences": [],
            "official_package_matches": occurrence["official_package_matches"],
        })
        if variant["erased"] != occurrence["erased"]:
            raise FirmwareError(
                f"inconsistent erased state for {occurrence['role']} {key}"
            )
        occurrence_id = f"occurrence-{index + 1:03d}"
        occurrence["id"] = occurrence_id
        variant["occurrences"].append(occurrence_id)

    occurrence_by_id = {
        occurrence["id"]: occurrence for occurrence in occurrences
    }
    own_lg_evidence: dict[tuple[str, str], set[str]] = defaultdict(set)
    for role, variants_by_hash in grouped.items():
        for digest, variant in variants_by_hash.items():
            observed: dict[tuple[Any, Any], int] = defaultdict(int)
            language_pairings: dict[
                tuple[int | None, str | None], int
            ] = defaultdict(int)
            embedded_langpacks: set[str] = set()
            embedded_versions: set[int] = set()
            normalized_bcore_hashes: set[str] = set()
            source_hashes: set[str] = set()
            stored_field_values: set[str] = set()
            scopes: list[dict[str, Any]] = []
            for occurrence_id in variant["occurrences"]:
                occurrence = occurrence_by_id[occurrence_id]
                source = artifact_by_id[occurrence["source_id"]]
                source_hashes.add(occurrence["source_sha256"])
                direct_scope = dict(occurrence["scope"])
                scopes.append(direct_scope)
                direct_lg = normalize_lg(direct_scope.get("langpack"))
                if direct_lg is not None:
                    own_lg_evidence[(role, digest)].add(direct_lg)
                if occurrence["source_id"] in metadata_bearing_sources:
                    firmware = source["metadata"].get("firmware") or {}
                    observed[(
                        firmware.get("software_version"),
                        firmware.get("langpack"),
                    )] += 1
                    embedded_langpack = occurrence.get(
                        "embedded_langpack"
                        if role == "LangPack" else "paired_langpack"
                    )
                    if isinstance(embedded_langpack, str):
                        embedded_langpacks.add(embedded_langpack)
                    if role in LANGUAGE_ROLES:
                        language_pairings[(
                            _software_version(firmware.get("software_version")),
                            normalize_lg(embedded_langpack),
                        )] += 1
                for match in occurrence["official_package_matches"]:
                    version_value = (
                        match.get("embedded_bcore_software_version")
                        if role == "BCORE"
                        else match.get("software_version")
                    )
                    package_scope = {
                        "software_version": _software_version(version_value),
                        "langpack": normalize_lg(match.get("scope_langpack")),
                        "langpack_source": match.get(
                            "scope_langpack_source", "unknown"
                        ),
                        "t9_version": t9_version(
                            match.get("scope_t9_version")
                        ),
                        "evidence_source": "exact-official-package",
                    }
                    scopes.append(package_scope)
                    package_lg = package_scope["langpack"]
                    if package_lg is not None:
                        own_lg_evidence[(role, digest)].add(package_lg)
                if role == "BCORE":
                    bcore = source["metadata"].get("bcore") or {}
                    version = _software_version(
                        bcore.get("software_version")
                    )
                    if version is not None:
                        embedded_versions.add(version)
                    normalization = occurrence.get("normalization")
                    if isinstance(normalization, dict):
                        normalized = normalization.get("normalized_sha256")
                        stored_value = normalization.get("stored_value")
                        if isinstance(normalized, str):
                            normalized_bcore_hashes.add(normalized)
                        if isinstance(stored_value, str):
                            stored_field_values.add(stored_value)
            variant["evidence"] = {
                "embedded_software_versions": sorted(embedded_versions),
                "embedded_langpacks": sorted(embedded_langpacks),
                "observed_fullflash_metadata": [
                    {
                        "software_version": version,
                        "langpack": langpack,
                        "occurrences": count,
                    }
                    for (version, langpack), count in sorted(
                        observed.items(),
                        key=lambda item: (
                            item[0][0] if item[0][0] is not None else -1,
                            item[0][1] or "",
                        ),
                    )
                ],
                "observed_language_pairings": [
                    {
                        "software_version": version,
                        "embedded_langpack": langpack,
                        "occurrences": count,
                    }
                    for (version, langpack), count in sorted(
                        language_pairings.items(),
                        key=lambda item: (
                            item[0][0] if item[0][0] is not None else -1,
                            item[0][1] or "",
                        ),
                    )
                ],
                "scope_observations": sorted(
                    scopes,
                    key=lambda item: (
                        item["software_version"]
                        if item["software_version"] is not None else -1,
                        item["langpack"] or "",
                        item["t9_version"]
                        if item["t9_version"] is not None else -1,
                        item["evidence_source"],
                    ),
                ),
                "bcore_field_330_normalized_sha256": sorted(
                    normalized_bcore_hashes
                ),
                "source_sha256": sorted(source_hashes),
                "bcore_field_330_stored_values": sorted(stored_field_values),
            }
            variant["_scopes"] = scopes

    reference_scopes, _scope_resolutions = resolve_scope_references(
        occurrences, artifacts
    )
    for occurrence in occurrences:
        if occurrence["id"] not in reference_scopes:
            resolved = _official_reference_scope(occurrence)
            if resolved is not None:
                reference_scopes[occurrence["id"]] = resolved
        if occurrence["id"] in reference_scopes:
            occurrence["resolved_scope"] = reference_scopes[occurrence["id"]]

    official_sources = (
        [_display_path(official_catalog)] if official_catalog is not None else []
    )
    collapsed, lg_evidence = lg_collapse_evidence(
        dict(own_lg_evidence), official_lg_evidence, official_sources
    )
    lg_evidence_by_key = {
        (item["role"], item["sha256"]): item for item in lg_evidence
    }
    for role, variants_by_hash in grouped.items():
        short_names = _short_hashes(variants_by_hash)
        for digest, variant in variants_by_hash.items():
            key = (role, digest)
            variant["lg_evidence"] = lg_evidence_by_key.get(key, {
                "role": role,
                "sha256": digest,
                "known_lgs": [],
                "own_catalog_lgs": [],
                "peer_catalog_lgs": [],
                "peer_catalogs": official_sources,
                "lg_independent": False,
                "proof": None,
            })
            variant.pop("_scopes")
            variant_occurrences = [
                occurrence_by_id[occurrence_id]
                for occurrence_id in variant["occurrences"]
            ]
            materialization_scopes = [
                reference_scopes.get(occurrence["id"], occurrence["scope"])
                for occurrence in variant_occurrences
            ]
            if variant["erased"]:
                variant["_paths_by_scope"] = {}
            else:
                storage, paths_by_scope = scoped_payload_plan(
                    short_names[digest],
                    materialization_scopes,
                    _role_slug(role),
                    (
                        _eeprom_labels_by_scope(
                            variant_occurrences, artifact_by_id,
                            reference_scopes,
                        )
                        if role == "EEPROM" else None
                    ),
                )
                variant.update(storage)
                variant["_paths_by_scope"] = paths_by_scope

    for occurrence in occurrences:
        variant = grouped[occurrence["role"]][occurrence["sha256"]]
        if occurrence["erased"]:
            continue
        scope = reference_scopes.get(
            occurrence["id"], occurrence["scope"]
        )
        occurrence["payload_path"] = select_payload_path(
            variant["_paths_by_scope"],
            scope["software_version"],
            scope["langpack"],
            scope["t9_version"],
            _role_slug(occurrence["role"]),
            (
                _eeprom_snr_label(
                    artifact_by_id[occurrence["source_id"]].get("metadata")
                )
                if occurrence["role"] == "EEPROM" else None
            ),
        )

    region_rows = []
    for part in partition_layout(loaded_layout.layout):
        variants = sorted(
            grouped.get(part.label, {}).values(),
            key=lambda item: item["sha256"],
        )
        source_variant_count = len({
            occurrence_by_id[occurrence_id]["source_sha256"]
            for variant in variants
            for occurrence_id in variant["occurrences"]
        })
        normalized_variant_count = len(variants)
        region_rows.append({
            "role": part.label,
            "kind": part.kind,
            "layout_range": _range(part.start, part.end),
            "native_range": _range(
                loaded_layout.layout.base + part.start,
                loaded_layout.layout.base + part.end,
            ),
            "occurrences": sum(
                len(variant["occurrences"]) for variant in variants
            ),
            "variant_count": normalized_variant_count,
            "source_variant_count": source_variant_count,
            "normalized_variant_count": normalized_variant_count,
            "collapsed_variants": (
                source_variant_count - normalized_variant_count
            ),
            "official_package_actions": official_coverage.get(part.label, []),
            "variants": variants,
        })

    all_hashes = {occurrence["sha256"] for occurrence in occurrences}
    role_variants = sum(row["variant_count"] for row in region_rows)
    source_variants = sum(
        row["source_variant_count"] for row in region_rows
    )
    fullflash_names = _short_hashes(
        artifact["sha256"] for artifact in complete
    )
    recipe_documents: dict[str, dict[str, Any]] = {}
    for artifact in complete:
        source_occurrences = sorted(
            (
                occurrence for occurrence in occurrences
                if occurrence["source_id"] == artifact["id"]
            ),
            key=lambda item: item["source_range"]["from"],
        )
        recipe_path = (
            f"fullflashes/{fullflash_names[artifact['sha256']]}.json"
        )
        reset_ownership = None
        reset_offset = loaded_layout.layout.reset_offset
        if reset_offset is not None:
            owner = next(
                occurrence for occurrence in source_occurrences
                if occurrence["layout_range"]["from"] <= reset_offset
                < occurrence["layout_range"]["to_exclusive"]
            )
            reset_ownership = {
                "storage": (
                    "native"
                    if loaded_layout.layout.base + reset_offset == 0
                    else "low-alias"
                ),
                "layout_offset": reset_offset,
                "owning_role": owner["role"],
                "owning_payload_path": owner["payload_path"],
                "duplicates_payload_bytes": False,
            }
        if any(
            ("payload_path" in occurrence)
            != (not occurrence["erased"])
            for occurrence in source_occurrences
        ):
            raise FirmwareError(
                f"fullflash source has ambiguous payload scope: "
                f"{artifact['path']}"
            )
        recipe = {
            "schema": RECIPE_SCHEMA,
            "schema_version": RECIPE_SCHEMA_VERSION,
            "fullflash": {
                "sha256": artifact["sha256"],
                "size": artifact["size"],
                "source_id": artifact["id"],
                "source_path": artifact["path"],
            },
            "layout": {
                "name": loaded_layout.layout.name,
                "base": loaded_layout.layout.base,
                "length": loaded_layout.layout.length,
                "catalog_path": loaded_layout.source.name,
                "catalog_sha256": _sha256(loaded_layout.catalog_bytes),
            },
            "reset_ownership": reset_ownership,
            "slices": [
                {
                    "order": order,
                    "role": occurrence["role"],
                    **(
                        {"erased": True}
                        if occurrence["erased"]
                        else {
                            "payload_path": occurrence["payload_path"],
                            "normalization": occurrence["normalization"],
                        }
                    ),
                    "sha256": occurrence["sha256"],
                    "source_sha256": occurrence["source_sha256"],
                    "size": occurrence["size"],
                    "source_range": occurrence["source_range"],
                }
                for order, occurrence in enumerate(source_occurrences)
            ],
        }
        recipe_documents[recipe_path] = recipe
        artifact["recipe"] = {
            "status": "materialized" if write_splits else "not-written",
            "path": recipe_path,
            "reset_ownership": reset_ownership,
        }

    for artifact in standalone:
        occurrence = next(
            item for item in occurrences
            if item["source_id"] == artifact["id"]
        )
        artifact["payload_path"] = occurrence["payload_path"]

    for region in region_rows:
        for variant in region["variants"]:
            variant.pop("_paths_by_scope", None)

    if write_splits:
        assert corpus_root is not None
        for region in region_rows:
            for variant in region["variants"]:
                if variant["erased"]:
                    continue
                key = (region["role"], variant["sha256"])
                _atomic_output(
                    corpus_root / variant["payload_path"],
                    payload_data[key],
                    force,
                )
                for relative in variant["symlink_paths"]:
                    atomic_relative_symlink(
                        corpus_root, relative, variant["payload_path"], force
                    )
        for relative, recipe in recipe_documents.items():
            recipe_data = (
                json.dumps(recipe, indent=2, sort_keys=True) + "\n"
            ).encode("utf-8")
            _atomic_output(corpus_root / relative, recipe_data, force)

    regular_payload_bytes = sum(
        variant["size"]
        for region in region_rows for variant in region["variants"]
        if not variant["erased"]
    )
    regular_payload_files = sum(
        not variant["erased"]
        for region in region_rows for variant in region["variants"]
    )
    payload_symlinks = sum(
        len(variant.get("symlink_paths", []))
        for region in region_rows for variant in region["variants"]
    )
    erased_variants = sum(
        variant["erased"]
        for region in region_rows for variant in region["variants"]
    )
    erased_occurrences = sum(
        len(variant["occurrences"])
        for region in region_rows for variant in region["variants"]
        if variant["erased"]
    )
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "layout": {
            "name": loaded_layout.layout.name,
            "base": loaded_layout.layout.base,
            "length": loaded_layout.layout.length,
            "catalog_path": _display_path(loaded_layout.source),
            "catalog_sha256": _sha256(loaded_layout.catalog_bytes),
        },
        "inputs": {
            "roots": [_display_path(root) for root in roots],
            "official_catalog": (
                _display_path(official_catalog)
                if official_catalog is not None else None
            ),
            "collection_manifest": collection_path,
            "corpus_root": (
                _display_path(corpus_root) if corpus_root is not None else None
            ),
        },
        "summary": {
            "source_artifacts": len(artifacts),
            "complete_fullflashes": len(complete),
            "partial_fullflashes": len(partial),
            "unique_fullflash_payloads": len({
                artifact["sha256"] for artifact in complete
            }),
            "standalone_regions": len(standalone),
            "excluded_candidates": len(excluded),
            "region_occurrences": len(occurrences),
            "source_region_variants": source_variants,
            "normalized_region_variants": role_variants,
            "collapsed_variants": source_variants - role_variants,
            "role_scoped_variants": role_variants,
            "globally_unique_payloads": len(all_hashes),
            "regular_payload_files": regular_payload_files,
            "regular_payload_bytes": regular_payload_bytes,
            "official_backed_variants": 0,
            "official_backed_paths": 0,
            "official_backed_bytes": 0,
            "payload_symlinks": payload_symlinks,
            "payload_paths": regular_payload_files + payload_symlinks,
            "implicit_erased_role_variants": erased_variants,
            "implicit_erased_occurrences": erased_occurrences,
            "fullflash_recipes": len(recipe_documents),
            "official_packages": official_summary,
            "lg_independent_variants": len(collapsed),
        },
        "lg_evidence": lg_evidence,
        "artifacts": artifacts,
        "excluded": sorted(excluded, key=lambda item: item["path"]),
        "occurrences": occurrences,
        "regions": region_rows,
    }


def _report_model_name(corpus: dict[str, Any]) -> str:
    corpus_root = corpus.get("inputs", {}).get("corpus_root")
    if corpus_root:
        path = Path(corpus_root)
        if path.name in {"community", "official"}:
            path = path.parent
        if path.name:
            return path.name
    return corpus["layout"]["name"]


def render_report(corpus: dict[str, Any]) -> str:
    summary = corpus["summary"]
    official = summary["official_packages"]
    model = _report_model_name(corpus)
    attributed_variants = sum(
        bool(variant["official_package_matches"])
        for region in corpus["regions"] for variant in region["variants"]
    )
    lines = [
        f"# {model} Community Fullflash Corpus",
        "",
        "This report is generated by `siemens_tools fullflash catalog build`. It "
        "normalizes online-collected fullflash images of uncertain provenance "
        "and groups canonical layout slices by logical role and SHA-256 "
        "without assigning "
        "semantics to `UNKNOWN_n` data. Validated PMB7850 BCORE payloads use "
        "their `+0x330..+0x33F` field-erased bytes as stored identity. "
        "Complete structurally valid statistics-bearing partitions erase "
        "statistics `+0x000..+0x0FF` and page-relative tail "
        "`+0x1FC..+0x1FF` while preserving `+0x100..+0x1FB` and field-level "
        "restoration metadata; updater finalization is resolved independently.",
        "",
        "## Inventory",
        "",
        f"- Complete fullflashes: **{summary['complete_fullflashes']}** "
        f"(**{summary['unique_fullflash_payloads']}** unique payloads).",
        f"- Partial fullflashes: **{summary['partial_fullflashes']}**.",
        f"- Cataloged source artifacts: **{summary['source_artifacts']}** "
        f"({summary['standalone_regions']} standalone regions).",
        f"- Region occurrences: **{summary['region_occurrences']}**.",
        f"- Role-scoped variants: **{summary['role_scoped_variants']}**; "
        f"globally unique payloads: **{summary['globally_unique_payloads']}**.",
        f"- Evidence-scoped payload store: "
        f"**{summary['regular_payload_files']}** regular files / "
        f"**{summary['regular_payload_bytes']:,}** bytes plus "
        f"**{summary['payload_symlinks']}** relative symlinks.",
        *(
            [
                f"- Official-backed payloads: "
                f"**{summary['official_backed_variants']}** variants / "
                f"**{summary['official_backed_paths']}** paths / "
                f"**{summary['official_backed_bytes']:,}** bytes."
            ]
            if "official_backed_variants" in summary else []
        ),
        f"- Implicit fully erased regions: "
        f"**{summary['implicit_erased_role_variants']}** role variants / "
        f"**{summary['implicit_erased_occurrences']}** occurrences.",
        f"- Fullflash reconstruction recipes: **{summary['fullflash_recipes']}**.",
        f"- Region variants: **{summary['source_region_variants']}** source / "
        f"**{summary['normalized_region_variants']}** normalized / "
        f"**{summary['collapsed_variants']}** collapsed.",
        f"- LG-independent variants: **{summary['lg_independent_variants']}**.",
        f"- Preserved payload root: `{corpus['inputs']['corpus_root']}`.",
        "",
        "A role-scoped variant is one `(logical role, SHA-256)` pair. One variant owns one",
        "regular payload under its lowest evidence-backed SW/LG/T9 scope (or lowest SW",
        "for BCORE) and relative symlinks under its other scopes. EEPROM filenames use",
        "`<snr6>-<sha12>.bin` when blocks 76/5009 decode to one consistent 14-digit IMEI",
        "body; `snr6` is its final six-digit handset serial field, with leading zeros",
        "preserved. Missing, malformed, or conflicting IMEI evidence retains",
        "`<sha12>.bin`. The global count removes identical bytes across roles and scopes.",
        "BCORE and statistics-bearing occurrences retain field metadata and raw hashes",
        "for byte-exact recipe restoration. Fully `FF` canonical variants retain",
        "their hashes and",
        "evidence but are reconstructed from `erased: true` instead of owning payload files.",
        "",
        "## Region Summary",
        "",
        "| Role | Canonical layout range | Occurrences | Source variants | "
        "Normalized variants | Collapsed | Stored paths |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for region in corpus["regions"]:
        start = region["layout_range"]["from"]
        end = region["layout_range"]["to_exclusive"]
        lines.append(
            f"| {region['role']} | `0x{start:06X}..0x{end - 1:06X}` | "
            f"{region['occurrences']} | {region['source_variant_count']} | "
            f"{region['normalized_variant_count']} | "
            f"{region['collapsed_variants']} | "
            f"{sum(len(payload_paths(item)) for item in region['variants'])} |"
        )

    artifact_by_id = {
        artifact["id"]: artifact for artifact in corpus["artifacts"]
    }
    partial_artifacts = [
        artifact for artifact in corpus["artifacts"]
        if artifact["kind"] == "partial-fullflash"
    ]
    lines.extend([
        "",
        "## Partial Fullflashes",
        "",
        "Mapped partial captures donate only fully bounded canonical regions. "
        "Reset failures quarantine the partition that owns the reset slot; "
        "missing, clipped, outside-layout, and quarantined bytes create no "
        "occurrence or implicit erase evidence.",
        "",
        "| Source | Coverage | Harvested | Quarantined |",
        "|---|---:|---:|---|",
    ])
    for artifact in partial_artifacts:
        coverage = artifact.get("coverage") or {}
        quarantined = ", ".join(
            f"{item['role']} ({item['reset_reason']})"
            for item in artifact.get("quarantined_regions", [])
        ) or "none"
        lines.append(
            f"| `{artifact['path']}` | "
            f"{coverage.get('covered_bytes', 0):,} bytes | "
            f"{len(artifact.get('harvested_ranges', []))} regions | "
            f"{quarantined} |"
        )

    standalone_occurrences = [
        occurrence for occurrence in corpus["occurrences"]
        if occurrence["source_kind"] == "standalone-region"
    ]
    lines.extend([
        "",
        "## Standalone Regions",
        "",
        "Standalone occurrences reference the same canonical",
        "`<sw>/<lg>/<t9>/<role>/<hash>.bin` store (with `<snr6>-<hash>.bin` for",
        "decoded EEPROM), or the SW-only `<sw>/bcore/<hash>.bin` store, as complete-image",
        "slices and retain their original provenance path in the catalog. Acceptance",
        "requires canonical size and a role-specific structural check.",
        "",
        "| Role | Source | Stored SHA-256 | Normalized fullflash matches | Evidence |",
        "|---|---|---|---:|---|",
    ])
    for occurrence in standalone_occurrences:
        artifact = artifact_by_id[occurrence["source_id"]]
        evidence = {
            "BCORE": "BCORE header + valid reset target",
            "T9": "validated structured T9/S9 header",
            "EEPROM": "parsed EELITE/EEFULL directory",
        }[occurrence["role"]]
        lines.append(
            f"| {occurrence['role']} | `{artifact['path']}` | "
            f"`{occurrence['sha256'][:12]}...` | "
            f"{len(occurrence['exact_fullflash_matches'])} | {evidence} |"
        )

    standalone_eeproms = [
        occurrence for occurrence in standalone_occurrences
        if occurrence["role"] == "EEPROM"
    ]
    standalone_eeprom_matches = sum(
        bool(occurrence["exact_fullflash_matches"])
        for occurrence in standalone_eeproms
    )
    if model == "C55":
        standalone_summary = [
            "Five standalone EEPROM files duplicate complete-image EEPROM regions. "
            "`C55_SW24_flash-range_b6b4e359.bin` contributes one additional parsed "
            "EEPROM variant."
        ]
    else:
        standalone_summary = [
            f"Standalone EEPROM matching: **{standalone_eeprom_matches}** have at "
            "least one exact complete-image match; "
            f"**{len(standalone_eeproms) - standalone_eeprom_matches}** contribute "
            "additional parsed variants."
        ]
    lines.extend([
        "",
        *standalone_summary,
        "",
        "## Version Evidence",
        "",
        "Numeric directory names record observed version evidence, not proven "
        "software compatibility. BCORE uses its embedded SW, falling back to "
        "collection or source-path hints only when that byte is invalid; every "
        "other dump role uses fullflash SW. Complete-image LangPack uses its fixed "
        "embedded LG and T9 uses the paired LangPack LG; both fall back to primary "
        "fullflash metadata. All other complete-image roles use primary fullflash "
        "LG. Every complete-image role uses the validated co-occurring T9 version. "
        "Standalone T9 uses its own header, while unrelated standalone regions use "
        "`unknown`. LG-independence remains analysis evidence; BCORE and FFS(A) "
        "omit LG/T9 from physical paths. Exact official-package matches may "
        "resolve incomplete storage scopes and back payloads while the observed "
        "scope remains provenance.",
        "",
        "| Stored BCORE payload | Embedded SW | Observed fullflash SW/LG | "
        "Raw variants |",
        "|---|---:|---|---|",
    ])
    bcore = next(
        region for region in corpus["regions"] if region["role"] == "BCORE"
    )
    for variant in bcore["variants"]:
        evidence = variant["evidence"]
        embedded = ", ".join(
            str(item) for item in evidence["embedded_software_versions"]
        ) or "unknown"
        observed = ", ".join(
            f"{item['software_version']}/{item['langpack']}"
            for item in evidence["observed_fullflash_metadata"]
        ) or "standalone only"
        raw_variants = len(evidence["source_sha256"])
        lines.append(
            f"| `{variant['sha256'][:12]}...` | {embedded} | {observed} | "
            f"{raw_variants} |"
        )

    language_disagreements = []
    for occurrence in corpus["occurrences"]:
        if (
            occurrence["source_kind"] != "complete-fullflash"
            or occurrence["role"] != "LangPack"
        ):
            continue
        artifact = artifact_by_id[occurrence["source_id"]]
        parent = (artifact["metadata"].get("firmware") or {}).get("langpack")
        embedded = occurrence.get("embedded_langpack")
        if parent != embedded:
            language_disagreements.append((artifact, parent, embedded))
    lines.extend([
        "",
        "## Language Evidence",
        "",
        "LangPack LG is parsed only from the fixed NUL-terminated "
        "`@lg[0-9]+` field at payload offset `0x15`. Parent fullflash metadata "
        "selects headerless-region paths but not valid LangPack/T9 LG paths. "
        "T9 version is the decimal byte at canonical T9 offset `+0x08`, accepted "
        "only with the structured `T9`/`S9` header. Every complete-image role uses "
        "that T9 version. Unknown LG never contributes to an "
        "LG-independence proof, but proven independence does not collapse paths.",
        "",
        f"The corpus has **{len(language_disagreements)}** parent/header "
        "disagreements:",
        "",
        "| Fullflash SHA-256 | SW | Parent LG | Embedded LG |",
        "|---|---:|---|---|",
    ])
    for artifact, parent, embedded in language_disagreements:
        firmware = artifact["metadata"].get("firmware") or {}
        lines.append(
            f"| `{artifact['sha256'][:12]}...` | "
            f"{firmware.get('software_version')} | {parent or 'unknown'} | "
            f"{embedded or 'unknown'} |"
        )

    lines.extend([
        "",
        "## Official Package Coverage",
        "",
        "Coverage reports package write-or-erase ownership independently from "
        "exact byte matching. `full` means every byte in the canonical region "
        "is acted upon; `partial` means only part of the region is acted upon.",
        "",
        "| Role | Package actions |",
        "|---|---|",
    ])
    for region in corpus["regions"]:
        action_parts = []
        for item in region["official_package_actions"]:
            count = item["package_count"]
            semantic = (
                " / BCORE_AUX"
                if (
                    item["package_kind"].lower() == "xbb"
                    and region["role"] != "BCORE"
                )
                else ""
            )
            action_parts.append(
                f"{item['package_kind'].upper()} {item['coverage']}{semantic} "
                f"({count} payload{'s' if count != 1 else ''})"
            )
        actions = "; ".join(action_parts) or "none observed"
        lines.append(f"| {region['role']} | {actions} |")

    lines.extend([
        "",
        "## Excluded Inputs",
        "",
        "| Source | Size | Reason |",
        "|---|---:|---|",
    ])
    for item in corpus["excluded"]:
        lines.append(
            f"| `{item['path']}` | {item['size']} | `{item['reason']}` |"
        )
    if model == "C55":
        excluded_summary = [
            "The quarter-size `langpack_small`, two half-size `t9_small` files, "
            "`clear_5E2`, and the proprietary Martech backup remain documented but "
            "uncataloged. Size or a source filename alone is not placement evidence."
        ]
    else:
        excluded_summary = [
            "Excluded inputs retain their measured size, SHA-256, provenance, and "
            "rejection reason. Size or a source filename alone is not placement "
            "evidence."
        ]
    lines.extend([
        "",
        *excluded_summary,
        "",
        "## Official Package Attribution",
        "",
        f"The scan parsed **{official['unique_payloads_parsed']}** unique raw "
        f"package payloads and found **{official['exact_region_payloads']}** "
        f"fully covered package-region payloads. **{attributed_variants}** "
        "catalog variants have at least one exact official match.",
        "",
        "An official package is attributed to a variant only when its combined "
        "write/erase action mask covers every byte of the canonical region and "
        "the materialized bytes match exactly. Sparse holes therefore never "
        "count as exact matches.",
        "",
        "## Evidence Limits",
        "",
        "| Theory | Discriminating probe | Result |",
        "|---|---|---|",
        "| Unmatched variants are personalized or modified | Recover an official "
        "package or compare independent factory captures byte-for-byte | "
        "**OPEN**: absence of an exact package match does not identify origin |",
    ])

    if model == "C55":
        filename_result = (
            "**DISPROVEN**: the 256 KiB file named `bootcore` validates as T9 "
            "at `0x080000`"
        )
        bcore_sw_result = (
            "**DISPROVEN**: SW24 captures contain BCORE SW7/11/12/18/21, while "
            "one T9 payload occurs under several software versions"
        )
        langpack_result = (
            "**DISPROVEN**: six parents report `lg1` while five embedded payloads "
            "report `lg91` and one reports `lg4`; T9 follows the embedded pairing"
        )
    else:
        misleading_standalone = next((
            (occurrence, artifact_by_id[occurrence["source_id"]])
            for occurrence in standalone_occurrences
            if (
                occurrence["role"] == "T9"
                and "bootcore" in Path(
                    artifact_by_id[occurrence["source_id"]]["path"]
                ).name.lower()
            )
        ), None)
        if misleading_standalone is None:
            filename_result = (
                "**DISPROVEN by classification policy/tests**: accepted standalone "
                "roles require canonical placement and structural evidence"
            )
        else:
            occurrence, artifact = misleading_standalone
            t9_start = occurrence["layout_range"]["from"]
            filename_result = (
                f"**DISPROVEN**: `{Path(artifact['path']).name}` validates as T9 "
                f"at `0x{t9_start:06X}`"
            )

        bcore_parent_mismatches = 0
        for variant in bcore["variants"]:
            evidence = variant["evidence"]
            embedded_versions = set(evidence["embedded_software_versions"])
            bcore_parent_mismatches += sum(
                (
                    item["software_version"] is not None
                    and bool(embedded_versions)
                    and item["software_version"] not in embedded_versions
                )
                for item in evidence["observed_fullflash_metadata"]
            )
        if bcore_parent_mismatches:
            bcore_sw_result = (
                f"**DISPROVEN for {bcore_parent_mismatches} observed pairing(s)**: "
                "embedded BCORE SW and parent fullflash SW disagree"
            )
        else:
            bcore_sw_result = (
                "**OPEN**: this corpus has no parent/embedded BCORE SW disagreement"
            )
        if language_disagreements:
            langpack_result = (
                f"**DISPROVEN for {len(language_disagreements)} fullflash(es)**: "
                "parent and embedded LangPack LG disagree"
            )
        else:
            langpack_result = (
                "**OPEN**: this corpus has no parent/embedded LangPack LG disagreement"
            )

    lines.extend([
        "| A source filename identifies its logical role | Require canonical "
        f"placement plus a role-specific marker/parser | {filename_result} |",
        "| Canonical size alone validates a standalone region | Replace markers "
        "or parser structure while retaining size | **DISPROVEN by tests**: "
        "classification rejects malformed equal-size inputs |",
        "| A region omitted by XBZ is software-independent | Compare parsers and "
        "runtime behavior across software versions, including XFS and EEPROM "
        "schemas | **OPEN**: package omission proves persistence policy, not "
        "cross-version compatibility |",
        "| The parent fullflash SW is the region's compatibility version | "
        f"Compare embedded BCORE SW and shared payloads across parents | {bcore_sw_result} |",
        "| Parent fullflash LG identifies LangPack and T9 | Parse the fixed "
        f"LangPack header and compare exact fullflash/package pairings | {langpack_result} |",
    ])
    return "\n".join(lines) + "\n"


def _load_prior_owned_paths(
    catalog_path: Path, corpus_root: Path,
) -> tuple[int | None, set[Path]]:
    if not catalog_path.exists():
        return None, set()
    try:
        document = json.loads(catalog_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(
            f"invalid existing corpus catalog {catalog_path}: {exc}"
        ) from exc
    if document.get("schema") != SCHEMA:
        raise FirmwareError(
            f"existing output is not a Siemens community fullflash corpus: "
            f"{catalog_path}"
        )
    version = document.get("schema_version")
    if not isinstance(version, int):
        raise FirmwareError("existing corpus catalog has no schema version")
    if version != SCHEMA_VERSION:
        raise FirmwareError(
            f"existing community catalog is schema {version}; rebuild into "
            "an empty catalog root"
        )
    root = Path(os.path.abspath(corpus_root))
    owned: set[Path] = set()

    def add(value: object, *, corpus_relative: bool = False) -> None:
        if not isinstance(value, str):
            return
        source = (
            corpus_root / value
            if corpus_relative
            else Path(value) if Path(value).is_absolute() else Path.cwd() / value
        )
        candidate = Path(os.path.abspath(source))
        try:
            candidate.relative_to(root)
        except ValueError as exc:
            raise FirmwareError(
                f"existing catalog owns path outside corpus root: {value}"
            ) from exc
        owned.add(candidate)

    for region in document.get("regions", []):
        if not isinstance(region, dict):
            continue
        for variant in region.get("variants", []):
            if not isinstance(variant, dict):
                continue
            for value in payload_paths(variant):
                add(value, corpus_relative=True)
    for artifact in document.get("artifacts", []):
        if isinstance(artifact, dict):
            add(
                (artifact.get("recipe") or {}).get("path"),
                corpus_relative=True,
            )
    return version, owned


def _new_owned_paths(corpus: dict[str, Any], corpus_root: Path) -> set[Path]:
    result = {
        Path(os.path.abspath(corpus_root / relative))
        for region in corpus["regions"]
        for variant in region["variants"]
        for relative in payload_paths(variant)
    }
    result.update(
        Path(os.path.abspath(corpus_root / artifact["recipe"]["path"]))
        for artifact in corpus["artifacts"]
        if artifact["kind"] == "complete-fullflash"
    )
    return result


def _prune_owned_paths(
    corpus_root: Path, stale_paths: Iterable[Path],
) -> None:
    root = Path(os.path.abspath(corpus_root))
    for path in sorted(set(stale_paths)):
        try:
            path.relative_to(root)
        except ValueError as exc:
            raise FirmwareError(
                f"refusing to prune path outside corpus: {path}"
            ) from exc
        if os.path.lexists(path):
            if not path.is_symlink() and not path.is_file():
                raise FirmwareError(f"owned corpus path is not a file: {path}")
            path.unlink()
    directories = sorted(
        (path for path in corpus_root.rglob("*") if path.is_dir()),
        key=lambda path: len(path.parts),
        reverse=True,
    )
    for directory in directories:
        try:
            directory.rmdir()
        except OSError:
            pass


def command_corpus(args: argparse.Namespace) -> None:
    output = args.catalog / "catalog.json"
    official_catalog = args.official_catalog / "catalog.json"
    loaded_layout = load_layout(args.layout, args.layout_file)
    materialize = args.materialize
    if args.patch_archive is not None and not materialize:
        raise FirmwareError("--patch-archive requires --materialize")
    corpus_root = args.catalog
    _, prior_owned = _load_prior_owned_paths(output, corpus_root)
    corpus = build_corpus(
        args.inputs,
        loaded_layout,
        official_catalog=official_catalog,
        collection_manifest=args.collection_manifest,
        write_splits=materialize,
        corpus_root=corpus_root if materialize else None,
        force=args.force,
    )
    catalog_data = (
        json.dumps(corpus, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
    ).encode("utf-8")
    catalog_status = _atomic_output(output, catalog_data, args.force)

    if materialize and args.force:
        _prune_owned_paths(
            corpus_root, prior_owned - _new_owned_paths(corpus, corpus_root)
        )

    if materialize:
        if args.patch_archive is not None:
            from .patch_tagging import apply_patch_tag_plan, plan_patch_tags
            patch_plan = plan_patch_tags(
                model=loaded_layout.layout.name.split("/", 1)[0],
                dump_catalog_path=output,
                official_catalog_path=official_catalog,
                archive_path=args.patch_archive,
            )
            apply_patch_tag_plan(patch_plan)
        from .catalog_minimize import minimize_catalog
        minimize_catalog(output, official_catalog)
        corpus = json.loads(output.read_text(encoding="utf-8"))

    report_status = None
    if args.report_output is not None:
        report_status = _atomic_output(
            args.report_output, render_report(corpus).encode("utf-8"), args.force
        )
    if materialize and args.force:
        _prune_owned_paths(
            corpus_root, prior_owned - _new_owned_paths(corpus, corpus_root)
        )
    print(
        f"{catalog_status}: {output} "
        f"({corpus['summary']['source_artifacts']} sources, "
        f"{corpus['summary']['region_occurrences']} occurrences)"
    )
    if args.report_output is not None:
        print(f"{report_status}: {args.report_output}")
