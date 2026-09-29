from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path, PurePosixPath
from typing import Any

from .xbi import FirmwareError


HASH_RE = re.compile(r"[0-9a-fA-F]{12,64}")


def _read_catalog(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid firmware catalog {path}: {exc}") from exc
    if not isinstance(document, dict) or document.get("schema") != \
            "siemens-official-corpus":
        raise FirmwareError(f"not a firmware catalog: {path}")
    if document.get("schema_version") != 9:
        raise FirmwareError(f"firmware catalog is not schema 9: {path}")
    return document


def _catalog_path(root: Path) -> Path:
    root = root.resolve()
    if not root.is_dir():
        raise FirmwareError(f"firmware corpus root is not a directory: {root}")
    path = root / "catalog.json"
    if not path.is_file():
        raise FirmwareError(f"firmware corpus catalog is missing: {path}")
    return path


def _candidates(identifier: str, root: Path) -> set[str]:
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


def _summary(package: dict[str, Any]) -> dict[str, Any]:
    return {
        "sha256": package.get("sha256"),
        "scope": package.get("scope"),
        "metadata": package.get("metadata"),
        "recipe": package.get("recipe", {}).get("path"),
        "sources": package.get("sources", []),
        "regions": [{
            "role": region.get("role"),
            "coverage": region.get("coverage"),
            "range": region.get("range"),
            "sha256": region.get("sha256"),
            "erased": region.get("erased") is True,
        } for region in package.get("regions", [])],
    }


def catalog_info(root: Path, identifier: str) -> dict[str, Any]:
    catalog_path = _catalog_path(root)
    document = _read_catalog(catalog_path)
    root = catalog_path.parent.resolve()
    candidates = _candidates(identifier, root)
    packages = document.get("packages", [])
    matched: set[str] = set()

    for package in packages:
        recipe = package.get("recipe", {}).get("path")
        if isinstance(recipe, str) and recipe in candidates:
            matched.add(package["sha256"])
        for source in package.get("sources", []):
            if source.get("path") in candidates:
                matched.add(package["sha256"])

    if HASH_RE.fullmatch(identifier):
        digest_matches = [
            package for package in packages
            if str(package.get("sha256", "")).startswith(identifier.lower())
        ]
        if len(digest_matches) > 1:
            raise FirmwareError(f"ambiguous package hash prefix: {identifier}")
        matched.update(package["sha256"] for package in digest_matches)

    for region in document.get("regions", []):
        for variant in region.get("variants", []):
            paths = [variant.get("payload_path"), *variant.get("symlink_paths", [])]
            if any(path in candidates for path in paths):
                matched.update(
                    item["package_sha256"]
                    for item in variant.get("occurrences", [])
                )

    source_path = Path(identifier)
    if source_path.is_file() and not matched:
        digest = hashlib.sha256(source_path.read_bytes()).hexdigest()
        for package in packages:
            if package.get("sha256") == digest or any(
                source.get("container_sha256") == digest
                for source in package.get("sources", [])
            ):
                matched.add(package["sha256"])

    result = [
        _summary(package) for package in packages
        if package.get("sha256") in matched
    ]
    if not result:
        raise FirmwareError(f"catalog identifier not found: {identifier}")
    result.sort(key=lambda item: item["sha256"])
    return {
        "schema": "siemens-firmware-catalog-info",
        "schema_version": 1,
        "catalog": catalog_path.as_posix(),
        "catalog_schema_version": document.get("schema_version"),
        "query": identifier,
        "matches": result,
    }


def _print_info(info: dict[str, Any]) -> None:
    print(f"catalog: {info['catalog']}")
    print(f"query: {info['query']}")
    print(f"packages: {len(info['matches'])}")
    for package in info["matches"]:
        print(f"PACKAGE {package['sha256']}")
        print(f"  recipe: {package['recipe']}")
        print(f"  scope: {json.dumps(package['scope'], sort_keys=True)}")
        print(f"  sources: {len(package['sources'])}")
        print(f"  complete regions: {len(package['regions'])}")


def command_catalog_info(args: Any) -> None:
    info = catalog_info(args.catalog, args.identifier)
    if args.json:
        print(json.dumps(info, indent=2, sort_keys=True, ensure_ascii=False))
    else:
        _print_info(info)
