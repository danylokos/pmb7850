from __future__ import annotations

import os
import re
import tempfile
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

from ..firmware.xbi import FirmwareError


LG_PATTERN = re.compile(r"lg[0-9]+", re.IGNORECASE)
UNKNOWN = "unknown"
T9_VERSION_OFFSET = 0x08
T9_HEADER_SIZE = 0x20


def software_version(value: object) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value if value >= 0 else None
    if isinstance(value, float):
        return int(value) if value >= 0 and value.is_integer() else None
    if isinstance(value, str) and value.isdigit():
        return int(value)
    return None


def normalize_lg(value: object) -> str | None:
    if not isinstance(value, str):
        return None
    candidate = value.strip().lower()
    return candidate if LG_PATTERN.fullmatch(candidate) else None


def t9_version(value: object) -> int | None:
    version = software_version(value)
    return version if version is not None and version < 0xFF else None


def embedded_t9_version(data: bytes) -> int | None:
    sample = data[:0x40]
    if (
        len(sample) < T9_HEADER_SIZE
        or not sample.startswith(b"T9")
        or sample[2:8] in (b"\0" * 6, b"\xFF" * 6)
        or sample[10:12] != b"S9"
        or sample.count(0) < 8
    ):
        return None
    return t9_version(sample[T9_VERSION_OFFSET])


def scope_component(value: int | str | None) -> str:
    return str(value) if value is not None else UNKNOWN


def t9_scope_component(value: int | None) -> str:
    version = t9_version(value)
    return f"{version:02d}" if version is not None else UNKNOWN


def ffs_label_sort_key(value: str) -> tuple[Any, ...]:
    folded = value.casefold()
    if folded == "standard":
        return 0, 0, folded, value
    match = re.fullmatch(r"(?:xx-)?ffs-lg(\d+)", folded)
    if match is not None:
        number = int(match.group(1), 10)
        return (1 if number == 1 else 2), number, folded, value
    return 3, 0, folded, value


def lg_collapse_evidence(
    own: dict[tuple[str, str], set[str]],
    peer: dict[tuple[str, str], set[str]],
    peer_sources: Iterable[str],
) -> tuple[set[tuple[str, str]], list[dict[str, Any]]]:
    keys = sorted(set(own) | set(peer))
    collapsed: set[tuple[str, str]] = set()
    rows: list[dict[str, Any]] = []
    sources = sorted(set(peer_sources))
    for role, digest in keys:
        own_lgs = set(own.get((role, digest), set()))
        peer_lgs = set(peer.get((role, digest), set()))
        known_lgs = sorted(own_lgs | peer_lgs)
        independent = len(known_lgs) >= 2
        key = (role, digest)
        if independent:
            collapsed.add(key)
        rows.append({
            "role": role,
            "sha256": digest,
            "known_lgs": known_lgs,
            "own_catalog_lgs": sorted(own_lgs),
            "peer_catalog_lgs": sorted(peer_lgs),
            "peer_catalogs": sources,
            "lg_independent": independent,
            "proof": (
                {
                    "rule": "identical-role-hash-observed-under-two-known-lgs",
                    "minimum_distinct_known_lgs": 2,
                }
                if independent else None
            ),
        })
    return collapsed, rows


ScopeKey = tuple[int | None, str | None, int | None]
CompleteScope = tuple[int, str, int]


def select_lg_scope(
    role: str,
    primary_lg: object,
    embedded_langpack: object = None,
    *,
    primary_source: str,
) -> tuple[str | None, str]:
    primary = normalize_lg(primary_lg)
    embedded = normalize_lg(embedded_langpack)
    if role == "LangPack" and embedded is not None:
        return embedded, "embedded-langpack"
    if role == "T9" and embedded is not None:
        return embedded, "paired-langpack"
    if primary is not None:
        return primary, primary_source
    return None, "unknown"


def scope_key(
    software: object, langpack: object, t9: object, role_slug: str,
) -> ScopeKey:
    version = software_version(software)
    if role_slug in ("bcore", "ffs_a"):
        return version, None, None
    return version, normalize_lg(langpack), t9_version(t9)


def _scope_sort_key(scope: ScopeKey) -> tuple[Any, ...]:
    version, lg, t9 = scope
    lg_number = int(lg[2:]) if lg is not None else None
    return (
        version is None,
        version if version is not None else 0,
        lg_number is None,
        lg_number if lg_number is not None else 0,
        t9 is None,
        t9 if t9 is not None else 0,
    )


def complete_scope(scope: dict[str, Any]) -> CompleteScope | None:
    version = software_version(scope.get("software_version"))
    lg = normalize_lg(scope.get("langpack"))
    t9 = t9_version(scope.get("t9_version"))
    if version is None or lg is None or t9 is None:
        return None
    return version, lg, t9


def scope_needs_reference(role: str, scope: dict[str, Any]) -> bool:
    if role in ("BCORE", "FFS(A)"):
        return software_version(scope.get("software_version")) is None
    return complete_scope(scope) is None


def _compatible_scope(
    observed: dict[str, Any], candidate: CompleteScope,
) -> bool:
    values = (
        software_version(observed.get("software_version")),
        normalize_lg(observed.get("langpack")),
        t9_version(observed.get("t9_version")),
    )
    return all(
        value is None or value == expected
        for value, expected in zip(values, candidate)
    )


def _complete_scope_sort_key(scope: CompleteScope) -> tuple[Any, ...]:
    version, lg, t9 = scope
    return version, int(lg[2:]), lg, t9


def resolve_scope_references(
    occurrences: Iterable[dict[str, Any]],
    artifacts: Iterable[dict[str, Any]],
) -> tuple[dict[str, dict[str, Any]], list[dict[str, Any]]]:
    # Resolve incomplete paths from direct evidence without rewriting scope.
    occurrence_rows = list(occurrences)
    artifacts_by_id = {
        artifact.get("id"): artifact
        for artifact in artifacts
        if isinstance(artifact.get("id"), str)
    }
    direct_kinds = {"complete-fullflash", "partial-fullflash"}
    pairs_by_source: dict[str, set[tuple[str, str]]] = defaultdict(set)
    pairs_by_scope: dict[CompleteScope, set[tuple[str, str]]] = defaultdict(set)
    candidates_by_pair: dict[
        tuple[str, str], dict[CompleteScope, list[dict[str, Any]]]
    ] = defaultdict(lambda: defaultdict(list))

    for occurrence in occurrence_rows:
        source_id = occurrence.get("source_id")
        role = occurrence.get("role")
        digest = occurrence.get("sha256")
        if (
            occurrence.get("erased")
            or not isinstance(source_id, str)
            or not isinstance(role, str)
            or not isinstance(digest, str)
        ):
            continue
        pair = role, digest
        pairs_by_source[source_id].add(pair)
        artifact = artifacts_by_id.get(source_id, {})
        if artifact.get("kind") not in direct_kinds:
            continue
        scope = occurrence.get("scope")
        if not isinstance(scope, dict):
            continue
        complete = complete_scope(scope)
        if complete is None:
            continue
        pairs_by_scope[complete].add(pair)
        candidates_by_pair[pair][complete].append(occurrence)

    references: dict[str, dict[str, Any]] = {}
    resolutions: list[dict[str, Any]] = []
    kind_priority = {"complete-fullflash": 0, "partial-fullflash": 1}
    for occurrence in occurrence_rows:
        occurrence_id = occurrence.get("id")
        source_id = occurrence.get("source_id")
        role = occurrence.get("role")
        digest = occurrence.get("sha256")
        observed = occurrence.get("scope")
        if (
            occurrence.get("erased")
            or not isinstance(occurrence_id, str)
            or not isinstance(source_id, str)
            or not isinstance(role, str)
            or not isinstance(digest, str)
            or not isinstance(observed, dict)
            or not scope_needs_reference(role, observed)
        ):
            continue
        candidates = candidates_by_pair.get((role, digest), {})
        compatible = [
            scope for scope in candidates
            if _compatible_scope(observed, scope)
        ]
        if not compatible:
            continue
        source_pairs = pairs_by_source.get(source_id, set())

        def candidate_key(scope: CompleteScope) -> tuple[Any, ...]:
            evidence = candidates[scope]
            quality = min(
                kind_priority.get(
                    artifacts_by_id.get(item.get("source_id"), {}).get("kind"),
                    len(kind_priority),
                )
                for item in evidence
            )
            overlap = len(source_pairs & pairs_by_scope[scope])
            return -overlap, quality, *_complete_scope_sort_key(scope)

        selected = min(compatible, key=candidate_key)
        evidence = min(
            candidates[selected],
            key=lambda item: (
                kind_priority.get(
                    artifacts_by_id.get(item.get("source_id"), {}).get("kind"),
                    len(kind_priority),
                ),
                str(item.get("source_id", "")),
                str(item.get("id", "")),
            ),
        )
        reference = dict(observed)
        reference.update({
            "software_version": selected[0],
            "langpack": selected[1],
            "t9_version": selected[2],
        })
        references[occurrence_id] = reference
        resolutions.append({
            "occurrence_id": occurrence_id,
            "source_id": source_id,
            "role": role,
            "sha256": digest,
            "observed_scope": {
                "software_version": software_version(
                    observed.get("software_version")
                ),
                "langpack": normalize_lg(observed.get("langpack")),
                "t9_version": t9_version(observed.get("t9_version")),
            },
            "reference_scope": {
                "software_version": selected[0],
                "langpack": selected[1],
                "t9_version": selected[2],
            },
            "evidence_occurrence_id": evidence.get("id"),
            "evidence_source_id": evidence.get("source_id"),
            "evidence_kind": artifacts_by_id.get(
                evidence.get("source_id"), {}
            ).get("kind"),
            "artifact_overlap": len(source_pairs & pairs_by_scope[selected]),
            "candidate_count": len(compatible),
        })
    return references, sorted(
        resolutions,
        key=lambda item: (
            item["source_id"], item["role"], item["sha256"],
            item["occurrence_id"],
        ),
    )


def scoped_payload_plan(
    short_name: str,
    scopes: Iterable[dict[str, Any]],
    role_slug: str,
    labels_by_scope: dict[ScopeKey, set[str]] | None = None,
) -> tuple[dict[str, Any], dict[ScopeKey, dict[str | None, str]]]:
    grouped: set[ScopeKey] = set()
    for scope in scopes:
        grouped.add(scope_key(
            scope.get("software_version"),
            scope.get("langpack"),
            scope.get("t9_version"),
            role_slug,
        ))
    if not grouped:
        grouped.add(scope_key(None, None, None, role_slug))

    paths_by_scope: dict[ScopeKey, dict[str | None, str]] = {}
    candidates: list[tuple[tuple[Any, ...], str]] = []
    for key in grouped:
        version, lg, t9 = key
        labels: list[str | None] = (
            sorted(
                labels_by_scope.get(key, set()),
                key=ffs_label_sort_key,
            )
            if labels_by_scope is not None
            else []
        )
        if not labels:
            labels = [None]
        prefix = (
            f"{scope_component(version)}/{role_slug}"
            if role_slug in ("bcore", "ffs_a")
            else (
                f"{scope_component(version)}/{scope_component(lg)}/"
                f"{t9_scope_component(t9)}/{role_slug}"
            )
        )
        selected_label = labels[0]
        filename = (
            f"{selected_label}-{short_name}"
            if selected_label is not None else short_name
        )
        path = f"{prefix}/{filename}.bin"
        scoped_paths = {label: path for label in labels}
        candidates.append((
            (*_scope_sort_key(key), *ffs_label_sort_key(selected_label or "")),
            path,
        ))
        paths_by_scope[key] = scoped_paths

    ordered_paths = [path for _key, path in sorted(candidates)]
    return {
        "payload_path": ordered_paths[0],
        "symlink_paths": ordered_paths[1:],
    }, paths_by_scope


def select_payload_path(
    paths_by_scope: dict[ScopeKey, dict[str | None, str]],
    software: int | None,
    langpack: str | None,
    t9: int | None,
    role_slug: str,
    label: str | None = None,
) -> str:
    key = scope_key(software, langpack, t9, role_slug)
    candidates = paths_by_scope.get(key)
    if candidates is None or label not in candidates:
        raise FirmwareError(
            "no unique payload path for "
            f"software={software} langpack={normalize_lg(langpack)} "
            f"t9={t9_version(t9)} role={role_slug} label={label}"
        )
    return candidates[label]


def payload_paths(variant: dict[str, Any]) -> list[str]:
    canonical = variant.get("payload_path")
    if not isinstance(canonical, str):
        return []
    aliases = variant.get("symlink_paths", [])
    if not isinstance(aliases, list) or not all(
        isinstance(path, str) for path in aliases
    ):
        raise FirmwareError("variant symlink_paths is invalid")
    return [canonical, *aliases]


def atomic_relative_symlink(
    root: Path, relative: str, target_relative: str, force: bool,
) -> str:
    link = root / relative
    target = root / target_relative
    link.parent.mkdir(parents=True, exist_ok=True)
    desired = os.path.relpath(target, link.parent)
    if link.is_symlink() and os.readlink(link) == desired:
        return "reused"
    if os.path.lexists(link) and not force:
        raise FirmwareError(f"output already exists: {link} (use --force)")
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{link.name}.", suffix=".part", dir=link.parent
    )
    os.close(fd)
    os.unlink(temporary_name)
    temporary = Path(temporary_name)
    try:
        temporary.symlink_to(desired)
        os.replace(temporary, link)
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise
    return "written"
