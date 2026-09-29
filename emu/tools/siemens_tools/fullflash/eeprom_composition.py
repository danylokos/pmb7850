from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

from .. import eeprom
from ..firmware.xbi import FirmwareError
from .eeprom_packing import (
    A52_EEPROM_POLICY,
    A55_EEPROM_POLICY,
    A60_EEPROM_POLICY,
    A62_EEPROM_POLICY,
    A65_EEPROM_POLICY,
    C55_EEPROM_POLICY,
    C60_EEPROM_POLICY,
    CF62_EEPROM_POLICY,
    M55_EEPROM_POLICY,
    MC60_EEPROM_POLICY,
    SL55_EEPROM_POLICY,
    S55_EEPROM_POLICY,
    EepromPackingPolicy,
    pack_a52_eeprom,
    pack_a55_eeprom,
    pack_a60_eeprom,
    pack_a62_eeprom,
    pack_a65_eeprom,
    pack_c55_eeprom,
    pack_c60_eeprom,
    pack_cf62_eeprom,
    pack_m55_eeprom,
    pack_mc60_eeprom,
    pack_sl55_eeprom,
    pack_s55_eeprom,
)


_MAP_INFO_RE = re.compile(r"^\[MapFileInfo\]\s*(.*?)(?=^\[)", re.M | re.S)
_MAP_FIELD_RE = re.compile(r"^(\w+)\s*=\s*([^;\r\n]+)", re.M)
_IDENTITY_LAYOUT = {
    76: (2, 0),
    5008: (8, 0),
    5009: (8, 0),
    5077: (8, 0),
}


@dataclass(frozen=True)
class EepromCompositionPolicy:
    model: str
    product: int
    packing: EepromPackingPolicy
    packer: Callable[
        [dict[int, bytes], dict[int, int], dict[int, int]],
        tuple[bytes, dict[str, Any]],
    ]


_POLICIES = {
    "A52": EepromCompositionPolicy(
        "A52", 226, A52_EEPROM_POLICY, pack_a52_eeprom,
    ),
    "A55": EepromCompositionPolicy(
        "A55", 196, A55_EEPROM_POLICY, pack_a55_eeprom,
    ),
    "A60": EepromCompositionPolicy(
        "A60", 39, A60_EEPROM_POLICY, pack_a60_eeprom,
    ),
    "A62": EepromCompositionPolicy(
        "A62", 231, A62_EEPROM_POLICY, pack_a62_eeprom,
    ),
    "A65": EepromCompositionPolicy(
        "A65", 230, A65_EEPROM_POLICY, pack_a65_eeprom,
    ),
    "C55": EepromCompositionPolicy(
        "C55", 130, C55_EEPROM_POLICY, pack_c55_eeprom,
    ),
    "C60": EepromCompositionPolicy(
        "C60", 40, C60_EEPROM_POLICY, pack_c60_eeprom,
    ),
    "M55": EepromCompositionPolicy(
        "M55", 86, M55_EEPROM_POLICY, pack_m55_eeprom,
    ),
    "MC60": EepromCompositionPolicy(
        "MC60", 132, MC60_EEPROM_POLICY, pack_mc60_eeprom,
    ),
    "SL55": EepromCompositionPolicy(
        "SL55", 36, SL55_EEPROM_POLICY, pack_sl55_eeprom,
    ),
    "S55": EepromCompositionPolicy(
        "S55", 84, S55_EEPROM_POLICY, pack_s55_eeprom,
    ),
    "CF62": EepromCompositionPolicy(
        "CF62", 228, CF62_EEPROM_POLICY, pack_cf62_eeprom,
    ),
}


@dataclass(frozen=True)
class EepromCompositionResult:
    image: bytes
    manifest: dict[str, Any]


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _map_info(raw: bytes, path: Path) -> dict[str, Any]:
    try:
        text = raw.decode("latin-1")
    except UnicodeDecodeError as exc:
        raise FirmwareError(f"invalid EEPROM map {path}: {exc}") from exc
    match = _MAP_INFO_RE.search(text)
    if match is None:
        raise FirmwareError(f"EEPROM source is not a Siemens map: {path}")
    fields = {
        key.casefold(): value.strip()
        for key, value in _MAP_FIELD_RE.findall(match.group(1))
    }
    try:
        product = int(fields["product"], 0)
        software_version = int(fields["swversion"], 0)
    except (KeyError, ValueError) as exc:
        raise FirmwareError(
            f"EEPROM map lacks numeric Product/SWVersion metadata: {path}"
        ) from exc
    return {
        "product": product,
        "software_version": software_version,
        "provider": fields.get("provider"),
        "map_version": fields.get("mapver"),
        "date": fields.get("date"),
        "time": fields.get("time"),
    }


def _record_manifest(
    block_id: int,
    payload: bytes,
    memory_class: int,
    version: int,
    source: str,
    provenance: str,
) -> dict[str, Any]:
    return {
        "id": block_id,
        "length": len(payload),
        "memory_class": memory_class,
        "version": version,
        "source": source,
        "provenance": provenance,
        "payload_sha256": _sha256(payload),
    }


def _load_donor(
    donor_path: Path,
    policy: EepromCompositionPolicy,
) -> tuple[Path, bytes, dict[int, eeprom.Block]]:
    path = donor_path.resolve()
    try:
        raw = path.read_bytes()
        region = eeprom.find_eeprom_region(raw)
        inventory = eeprom.load_dump_blocks(raw, region)
    except (OSError, UnicodeError, ValueError) as exc:
        raise FirmwareError(f"cannot load EEPROM donor {path}: {exc}") from exc
    if len(raw) - region.region_file_base < policy.packing.region_size:
        raise FirmwareError(
            f"EEPROM donor has fewer than {policy.packing.region_size:#x} bytes "
            f"at the {policy.model} EEPROM base"
        )
    if region.region_linear_base != policy.packing.linear_base:
        raise FirmwareError(
            f"EEPROM donor base {region.region_linear_base:#x} does not match "
            f"{policy.model} base {policy.packing.linear_base:#x}"
        )
    return path, raw, inventory


def compose_eeprom(
    map_path: Path,
    *,
    model: str,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
    normalization_block_ids: frozenset[int] | None = None,
) -> EepromCompositionResult:
    """Build one deterministic logical EEPROM region from a factory map.

    ``profile_override`` and ``normalization_block_ids`` are programmatic-only
    hooks for bounded ablation experiments. Production recipes identify a
    built-in profile by name and, when that profile names exact donor records,
    hash-pin the complete donor fullflash.
    """
    try:
        policy = _POLICIES[model.upper()]
    except KeyError as exc:
        supported = ", ".join(sorted(_POLICIES))
        raise FirmwareError(
            f"logical EEPROM composition does not support {model}; supported: {supported}"
        ) from exc

    path = map_path.resolve()
    try:
        raw = path.read_bytes()
        inventory = eeprom.parse_map_file(path)
    except (OSError, UnicodeError, ValueError) as exc:
        raise FirmwareError(f"cannot load EEPROM map {path}: {exc}") from exc
    info = _map_info(raw, path)
    if info["product"] != policy.product:
        raise FirmwareError(
            f"EEPROM map Product {info['product']} is not {policy.model} "
            f"product {policy.product}"
        )
    if not inventory:
        raise FirmwareError(f"EEPROM map has no parameter blocks: {path}")

    blocks: dict[int, bytes] = {}
    memory_classes: dict[int, int] = {}
    versions: dict[int, int] = {}
    records: dict[int, dict[str, Any]] = {}
    for block_id, block in inventory.items():
        if block.memory_class not in (2, 8) or block.version is None:
            raise FirmwareError(
                f"EEPROM map block {block_id} lacks valid Memory/Version metadata"
            )
        blocks[block_id] = block.payload
        memory_classes[block_id] = block.memory_class
        versions[block_id] = block.version
        records[block_id] = _record_manifest(
            block_id, block.payload, block.memory_class, block.version,
            "factory-map", block.name or "Siemens factory default",
        )

    profile = profile_override
    if profile is not None and profile_name is not None and profile.name != profile_name:
        raise FirmwareError("profile override does not match the requested profile name")
    if profile is None and profile_name is not None:
        try:
            profile = eeprom.get_eeprom_profile(profile_name)
        except ValueError as exc:
            raise FirmwareError(str(exc)) from exc

    if (
        info["software_version"] != software_version
        and (
            profile is None
            or (info["software_version"], software_version)
            not in profile.compatible_map_software_versions
        )
    ):
        raise FirmwareError(
            f"EEPROM map SWVersion {info['software_version']} does not match "
            f"assembled SW{software_version}"
        )

    normalizations: list[dict[str, Any]] = []
    omitted: list[int] = []
    donor_manifest = None
    if profile is not None:
        if profile.product != policy.product:
            raise FirmwareError(
                f"EEPROM profile {profile.name} is not for {policy.model} "
                f"product {policy.product}"
            )
        if software_version not in profile.software_versions:
            raise FirmwareError(
                f"EEPROM profile {profile.name} does not support SW{software_version}"
            )
        map_hash = _sha256(raw)
        allowed_map_hashes = {
            value for value in (
                profile.map_sha256,
                *profile.compatible_map_sha256s,
            ) if value is not None
        }
        if profile.map_sha256 is not None and map_hash not in allowed_map_hashes:
            raise FirmwareError(
                f"EEPROM map SHA-256 {map_hash} does not match profile "
                f"{profile.name} maps {sorted(allowed_map_hashes)}"
            )

        normalize = (
            frozenset((5011, 5372))
            if policy.model == "C55" and normalization_block_ids is None
            else (normalization_block_ids or frozenset())
        )
        allowed_normalizations = {5011, 5372} if policy.model == "C55" else set()
        unknown_normalizations = normalize - allowed_normalizations
        if unknown_normalizations:
            raise FirmwareError(
                f"unsupported {policy.model} map normalizations: "
                f"{sorted(unknown_normalizations)}"
            )
        if normalize:
            if 5011 not in blocks or 5372 not in blocks:
                raise FirmwareError("C55 factory map lacks normalization blocks 5011/5372")
            if 5011 in normalize:
                blocks[5011] = bytes(36)
                memory_classes[5011] = 8
                versions[5011] = 2
                records[5011] = _record_manifest(
                    5011, blocks[5011], 8, 2, "profile-normalization",
                    "runtime-proven C55 SW24 length/version correction",
                )
                normalizations.append({"id": 5011, "length": 36, "version": 2})
            if 5372 in normalize:
                versions[5372] = 0
                records[5372] = _record_manifest(
                    5372, blocks[5372], memory_classes[5372], 0,
                    "profile-normalization",
                    "factory payload with runtime-proven C55 SW24 version correction",
                )
                normalizations.append(
                    {"id": 5372, "length": len(blocks[5372]), "version": 0}
                )

        profile_record_ids = {record.block_id for record in profile.records}
        donor_ids = set(profile.donor_blocks)
        overlap = profile_record_ids & donor_ids
        if overlap:
            raise FirmwareError(
                f"EEPROM profile {profile.name} defines blocks as both profile and donor: "
                f"{sorted(overlap)}"
            )
        omitted_ids = set(profile.omitted_blocks)
        if donor_ids & omitted_ids:
            raise FirmwareError(
                f"EEPROM profile {profile.name} both donates and omits blocks: "
                f"{sorted(donor_ids & omitted_ids)}"
            )

        for record in profile.records:
            blocks[record.block_id] = record.payload
            memory_classes[record.block_id] = record.memory_class
            versions[record.block_id] = record.version
            records[record.block_id] = _record_manifest(
                record.block_id, record.payload, record.memory_class,
                record.version, "profile", record.provenance,
            ) | {
                "payload_kind": record.payload_kind,
                "observed_bytes": record.observed_bytes,
            }

        if donor_ids:
            if donor_path is None:
                raise FirmwareError(
                    f"EEPROM profile {profile.name} requires --eeprom-donor"
                )
            donor_resolved, donor_raw, donor_inventory = _load_donor(donor_path, policy)
            donor_hash = _sha256(donor_raw)
            if profile.donor_sha256 is not None and donor_hash != profile.donor_sha256:
                raise FirmwareError(
                    f"EEPROM donor SHA-256 {donor_hash} does not match profile "
                    f"{profile.name} donor {profile.donor_sha256}"
                )
            missing = sorted(donor_ids - set(donor_inventory))
            if missing:
                raise FirmwareError(f"EEPROM donor lacks profile blocks: {missing}")
            for block_id in sorted(donor_ids):
                block = donor_inventory[block_id]
                if block.memory_class not in (2, 8) or block.version is None:
                    raise FirmwareError(
                        f"EEPROM donor block {block_id} lacks valid Memory/Version metadata"
                    )
                blocks[block_id] = block.payload
                memory_classes[block_id] = block.memory_class
                versions[block_id] = block.version
                records[block_id] = _record_manifest(
                    block_id, block.payload, block.memory_class, block.version,
                    "donor-fullflash", f"exact active record from {donor_resolved}",
                )
            donor_manifest = {
                "path": str(donor_resolved),
                "sha256": donor_hash,
                "blocks": sorted(donor_ids),
            }
        elif donor_path is not None:
            raise FirmwareError(f"EEPROM profile {profile.name} does not require a donor")

        for block_id in profile.omitted_blocks:
            blocks.pop(block_id, None)
            memory_classes.pop(block_id, None)
            versions.pop(block_id, None)
            records.pop(block_id, None)
            omitted.append(block_id)
    elif donor_path is not None:
        raise FirmwareError("--eeprom-donor requires a named EEPROM profile")
    elif normalization_block_ids:
        raise FirmwareError("map normalizations require an EEPROM profile")

    if (imei is None) != (fsn is None):
        raise FirmwareError("EEPROM identity requires both IMEI and FSN")
    identity_manifest = None
    if imei is not None and fsn is not None:
        try:
            identity = eeprom.generate_identity_bundle(imei, fsn)
        except ValueError as exc:
            raise FirmwareError(str(exc)) from exc
        for text_id, raw_hex in identity["blocks"].items():
            block_id = int(text_id)
            payload = bytes.fromhex(raw_hex)
            memory_class, version = _IDENTITY_LAYOUT[block_id]
            blocks[block_id] = payload
            memory_classes[block_id] = memory_class
            versions[block_id] = version
            records[block_id] = _record_manifest(
                block_id, payload, memory_class, version, "generated-identity",
                "deterministic Freia model-ID-7 unlocked identity",
            )
        identity_manifest = {
            "profile": identity["profile"],
            "profile_version": identity["profile_version"],
            "imei": imei,
            "fsn": f"{fsn:08X}",
            "blocks": sorted(int(value) for value in identity["blocks"]),
        }

    try:
        image, packing = policy.packer(blocks, memory_classes, versions)
    except (ValueError, KeyError) as exc:
        raise FirmwareError(f"cannot pack {policy.model} EEPROM: {exc}") from exc
    if len(image) != policy.packing.region_size:
        raise FirmwareError(f"packed {policy.model} EEPROM has the wrong region size")
    ordered_records = [records[block_id] for block_id in sorted(records)]
    source_counts: dict[str, int] = {}
    source_bytes: dict[str, int] = {}
    for record in ordered_records:
        source = str(record["source"])
        source_counts[source] = source_counts.get(source, 0) + 1
        source_bytes[source] = source_bytes.get(source, 0) + int(record["length"])
    manifest = {
        "model": policy.model,
        "map": {"path": str(path), "sha256": _sha256(raw), **info},
        "donor": donor_manifest,
        "profile": profile.name if profile is not None else None,
        "normalizations": normalizations,
        "omitted_blocks": omitted,
        "identity": identity_manifest,
        "record_count": len(ordered_records),
        "source_counts": dict(sorted(source_counts.items())),
        "source_bytes": dict(sorted(source_bytes.items())),
        "records": ordered_records,
        "packing": packing,
        "sha256": _sha256(image),
    }
    return EepromCompositionResult(image=image, manifest=manifest)


def compose_c55_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
    normalization_block_ids: frozenset[int] | None = None,
) -> EepromCompositionResult:
    """Compatibility wrapper for the original C55 composition API."""
    return compose_eeprom(
        map_path, model="C55", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
        normalization_block_ids=normalization_block_ids,
    )

def compose_a55_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="A55", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_a52_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="A52", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_a60_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="A60", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_a62_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="A62", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_a65_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="A65", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_m55_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="M55", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_mc60_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="MC60", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_cf62_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="CF62", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_c60_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="C60", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_sl55_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="SL55", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )


def compose_s55_eeprom(
    map_path: Path,
    *,
    software_version: int,
    profile_name: str | None = None,
    imei: str | None = None,
    fsn: int | None = None,
    donor_path: Path | None = None,
    profile_override: eeprom.EepromProfile | None = None,
) -> EepromCompositionResult:
    return compose_eeprom(
        map_path, model="S55", software_version=software_version,
        profile_name=profile_name, imei=imei, fsn=fsn, donor_path=donor_path,
        profile_override=profile_override,
    )
