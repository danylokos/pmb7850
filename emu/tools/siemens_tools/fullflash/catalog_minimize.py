from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
from typing import Any

from .catalog_backing import validate_official_backing
from .corpus import (
    SCHEMA,
    SCHEMA_VERSION,
    _atomic_output,
    render_report,
)
from .corpus_scope import atomic_relative_symlink, normalize_lg, payload_paths
from .official_corpus import SCHEMA as OFFICIAL_SCHEMA
from .official_corpus import SCHEMA_VERSION as OFFICIAL_VERSION
from ..firmware.xbi import FirmwareError


REPORT_SCHEMA = "siemens-community-catalog-minimization"
REPORT_VERSION = 1


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _read_catalog(path: Path, schema: str, versions: tuple[int, ...]) -> tuple[
    dict[str, Any], bytes
]:
    try:
        data = path.read_bytes()
        document = json.loads(data.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid catalog {path}: {exc}") from exc
    if (
        not isinstance(document, dict)
        or document.get("schema") != schema
        or document.get("schema_version") not in versions
    ):
        rendered = ", ".join(map(str, versions))
        raise FirmwareError(
            f"catalog {path} is not {schema} schema {rendered}"
        )
    return document, data


def _safe_path(root: Path, relative: object, description: str) -> Path:
    if not isinstance(relative, str):
        raise FirmwareError(f"{description} is not a path")
    value = Path(relative)
    if (
        value.is_absolute()
        or not value.parts
        or any(part in ("", ".", "..") for part in value.parts)
    ):
        raise FirmwareError(f"unsafe {description}: {relative!r}")
    return root / value


def _verified_bytes(
    path: Path, size: int, digest: str, description: str,
) -> bytes:
    if not path.is_file():
        raise FirmwareError(f"{description} is missing: {path}")
    data = path.read_bytes()
    if len(data) != size or _sha256(data) != digest:
        raise FirmwareError(f"{description} size or hash mismatch: {path}")
    return data


def _scope_from_path(relative: str, role: str) -> tuple[object, object, object]:
    parts = Path(relative).parts

    def number(value: str) -> int | None:
        return int(value) if value.isascii() and value.isdigit() else None

    software = number(parts[0]) if parts else None
    if role in ("BCORE", "FFS(A)"):
        return software, None, None
    if len(parts) < 3:
        return software, None, None
    return software, normalize_lg(parts[1]), number(parts[2])


def _target_path(
    source: str, targets: list[str], role: str,
) -> str:
    source_scope = _scope_from_path(source, role)

    def rank(target: str) -> tuple[bool, bool, bool, str]:
        target_scope = _scope_from_path(target, role)
        return (
            source_scope[0] != target_scope[0],
            source_scope[1] != target_scope[1],
            source_scope[2] != target_scope[2],
            target,
        )

    return min(targets, key=rank)


def _variant_index(
    document: dict[str, Any],
) -> dict[tuple[str, str, int], dict[str, Any]]:
    result: dict[tuple[str, str, int], dict[str, Any]] = {}
    for region in document.get("regions", []):
        role = region.get("role")
        for variant in region.get("variants", []):
            if variant.get("erased") is True:
                continue
            digest = variant.get("sha256")
            size = variant.get("size")
            if (
                not isinstance(role, str)
                or not isinstance(digest, str)
                or len(digest) != 64
                or not isinstance(size, int)
                or size < 0
            ):
                raise FirmwareError("catalog has an invalid non-erased variant")
            key = (role, digest.lower(), size)
            if key in result:
                raise FirmwareError(
                    f"catalog has duplicate variant: {role} {digest}"
                )
            result[key] = variant
    return result


def _verify_community_variant(
    root: Path, variant: dict[str, Any],
) -> bytes:
    paths = payload_paths(variant)
    if not paths:
        raise FirmwareError("non-erased community variant has no payload path")
    size, digest = variant["size"], variant["sha256"]
    canonical = _safe_path(root, paths[0], "community payload path")
    data = _verified_bytes(canonical, size, digest, "community payload")
    if variant.get("official_backing") is None and canonical.is_symlink():
        raise FirmwareError(
            f"community canonical payload is not regular: {canonical}"
        )
    for relative in paths[1:]:
        alias = _safe_path(root, relative, "community payload alias")
        if not alias.is_symlink():
            raise FirmwareError(
                f"community payload alias is not a symlink: {alias}"
            )
        if _verified_bytes(
            alias, size, digest, "community payload alias"
        ) != data:
            raise FirmwareError(f"community payload aliases disagree: {alias}")
        if variant.get("official_backing") is None \
                and alias.resolve() != canonical.resolve():
            raise FirmwareError(
                f"community payload alias target mismatch: {alias}"
            )
    return data


def plan_minimization(
    community_catalog_path: Path, official_catalog_path: Path,
) -> dict[str, Any]:
    community_catalog_path = community_catalog_path.resolve()
    official_catalog_path = official_catalog_path.resolve()
    community, _community_bytes = _read_catalog(
        community_catalog_path, SCHEMA, (SCHEMA_VERSION,)
    )
    official, official_bytes = _read_catalog(
        official_catalog_path, OFFICIAL_SCHEMA, (OFFICIAL_VERSION,)
    )
    layout_fields = ("name", "base", "length", "catalog_sha256")
    if any(
        community.get("layout", {}).get(field)
        != official.get("layout", {}).get(field)
        for field in layout_fields
    ):
        raise FirmwareError("community and official catalog layouts differ")
    validate_official_backing(community, community_catalog_path)

    document = copy.deepcopy(community)
    community_root = community_catalog_path.parent
    official_root = official_catalog_path.parent
    official_index = _variant_index(official)
    official_catalog_relative = os.path.relpath(
        official_catalog_path, community_root
    )
    official_catalog_digest = _sha256(official_bytes)
    matches: list[dict[str, Any]] = []

    for region in document.get("regions", []):
        role = region["role"]
        for variant in region.get("variants", []):
            if variant.get("erased") is True:
                continue
            digest, size = variant["sha256"], variant["size"]
            already_backed = isinstance(variant.get("official_backing"), dict)
            official_variant = official_index.get((role, digest.lower(), size))
            if official_variant is None:
                continue
            community_data = _verify_community_variant(
                community_root, variant
            )
            targets = sorted(payload_paths(official_variant))
            if not targets:
                raise FirmwareError(
                    f"official variant has no payload path: {role} {digest}"
                )
            records = []
            for source in payload_paths(variant):
                target = _target_path(source, targets, role)
                official_path = _safe_path(
                    official_root, target, "official payload path"
                )
                official_data = _verified_bytes(
                    official_path, size, digest, "official payload"
                )
                if community_data != official_data:
                    raise FirmwareError(
                        f"community and official payload bytes disagree: "
                        f"{role} {digest}"
                    )
                records.append({
                    "path": source,
                    "target": os.path.relpath(official_path, community_root),
                })
            backing = {
                "catalog": official_catalog_relative,
                "catalog_sha256": official_catalog_digest,
                "paths": records,
            }
            variant["official_backing"] = backing
            matches.append({
                "role": role,
                "sha256": digest,
                "size": size,
                "already_backed": already_backed,
                "paths": records,
            })

    summary = document.get("summary")
    if not isinstance(summary, dict):
        raise FirmwareError("community catalog has no summary")
    backed = [
        variant
        for region in document.get("regions", [])
        for variant in region.get("variants", [])
        if isinstance(variant.get("official_backing"), dict)
    ]
    unbacked = [
        variant
        for region in document.get("regions", [])
        for variant in region.get("variants", [])
        if variant.get("erased") is not True
        and not isinstance(variant.get("official_backing"), dict)
    ]
    summary["regular_payload_files"] = len(unbacked)
    summary["regular_payload_bytes"] = sum(item["size"] for item in unbacked)
    summary["official_backed_variants"] = len(backed)
    summary["official_backed_paths"] = sum(
        len(item["official_backing"]["paths"]) for item in backed
    )
    summary["official_backed_bytes"] = sum(item["size"] for item in backed)
    summary["payload_symlinks"] = (
        sum(len(item.get("symlink_paths", [])) for item in unbacked)
        + summary["official_backed_paths"]
    )
    summary["payload_paths"] = (
        summary["regular_payload_files"] + summary["payload_symlinks"]
    )
    report = {
        "schema": REPORT_SCHEMA,
        "schema_version": REPORT_VERSION,
        "catalog": community_catalog_path.as_posix(),
        "official_catalog": official_catalog_path.as_posix(),
        "input_schema_version": community["schema_version"],
        "output_schema_version": SCHEMA_VERSION,
        "matching_variants": len(matches),
        "newly_backed_variants": sum(
            not item["already_backed"] for item in matches
        ),
        "official_backed_paths": sum(len(item["paths"]) for item in matches),
        "bytes_removed": sum(
            item["size"] for item in matches if not item["already_backed"]
        ),
        "matches": matches,
    }
    return {"document": document, "report": report}


def minimize_catalog(
    community_catalog_path: Path,
    official_catalog_path: Path,
    *,
    dry_run: bool = False,
    report_output: Path | None = None,
) -> dict[str, Any]:
    plan = plan_minimization(community_catalog_path, official_catalog_path)
    report = plan["report"]
    report["dry_run"] = dry_run
    if not dry_run:
        root = community_catalog_path.resolve().parent
        for match in report["matches"]:
            for record in match["paths"]:
                atomic_relative_symlink(
                    root, record["path"], record["target"], True
                )
        catalog_data = (
            json.dumps(
                plan["document"], indent=2, sort_keys=True,
                ensure_ascii=False,
            ) + "\n"
        ).encode("utf-8")
        resolved_catalog = community_catalog_path.resolve()
        if resolved_catalog.read_bytes() != catalog_data:
            _atomic_output(resolved_catalog, catalog_data, True)
        validate_official_backing(
            plan["document"], community_catalog_path.resolve()
        )
    if report_output is not None and not dry_run:
        _atomic_output(
            report_output,
            render_report(plan["document"]).encode("utf-8"),
            True,
        )
    return report
