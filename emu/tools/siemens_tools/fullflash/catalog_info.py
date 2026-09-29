from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path, PurePosixPath
from typing import Any

from .catalog_backing import COMMUNITY_VERSION, validate_official_backing
from .corpus import SCHEMA
from .corpus_scope import resolve_scope_references
from ..firmware.xbi import FirmwareError


HASH_RE = re.compile(r"[0-9a-fA-F]{12,64}")


def _read_catalog(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid community catalog {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("schema") != \
            SCHEMA:
        raise FirmwareError(f"not a community catalog: {path}")
    if document.get("schema_version") != COMMUNITY_VERSION:
        raise FirmwareError(
            f"community catalog is not schema {COMMUNITY_VERSION}: {path}"
        )
    validate_official_backing(document, path)
    return document


def _catalog_path(root: Path) -> Path:
    root = root.resolve()
    if not root.is_dir():
        raise FirmwareError(f"community corpus root is not a directory: {root}")
    path = root / "catalog.json"
    if not path.is_file():
        raise FirmwareError(f"community corpus catalog is missing: {path}")
    return path


def _relative_candidates(identifier: str, root: Path) -> set[str]:
    result = {identifier, PurePosixPath(identifier).as_posix()}
    path = Path(identifier)
    if path.exists():
        resolved = path.resolve()
        result.add(resolved.as_posix())
        try:
            result.add(resolved.relative_to(root).as_posix())
        except ValueError:
            pass
    return result


def _artifact_rows(
    document: dict[str, Any], catalog_path: Path, identifier: str,
) -> tuple[list[dict[str, Any]], str | None]:
    artifacts = [
        item for item in document.get("artifacts", [])
        if item.get("kind") in {
            "complete-fullflash", "partial-fullflash", "standalone-region",
        }
    ]
    root = catalog_path.parent.resolve()
    candidates = _relative_candidates(identifier, root)
    matched_ids: set[str] = set()
    selected_payload: str | None = None

    for artifact in artifacts:
        recipe = artifact.get("recipe") or {}
        fields = (
            artifact.get("id"), artifact.get("path"), recipe.get("path"),
            artifact.get("payload_path"),
        )
        if any(isinstance(value, str) and value in candidates for value in fields):
            matched_ids.add(artifact["id"])

    if HASH_RE.fullmatch(identifier):
        digest_matches = [
            item for item in artifacts
            if str(item.get("sha256", "")).startswith(identifier.lower())
        ]
        if len(digest_matches) > 1:
            raise FirmwareError(f"ambiguous fullflash hash prefix: {identifier}")
        matched_ids.update(item["id"] for item in digest_matches)

    payload_occurrences = [
        item for item in document.get("occurrences", [])
        if item.get("payload_path") in candidates
    ]
    if payload_occurrences:
        selected_payload = payload_occurrences[0]["payload_path"]
        matched_ids.update(item["source_id"] for item in payload_occurrences)

    source_path = Path(identifier)
    if source_path.is_file() and not matched_ids:
        digest = hashlib.sha256(source_path.read_bytes()).hexdigest()
        matched_ids.update(
            item["id"] for item in artifacts if item.get("sha256") == digest
        )

    rows = [item for item in artifacts if item.get("id") in matched_ids]
    if not rows:
        raise FirmwareError(f"catalog identifier not found: {identifier}")
    return sorted(rows, key=lambda item: item["sha256"]), selected_payload


def catalog_info(root: Path, identifier: str) -> dict[str, Any]:
    catalog_path = _catalog_path(root)
    document = _read_catalog(catalog_path)
    artifacts, selected_payload = _artifact_rows(
        document, catalog_path, identifier
    )
    _references, resolutions = resolve_scope_references(
        document.get("occurrences", []),
        document.get("artifacts", []),
    )
    resolutions_by_source: dict[str, list[dict[str, Any]]] = {}
    for resolution in resolutions:
        resolutions_by_source.setdefault(
            resolution["source_id"], []
        ).append(resolution)

    return {
        "schema": "siemens-community-catalog-info",
        "schema_version": 1,
        "catalog": catalog_path.as_posix(),
        "catalog_schema_version": document.get("schema_version"),
        "query": identifier,
        "selected_payload": selected_payload,
        "matches": [{
            "id": artifact.get("id"),
            "kind": artifact.get("kind"),
            "role": artifact.get("role"),
            "sha256": artifact.get("sha256"),
            "path": artifact.get("path"),
            "coverage": artifact.get("coverage"),
            "harvested_ranges": artifact.get("harvested_ranges", []),
            "quarantined_regions": artifact.get("quarantined_regions", []),
            "recipe": (artifact.get("recipe") or {}).get("path"),
            "recipe_status": (
                (artifact.get("recipe") or {}).get("status") or "absent"
            ),
            "scope_resolutions": resolutions_by_source.get(
                artifact.get("id"), []
            ),
            "patch_analysis": artifact.get("metadata", {}).get("patch_analysis"),
        } for artifact in artifacts],
    }


def _format_scope(value: object) -> str:
    if not isinstance(value, dict):
        return "unknown/unknown/unknown"
    software = value.get("software_version")
    langpack = value.get("langpack")
    t9 = value.get("t9_version")
    return f"{software if software is not None else 'unknown'}/" + (
        f"{langpack if langpack is not None else 'unknown'}/"
        f"{t9 if t9 is not None else 'unknown'}"
    )


def _format_range(value: object) -> str:
    if not isinstance(value, dict):
        return "unknown"
    start, end = value.get("from"), value.get("to_exclusive")
    if not isinstance(start, int) or not isinstance(end, int):
        return "unknown"
    return f"0x{start:X}-0x{end:X}"


def _print_info(info: dict[str, Any]) -> None:
    print(f"catalog: {info['catalog']}")
    print(f"query: {info['query']}")
    if info["selected_payload"] is not None:
        print(f"payload: {info['selected_payload']}")
    print(f"artifacts: {len(info['matches'])}")
    for item in info["matches"]:
        print(f"ARTIFACT {item['sha256']}")
        print(f"  kind: {item['kind']}")
        print(f"  source: {item['path']}")
        coverage = item.get("coverage") or {}
        if coverage:
            print(
                f"  coverage: {coverage.get('covered_bytes')} covered, "
                f"{coverage.get('missing_bytes')} missing"
            )
        print(f"  harvested regions: {len(item['harvested_ranges'])}")
        for region in item["harvested_ranges"]:
            print(
                f"    {region.get('role')}: "
                f"{_format_range(region.get('layout_range'))}"
            )
        print(f"  quarantined regions: {len(item['quarantined_regions'])}")
        for region in item["quarantined_regions"]:
            print(
                f"    {region.get('role')}: "
                f"{_format_range(region.get('layout_range'))} "
                f"reason={region.get('reset_reason')}"
            )
        print(
            f"  recipe: {item['recipe_status']}"
            + (f" ({item['recipe']})" if item['recipe'] else "")
        )
        resolutions = item["scope_resolutions"]
        print(f"  normalized scope references: {len(resolutions)}")
        for resolution in resolutions:
            print(
                f"    {resolution['role']}: "
                f"{_format_scope(resolution['observed_scope'])} -> "
                f"{_format_scope(resolution['reference_scope'])} "
                f"via {resolution['evidence_source_id']}"
            )
        analysis = item["patch_analysis"]
        if not isinstance(analysis, dict):
            print("  patch analysis: unavailable")
            continue
        baseline = analysis.get("baseline", {})
        print(f"  baseline package: {baseline.get('package_sha256')}")
        patches = analysis.get("identified_patches", [])
        print(f"  identified patches: {len(patches)}")
        for patch in patches:
            print(
                f"    {patch.get('archive_key')}/{patch.get('patch_id')}: "
                f"{patch.get('title', '')}"
            )
        unknown = analysis.get("unidentified_regions", [])
        print(f"  unidentified regions: {len(unknown)}")
        for region in unknown:
            print(
                f"    {region.get('id')} role={region.get('role')} "
                f"layout={_format_range(region.get('layout_range'))} "
                f"length={region.get('length')}"
            )


def command_catalog_info(args: Any) -> None:
    info = catalog_info(args.catalog, args.identifier)
    if args.json:
        print(json.dumps(info, indent=2, sort_keys=True, ensure_ascii=False))
    else:
        _print_info(info)
