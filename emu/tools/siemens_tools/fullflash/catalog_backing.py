from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Any

from .corpus_scope import payload_paths
from ..firmware.xbi import FirmwareError


COMMUNITY_SCHEMA = "siemens-community-fullflash-corpus"
COMMUNITY_VERSION = 14
OFFICIAL_SCHEMA = "siemens-official-corpus"
OFFICIAL_VERSION = 9


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _relative_path(value: object, description: str) -> Path:
    if not isinstance(value, str):
        raise FirmwareError(f"{description} is not a path")
    path = Path(value)
    if (
        path.is_absolute()
        or not path.parts
        or any(part in ("", ".") for part in path.parts)
    ):
        raise FirmwareError(f"unsafe {description}: {value!r}")
    return path


def _owned_path(root: Path, value: object, description: str) -> Path:
    relative = _relative_path(value, description)
    if ".." in relative.parts:
        raise FirmwareError(f"unsafe {description}: {value!r}")
    return root / relative


def _external_path(root: Path, value: object, description: str) -> Path:
    relative = _relative_path(value, description)
    return Path(os.path.abspath(root / relative))


def _read_json(path: Path, description: str) -> tuple[dict[str, Any], bytes]:
    try:
        data = path.read_bytes()
        document = json.loads(data.decode("utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid {description} {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise FirmwareError(f"{description} is not an object: {path}")
    return document, data


def _verify_bytes(
    path: Path, size: object, digest: object, description: str,
) -> bytes:
    if (
        not isinstance(size, int)
        or size < 0
        or not isinstance(digest, str)
        or len(digest) != 64
    ):
        raise FirmwareError(f"{description} has invalid size or hash")
    if not path.is_file():
        raise FirmwareError(f"{description} is missing: {path}")
    data = path.read_bytes()
    if len(data) != size or _sha256(data) != digest:
        raise FirmwareError(f"{description} size or hash mismatch: {path}")
    return data


def validate_official_backing(
    document: dict[str, Any], catalog_path: Path,
) -> dict[str, int]:
    """Validate schema-14 community links against their official catalog."""
    if document.get("schema") != COMMUNITY_SCHEMA:
        raise FirmwareError(f"not a community catalog: {catalog_path}")
    if document.get("schema_version") != COMMUNITY_VERSION:
        raise FirmwareError(
            f"community catalog is not schema {COMMUNITY_VERSION}: "
            f"{catalog_path}"
        )

    community_root = catalog_path.parent
    official_cache: dict[
        Path, tuple[dict[str, Any], str, dict[tuple[str, str, int], dict[str, Any]]]
    ] = {}
    variants = paths = byte_count = 0
    for region in document.get("regions", []):
        if not isinstance(region, dict) or not isinstance(region.get("role"), str):
            raise FirmwareError("community catalog has an invalid region")
        role = region["role"]
        for variant in region.get("variants", []):
            if not isinstance(variant, dict):
                raise FirmwareError("community catalog has an invalid variant")
            backing = variant.get("official_backing")
            if backing is None:
                continue
            if variant.get("erased") is True or not isinstance(backing, dict):
                raise FirmwareError("erased or invalid variant has official backing")
            catalog = _external_path(
                community_root, backing.get("catalog"),
                "official backing catalog path"
            )
            if catalog.name != "catalog.json":
                raise FirmwareError(
                    f"official backing does not reference catalog.json: {catalog}"
                )
            cached = official_cache.get(catalog)
            if cached is None:
                official, catalog_bytes = _read_json(
                    catalog, "official backing catalog"
                )
                if (
                    official.get("schema") != OFFICIAL_SCHEMA
                    or official.get("schema_version") != OFFICIAL_VERSION
                ):
                    raise FirmwareError(
                        f"unsupported official backing catalog: {catalog}"
                    )
                index: dict[tuple[str, str, int], dict[str, Any]] = {}
                for official_region in official.get("regions", []):
                    official_role = official_region.get("role")
                    for official_variant in official_region.get("variants", []):
                        if official_variant.get("erased") is True:
                            continue
                        key = (
                            official_role,
                            official_variant.get("sha256"),
                            official_variant.get("size"),
                        )
                        if (
                            not isinstance(key[0], str)
                            or not isinstance(key[1], str)
                            or not isinstance(key[2], int)
                            or key in index
                        ):
                            raise FirmwareError(
                                f"ambiguous official backing variant: {catalog}"
                            )
                        index[key] = official_variant
                cached = official, _sha256(catalog_bytes), index
                official_cache[catalog] = cached
            _official, catalog_digest, official_index = cached
            if backing.get("catalog_sha256") != catalog_digest:
                raise FirmwareError(
                    f"official backing catalog hash mismatch: {catalog}"
                )

            digest, size = variant.get("sha256"), variant.get("size")
            key = (role, digest, size)
            official_variant = official_index.get(key)
            if official_variant is None:
                raise FirmwareError(
                    f"official backing variant is absent: {role} {digest}"
                )
            official_root = catalog.parent
            official_paths = {
                Path(os.path.abspath(official_root / relative))
                for relative in payload_paths(official_variant)
            }
            records = backing.get("paths")
            if not isinstance(records, list):
                raise FirmwareError("official backing paths is not an array")
            recorded_paths = [
                record.get("path") for record in records
                if isinstance(record, dict)
            ]
            if (
                len(recorded_paths) != len(records)
                or recorded_paths != payload_paths(variant)
            ):
                raise FirmwareError(
                    f"official backing paths do not cover variant: {role} {digest}"
                )
            for record in records:
                local = _owned_path(
                    community_root, record.get("path"),
                    "official-backed community path"
                )
                target = _external_path(
                    community_root, record.get("target"),
                    "official backing target"
                )
                try:
                    target.relative_to(Path(os.path.abspath(official_root)))
                except ValueError as exc:
                    raise FirmwareError(
                        f"official backing target escapes official corpus: {target}"
                    ) from exc
                if target not in official_paths:
                    raise FirmwareError(
                        f"official backing target is not cataloged: {target}"
                    )
                if not local.is_symlink():
                    raise FirmwareError(
                        f"official-backed community path is not a symlink: {local}"
                    )
                direct = Path(os.path.abspath(local.parent / os.readlink(local)))
                if direct != target:
                    raise FirmwareError(
                        f"official-backed community link target mismatch: {local}"
                    )
                target_data = _verify_bytes(
                    target, size, digest, "official backing target"
                )
                if _verify_bytes(
                    local, size, digest, "official-backed community path"
                ) != target_data:
                    raise FirmwareError(
                        f"official backing bytes disagree: {local}"
                    )
            variants += 1
            paths += len(records)
            byte_count += size
    summary = document.get("summary")
    if not isinstance(summary, dict) or any(
        summary.get(field) != expected
        for field, expected in (
            ("official_backed_variants", variants),
            ("official_backed_paths", paths),
            ("official_backed_bytes", byte_count),
        )
    ):
        raise FirmwareError("official backing summary does not match records")
    return {"variants": variants, "paths": paths, "bytes": byte_count}
