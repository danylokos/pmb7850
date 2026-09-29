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

from .normalization import normalize_partition
from .corpus import (
    LANGPACK_LG_FIELD_END,
    OFFICIAL_SUFFIXES,
    _display_path,
    _langpack_lg,
    _materialized_firmware_lg,
    _model_matches,
    _range,
    _role_slug,
    _short_hashes,
)
from .corpus_scope import (
    T9_HEADER_SIZE,
    atomic_relative_symlink,
    embedded_t9_version,
    lg_collapse_evidence,
    ffs_label_sort_key,
    normalize_lg,
    payload_paths,
    scope_component,
    scope_key,
    scoped_payload_plan,
    select_lg_scope,
    select_payload_path,
    software_version,
    t9_scope_component,
    t9_version,
)
from ..layout.catalog import LoadedLayout, load_layout
from .mining import mine_fullflash
from ..layout.partitions import partition_layout
from ..firmware.compression import materialize_xbi
from ..firmware.containers import detect_exe_type, extract_exe
from ..firmware.ffsinit import (
    build_ffsinit_reference_index,
    is_ffsinit,
    recover_ffsinit_bin_reference,
)
from ..firmware.inspection import _json_value
from ..firmware.xbi import FirmwareError, is_xbi, parse_xbi


SCHEMA = "siemens-official-corpus"
SCHEMA_VERSION = 9
RECIPE_SCHEMA = "siemens-official-package-recipe"
RECIPE_SCHEMA_VERSION = 2
EXECUTABLE_SUFFIXES = {".exe"}
BCORE_AUX_ROLE = "BCORE_AUX"


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


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


def _candidate_paths(roots: Iterable[Path]) -> list[Path]:
    result: dict[Path, Path] = {}
    for root in roots:
        if not root.exists():
            raise FirmwareError(f"official corpus input does not exist: {root}")
        paths = [root] if root.is_file() else root.rglob("*")
        for path in paths:
            if not path.is_file():
                continue
            result.setdefault(path.resolve(), path)
    if not result:
        raise FirmwareError("official corpus inputs contain no packages")
    return sorted(result.values(), key=lambda path: path.as_posix())


def discover_official_packages(
    roots: Iterable[Path],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    candidates = _candidate_paths(roots)
    ffsinit_references = build_ffsinit_reference_index(candidates)
    unique: dict[str, dict[str, Any]] = {}
    excluded: list[dict[str, Any]] = []
    for path in candidates:
        raw = path.read_bytes()
        source_sha = _sha256(raw)
        payloads: list[bytes]
        container_type: str
        transform: dict[str, Any] | None = None
        if is_ffsinit(path, raw):
            try:
                recovery = recover_ffsinit_bin_reference(
                    path, raw, ffsinit_references
                )
            except FirmwareError as exc:
                excluded.append({
                    "path": _display_path(path),
                    "size": len(raw),
                    "sha256": source_sha,
                    "reason": "ffsinit-conversion-failed",
                    "detail": str(exc),
                })
                continue
            reference = recovery.reference
            identity = recovery.identity
            payloads = [reference.xfs]
            exact = recovery.confidence == "byte-exact"
            container_type = (
                "ffsinit-reference"
                if exact else "ffsinit-tree-reference"
            )
            transform = {
                "kind": (
                    "byte-exact-ffsinit-reference"
                    if exact else "heuristic-ffsinit-tree-reference"
                ),
                "confidence": recovery.confidence,
                "match_kind": recovery.match_kind,
                "zip_sha256": identity.zip_sha256,
                "zip_size": identity.zip_size,
                "tree_sha256": identity.tree_sha256,
                "entry_order_sha256": identity.entry_order_sha256,
                "reference_path": _display_path(reference.xfs_path),
                "reference_sha256": reference.xfs_sha256,
            }
        elif is_xbi(raw):
            payloads = [raw]
            container_type = "raw"
        else:
            container_type = detect_exe_type(raw) or ""
            if not container_type:
                if (path.suffix.lower() in OFFICIAL_SUFFIXES
                        or path.suffix.lower() in EXECUTABLE_SUFFIXES):
                    excluded.append({
                        "path": _display_path(path),
                        "size": len(raw),
                        "sha256": source_sha,
                        "reason": "unsupported-container",
                    })
                continue
            payloads = extract_exe(raw)
        for payload_index, payload in enumerate(payloads):
            if not is_xbi(payload):
                excluded.append({
                    "path": _display_path(path),
                    "size": len(payload),
                    "sha256": _sha256(payload),
                    "payload_index": payload_index,
                    "reason": "embedded-payload-is-not-xbi",
                })
                continue
            digest = _sha256(payload)
            row = unique.setdefault(digest, {
                "sha256": digest,
                "size": len(payload),
                "_payload": payload,
                "sources": [],
            })
            row["sources"].append({
                "path": _display_path(path),
                "container_type": container_type,
                "container_sha256": source_sha,
                "container_size": len(raw),
                "payload_index": None if container_type == "raw" else payload_index,
                **({"transform": transform} if transform is not None else {}),
            })
    for row in unique.values():
        row["sources"].sort(key=lambda item: (
            item["path"],
            item["payload_index"] if item["payload_index"] is not None else -1,
        ))
    return (
        sorted(unique.values(), key=lambda item: item["sha256"]),
        sorted(excluded, key=lambda item: (item["path"], item.get("payload_index", -1))),
    )


def _mask_ranges(
    mask: bytes, start: int, end: int,
) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    cursor = start
    while cursor < end:
        while cursor < end and not mask[cursor]:
            cursor += 1
        first = cursor
        while cursor < end and mask[cursor]:
            cursor += 1
        if first < cursor:
            result.append((first, cursor))
    return result


def _range_slug(start: int, end: int) -> str:
    return f"{start:06x}-{end - 1:06x}"


def _package_metadata(info: Any) -> dict[str, Any]:
    return {
        "format": {
            "version": info.format_version,
            "signed": info.signed,
            "signature_size": info.signature_size,
            "logical_size": info.size,
        },
        "fields": _json_value(info.fields),
        "write_frame_count": len(info.writes),
    }


def _source_software_hint(sources: Iterable[dict[str, Any]]) -> int | None:
    candidates: set[int] = set()
    for source in sources:
        value = source.get("path")
        if not isinstance(value, str):
            continue
        path = Path(value)
        for part in reversed(path.parts[:-1]):
            match = re.fullmatch(r"(?:v\.)?(\d{1,3})", part, re.IGNORECASE)
            if match is not None:
                version = software_version(match.group(1))
                if version is not None:
                    candidates.add(version)
                break
        for pattern in (
            r"(?:^|[_-])SW(\d{1,3})(?:[_-]|$)",
            r"(?:^|[_-])(\d{1,3})$",
        ):
            match = re.search(pattern, path.stem, re.IGNORECASE)
            if match is not None:
                version = software_version(match.group(1))
                if version is not None:
                    candidates.add(version)
                break
    if len(candidates) > 1:
        raise FirmwareError(
            "conflicting source-path software hints: "
            + ", ".join(str(value) for value in sorted(candidates))
        )
    return next(iter(candidates), None)


def _ffs_labels(sources: Iterable[dict[str, Any]]) -> list[str]:
    labels: set[str] = set()
    pattern = re.compile(
        r"^(?:FFSInit_)?[^_]+_[12]_(?:synth-)?(.+?)_\d{1,3}_\d+$",
        re.IGNORECASE,
    )
    for source in sources:
        value = source.get("path")
        if not isinstance(value, str):
            continue
        match = pattern.fullmatch(Path(value).stem)
        if match is None:
            continue
        label = match.group(1)
        if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.-]*", label):
            labels.add(label)
    return sorted(labels or {"unknown"}, key=ffs_label_sort_key)


def build_official_corpus(
    roots: Iterable[Path],
    loaded_layout: LoadedLayout,
    *,
    materialize: bool = False,
    corpus_root: Path | None = None,
    force: bool = False,
) -> dict[str, Any]:
    roots = list(roots)
    if materialize and corpus_root is None:
        raise FirmwareError("materialized official corpus requires corpus_root")
    discovered, excluded = discover_official_packages(roots)
    packages: list[dict[str, Any]] = []
    grouped: dict[str, dict[str, dict[str, Any]]] = defaultdict(dict)
    payload_data: dict[tuple[str, str], bytes] = {}
    own_lg_evidence: dict[tuple[str, str], set[str]] = defaultdict(set)
    fragment_data: dict[tuple[str, str, str], bytes] = {}

    for discovered_row in discovered:
        payload = discovered_row.pop("_payload")
        info = parse_xbi(payload)
        if info.get("flash_size") != loaded_layout.layout.length:
            excluded.append({
                "sha256": discovered_row["sha256"],
                "size": discovered_row["size"],
                "sources": discovered_row["sources"],
                "reason": "flash-size-does-not-match-layout",
            })
            continue
        model = info.get("model")
        if not (
            _model_matches(loaded_layout.layout.name, str(model))
            or (isinstance(model, str) and model.startswith("BOOTL55"))
        ):
            excluded.append({
                "sha256": discovered_row["sha256"],
                "size": discovered_row["size"],
                "sources": discovered_row["sources"],
                "reason": "model-does-not-match-layout",
            })
            continue

        expanded = materialize_xbi(payload, info)
        ownership = bytes(
            write or erase for write, erase
            in zip(expanded.write_mask, expanded.erase_mask)
        )
        langpack_part = next(
            part for part in partition_layout(loaded_layout.layout)
            if part.label == "LangPack"
        )
        embedded_lg = None
        if all(ownership[
            langpack_part.start:
            langpack_part.start + LANGPACK_LG_FIELD_END
        ]):
            embedded_lg = _langpack_lg(
                expanded.flash[langpack_part.start:langpack_part.end]
            )
        declared_lg = normalize_lg(info.get("langpack"))
        materialized_lg = _materialized_firmware_lg(
            expanded.flash, ownership
        )
        package_lg = materialized_lg or declared_lg
        package_lg_source = (
            "fullflash-metadata" if materialized_lg is not None
            else "package-metadata" if declared_lg is not None else "unknown"
        )
        package_sw = software_version(info.get("svn"))
        t9_part = next(
            (
                part for part in partition_layout(loaded_layout.layout)
                if part.label == "T9"
            ),
            None,
        )
        embedded_t9 = None
        if t9_part is not None and all(
            ownership[t9_part.start:t9_part.start + T9_HEADER_SIZE]
        ):
            embedded_t9 = embedded_t9_version(
                expanded.flash[t9_part.start:t9_part.end]
            )
        declared_t9 = t9_version(info.get("t9"))
        package_t9 = (
            embedded_t9 if embedded_t9 is not None else declared_t9
        )
        package_ffs_labels = _ffs_labels(discovered_row["sources"])
        is_bcore_bundle = isinstance(model, str) and model.startswith("BOOTL55")
        package = {
            "sha256": discovered_row["sha256"],
            "size": discovered_row["size"],
            "sources": discovered_row["sources"],
            "metadata": _package_metadata(info),
            "scope": {
                "software_version": package_sw,
                "langpack": package_lg,
                "t9_version": package_t9,
                "langpack_source": package_lg_source,
                "t9_version_source": (
                    "embedded-t9"
                    if embedded_t9 is not None
                    else "declared-package"
                    if declared_t9 is not None
                    else "unknown"
                ),
            },
            "write_ranges": [
                _range(start, end)
                for start, end in _mask_ranges(
                    expanded.write_mask, 0, loaded_layout.layout.length
                )
            ],
            "erase_ranges": [
                _range(start, end)
                for start, end in _mask_ranges(
                    expanded.erase_mask, 0, loaded_layout.layout.length
                )
            ],
            "regions": [],
            "_operations": [],
        }

        if is_bcore_bundle:
            bcore_part = next(
                (
                    part for part in partition_layout(loaded_layout.layout)
                    if part.label == "BCORE"
                ),
                None,
            )
            if (
                bcore_part is None
                or ownership[bcore_part.start:bcore_part.end].count(1)
                != bcore_part.end - bcore_part.start
            ):
                raise FirmwareError(
                    "BOOTL55 package has no complete BCORE region"
                )
            bcore_source = expanded.flash[bcore_part.start:bcore_part.end]
            bcore_audit = mine_fullflash(
                bcore_source,
                layout=loaded_layout.layout,
                layout_file=loaded_layout.source,
            )
            bcore = bcore_audit.get("bcore", {}).get("selected")
            embedded_bcore_sw = software_version(
                (bcore or {}).get("software_version")
            )
            hinted_bcore_sw = _source_software_hint(package["sources"])
            package["bundle"] = {
                "role": "BCORE",
                "software_version": (
                    embedded_bcore_sw
                    if embedded_bcore_sw is not None else hinted_bcore_sw
                ),
                "software_version_source": (
                    "embedded-bcore"
                    if embedded_bcore_sw is not None
                    else "source-path"
                    if hinted_bcore_sw is not None
                    else "unknown"
                ),
            }

        partial_erases: list[dict[str, Any]] = []
        partial_writes: list[dict[str, Any]] = []
        for part in partition_layout(loaded_layout.layout):
            part_ownership = ownership[part.start:part.end]
            owned = part_ownership.count(1)
            if not owned:
                continue
            full = owned == part.end - part.start
            write_ranges = _mask_ranges(
                expanded.write_mask, part.start, part.end
            )
            erase_ranges = _mask_ranges(
                expanded.erase_mask, part.start, part.end
            )
            region_row: dict[str, Any] = {
                "role": part.label,
                "coverage": "full" if full else "partial",
                "owned_bytes": owned,
                "range": _range(part.start, part.end),
                "write_ranges": [_range(start, end) for start, end in write_ranges],
                "erase_ranges": [_range(start, end) for start, end in erase_ranges],
            }
            if full:
                source_region = expanded.flash[part.start:part.end]
                normalization = None
                embedded_bcore_sw = None
                bcore = None
                if part.label == "BCORE":
                    audit = mine_fullflash(
                        source_region,
                        layout=loaded_layout.layout,
                        layout_file=loaded_layout.source,
                    )
                    bcore = audit.get("bcore", {}).get("selected")
                    embedded_bcore_sw = software_version(
                        (bcore or {}).get("software_version")
                    )
                stored_region, normalization = normalize_partition(
                    source_region,
                    loaded_layout.layout,
                    role=part.label,
                    partition_start=part.start,
                    partition_end=part.end,
                    full_boundary_present=True,
                    bcore_metadata=bcore,
                )
                digest = _sha256(stored_region)
                key = (part.label, digest)
                existing = payload_data.setdefault(key, stored_region)
                if existing != stored_region:
                    raise FirmwareError(
                        f"official region hash collision for {part.label} {digest}"
                    )
                scope_lg, scope_lg_source = select_lg_scope(
                    part.label,
                    package_lg,
                    embedded_lg,
                    primary_source=package_lg_source,
                )
                scope_t9 = package_t9
                if part.label == "BCORE":
                    hinted_bcore_sw = _source_software_hint(package["sources"])
                    scope_sw = (
                        embedded_bcore_sw
                        if embedded_bcore_sw is not None else hinted_bcore_sw
                    )
                    scope_sw_source = (
                        "embedded-bcore"
                        if embedded_bcore_sw is not None
                        else "source-path"
                        if hinted_bcore_sw is not None
                        else "unknown"
                    )
                    if is_bcore_bundle:
                        scope_lg = None
                        scope_lg_source = "unknown"
                        scope_t9 = None
                        package["bundle"] = {
                            "role": "BCORE",
                            "software_version": scope_sw,
                            "software_version_source": scope_sw_source,
                        }
                else:
                    scope_sw = package_sw
                    scope_sw_source = (
                        "declared-package" if package_sw is not None else "unknown"
                    )
                scope = {
                    "software_version": scope_sw,
                    "software_version_source": scope_sw_source,
                    "langpack": scope_lg,
                    "langpack_source": scope_lg_source,
                    "t9_version": scope_t9,
                    "evidence_source": "official-package",
                }
                if scope_lg is not None:
                    own_lg_evidence[key].add(scope_lg)
                variant = grouped[part.label].setdefault(digest, {
                    "sha256": digest,
                    "size": part.end - part.start,
                    "erased": False,
                    "_all_erase_only": True,
                    "occurrences": [],
                    "_scopes": [],
                })
                occurrence = {
                    "package_sha256": package["sha256"],
                    "role": part.label,
                    "sha256": digest,
                    "source_sha256": _sha256(source_region),
                    "normalization": normalization,
                    "scope": scope,
                    "range": _range(part.start, part.end),
                }
                if part.label == "FFS(A)":
                    occurrence["ffs_labels"] = package_ffs_labels
                variant["occurrences"].append(occurrence)
                variant["_scopes"].append(scope)
                erase_only = (
                    not any(expanded.write_mask[part.start:part.end])
                    and all(expanded.erase_mask[part.start:part.end])
                )
                variant["_all_erase_only"] = (
                    variant["_all_erase_only"] and erase_only
                )
                if erase_only:
                    region_row["erased"] = True
                    operation = {
                        "action": "replace",
                        "role": part.label,
                        "range": _range(part.start, part.end),
                        "erased": True,
                        "sha256": _sha256(source_region),
                        "size": len(source_region),
                    }
                else:
                    operation = {
                        "action": "replace",
                        "role": part.label,
                        "range": _range(part.start, part.end),
                        "sha256": digest,
                        "source_sha256": _sha256(source_region),
                        "normalization": normalization,
                        "size": len(source_region),
                        "_variant_key": key,
                        "_scope": scope,
                        "_label": (
                            package_ffs_labels[0]
                            if part.label == "FFS(A)" else None
                        ),
                    }
                package["_operations"].append(operation)
                region_row.update({
                    "sha256": digest,
                    "source_sha256": _sha256(source_region),
                    "normalization": normalization,
                    "embedded_bcore_software_version": embedded_bcore_sw,
                })
            else:
                if is_bcore_bundle:
                    region_row["semantic_role"] = BCORE_AUX_ROLE
                for start, end in erase_ranges:
                    operation = {
                        "action": "erase",
                        "role": part.label,
                        "range": _range(start, end),
                        "erased": True,
                        "sha256": _sha256(b"\xFF" * (end - start)),
                        "size": end - start,
                    }
                    if is_bcore_bundle:
                        operation["semantic_role"] = BCORE_AUX_ROLE
                    partial_erases.append(operation)
                for start, end in write_ranges:
                    fragment = expanded.flash[start:end]
                    digest = _sha256(fragment)
                    if is_bcore_bundle:
                        bundle = package.get("bundle")
                        if not isinstance(bundle, dict):
                            raise FirmwareError(
                                "BOOTL55 package has no complete BCORE bundle"
                            )
                        fragment_prefix = (
                            f"{scope_component(bundle['software_version'])}/bcore"
                        )
                    else:
                        sw_component = (
                            str(package_sw)
                            if package_sw is not None else "unknown"
                        )
                        lg_component = package_lg or "unknown"
                        t9_component = t9_scope_component(package_t9)
                        fragment_prefix = (
                            f"{sw_component}/{lg_component}/{t9_component}/"
                            f"{_role_slug(part.label)}"
                        )
                    fragment_key = (
                        fragment_prefix,
                        _range_slug(start, end),
                        digest,
                    )
                    existing = fragment_data.setdefault(fragment_key, fragment)
                    if existing != fragment:
                        raise FirmwareError(
                            f"official fragment hash collision: {digest}"
                        )
                    operation = {
                        "action": "write",
                        "role": part.label,
                        "range": _range(start, end),
                        "sha256": digest,
                        "size": end - start,
                        "_fragment_key": fragment_key,
                    }
                    if is_bcore_bundle:
                        operation["semantic_role"] = BCORE_AUX_ROLE
                    partial_writes.append(operation)
                region_row["fragments"] = len(write_ranges)
            package["regions"].append(region_row)
        if is_bcore_bundle and "bundle" not in package:
            raise FirmwareError("BOOTL55 package has no complete BCORE region")
        package["_operations"].extend(partial_erases)
        package["_operations"].extend(partial_writes)
        packages.append(package)

    collapsed, lg_evidence = lg_collapse_evidence(
        dict(own_lg_evidence), {}, ()
    )
    evidence_by_key = {
        (item["role"], item["sha256"]): item for item in lg_evidence
    }
    for role, variants in grouped.items():
        short_names = _short_hashes(variants)
        for digest, variant in variants.items():
            key = (role, digest)
            scopes = variant.pop("_scopes")
            variant["erased"] = variant.pop("_all_erase_only")
            variant["lg_evidence"] = evidence_by_key.get(key, {
                "role": role,
                "sha256": digest,
                "known_lgs": [],
                "own_catalog_lgs": [],
                "peer_catalog_lgs": [],
                "peer_catalogs": [],
                "lg_independent": False,
                "proof": None,
            })
            if variant["erased"]:
                variant["_paths_by_scope"] = {}
            else:
                role_slug = _role_slug(role)
                labels_by_scope = None
                if role == "FFS(A)":
                    labels_by_scope = defaultdict(set)
                    for occurrence in variant["occurrences"]:
                        scope = occurrence["scope"]
                        key_scope = scope_key(
                            scope["software_version"],
                            scope["langpack"],
                            scope["t9_version"],
                            role_slug,
                        )
                        labels_by_scope[key_scope].update(
                            occurrence["ffs_labels"]
                        )
                storage, paths_by_scope = scoped_payload_plan(
                    short_names[digest],
                    scopes,
                    role_slug,
                    dict(labels_by_scope) if labels_by_scope is not None else None,
                )
                variant.update(storage)
                variant["_paths_by_scope"] = paths_by_scope

    fragment_names: dict[tuple[str, str, str], str] = {}
    fragment_groups: dict[
        tuple[str, str], list[tuple[str, str, str]],
    ] = defaultdict(list)
    for key in fragment_data:
        fragment_groups[key[:2]].append(key)
    for prefix, keys in fragment_groups.items():
        short_names = _short_hashes(key[2] for key in keys)
        for key in keys:
            fragment_names[key] = (
                f"{prefix[0]}/fragments/{prefix[1]}/"
                f"{short_names[key[2]]}.bin"
            )

    package_names = _short_hashes(item["sha256"] for item in packages)
    recipe_documents: dict[str, dict[str, Any]] = {}
    for package in packages:
        operations = []
        for order, operation in enumerate(package.pop("_operations")):
            operation = dict(operation)
            key = operation.pop("_variant_key", None)
            scope = operation.pop("_scope", None)
            fragment_key = operation.pop("_fragment_key", None)
            label = operation.pop("_label", None)
            if key is not None:
                variant = grouped[key[0]][key[1]]
                operation["payload_path"] = select_payload_path(
                    variant["_paths_by_scope"],
                    scope["software_version"],
                    scope["langpack"],
                    scope["t9_version"],
                    _role_slug(key[0]),
                    label,
                )
            if fragment_key is not None:
                operation["payload_path"] = fragment_names[fragment_key]
            operation["order"] = order
            operations.append(operation)
        recipe_path = f"packages/{package_names[package['sha256']]}.json"
        recipe_package = {
            "sha256": package["sha256"],
            "size": package["size"],
            "sources": package["sources"],
            "metadata": package["metadata"],
        }
        if "bundle" in package:
            recipe_package["bundle"] = package["bundle"]
        recipe = {
            "schema": RECIPE_SCHEMA,
            "schema_version": RECIPE_SCHEMA_VERSION,
            "layout": {
                "name": loaded_layout.layout.name,
                "base": loaded_layout.layout.base,
                "length": loaded_layout.layout.length,
                "catalog_path": loaded_layout.source.name,
                "catalog_sha256": _sha256(loaded_layout.catalog_bytes),
            },
            "package": recipe_package,
            "operations": operations,
        }
        recipe_documents[recipe_path] = recipe
        package["recipe"] = {
            "path": recipe_path,
            "status": "materialized" if materialize else "not-written",
        }

    for variants in grouped.values():
        for variant in variants.values():
            variant.pop("_paths_by_scope", None)

    region_rows = []
    for part in partition_layout(loaded_layout.layout):
        variants = sorted(
            grouped.get(part.label, {}).values(),
            key=lambda item: item["sha256"],
        )
        source_variant_count = len({
            occurrence["source_sha256"]
            for variant in variants
            for occurrence in variant["occurrences"]
        })
        normalized_variant_count = len(variants)
        region_rows.append({
            "role": part.label,
            "kind": part.kind,
            "range": _range(part.start, part.end),
            "variant_count": normalized_variant_count,
            "source_variant_count": source_variant_count,
            "normalized_variant_count": normalized_variant_count,
            "collapsed_variants": (
                source_variant_count - normalized_variant_count
            ),
            "variants": variants,
        })

    if materialize:
        assert corpus_root is not None
        for role, variants in grouped.items():
            for digest, variant in variants.items():
                if variant["erased"]:
                    continue
                _atomic_output(
                    corpus_root / variant["payload_path"],
                    payload_data[(role, digest)],
                    force,
                )
                for relative in variant["symlink_paths"]:
                    atomic_relative_symlink(
                        corpus_root, relative, variant["payload_path"], force
                    )
        for key, data in fragment_data.items():
            _atomic_output(corpus_root / fragment_names[key], data, force)
        for relative, recipe in recipe_documents.items():
            _atomic_output(
                corpus_root / relative,
                (
                    json.dumps(recipe, indent=2, sort_keys=True) + "\n"
                ).encode("utf-8"),
                force,
            )

    references = sum(len(item["sources"]) for item in packages)
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
            "corpus_root": (
                _display_path(corpus_root) if corpus_root is not None else None
            ),
        },
        "summary": {
            "package_references": references,
            "unique_packages": len(packages),
            "excluded": len(excluded),
            "source_region_variants": sum(
                item["source_variant_count"] for item in region_rows
            ),
            "normalized_region_variants": sum(
                item["normalized_variant_count"] for item in region_rows
            ),
            "collapsed_variants": sum(
                item["collapsed_variants"] for item in region_rows
            ),
            "complete_region_variants": sum(
                len(item["variants"]) for item in region_rows
            ),
            "regular_complete_payload_files": sum(
                not variant["erased"]
                for item in region_rows for variant in item["variants"]
            ),
            "regular_complete_payload_bytes": sum(
                variant["size"]
                for item in region_rows for variant in item["variants"]
                if not variant["erased"]
            ),
            "complete_payload_symlinks": sum(
                len(variant.get("symlink_paths", []))
                for item in region_rows for variant in item["variants"]
            ),
            "complete_payload_paths": sum(
                len(payload_paths(variant))
                for item in region_rows for variant in item["variants"]
            ),
            "implicit_erased_variants": sum(
                variant["erased"]
                for item in region_rows for variant in item["variants"]
            ),
            "partial_fragment_files": len(fragment_data),
            "partial_fragment_bytes": sum(map(len, fragment_data.values())),
            "package_recipes": len(recipe_documents),
            "lg_independent_variants": len(collapsed),
            "ffsinit_references": sum(
                source["container_type"].startswith("ffsinit-")
                for package in packages for source in package["sources"]
            ),
            "ffsinit_exact_references": sum(
                source["container_type"] == "ffsinit-reference"
                for package in packages for source in package["sources"]
            ),
            "ffsinit_heuristic_references": sum(
                source["container_type"] == "ffsinit-tree-reference"
                for package in packages for source in package["sources"]
            ),
            "ffsinit_conversion_failures": sum(
                item.get("reason") == "ffsinit-conversion-failed"
                for item in excluded
            ),
        },
        "lg_evidence": lg_evidence,
        "packages": packages,
        "excluded": excluded,
        "regions": region_rows,
    }


def _report_model_name(corpus: dict[str, Any]) -> str:
    models = [
        model
        for package in corpus.get("packages", [])
        if isinstance(
            model := (
                (package.get("metadata") or {}).get("fields") or {}
            ).get("model"),
            str,
        )
        and not model.startswith("BOOTL")
    ]
    if models:
        return min(set(models), key=lambda model: (-models.count(model), model))
    corpus_root = corpus.get("inputs", {}).get("corpus_root")
    if corpus_root:
        path = Path(corpus_root)
        if path.name in {"community", "official"}:
            path = path.parent
        if path.name:
            return path.name
    return corpus["layout"]["name"]


def render_official_report(corpus: dict[str, Any]) -> str:
    summary = corpus["summary"]
    model = _report_model_name(corpus)
    storage_description = (
        "Complete payloads normally use `<sw>/<lg>/<t9>/<region>/...`; BCORE "
        "and FFS(A) are LG/T9-independent at `<sw>/bcore/...` and "
        "`<sw>/ffs_a/<label>-<hash>.bin`. Each SW/hash keeps only its "
        "highest-priority label (`Standard`, `FFS-LG01`, other `FFS-LGNN`, "
        "then lexical); relative symlinks expose identical bytes only across "
        "software versions. Primary LG comes from valid materialized fullflash "
        "metadata at `+0x7FF50`, falling back to package metadata. LangPack and "
        "T9 alone use the embedded or paired LangPack LG when valid. Structured "
        "T9 `+0x08` evidence overrides declared package metadata; missing or "
        "malformed evidence falls back to declared T9, then `unknown`. FFS(A) "
        "filenames prefix each "
        "content hash with its carrier or brand label from package provenance."
    )
    if any(package.get("bundle") for package in corpus["packages"]):
        storage_description += (
            " BOOTL55 XBB trailer fragments use the BCORE bundle scope at "
            "`<sw>/bcore/fragments/...` while retaining their physical region role."
        )
    lines = [
        f"# {model} Official Package Corpus",
        "",
        "Generated by `siemens_tools firmware catalog build`. Package binaries "
        "remain in the source archive; this corpus stores decoded metadata, "
        "recipes, complete canonical regions, and partial write fragments.",
        "FFSInit sources use byte-exact EXE/XFS references where available. "
        "BIN-only recovery may also reuse the sole same-model/SW XFS reference "
        "for an identical canonical file tree; unsupported or ambiguous trees "
        "remain excluded.",
        "",
        "Complete structurally valid statistics-bearing partitions erase "
        "statistics `+0x000..+0x0FF` and page-relative tail "
        "`+0x1FC..+0x1FF` while preserving `+0x100..+0x1FB` and field-level "
        "restoration metadata; updater finalization is resolved independently.",
        "",
        storage_description,
        "",
        "## Inventory",
        "",
        f"- Package references: **{summary['package_references']}**.",
        f"- Unique embedded payloads: **{summary['unique_packages']}**.",
        f"- FFSInit references: **{summary['ffsinit_references']}** "
        f"(**{summary['ffsinit_exact_references']}** byte-exact, "
        f"**{summary['ffsinit_heuristic_references']}** canonical-tree).",
        f"- Unsupported FFSInit conversions: "
        f"**{summary['ffsinit_conversion_failures']}**.",
        f"- Complete role variants: **{summary['complete_region_variants']}**.",
        f"- Region variants: **{summary['source_region_variants']}** source / "
        f"**{summary['normalized_region_variants']}** normalized / "
        f"**{summary['collapsed_variants']}** collapsed.",
        f"- Complete payload store: "
        f"**{summary['regular_complete_payload_files']}** regular files / "
        f"**{summary['regular_complete_payload_bytes']:,}** bytes plus "
        f"**{summary['complete_payload_symlinks']}** relative symlinks "
        f"(**{summary['complete_payload_paths']}** paths).",
        f"- Implicit erase-only variants: "
        f"**{summary['implicit_erased_variants']}**.",
        f"- Partial write fragments: **{summary['partial_fragment_files']}** "
        f"files / **{summary['partial_fragment_bytes']:,}** bytes.",
        f"- Package recipes: **{summary['package_recipes']}**.",
        f"- LG-independent variants: **{summary['lg_independent_variants']}**.",
        "",
        "## Canonical Regions",
        "",
        "| Role | Range | Source variants | Normalized variants | Collapsed |",
        "|---|---:|---:|---:|---:|",
    ]
    for region in corpus["regions"]:
        start = region["range"]["from"]
        end = region["range"]["to_exclusive"]
        lines.append(
            f"| {region['role']} | `0x{start:06X}..0x{end - 1:06X}` | "
            f"{region['source_variant_count']} | "
            f"{region['normalized_variant_count']} | "
            f"{region['collapsed_variants']} |"
        )
    lines.extend([
        "",
        "Erase and write masks remain separate in every package record. Fully "
        "owned erase-only regions use `erased: true`; sparse holes are not "
        "operations and therefore preserve the composition baseline.",
    ])
    return "\n".join(lines) + "\n"


def _owned_paths(document: dict[str, Any], root: Path) -> set[Path]:
    version = document.get("schema_version")
    if document.get("schema") != SCHEMA or version != SCHEMA_VERSION:
        raise FirmwareError(
            f"existing official catalog is schema {version}; rebuild into "
            "an empty catalog root"
        )
    resolved_root = Path(os.path.abspath(root))
    result: set[Path] = set()

    def add(value: object) -> None:
        if not isinstance(value, str):
            return
        candidate = Path(os.path.abspath(root / value))
        try:
            candidate.relative_to(resolved_root)
        except ValueError as exc:
            raise FirmwareError(
                f"official corpus path escapes root: {value}"
            ) from exc
        result.add(candidate)

    for region in document.get("regions", []):
        for variant in region.get("variants", []):
            for value in payload_paths(variant):
                add(value)
    for package in document.get("packages", []):
        add((package.get("recipe") or {}).get("path"))
        recipe_value = (package.get("recipe") or {}).get("path")
        if not isinstance(recipe_value, str):
            continue
        recipe_path = root / recipe_value
        if not recipe_path.is_file():
            continue
        try:
            recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            continue
        for operation in recipe.get("operations", []):
            if operation.get("action") == "write":
                add(operation.get("payload_path"))
    return result


def _prune(root: Path, stale: Iterable[Path]) -> None:
    resolved_root = Path(os.path.abspath(root))
    for path in sorted(set(stale)):
        try:
            path.relative_to(resolved_root)
        except ValueError as exc:
            raise FirmwareError(
                f"official corpus path escapes root: {path}"
            ) from exc
        if os.path.lexists(path):
            if not path.is_symlink() and not path.is_file():
                raise FirmwareError(f"official corpus path is not a file: {path}")
            path.unlink()
    for directory in sorted(
        (path for path in root.rglob("*") if path.is_dir()),
        key=lambda path: len(path.parts),
        reverse=True,
    ):
        try:
            directory.rmdir()
        except OSError:
            pass


def command_official_corpus(args: argparse.Namespace) -> None:
    output = args.catalog / "catalog.json"
    loaded = load_layout(args.layout, args.layout_file)
    root = args.catalog
    prior: set[Path] = set()
    if output.exists():
        try:
            old = json.loads(output.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise FirmwareError(f"invalid existing official catalog: {exc}") from exc
        prior = _owned_paths(old, root)
    corpus = build_official_corpus(
        args.inputs,
        loaded,
        materialize=args.materialize,
        corpus_root=root if args.materialize else None,
        force=args.force,
    )
    status = _atomic_output(
        output,
        (
            json.dumps(corpus, indent=2, sort_keys=True, ensure_ascii=False) + "\n"
        ).encode("utf-8"),
        args.force,
    )
    if args.materialize and args.force:
        current = _owned_paths(corpus, root)
        _prune(root, prior - current)
    print(
        f"{status}: {output} "
        f"({corpus['summary']['package_references']} references, "
        f"{corpus['summary']['unique_packages']} unique packages)"
    )
    if args.report_output is not None:
        report_status = _atomic_output(
            args.report_output,
            render_official_report(corpus).encode("utf-8"),
            args.force,
        )
        print(f"{report_status}: {args.report_output}")
