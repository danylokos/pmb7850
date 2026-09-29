from __future__ import annotations

import hashlib
import json
import os
import re
import struct
import sys
import tempfile
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Iterable

from .. import eeprom
from ..firmware.xbi import FirmwareError
from ..layout.catalog import LoadedLayout, load_layout
from ..layout.partitions import partition_layout
from .bcore_key import derive_bcore_key, hash_bootkey
from .catalog_backing import COMMUNITY_VERSION
from .corpus import SCHEMA as COMMUNITY_SCHEMA
from .eeprom_composition import EepromCompositionResult, compose_eeprom
from .official_corpus import (
    RECIPE_SCHEMA as OFFICIAL_RECIPE_SCHEMA,
    RECIPE_SCHEMA_VERSION as OFFICIAL_RECIPE_VERSION,
    SCHEMA as OFFICIAL_SCHEMA,
    SCHEMA_VERSION as OFFICIAL_VERSION,
)
from .ranges import _reject_duplicate_json_keys
from .reconstruct import reconstruct_recipe
from .statistics import PAGE_SIZE, inspect_statistics


SYNTHETIC_RECIPE_SCHEMA = "siemens-synthetic-fullflash-recipe"
SYNTHETIC_RECIPE_VERSION = 4
SYNTHETIC_RECIPE_VERSIONS = (1, 2, 3, SYNTHETIC_RECIPE_VERSION)
_HASH_PREFIX = re.compile(r"[0-9a-fA-F]{12,64}")
_LG = re.compile(r"lg(\d+)", re.IGNORECASE)
_SUPPORTED_MODELS = (
    "A52", "A55", "A60", "A62", "A65", "C55", "C60", "CF62",
    "M55", "MC60", "S55", "SL55",
)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _read_json(path: Path, what: str) -> tuple[bytes, dict[str, Any]]:
    try:
        raw = path.read_bytes()
        document = json.loads(
            raw.decode("utf-8"), object_pairs_hook=_reject_duplicate_json_keys
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid {what} {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise FirmwareError(f"{what} is not a JSON object: {path}")
    return raw, document


def _catalog_path(value: Path) -> Path:
    path = value.resolve()
    return path / "catalog.json" if path.is_dir() else path


@dataclass(frozen=True)
class Catalog:
    source: str
    root: Path
    path: Path
    raw: bytes
    document: dict[str, Any]

    @property
    def sha256(self) -> str:
        return _sha256(self.raw)


@dataclass(frozen=True)
class AssemblyContext:
    model: str
    repository: Path
    layout: LoadedLayout
    community: Catalog
    official: Catalog


@dataclass(frozen=True)
class Baseline:
    artifact: dict[str, Any]
    image: bytes
    scopes: dict[str, dict[str, Any]]

    @property
    def sha256(self) -> str:
        return str(self.artifact["sha256"])


@dataclass(frozen=True)
class RegionCandidate:
    source: str
    role: str
    start: int
    end: int
    sha256: str
    payload_path: str | None
    erased: bool
    scope: dict[str, Any]
    normalization: dict[str, Any] | None
    aliases: tuple[str, ...]

    @property
    def label(self) -> str:
        sw = self.scope.get("software_version", "?")
        lg = self.scope.get("langpack", "?")
        t9 = self.scope.get("t9_version", "?")
        return f"{self.source}:{self.sha256[:12]} {sw}/{lg}/{t9}"


@dataclass(frozen=True)
class MobSwSelection:
    sha256: str
    recipe_path: str
    scope: dict[str, Any]
    fields: dict[str, Any]
    normalization: dict[str, Any]
    aliases: tuple[str, ...]


@dataclass(frozen=True)
class AssemblyRequest:
    model: str
    community_catalog: Path
    official_catalog: Path
    baseline: str | None
    region_overrides: tuple[tuple[str, str, str], ...]
    mob_sw: str | None
    flash_ids: tuple[tuple[int, int], ...]
    bootkey_action: str
    bootkey_hash: bytes | None
    bootkey_source: dict[str, Any] | None
    entry_target: int | None
    interactive: bool
    eeprom_map: Path | None = None
    eeprom_profile: str | None = None
    eeprom_imei: str | None = None
    eeprom_fsn: int | None = None
    eeprom_donor: Path | None = None
    expected_eeprom_map_hash: str | None = None
    expected_eeprom_donor_hash: str | None = None
    expected_eeprom_hash: str | None = None
    expected_eeprom_manifest: dict[str, Any] | None = None
    eeprom_profile_override: eeprom.EepromProfile | None = None
    eeprom_normalization_block_ids: frozenset[int] | None = None
    expected_catalog_hashes: dict[str, str] | None = None
    scope_hint: dict[str, int] | None = None


@dataclass(frozen=True)
class AssemblyResult:
    image: bytes
    context: AssemblyContext
    baseline: Baseline
    regions: tuple[RegionCandidate, ...]
    mob_sw: MobSwSelection | None
    request: AssemblyRequest
    scope: dict[str, int]
    serial: str
    serial_source: str
    checksum: dict[str, Any]
    eeprom: EepromCompositionResult | None
    operations: tuple[dict[str, Any], ...]
    warnings: tuple[str, ...]


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[4]


def _canonical_model(model: str, repository: Path) -> str:
    matches = [
        name for name in _SUPPORTED_MODELS if name.casefold() == model.casefold()
    ]
    if len(matches) != 1 or matches[0] not in _SUPPORTED_MODELS:
        supported = ", ".join(_SUPPORTED_MODELS)
        raise FirmwareError(f"unsupported model {model!r}; choose one of {supported}")
    return matches[0]


def _load_catalog(
    source: str,
    value: Path,
    schema: str,
    version: int,
) -> Catalog:
    path = _catalog_path(value)
    raw, document = _read_json(path, f"{source} catalog")
    if document.get("schema") != schema or document.get("schema_version") != version:
        raise FirmwareError(
            f"unsupported {source} catalog schema at {path}: "
            f"{document.get('schema')!r}/{document.get('schema_version')!r}"
        )
    return Catalog(source, path.parent, path, raw, document)


def _validate_catalog_layout(
    catalog: Catalog, loaded: LoadedLayout,
) -> None:
    value = catalog.document.get("layout")
    if not isinstance(value, dict):
        raise FirmwareError(f"{catalog.source} catalog has no layout identity")
    expected = {
        "name": loaded.layout.name,
        "base": loaded.layout.base,
        "length": loaded.layout.length,
    }
    for key, expected_value in expected.items():
        if value.get(key) != expected_value:
            raise FirmwareError(
                f"{catalog.source} catalog layout {key} "
                f"{value.get(key)!r} != {expected_value!r}"
            )


def load_assembly_context(
    model: str,
    community_catalog: Path | None = None,
    official_catalog: Path | None = None,
    expected_hashes: dict[str, str] | None = None,
) -> AssemblyContext:
    repository = _repo_root()
    canonical = _canonical_model(model, repository)
    corpus = repository / "fw/corpus" / canonical
    community = _load_catalog(
        "community",
        community_catalog or corpus / "community",
        COMMUNITY_SCHEMA,
        COMMUNITY_VERSION,
    )
    official = _load_catalog(
        "official",
        official_catalog or corpus / "official",
        OFFICIAL_SCHEMA,
        OFFICIAL_VERSION,
    )
    if expected_hashes is not None:
        for catalog in (community, official):
            expected = expected_hashes.get(catalog.source)
            if expected != catalog.sha256:
                raise FirmwareError(
                    f"{catalog.source} catalog hash {catalog.sha256} != {expected}"
                )
    loaded = load_layout(canonical)
    _validate_catalog_layout(community, loaded)
    _validate_catalog_layout(official, loaded)
    community_layout_hash = community.document["layout"].get("catalog_sha256")
    official_layout_hash = official.document["layout"].get("catalog_sha256")
    if community_layout_hash != official_layout_hash:
        raise FirmwareError(
            "community and official catalogs pin different layout sources"
        )
    return AssemblyContext(canonical, repository, loaded, community, official)


def _hash_match(identifier: str, digest: object) -> bool:
    return (
        isinstance(digest, str)
        and _HASH_PREFIX.fullmatch(identifier) is not None
        and digest.startswith(identifier.lower())
    )


def _identifier_forms(identifier: str, *roots: Path) -> set[str]:
    forms = {identifier, Path(identifier).as_posix()}
    path = Path(identifier)
    if not path.exists():
        return forms
    resolved = path.resolve()
    forms.add(resolved.as_posix())
    for root in roots:
        try:
            forms.add(resolved.relative_to(root.resolve()).as_posix())
        except ValueError:
            pass
    if resolved.is_file():
        forms.add(_sha256(resolved.read_bytes()))
    return forms


def _baseline_artifacts(context: AssemblyContext) -> list[dict[str, Any]]:
    return sorted(
        (
            item for item in context.community.document.get("artifacts", [])
            if item.get("kind") == "complete-fullflash"
            and (item.get("recipe") or {}).get("status") == "materialized"
        ),
        key=lambda item: (str(item.get("sha256")), str(item.get("id"))),
    )


def _resolve_baseline_artifact(
    context: AssemblyContext, identifier: str,
) -> dict[str, Any]:
    matches = []
    identifiers = _identifier_forms(
        identifier, context.community.root, context.repository,
    )
    for artifact in _baseline_artifacts(context):
        recipe = artifact.get("recipe") or {}
        aliases = {
            artifact.get("id"), artifact.get("path"), recipe.get("path"),
            artifact.get("sha256"),
        }
        if identifiers & aliases or any(
            _hash_match(value, artifact.get("sha256")) for value in identifiers
        ):
            matches.append(artifact)
    if not matches:
        raise FirmwareError(f"community baseline not found: {identifier}")
    if len(matches) != 1:
        raise FirmwareError(f"ambiguous community baseline: {identifier}")
    return matches[0]


def _baseline_scopes(
    context: AssemblyContext, artifact: dict[str, Any],
) -> dict[str, dict[str, Any]]:
    result = {}
    for occurrence in context.community.document.get("occurrences", []):
        if occurrence.get("source_id") != artifact.get("id"):
            continue
        role = occurrence.get("role")
        scope = occurrence.get("scope")
        if isinstance(role, str) and isinstance(scope, dict):
            result[role] = scope
    return result


def _load_baseline(
    context: AssemblyContext, identifier: str,
) -> Baseline:
    artifact = _resolve_baseline_artifact(context, identifier)
    recipe_value = (artifact.get("recipe") or {}).get("path")
    if not isinstance(recipe_value, str):
        raise FirmwareError("community baseline has no materialized recipe")
    image = reconstruct_recipe(context.community.root / recipe_value)
    if len(image) != context.layout.layout.length:
        raise FirmwareError("community baseline length does not match layout")
    metadata = artifact.get("metadata") or {}
    firmware = metadata.get("firmware") if isinstance(metadata, dict) else None
    if (
        not isinstance(firmware, dict)
        or str(firmware.get("model", "")).casefold() != context.model.casefold()
    ):
        raise FirmwareError("community baseline model does not match requested model")
    return Baseline(artifact, image, _baseline_scopes(context, artifact))


def _range_value(region: dict[str, Any], source: str) -> tuple[int, int]:
    value = region.get("layout_range" if source == "community" else "range")
    if not isinstance(value, dict):
        raise FirmwareError(f"{source} region has no layout range")
    start, end = value.get("from"), value.get("to_exclusive")
    if not isinstance(start, int) or not isinstance(end, int) or end <= start:
        raise FirmwareError(f"{source} region has an invalid layout range")
    return start, end


def _candidate_aliases(
    variant: dict[str, Any], occurrence: dict[str, Any], source: str,
) -> tuple[str, ...]:
    values: set[str] = set()
    for value in (
        variant.get("sha256"), variant.get("payload_path"),
        occurrence.get("id"), occurrence.get("payload_path"),
        occurrence.get("source_id"), occurrence.get("source_sha256"),
        occurrence.get("package_sha256"),
    ):
        if isinstance(value, str):
            values.add(value)
    for value in variant.get("symlink_paths", []):
        if isinstance(value, str):
            values.add(value)
    if source == "community":
        for match in variant.get("official_package_matches", []):
            if isinstance(match, dict):
                for key in ("payload_sha256", "path"):
                    value = match.get(key)
                    if isinstance(value, str):
                        values.add(value)
    return tuple(sorted(values))


def region_candidates(
    context: AssemblyContext, source: str, role: str,
) -> list[RegionCandidate]:
    catalog = context.community if source == "community" else context.official
    global_occurrences = {
        item.get("id"): item
        for item in catalog.document.get("occurrences", [])
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    }
    candidates: list[RegionCandidate] = []
    for region in catalog.document.get("regions", []):
        if not isinstance(region, dict) or region.get("role") != role:
            continue
        start, end = _range_value(region, source)
        for variant in region.get("variants", []):
            if not isinstance(variant, dict):
                continue
            occurrences: list[dict[str, Any]] = []
            for item in variant.get("occurrences", []):
                if isinstance(item, str):
                    occurrence = global_occurrences.get(item)
                    if occurrence is not None:
                        occurrences.append(occurrence)
                elif isinstance(item, dict):
                    occurrences.append(item)
            if not occurrences:
                occurrences = [{}]
            for occurrence in occurrences:
                scope = occurrence.get("scope")
                candidates.append(RegionCandidate(
                    source=source,
                    role=role,
                    start=start,
                    end=end,
                    sha256=str(variant.get("sha256")),
                    payload_path=(
                        str(variant["payload_path"])
                        if isinstance(variant.get("payload_path"), str) else None
                    ),
                    erased=variant.get("erased") is True,
                    scope=dict(scope) if isinstance(scope, dict) else {},
                    normalization=(
                        dict(occurrence["normalization"])
                        if isinstance(occurrence.get("normalization"), dict)
                        else None
                    ),
                    aliases=_candidate_aliases(variant, occurrence, source),
                ))
    unique: dict[tuple[Any, ...], RegionCandidate] = {}
    for candidate in candidates:
        key = (
            candidate.sha256, candidate.payload_path,
            candidate.scope.get("software_version"),
            candidate.scope.get("langpack"), candidate.scope.get("t9_version"),
        )
        existing = unique.get(key)
        if existing is None:
            unique[key] = candidate
        else:
            unique[key] = replace(
                existing,
                aliases=tuple(sorted(set(existing.aliases) | set(candidate.aliases))),
            )
    return sorted(unique.values(), key=lambda item: (
        item.scope.get("software_version")
        if isinstance(item.scope.get("software_version"), int)
        else -1,
        str(item.scope.get("langpack")),
        item.scope.get("t9_version") if isinstance(item.scope.get("t9_version"), int) else -1,
        item.sha256,
    ))


def _role_names(context: AssemblyContext) -> tuple[str, ...]:
    return tuple(part.label for part in partition_layout(context.layout.layout))


def _canonical_role(context: AssemblyContext, value: str) -> str:
    matches = [role for role in _role_names(context) if role.casefold() == value.casefold()]
    if len(matches) != 1:
        raise FirmwareError(f"unknown or ambiguous layout role: {value}")
    return matches[0]


def _role_part(context: AssemblyContext, role: str) -> Any:
    matches = [
        part for part in partition_layout(context.layout.layout)
        if part.label == role
    ]
    if len(matches) != 1:
        raise FirmwareError(f"layout role has no unique range: {role}")
    return matches[0]


def _tuple_key(
    context: AssemblyContext, candidate: RegionCandidate,
) -> str | None:
    sw = candidate.scope.get("software_version")
    lg_match = _LG.fullmatch(str(candidate.scope.get("langpack") or ""))
    lg = int(lg_match.group(1)) if lg_match else None
    t9 = candidate.scope.get("t9_version")
    if "T9" not in _role_names(context):
        t9 = 0
    if not all(isinstance(value, int) and 0 <= value <= 99 for value in (sw, lg, t9)):
        return None
    return f"{sw:02d}{lg:02d}{t9:02d}"


def _selection_key(context: AssemblyContext, candidate: RegionCandidate) -> str:
    return _tuple_key(context, candidate) or candidate.sha256[:12]


def _erased_candidate(
    context: AssemblyContext, role: str,
) -> RegionCandidate:
    part = _role_part(context, role)
    digest = _sha256(b"\xFF" * (part.end - part.start))
    return RegionCandidate(
        source="erased",
        role=role,
        start=part.start,
        end=part.end,
        sha256=digest,
        payload_path=None,
        erased=True,
        scope={},
        normalization=None,
        aliases=("all-ff", digest),
    )


def _scope_score(candidate: RegionCandidate, baseline_scope: dict[str, Any]) -> int:
    return sum(
        candidate.scope.get(key) == baseline_scope.get(key)
        for key in ("software_version", "langpack", "t9_version")
        if candidate.scope.get(key) is not None
    )


def _resolve_region_candidate(
    context: AssemblyContext,
    source: str,
    role: str,
    identifier: str,
    baseline_scope: dict[str, Any],
) -> RegionCandidate:
    candidates = region_candidates(context, source, role)
    tuple_matches = [
        item for item in candidates
        if _tuple_key(context, item) == identifier
    ]
    if tuple_matches:
        payloads = {item.sha256 for item in tuple_matches}
        if len(payloads) != 1:
            raise FirmwareError(
                f"ambiguous {source} {role} tuple {identifier}; "
                "select a SHA prefix or path"
            )
        return sorted(
            tuple_matches, key=lambda item: (item.sha256, item.payload_path or "")
        )[0]
    catalog = context.community if source == "community" else context.official
    identifiers = _identifier_forms(
        identifier, catalog.root, context.repository,
    )
    matches = [
        item for item in candidates
        if identifiers.intersection(item.aliases)
        or any(_hash_match(value, item.sha256) for value in identifiers)
    ]
    if not matches:
        raise FirmwareError(f"{source} {role} candidate not found: {identifier}")
    best_score = max(_scope_score(item, baseline_scope) for item in matches)
    matches = [item for item in matches if _scope_score(item, baseline_scope) == best_score]
    if len({item.sha256 for item in matches}) != 1:
        raise FirmwareError(f"ambiguous {source} {role} candidate: {identifier}")
    return sorted(matches, key=lambda item: (item.sha256, item.payload_path or ""))[0]


def _candidate_payload(context: AssemblyContext, candidate: RegionCandidate) -> bytes:
    length = candidate.end - candidate.start
    if candidate.erased:
        payload = b"\xFF" * length
    else:
        if candidate.payload_path is None:
            raise FirmwareError(f"{candidate.source} {candidate.role} has no payload")
        root = context.community.root if candidate.source == "community" else context.official.root
        path = root / candidate.payload_path
        if not path.is_file():
            raise FirmwareError(f"catalog payload is missing: {path}")
        payload = path.read_bytes()
    if len(payload) != length or _sha256(payload) != candidate.sha256:
        raise FirmwareError(
            f"{candidate.source} {candidate.role} payload size or hash mismatch"
        )
    return payload


def _mobsw_selections(context: AssemblyContext) -> list[MobSwSelection]:
    result = []
    for package in context.official.document.get("packages", []):
        if not isinstance(package, dict):
            continue
        metadata = package.get("metadata") or {}
        fields = metadata.get("fields") if isinstance(metadata, dict) else None
        if not isinstance(fields, dict) or fields.get("update_type") != "MobSw":
            continue
        recipe = package.get("recipe") or {}
        recipe_path = recipe.get("path")
        if not isinstance(recipe_path, str) or recipe.get("status") != "materialized":
            continue
        _raw, document = _read_json(
            context.official.root / recipe_path, "official MobSw recipe"
        )
        if (
            document.get("schema") != OFFICIAL_RECIPE_SCHEMA
            or document.get("schema_version") != OFFICIAL_RECIPE_VERSION
        ):
            raise FirmwareError(f"unsupported official MobSw recipe: {recipe_path}")
        normalizations = [
            operation.get("normalization")
            for operation in document.get("operations", [])
            if isinstance(operation, dict)
            and isinstance(operation.get("normalization"), dict)
            and operation["normalization"].get("kind") == "pmb7850-statistics-fields-erased"
        ]
        if len(normalizations) != 1:
            continue
        aliases = {str(package.get("sha256")), recipe_path}
        for source in package.get("sources", []):
            if isinstance(source, dict) and isinstance(source.get("path"), str):
                aliases.add(source["path"])
        result.append(MobSwSelection(
            sha256=str(package.get("sha256")),
            recipe_path=recipe_path,
            scope=dict(package.get("scope") or {}),
            fields=dict(fields),
            normalization=dict(normalizations[0]),
            aliases=tuple(sorted(aliases)),
        ))
    return sorted(result, key=lambda item: (
        item.scope.get("software_version", -1),
        str(item.scope.get("langpack")),
        item.scope.get("t9_version", -1), item.sha256,
    ))


def _resolve_mobsw(context: AssemblyContext, identifier: str) -> MobSwSelection:
    identifiers = _identifier_forms(
        identifier, context.official.root, context.repository,
    )
    matches = [
        item for item in _mobsw_selections(context)
        if identifiers.intersection(item.aliases)
        or any(_hash_match(value, item.sha256) for value in identifiers)
    ]
    if not matches:
        raise FirmwareError(f"official MobSw package not found: {identifier}")
    if len(matches) != 1:
        raise FirmwareError(f"ambiguous official MobSw package: {identifier}")
    selected = matches[0]
    if (
        selected.normalization.get("statistics_layout_offset")
        != context.layout.layout.statistic_offset
    ):
        raise FirmwareError("MobSw statistics location does not match the layout")
    return selected


def _short_hashes(candidates: Iterable[RegionCandidate]) -> dict[str, str]:
    digests = sorted({item.sha256 for item in candidates})
    length = 12
    while length < 64 and len({digest[:length] for digest in digests}) != len(digests):
        length += 1
    return {digest: digest[:length] for digest in digests}


def _resolve_explicit_regions(
    context: AssemblyContext,
    baseline: Baseline,
    overrides: Iterable[tuple[str, str, str]],
    scope_hint: dict[str, int] | None = None,
) -> tuple[RegionCandidate, ...]:
    order = {role: index for index, role in enumerate(_role_names(context))}
    selections = []
    seen_roles = set()
    for raw_role, source, identifier in overrides:
        role = _canonical_role(context, raw_role)
        if role in seen_roles:
            raise FirmwareError(f"region selected more than once: {role}")
        seen_roles.add(role)
        selections.append((order[role], role, source, identifier))
    resolved = []
    preferred_scope: dict[str, Any] = {}
    if scope_hint is not None:
        preferred_scope = {
            "software_version": scope_hint["software_version"],
            "langpack": f"lg{scope_hint['langpack']}",
            "t9_version": scope_hint["t9_version"],
        }
    for _index, role, source, identifier in sorted(selections):
        if source == "erased":
            if identifier != "all-ff":
                raise FirmwareError("erased region identifier must be all-ff")
            resolved.append(_erased_candidate(context, role))
        elif source in ("community", "official"):
            candidate_scope = dict(baseline.scopes.get(role, {}))
            candidate_scope.update(preferred_scope)
            resolved.append(_resolve_region_candidate(
                context, source, role, identifier, candidate_scope,
            ))
        else:
            raise FirmwareError(f"unsupported region source: {source}")
    return tuple(resolved)


def parse_flash_id(value: str) -> tuple[int, int]:
    fields = value.split(":")
    if len(fields) != 2:
        raise ValueError("flash ID must be MFR:DEVICE")
    try:
        manufacturer, device = (int(field, 0) for field in fields)
    except ValueError as exc:
        raise ValueError("flash ID must contain integer MFR:DEVICE values") from exc
    if not 0 <= manufacturer <= 0xFFFF or not 0 <= device <= 0xFFFF:
        raise ValueError("flash ID values must fit in 16 bits")
    return manufacturer, device


def parse_bootkey(value: str) -> bytes:
    try:
        result = bytes.fromhex(value)
    except ValueError as exc:
        raise ValueError("BOOTKEY must be exactly 32 hexadecimal digits") from exc
    if len(result) != 16:
        raise ValueError("BOOTKEY must be exactly 32 hexadecimal digits")
    return result


def _stat_fields(page: bytes) -> dict[str, Any]:
    parsed = inspect_statistics(page)
    if parsed.get("status") != "parsed":
        raise FirmwareError(
            "statistics page is not structurally valid: "
            f"{parsed.get('reason', parsed.get('status'))}"
        )
    return parsed


def _number(fields: dict[str, Any], name: str) -> int | None:
    value = fields.get(name)
    number = value.get("value") if isinstance(value, dict) else None
    return number if isinstance(number, int) else None


def _baseline_flash_ids(
    baseline: Baseline, page: bytes,
) -> tuple[tuple[int, int], ...]:
    result = []
    try:
        fields = _stat_fields(page)["fields"]
    except FirmwareError:
        fields = {}
    else:
        for index in range(2):
            manufacturer = _number(fields, f"descriptor_{index}_manufacturer_id")
            device = _number(fields, f"descriptor_{index}_device_id")
            if manufacturer is not None and device is not None:
                result.append((manufacturer, device))
    if result:
        return tuple(result)
    firmware = (baseline.artifact.get("metadata") or {}).get("firmware") or {}
    flash = firmware.get("flash") if isinstance(firmware, dict) else None
    if isinstance(flash, dict):
        try:
            return ((int(flash["manufacturer_id"], 0), int(flash["device_id"], 0)),)
        except (KeyError, TypeError, ValueError):
            pass
    raise FirmwareError("baseline has no usable flash ID for statistics")


def _write_ranges(page: bytearray, normalization: dict[str, Any]) -> None:
    ranges = normalization.get("checksum_ranges")
    if not isinstance(ranges, list) or not ranges:
        raise FirmwareError("MobSw statistics have no checksum ranges")
    cursor = 0x80
    for pair in ranges:
        if not isinstance(pair, dict):
            raise FirmwareError("MobSw checksum range is invalid")
        start = (pair.get("start") or {}).get("value")
        end = (pair.get("end") or {}).get("value")
        if not isinstance(start, int) or not isinstance(end, int):
            raise FirmwareError("MobSw checksum range endpoints are invalid")
        struct.pack_into("<II", page, cursor, start, end)
        cursor += 8
    if cursor + 4 > 0x100:
        raise FirmwareError("MobSw checksum range table is too large")
    page[cursor:0x100] = b"\xFF" * (0x100 - cursor)


def _write_flash_ids(page: bytearray, flash_ids: tuple[tuple[int, int], ...]) -> None:
    if not 1 <= len(flash_ids) <= 2:
        raise FirmwareError("statistics require one or two flash IDs")
    page[0x18:0x1A] = (
        ((flash_ids[0][1] & 0xFF) << 8) | (flash_ids[0][0] & 0xFF)
    ).to_bytes(2, "little")
    for index in range(2):
        offset = 0x26 + index * 0x0C
        page[offset:offset + 0x0C] = b"\xFF" * 0x0C
        if index < len(flash_ids):
            manufacturer, device = flash_ids[index]
            struct.pack_into("<HH", page, offset, manufacturer, device)
            struct.pack_into("<H", page, offset + 8, 0)
            struct.pack_into("<H", page, offset + 10, 0)


def _synthesize_statistics(
    current: bytes,
    selected: MobSwSelection,
    flash_ids: tuple[tuple[int, int], ...],
) -> bytes:
    page = bytearray(current)
    page[:0x100] = b"\xFF" * 0x100
    page[0x1A:0x1C] = b"\x00\x00"
    page[0x1C:0x22] = b"\x00" * 6
    page[0x24:0x26] = b"\x10\x00"
    _write_flash_ids(page, flash_ids)
    timestamp = selected.fields.get("reconfigure_time")
    if isinstance(timestamp, str):
        rendered = timestamp.replace(" ", "")
        if len(rendered) == 16 and rendered.isascii():
            page[0x40:0x50] = rendered.encode("ascii")
    _write_ranges(page, selected.normalization)
    return bytes(page)


def _patch_flash_ids(
    current: bytes, flash_ids: tuple[tuple[int, int], ...],
) -> bytes:
    _stat_fields(current)
    page = bytearray(current)
    _write_flash_ids(page, flash_ids)
    return bytes(page)


def _checksum_statistics(image: bytearray, statistic_offset: int) -> dict[str, Any]:
    page = bytes(image[statistic_offset:statistic_offset + PAGE_SIZE])
    parsed = _stat_fields(page)
    xor16 = 0
    sum16 = 0
    rendered_ranges = []
    for pair in parsed["checksum_ranges"]:
        start = pair["start"].get("value")
        end = pair["end"].get("value")
        if (
            not isinstance(start, int) or not isinstance(end, int)
            or start < 0 or end <= start or end > len(image)
            or start & 1 or end & 1
        ):
            raise FirmwareError("statistics checksum range exceeds the final image")
        for offset in range(start, end, 2):
            word = image[offset] | image[offset + 1] << 8
            xor16 ^= word
            sum16 = (sum16 + word) & 0xFFFF
        rendered_ranges.append({"from": start, "to_exclusive": end})
    struct.pack_into("<HH", image, statistic_offset + 0x10, xor16, sum16)
    return {"xor16": xor16, "sum16": sum16, "ranges": rendered_ranges}


def _owning_role(
    context: AssemblyContext, offset: int, length: int,
) -> str:
    matches = [
        part.label for part in partition_layout(context.layout.layout)
        if part.start <= offset and offset + length <= part.end
    ]
    if len(matches) != 1:
        raise FirmwareError(
            f"range {offset:#x}..{offset + length:#x} has no unique layout owner"
        )
    return matches[0]


def _bcore_field_offset(context: AssemblyContext) -> int:
    parts = [part for part in partition_layout(context.layout.layout) if part.label == "BCORE"]
    if len(parts) != 1 or parts[0].end - parts[0].start < 0x340:
        raise FirmwareError("layout has no validated BCORE +0x330 field")
    return parts[0].start + 0x330


def _jmps(target: int) -> bytes:
    if not 0 <= target <= 0xFFFFFF:
        raise FirmwareError("entry target must fit in a 24-bit C166 address")
    return bytes((0xFA, target >> 16)) + (target & 0xFFFF).to_bytes(2, "little")


def _effective_scope(
    context: AssemblyContext,
    baseline: Baseline, regions: Iterable[RegionCandidate],
) -> dict[str, int]:
    firmware = (baseline.artifact.get("metadata") or {}).get("firmware") or {}
    sw = firmware.get("software_version")
    langpack_scope = baseline.scopes.get("LangPack", {})
    t9_scope = baseline.scopes.get("T9", {})
    lg_value = langpack_scope.get("langpack", firmware.get("langpack"))
    t9 = t9_scope.get("t9_version")
    if t9 is None:
        t9 = langpack_scope.get("t9_version")
    if t9 is None and "T9" not in _role_names(context):
        t9 = 0
    match = _LG.fullmatch(str(lg_value or ""))
    lg = int(match.group(1)) if match else None
    for region in regions:
        if region.role == "LangPack":
            match = _LG.fullmatch(str(region.scope.get("langpack") or ""))
            if match:
                lg = int(match.group(1))
        elif region.role == "T9" and isinstance(region.scope.get("t9_version"), int):
            t9 = region.scope["t9_version"]
        elif (
            region.role.startswith("UNKNOWN_")
            and isinstance(region.scope.get("software_version"), int)
        ):
            sw = region.scope["software_version"]
    if not all(isinstance(value, int) and value >= 0 for value in (sw, lg, t9)):
        raise FirmwareError("cannot derive numeric SW/LG/T9 for synthetic filename")
    return {"software_version": sw, "langpack": lg, "t9_version": t9}


def _imei_serial(image: bytes) -> tuple[str | None, str]:
    try:
        region = eeprom.find_eeprom_region(image)
        inventory = eeprom.load_dump_blocks(image, region)
    except ValueError:
        return None, "output-hash-fallback"
    values = []
    for block_id in (76, 5009):
        if block_id not in inventory:
            continue
        value = eeprom.extract_block(inventory, block_id).get("imei")
        if isinstance(value, str) and len(value) == 14 and value.isdigit():
            values.append(value)
    return (
        (values[0][-6:], "consistent-eeprom-imei")
        if values and len(set(values)) == 1
        else (None, "output-hash-fallback")
    )


def _compatibility_warnings(
    scope: dict[str, int],
    regions: Iterable[RegionCandidate],
    mob_sw: MobSwSelection | None,
) -> list[str]:
    warnings = []
    for item in regions:
        mismatches = []
        sw = item.scope.get("software_version")
        lg_text = item.scope.get("langpack")
        t9 = item.scope.get("t9_version")
        lg_match = _LG.fullmatch(str(lg_text or ""))
        lg = int(lg_match.group(1)) if lg_match else None
        for name, observed, expected in (
            ("SW", sw, scope["software_version"]),
            ("LG", lg, scope["langpack"]),
            ("T9", t9, scope["t9_version"]),
        ):
            if observed is not None and observed != expected:
                mismatches.append(f"{name}{observed} != {expected}")
        if mismatches:
            warnings.append(
                f"{item.source} {item.role} {item.sha256[:12]} scope mismatch: "
                + ", ".join(mismatches)
            )
    if mob_sw is not None:
        sw = mob_sw.scope.get("software_version")
        if isinstance(sw, int) and sw != scope["software_version"]:
            warnings.append(
                f"MobSw {mob_sw.sha256[:12]} SW{sw} does not match final "
                f"SW{scope['software_version']}"
            )
    return warnings


def _personalization_conflicts(
    context: AssemblyContext,
    regions: Iterable[RegionCandidate],
    mob_sw: str | MobSwSelection | None,
    flash_ids: tuple[tuple[int, int], ...],
    bootkey_action: str,
    entry_target: int | None,
) -> list[str]:
    layout = context.layout.layout
    statistic_offset = layout.statistic_offset
    entry_offset = layout.entry_transfer_offset
    if statistic_offset is None or entry_offset is None:
        return ["layout has no statistics/finalization configuration"]
    erased_roles = {
        candidate.role for candidate in regions
        if candidate.source == "erased"
    }
    statistic_role = _owning_role(context, statistic_offset, PAGE_SIZE)
    entry_role = _owning_role(context, entry_offset, 4)
    bcore_role = _owning_role(context, _bcore_field_offset(context), 16)
    conflicts = []
    if statistic_role in erased_roles and mob_sw is not None:
        conflicts.append(
            f"--mobsw conflicts with erased statistics-owning role {statistic_role}"
        )
    if statistic_role in erased_roles and flash_ids:
        conflicts.append(
            f"--flash-id conflicts with erased statistics-owning role {statistic_role}"
        )
    if bcore_role in erased_roles and bootkey_action == "write-hash":
        conflicts.append(f"BOOTKEY write conflicts with erased owning role {bcore_role}")
    if entry_role in erased_roles and entry_target is not None:
        conflicts.append(
            f"--entry-target conflicts with erased owning role {entry_role}"
        )
    return conflicts


def _eeprom_conflicts(
    regions: Iterable[RegionCandidate], eeprom_map: Path | None,
) -> list[str]:
    if eeprom_map is None:
        return []
    return [
        "--eeprom-map conflicts with an explicit EEPROM region selection"
        for candidate in regions if candidate.role == "EEPROM"
    ][:1]


def _assemble_request(request: AssemblyRequest) -> AssemblyResult:
    context = load_assembly_context(
        request.model,
        request.community_catalog,
        request.official_catalog,
        request.expected_catalog_hashes,
    )
    baseline_id = request.baseline
    if baseline_id is None:
        raise FirmwareError("non-interactive assembly requires --baseline")
    baseline = _load_baseline(context, baseline_id)
    resolved = _resolve_explicit_regions(
        context, baseline, request.region_overrides, request.scope_hint,
    )
    mob_sw = _resolve_mobsw(context, request.mob_sw) if request.mob_sw else None

    layout = context.layout.layout
    statistic_offset = layout.statistic_offset
    entry_offset = layout.entry_transfer_offset
    if statistic_offset is None or entry_offset is None:
        raise FirmwareError("layout has no statistics/finalization configuration")
    if (
        statistic_offset + PAGE_SIZE > len(baseline.image)
        or entry_offset + 4 > len(baseline.image)
    ):
        raise FirmwareError("layout statistics/finalization locations exceed the image")
    bcore_offset = _bcore_field_offset(context)
    statistic_role = _owning_role(context, statistic_offset, PAGE_SIZE)
    entry_role = _owning_role(context, entry_offset, 4)
    bcore_role = _owning_role(context, bcore_offset, 16)
    erased_roles = {
        candidate.role for candidate in resolved
        if candidate.source == "erased"
    }
    conflicts = _personalization_conflicts(
        context, resolved, mob_sw, request.flash_ids,
        request.bootkey_action, request.entry_target,
    )
    conflicts.extend(_eeprom_conflicts(resolved, request.eeprom_map))
    if conflicts:
        raise FirmwareError(conflicts[0])

    baseline_page = baseline.image[statistic_offset:statistic_offset + PAGE_SIZE]
    baseline_entry = baseline.image[entry_offset:entry_offset + 4]
    baseline_bcore = baseline.image[bcore_offset:bcore_offset + 16]

    image = bytearray(baseline.image)
    scope = _effective_scope(context, baseline, resolved)
    if request.scope_hint is not None and scope != request.scope_hint:
        raise FirmwareError(
            f"synthetic recipe scope {request.scope_hint} resolved as {scope}"
        )
    operations: list[dict[str, Any]] = [{
        "kind": "community-baseline", "sha256": baseline.sha256,
        "recipe": (baseline.artifact.get("recipe") or {}).get("path"),
    }]
    for candidate in resolved:
        image[candidate.start:candidate.end] = _candidate_payload(context, candidate)
        operations.append({
            "kind": "region-override", "source": candidate.source,
            "identifier": "all-ff" if candidate.source == "erased" else candidate.sha256,
            "role": candidate.role, "sha256": candidate.sha256,
            "payload_path": candidate.payload_path,
            "range": {"from": candidate.start, "to_exclusive": candidate.end},
        })

    if statistic_role not in erased_roles:
        image[statistic_offset:statistic_offset + 0x100] = baseline_page[:0x100]
        image[statistic_offset + 0x1FC:statistic_offset + 0x200] = baseline_page[0x1FC:]
    if bcore_role not in erased_roles:
        image[bcore_offset:bcore_offset + 16] = baseline_bcore
    if entry_role not in erased_roles:
        image[entry_offset:entry_offset + 4] = baseline_entry

    if statistic_role not in erased_roles:
        current_page = bytes(image[statistic_offset:statistic_offset + PAGE_SIZE])
        if mob_sw is not None:
            flash_ids = request.flash_ids or _baseline_flash_ids(baseline, baseline_page)
            current_page = _synthesize_statistics(current_page, mob_sw, flash_ids)
            operations.append({
                "kind": "mobsw-statistics", "sha256": mob_sw.sha256,
                "generation": 0,
                "flash_ids": [
                    {"manufacturer": manufacturer, "device": device}
                    for manufacturer, device in flash_ids
                ],
            })
        elif request.flash_ids:
            current_page = _patch_flash_ids(current_page, request.flash_ids)
            operations.append({
                "kind": "flash-ids",
                "flash_ids": [
                    {"manufacturer": manufacturer, "device": device}
                    for manufacturer, device in request.flash_ids
                ],
            })
        image[statistic_offset:statistic_offset + PAGE_SIZE] = current_page

    warnings = []
    if request.bootkey_action == "write-hash":
        if request.bootkey_hash is None or len(request.bootkey_hash) != 16:
            raise FirmwareError("BOOTKEY hash must be exactly 16 bytes")
        image[bcore_offset:bcore_offset + 16] = request.bootkey_hash
        operations.append({
            "kind": "bcore-bootkey-digest",
            "offset": bcore_offset,
            "value": request.bootkey_hash.hex(),
        })
        if context.model != "C55":
            warnings.append(
                "BCORE BOOTKEY field placement is validated cross-model, but "
                "authentication semantics are proven only on C55"
            )
    elif request.bootkey_action == "erase":
        image[bcore_offset:bcore_offset + 16] = b"\xFF" * 16
        operations.append({"kind": "erase-bcore-bootkey", "offset": bcore_offset})
    elif request.bootkey_action != "preserve":
        raise FirmwareError(f"unsupported BOOTKEY action: {request.bootkey_action}")

    if request.entry_target is not None:
        encoded = _jmps(request.entry_target)
        image[entry_offset:entry_offset + 4] = encoded
        operations.append({
            "kind": "entry-transfer", "offset": entry_offset,
            "target": request.entry_target, "value": encoded.hex(),
        })

    eeprom_result = None
    if request.eeprom_map is not None:
        eeprom_result = compose_eeprom(
            request.eeprom_map,
            model=context.model,
            software_version=scope["software_version"],
            profile_name=request.eeprom_profile,
            imei=request.eeprom_imei,
            fsn=request.eeprom_fsn,
            donor_path=request.eeprom_donor,
            profile_override=request.eeprom_profile_override,
            normalization_block_ids=request.eeprom_normalization_block_ids,
        )
        actual_map_hash = eeprom_result.manifest["map"]["sha256"]
        if (
            request.expected_eeprom_map_hash is not None
            and actual_map_hash != request.expected_eeprom_map_hash
        ):
            raise FirmwareError(
                f"EEPROM map SHA-256 {actual_map_hash} != recipe "
                f"{request.expected_eeprom_map_hash}"
            )
        donor = eeprom_result.manifest["donor"]
        actual_donor_hash = donor["sha256"] if donor is not None else None
        if (
            request.expected_eeprom_donor_hash is not None
            and actual_donor_hash != request.expected_eeprom_donor_hash
        ):
            raise FirmwareError(
                f"EEPROM donor SHA-256 {actual_donor_hash} != recipe "
                f"{request.expected_eeprom_donor_hash}"
            )
        actual_eeprom_hash = eeprom_result.manifest["sha256"]
        if (
            request.expected_eeprom_hash is not None
            and actual_eeprom_hash != request.expected_eeprom_hash
        ):
            raise FirmwareError(
                f"EEPROM SHA-256 {actual_eeprom_hash} != recipe "
                f"{request.expected_eeprom_hash}"
            )
        if (
            request.expected_eeprom_manifest is not None
            and eeprom_result.manifest != request.expected_eeprom_manifest
        ):
            raise FirmwareError(
                "EEPROM composition manifest does not match schema-4 recipe"
            )
        eeprom_part = _role_part(context, "EEPROM")
        if eeprom_part.end - eeprom_part.start != len(eeprom_result.image):
            raise FirmwareError(
                "packed EEPROM size does not match the layout EEPROM range"
            )
        image[eeprom_part.start:eeprom_part.end] = eeprom_result.image
        operations.append({
            "kind": "logical-eeprom",
            "range": {"from": eeprom_part.start, "to_exclusive": eeprom_part.end},
            **eeprom_result.manifest,
        })

    if statistic_role in erased_roles:
        checksum = {}
        operations.append({
            "kind": "statistics-checksum-skipped",
            "reason": "owning-role-erased",
            "role": statistic_role,
        })
        warnings.append(
            f"statistics checksum skipped because {statistic_role} is erased"
        )
    else:
        checksum = _checksum_statistics(image, statistic_offset)
        operations.append({"kind": "statistics-checksum", **checksum})
    warnings.extend(_compatibility_warnings(scope, resolved, mob_sw))
    if eeprom_result is not None and request.eeprom_profile is not None:
        profile = eeprom.get_eeprom_profile(request.eeprom_profile)
        warnings.append(profile.warning)
        map_software_version = eeprom_result.manifest["map"]["software_version"]
        if map_software_version != scope["software_version"]:
            warnings.append(
                f"EEPROM profile {profile.name} explicitly assumes its SW"
                f"{map_software_version} factory map is compatible with assembled "
                f"SW{scope['software_version']}; this combination is not UI-qualified"
            )
    final_image = bytes(image)
    serial, serial_source = _imei_serial(final_image)
    digest = _sha256(final_image)
    if serial is None:
        serial = digest[:6]
        warnings.append(
            "EEPROM has no single consistent IMEI; filename serial uses the first "
            "six output-hash characters"
        )
    canonical_request = replace(
        request,
        model=context.model,
        baseline=baseline.sha256,
        region_overrides=tuple(
            (
                item.role,
                item.source,
                "all-ff" if item.source == "erased" else item.sha256,
            )
            for item in resolved
        ),
        mob_sw=mob_sw.sha256 if mob_sw else None,
        eeprom_map=(
            Path(eeprom_result.manifest["map"]["path"])
            if eeprom_result is not None else None
        ),
        eeprom_donor=(
            Path(eeprom_result.manifest["donor"]["path"])
            if eeprom_result is not None
            and eeprom_result.manifest["donor"] is not None else None
        ),
        expected_eeprom_map_hash=(
            eeprom_result.manifest["map"]["sha256"]
            if eeprom_result is not None else None
        ),
        expected_eeprom_donor_hash=(
            eeprom_result.manifest["donor"]["sha256"]
            if eeprom_result is not None
            and eeprom_result.manifest["donor"] is not None else None
        ),
        expected_eeprom_hash=(
            eeprom_result.manifest["sha256"]
            if eeprom_result is not None else None
        ),
        expected_eeprom_manifest=(
            eeprom_result.manifest if eeprom_result is not None else None
        ),
        flash_ids=request.flash_ids,
        interactive=False,
        expected_catalog_hashes={
            "community": context.community.sha256,
            "official": context.official.sha256,
        },
    )
    return AssemblyResult(
        final_image, context, baseline, tuple(resolved), mob_sw,
        canonical_request, scope, serial, serial_source, checksum, eeprom_result,
        tuple(operations), tuple(warnings),
    )


def _default_output(result: AssemblyResult) -> Path:
    scope = result.scope
    digest = _sha256(result.image)
    stem = (
        f"{result.context.model.lower()}sw{scope['software_version']:02d}"
        f"{scope['langpack']:02d}{scope['t9_version']:02d}-"
        f"{result.serial}-{digest[:12]}"
    )
    return result.context.repository / "fw/corpus" / result.context.model / "synth" / f"{stem}.bin"


def _recipe_region(candidate: RegionCandidate) -> dict[str, Any]:
    result = {
        "role": candidate.role,
        "source": candidate.source,
        "identifier": "all-ff" if candidate.source == "erased" else candidate.sha256,
    }
    if candidate.source == "erased":
        result.update({
            "range": {"from": candidate.start, "to_exclusive": candidate.end},
            "sha256": candidate.sha256,
        })
    return result


def _recipe_eeprom(result: AssemblyResult) -> dict[str, Any] | None:
    if result.eeprom is None:
        return None
    manifest = result.eeprom.manifest
    return manifest


def _recipe_document(result: AssemblyResult, output: Path) -> dict[str, Any]:
    request = result.request
    return {
        "schema": SYNTHETIC_RECIPE_SCHEMA,
        "schema_version": SYNTHETIC_RECIPE_VERSION,
        "model": result.context.model,
        "layout": {
            "name": result.context.layout.layout.name,
            "base": result.context.layout.layout.base,
            "length": result.context.layout.layout.length,
            "catalog_sha256": _sha256(result.context.layout.catalog_bytes),
        },
        "catalogs": {
            "community": {
                "path": str(result.context.community.path),
                "sha256": result.context.community.sha256,
                "schema_version": COMMUNITY_VERSION,
            },
            "official": {
                "path": str(result.context.official.path),
                "sha256": result.context.official.sha256,
                "schema_version": OFFICIAL_VERSION,
            },
        },
        "configuration": {
            "baseline": request.baseline,
            "regions": [_recipe_region(item) for item in result.regions],
            "mobsw": request.mob_sw,
            "eeprom": _recipe_eeprom(result),
            "flash_ids": [
                {"manufacturer": manufacturer, "device": device}
                for manufacturer, device in request.flash_ids
            ],
            "bootkey": {
                "action": request.bootkey_action,
                "hash": request.bootkey_hash.hex() if request.bootkey_hash else None,
                "source": request.bootkey_source,
            },
            "entry_target": request.entry_target,
        },
        "scope": result.scope,
        "identity": {
            "filename_serial": result.serial,
            "filename_serial_source": result.serial_source,
        },
        "application_order": [item["kind"] for item in result.operations],
        "operations": list(result.operations),
        "warnings": list(result.warnings),
        "output": {
            "path": str(output),
            "size": len(result.image),
            "sha256": _sha256(result.image),
        },
    }


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".part", dir=path.parent,
    )
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def _write_output_pair(
    output: Path, image: bytes, recipe: dict[str, Any], force: bool,
) -> tuple[Path, str]:
    recipe_path = output.with_suffix(".json")
    if recipe_path == output:
        raise FirmwareError("binary output path must not use the .json suffix")
    recipe_data = (json.dumps(recipe, indent=2, sort_keys=True) + "\n").encode("utf-8")
    existing_image = output.read_bytes() if output.is_file() else None
    existing_recipe = recipe_path.read_bytes() if recipe_path.is_file() else None
    if existing_image == image and existing_recipe == recipe_data:
        return recipe_path, "reused"
    if not force and (output.exists() or recipe_path.exists()):
        conflict = output if output.exists() else recipe_path
        raise FirmwareError(f"output already exists: {conflict} (use --force)")
    _atomic_write(output, image)
    _atomic_write(recipe_path, recipe_data)
    return recipe_path, "written"


def _request_from_args(args: Any) -> AssemblyRequest:
    if args.model is None:
        raise FirmwareError("assemble requires MODEL or --recipe")
    repository = _repo_root()
    model = _canonical_model(args.model, repository)
    corpus = repository / "fw/corpus" / model
    regions = []
    for value in args.regions:
        try:
            role, selection = value.split("=", 1)
            if selection.casefold() == "erased":
                source, identifier = "erased", "all-ff"
            else:
                source, identifier = selection.split(":", 1)
        except ValueError as exc:
            raise FirmwareError(
                "--region must be ROLE=erased, ROLE=community:ID, or "
                "ROLE=official:ID"
            ) from exc
        regions.append((role, source.casefold(), identifier))
    if len(args.flash_ids) > 2:
        raise FirmwareError("--flash-id may be supplied at most twice")
    if args.eeprom_map is None and any((
        args.eeprom_profile is not None,
        args.eeprom_imei is not None,
        args.eeprom_fsn is not None,
        args.eeprom_donor is not None,
    )):
        raise FirmwareError(
            "--eeprom-profile/--eeprom-imei/--eeprom-fsn/--eeprom-donor "
            "require --eeprom-map"
        )
    if args.eeprom_donor is not None and args.eeprom_profile is None:
        raise FirmwareError("--eeprom-donor requires --eeprom-profile")
    if (args.eeprom_imei is None) != (args.eeprom_fsn is None):
        raise FirmwareError("--eeprom-imei and --eeprom-fsn must be supplied together")
    bootkey_action = "preserve"
    bootkey_digest = None
    bootkey_source = None
    key_modes = sum((
        args.bootkey is not None,
        args.erase_bootkey,
        args.fsn is not None or args.skey is not None,
    ))
    if key_modes > 1:
        raise FirmwareError("BOOTKEY options are mutually exclusive")
    if (args.fsn is None) != (args.skey is None):
        raise FirmwareError("--fsn and --skey must be supplied together")
    if args.bootkey is not None:
        bootkey_action = "write-hash"
        bootkey_digest = hash_bootkey(args.bootkey)
        bootkey_source = {"kind": "bootkey", "bootkey": args.bootkey.hex()}
    elif args.fsn is not None:
        derived = derive_bcore_key(args.fsn, args.skey)
        bootkey_action = "write-hash"
        bootkey_digest = derived.bcore_hash
        bootkey_source = {"kind": "fsn-skey", **derived.as_dict()}
    elif args.erase_bootkey:
        bootkey_action = "erase"
        bootkey_source = {"kind": "erased"}
    return AssemblyRequest(
        model=model,
        community_catalog=args.community_catalog or corpus / "community",
        official_catalog=args.official_catalog or corpus / "official",
        baseline=args.baseline,
        region_overrides=tuple(regions),
        mob_sw=args.mobsw,
        flash_ids=tuple(args.flash_ids),
        bootkey_action=bootkey_action,
        bootkey_hash=bootkey_digest,
        bootkey_source=bootkey_source,
        entry_target=args.entry_target,
        interactive=(
            not args.non_interactive
            and sys.stdin.isatty()
            and sys.stdout.isatty()
        ),
        eeprom_map=args.eeprom_map,
        eeprom_profile=args.eeprom_profile,
        eeprom_imei=args.eeprom_imei,
        eeprom_fsn=args.eeprom_fsn,
        eeprom_donor=args.eeprom_donor,
    )


def _schema4_eeprom_replay_policy(
    selection: dict[str, Any],
) -> tuple[eeprom.EepromProfile | None, frozenset[int]]:
    """Recover a superseded zero-only profile from its schema-4 manifest.

    Schema 4 embeds every logical record but not arbitrary record payloads.  A
    historical profile is therefore replayable only when each embedded profile
    record is an identified deterministic synthesis and its digest agrees.
    Current profiles continue to come from the built-in registry so metadata
    tampering is still detected by the exact-manifest comparison.
    """
    profile_name = selection.get("profile")
    if not isinstance(profile_name, str):
        return None, frozenset()
    try:
        current = eeprom.get_eeprom_profile(profile_name)
    except ValueError as exc:
        raise FirmwareError(str(exc)) from exc

    records = selection.get("records")
    normalizations = selection.get("normalizations")
    if not isinstance(records, list) or not isinstance(normalizations, list):
        raise FirmwareError("schema-4 EEPROM manifest is incomplete")
    try:
        normalization_ids = frozenset(int(item["id"]) for item in normalizations)
    except (KeyError, TypeError, ValueError) as exc:
        raise FirmwareError("schema-4 EEPROM normalizations are invalid") from exc

    embedded_profile_records = [
        record for record in records
        if isinstance(record, dict) and record.get("source") == "profile"
    ]

    def signature(record: eeprom.ProfileRecord) -> tuple[int, int, int, int, str, int]:
        return (
            record.block_id,
            len(record.payload),
            record.memory_class,
            record.version,
            record.payload_kind,
            record.observed_bytes,
        )

    try:
        embedded_signature = tuple(
            (
                int(record["id"]),
                int(record["length"]),
                int(record["memory_class"]),
                int(record["version"]),
                str(record["payload_kind"]),
                int(record["observed_bytes"]),
            )
            for record in embedded_profile_records
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise FirmwareError("schema-4 EEPROM profile records are invalid") from exc

    if embedded_signature == tuple(signature(record) for record in current.records):
        return None, normalization_ids

    replay_records = []
    for record, fields in zip(embedded_profile_records, embedded_signature):
        block_id, length, memory_class, version, payload_kind, observed_bytes = fields
        if observed_bytes != 0 or not isinstance(record.get("provenance"), str):
            raise FirmwareError(
                f"schema-4 EEPROM profile block {block_id} cannot be replayed"
            )
        if payload_kind == "synthesized-zero" and length >= 0:
            payload = bytes(length)
        elif (
            payload_kind == "synthesized-cemu-battery-v1"
            and block_id == eeprom.BATTERY_CALIBRATION_BLOCK
            and length == eeprom.BATTERY_CALIBRATION_LENGTH
            and memory_class == 2
            and version == 2
        ):
            payload = eeprom.encode_battery_calibration(
                eeprom.CEMU_DEFAULT_BATTERY_CALIBRATION
            )
        else:
            raise FirmwareError(
                f"schema-4 EEPROM profile block {block_id} cannot be replayed"
            )
        if record.get("payload_sha256") != _sha256(payload):
            raise FirmwareError(
                f"schema-4 EEPROM profile block {block_id} cannot be replayed"
            )
        replay_records.append(eeprom.ProfileRecord(
            block_id,
            payload,
            memory_class,
            version,
            payload_kind,
            observed_bytes,
            record["provenance"],
        ))

    donor = selection.get("donor")
    donor_blocks: tuple[int, ...] = ()
    donor_sha256 = None
    if donor is not None:
        if not isinstance(donor, dict):
            raise FirmwareError("schema-4 EEPROM donor is invalid")
        try:
            donor_blocks = tuple(int(value) for value in donor["blocks"])
            donor_sha256 = str(donor["sha256"])
        except (KeyError, TypeError, ValueError) as exc:
            raise FirmwareError("schema-4 EEPROM donor is invalid") from exc
    map_selection = selection.get("map")
    omitted = selection.get("omitted_blocks")
    if not isinstance(map_selection, dict) or not isinstance(omitted, list):
        raise FirmwareError("schema-4 EEPROM profile metadata is invalid")
    try:
        omitted_blocks = tuple(int(value) for value in omitted)
        map_sha256 = str(map_selection["sha256"])
    except (KeyError, TypeError, ValueError) as exc:
        raise FirmwareError("schema-4 EEPROM profile metadata is invalid") from exc

    return eeprom.EepromProfile(
        name=current.name,
        product=current.product,
        software_versions=current.software_versions,
        ui_boot_qualified_software_versions=current.ui_boot_qualified_software_versions,
        intended_use=current.intended_use,
        warning=current.warning,
        records=tuple(replay_records),
        omitted_blocks=omitted_blocks,
        donor_blocks=donor_blocks,
        donor_sha256=donor_sha256,
        map_sha256=map_sha256,
        compatible_map_sha256s=current.compatible_map_sha256s,
        compatible_map_software_versions=current.compatible_map_software_versions,
    ), normalization_ids


def _request_from_recipe(path: Path) -> tuple[AssemblyRequest, dict[str, Any]]:
    _raw, recipe = _read_json(path, "synthetic fullflash recipe")
    if (
        recipe.get("schema") != SYNTHETIC_RECIPE_SCHEMA
        or recipe.get("schema_version") not in SYNTHETIC_RECIPE_VERSIONS
    ):
        raise FirmwareError(f"unsupported synthetic recipe schema: {path}")
    model = recipe.get("model")
    catalogs = recipe.get("catalogs")
    configuration = recipe.get("configuration")
    scope = recipe.get("scope")
    if (
        not isinstance(model, str)
        or not isinstance(catalogs, dict)
        or not isinstance(configuration, dict)
    ):
        raise FirmwareError("synthetic recipe is missing configuration")
    community = catalogs.get("community") or {}
    official = catalogs.get("official") or {}
    regions = configuration.get("regions")
    flash_ids = configuration.get("flash_ids")
    bootkey = configuration.get("bootkey") or {}
    eeprom_selection = configuration.get("eeprom")
    if (
        not isinstance(regions, list)
        or not isinstance(flash_ids, list)
        or (
            eeprom_selection is not None
            and not isinstance(eeprom_selection, dict)
        )
    ):
        raise FirmwareError("synthetic recipe selections are invalid")
    try:
        eeprom_map = None
        eeprom_profile = None
        eeprom_imei = None
        eeprom_fsn = None
        eeprom_map_hash = None
        eeprom_donor = None
        eeprom_donor_hash = None
        eeprom_hash = None
        eeprom_manifest = None
        eeprom_profile_override = None
        eeprom_normalization_block_ids = None
        if isinstance(eeprom_selection, dict):
            map_selection = eeprom_selection["map"]
            if not isinstance(map_selection, dict):
                raise TypeError("EEPROM map selection is not an object")
            eeprom_map = Path(map_selection["path"])
            eeprom_map_hash = map_selection["sha256"]
            eeprom_profile = eeprom_selection.get("profile")
            donor_selection = eeprom_selection.get("donor")
            if donor_selection is not None:
                if not isinstance(donor_selection, dict):
                    raise TypeError("EEPROM donor selection is not an object")
                eeprom_donor = Path(donor_selection["path"])
                eeprom_donor_hash = donor_selection["sha256"]
            if recipe.get("schema_version") == 4:
                eeprom_hash = eeprom_selection["sha256"]
                eeprom_manifest = eeprom_selection
                (
                    eeprom_profile_override,
                    eeprom_normalization_block_ids,
                ) = _schema4_eeprom_replay_policy(eeprom_selection)
            identity = eeprom_selection.get("identity")
            if identity is not None:
                if not isinstance(identity, dict):
                    raise TypeError("EEPROM identity selection is not an object")
                eeprom_imei = identity["imei"]
                eeprom_fsn = int(identity["fsn"], 16)
        request = AssemblyRequest(
            model=model,
            community_catalog=Path(community["path"]),
            official_catalog=Path(official["path"]),
            baseline=configuration["baseline"],
            region_overrides=tuple(
                (item["role"], item["source"], item["identifier"])
                for item in regions
            ),
            mob_sw=configuration.get("mobsw"),
            flash_ids=tuple(
                (item["manufacturer"], item["device"]) for item in flash_ids
            ),
            bootkey_action=bootkey["action"],
            bootkey_hash=(
                bytes.fromhex(bootkey["hash"])
                if isinstance(bootkey.get("hash"), str) else None
            ),
            bootkey_source=bootkey.get("source"),
            entry_target=configuration.get("entry_target"),
            interactive=False,
            eeprom_map=eeprom_map,
            eeprom_profile=eeprom_profile,
            eeprom_imei=eeprom_imei,
            eeprom_fsn=eeprom_fsn,
            eeprom_donor=eeprom_donor,
            expected_eeprom_map_hash=eeprom_map_hash,
            expected_eeprom_donor_hash=eeprom_donor_hash,
            expected_eeprom_hash=eeprom_hash,
            expected_eeprom_manifest=eeprom_manifest,
            eeprom_profile_override=eeprom_profile_override,
            eeprom_normalization_block_ids=eeprom_normalization_block_ids,
            expected_catalog_hashes={
                "community": community["sha256"],
                "official": official["sha256"],
            },
            scope_hint=(
                {
                    "software_version": int(scope["software_version"]),
                    "langpack": int(scope["langpack"]),
                    "t9_version": int(scope["t9_version"]),
                }
                if isinstance(scope, dict) else None
            ),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise FirmwareError("synthetic recipe configuration is invalid") from exc
    return request, recipe


def command_assemble(args: Any) -> None:
    if args.recipe is not None:
        conflicts = [
            name for name, selected in (
                ("MODEL", args.model is not None),
                ("--baseline", args.baseline is not None),
                ("--region", bool(args.regions)),
                ("--mobsw", args.mobsw is not None),
                ("--flash-id", bool(args.flash_ids)),
                ("--entry-target", args.entry_target is not None),
                ("--eeprom-map", args.eeprom_map is not None),
                ("--eeprom-profile", args.eeprom_profile is not None),
                ("--eeprom-imei", args.eeprom_imei is not None),
                ("--eeprom-fsn", args.eeprom_fsn is not None),
                ("--eeprom-donor", args.eeprom_donor is not None),
                ("--bootkey", args.bootkey is not None),
                ("--fsn", args.fsn is not None),
                ("--skey", args.skey is not None),
                ("--erase-bootkey", args.erase_bootkey),
                ("--community-catalog", args.community_catalog is not None),
                ("--official-catalog", args.official_catalog is not None),
                ("--non-interactive", args.non_interactive),
            )
            if selected
        ]
        if conflicts:
            raise FirmwareError(
                "--recipe cannot be combined with " + ", ".join(conflicts)
            )
        request, source_recipe = _request_from_recipe(args.recipe)
        result = _assemble_request(request)
        expected = (source_recipe.get("output") or {}).get("sha256")
        actual = _sha256(result.image)
        if expected != actual:
            raise FirmwareError(
                f"synthetic recipe reconstructed SHA-256 {actual}, expected {expected}"
            )
        source_output = (source_recipe.get("output") or {}).get("path")
        output = args.output or (Path(source_output) if isinstance(source_output, str) else None)
        if output is None:
            raise FirmwareError("synthetic recipe has no output path; pass -o")
    else:
        request = _request_from_args(args)
        output = args.output
        force = args.force
        if request.interactive:
            from .assembly_tui import run_assembly_tui

            selection = run_assembly_tui(request, output=output, force=force)
            request = selection.request
            output = selection.output
            force = selection.force
        result = _assemble_request(request)
        output = output or _default_output(result)
    recipe = _recipe_document(result, output)
    recipe_path, action = _write_output_pair(
        output, result.image, recipe,
        args.force if args.recipe is not None else force,
    )
    print(f"{action}: {output}")
    print(f"recipe: {recipe_path}")
    print(f"sha256: {_sha256(result.image)}")
    for warning in result.warnings:
        print(f"warning: {warning}", file=sys.stderr)


def _rol16(value: int, count: int) -> int:
    return ((value << count) | (value >> (16 - count))) & 0xFFFF


def _ror16(value: int, count: int) -> int:
    return ((value >> count) | (value << (16 - count))) & 0xFFFF


def _m55_secsi_esn(fsn_text: str) -> str:
    fsn = int(fsn_text, 16)
    words = (
        0,
        0,
        _rol16(fsn & 0xFFFF, 3) ^ _ror16(0x0001, 5),
        _ror16(fsn >> 16, 2) ^ _rol16(0x227E, 4),
    )
    prefix = b"".join(word.to_bytes(2, "little") for word in words)
    return (prefix + b"\xFF" * 8).hex().upper()


def _m55_secsi_customer(imei_text: str) -> str:
    digits = [int(digit) for digit in imei_text]
    total = sum(
        (value := digit * (2 if index & 1 else 1)) // 10 + value % 10
        for index, digit in enumerate(digits)
    )
    mirror = bytes(
        digits[2 * index] | digits[2 * index + 1] << 4 for index in range(7)
    ) + bytes((((10 - total % 10) % 10) << 4,))
    return mirror.hex().upper()
