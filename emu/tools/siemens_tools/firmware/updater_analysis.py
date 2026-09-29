from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Any

from ..layout import resolve_statistics_locations
from .compression import materialize_xbi
from .containers import extract_exe
from .xbi import FirmwareError, firmware_extension, is_xbi, parse_xbi


_FINALIZATION_CONSTRUCTOR_SIGNATURE = bytes.fromhex(
    "E6 FC FA 07 B8 C0 E6 FD F0 FF C4 D0 02 00"
)
_FINALIZATION_SIGNATURE_OFFSET = 0x16
_FINALIZATION_BYTES = bytes.fromhex("FA 07 F0 FF")


@dataclass(frozen=True)
class UpdaterSemanticProfile:
    profile_id: str
    models: frozenset[str]
    transport_sha256: str
    image_sha256: str
    constructor_address: int
    constructor_callsite_address: int
    command_handler_address: int
    checksum_function_address: int
    body_function_address: int
    configuration_initializer_address: int
    destination_loader_address: int
    destination_field_offsets: tuple[int, int]
    flash_writer_address: int


@dataclass(frozen=True)
class FlashConfigurationRecord:
    site_address: int
    initializer_callsite_address: int
    statistic_address: int
    finalization_address: int


UPDATER_PROFILES = (
    UpdaterSemanticProfile(
        "a52-a55-929f5d97", frozenset(("A52", "A55")),
        "929f5d97ee256726211f736b973abbd120341786b684bb0a5030f4736d72fbdd",
        "2d082dde58173ab71a4c71164bbc1757617b0d20a73e5fb61d24b7fcd54b57e8",
        0x8F04, 0x8FEA, 0x8F4C, 0x2EAE, 0x86E0,
        0x14E8, 0x1608, (0x59E, 0x5A0), 0x0FC2,
    ),
    UpdaterSemanticProfile(
        "a55-c55-889d44bd", frozenset(("A55", "C55")),
        "889d44bdfd5acbb37bf1130236f6c8615dd6c0555c7f0dbcdfa13dcd2b384f2f",
        "b1e82478adbf0a1882e2c1307fa9fd9e740a0c3320c5e0bf2d4d25a38c963516",
        0x8F04, 0x8FEA, 0x8F4C, 0x2E1A, 0x86E0,
        0x14E8, 0x1608, (0x59E, 0x5A0), 0x0FC2,
    ),
    UpdaterSemanticProfile(
        "a60-c60-mc60-831b984a", frozenset(("A60", "C60", "MC60")),
        "831b984a6efe6862a861c578995e7e15edcbb1d3fb86726ca75433e7bc25eb27",
        "8822996aff60e4b091ddc0e547447c2e8d316bb8ca5285dbf930bded250ba00c",
        0x8F46, 0x905C, 0x8F8E, 0x30EE, 0x8722,
        0x196C, 0x1A8C, (0x6CA, 0x6CC), 0x1470,
    ),
    UpdaterSemanticProfile(
        "a62-sw06-fd883bf9", frozenset(("A62",)),
        "fd883bf91b34790bdf2d4377769a887e8bfcf761d6a99a8aec85e7254b673985",
        "ecf7a6c5bb8a22d6299e23bf764d3a11112e23a27d8249e9887d379ba14b4cd0",
        0x2114, 0x222A, 0x215C, 0x0FC4, 0x18F0,
        0x34C2, 0x35E2, (0x6CA, 0x6CC), 0x8768,
    ),
    UpdaterSemanticProfile(
        "a62-sw07-ad5e395e", frozenset(("A62",)),
        "ad5e395e023fef75c6745d996b34dadb00e6d3ab6f8461ef80c56cd5b3329099",
        "947c363038cc1a9eca4150e556fbc9854c1dd461574f6b6c96b30a1e286d4ffb",
        0x2114, 0x222A, 0x215C, 0x0FC4, 0x18F0,
        0x34C2, 0x35E2, (0x6CA, 0x6CC), 0x8768,
    ),
    UpdaterSemanticProfile(
        "a65-c882b187", frozenset(("A65",)),
        "c882b18770b5396a4f1de7844a06443aa055fe08c639e1c9215a1668cdb26cdc",
        "b6d452c5a64704172441d3af3b76235febc23e9661232a73ad2662ee231fa0fb",
        0x2114, 0x222A, 0x215C, 0x0FC4, 0x18F0,
        0x62F8, 0x6418, (0x6CA, 0x6CC), 0x391A,
    ),
    UpdaterSemanticProfile(
        "c55-early-140061ff", frozenset(("C55",)),
        "140061ff1c1b900e1a8b95bca91ab624c6d1b56856b70a237d128e194e492f8e",
        "9e0dfef369fd894b2b82b5a5ad8ac7c73cddf916c4b482f8af69888f2f76e58d",
        0x167C, 0x1762, 0x16C4, 0x25D8, 0x0E58,
        0x3378, 0x3498, (0x59E, 0x5A0), 0x2C4E,
    ),
    UpdaterSemanticProfile(
        "c60-cf62-7650508e", frozenset(("C60", "CF62")),
        "7650508eb83aebf260154127aeac01ac70c2ab411c43827f73e45992584be004",
        "7c220550a89f2574d93a7b637f28386e3be8955c65d6ac18970809883236526f",
        0x8F46, 0x905C, 0x8F8E, 0x3168, 0x8722,
        0x196C, 0x1A8C, (0x6CA, 0x6CC), 0xFF1470,
    ),
    UpdaterSemanticProfile(
        "m55-25e960aa", frozenset(("M55",)),
        "25e960aa19f265cee1bd93d8d97d0a9b9cf13c3259449bd1e7be2be518f2c218",
        "45f6c8c1a9ebb77a57d43d8b8cb248e8d1360e92722cd68996e735ea5c48dab7",
        0x8F46, 0x905C, 0x8F8E, 0x2FFA, 0x8722,
        0x196C, 0x1A8C, (0x6CA, 0x6CC), 0x1470,
    ),
    UpdaterSemanticProfile(
        "s55-sl55-a4974aae", frozenset(("S55", "SL55")),
        "a4974aae58103ca7270903640fffe845172797cbab9d81dcfeaa73e5b04d0916",
        "ae7a9ca85ad47e1a4d393c2916212de44db1e433c398ce4cae2a2035f54fa675",
        0x20D4, 0x21BA, 0x211C, 0x0FA8, 0x18B0,
        0x365E, 0x377E, (0x59E, 0x5A0), 0x3DA4,
    ),
    UpdaterSemanticProfile(
        "s55-early-cea4f45b", frozenset(("S55",)),
        "cea4f45bee990f5d598470322f59afb2f991151432e910e82c4976225464e69c",
        "3adfc3213cb25013cf7e03dcb96e66eef30a5926e7e8721bf1eafe10e18f1139",
        0x34EE, 0x35D4, 0x3536, 0x8830, 0x2CCA,
        0x14E8, 0x1608, (0x59E, 0x5A0), 0x0FC2,
    ),
)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _all_occurrences(data: bytes, needle: bytes) -> list[int]:
    if not needle:
        return []
    result = []
    cursor = 0
    while True:
        offset = data.find(needle, cursor)
        if offset < 0:
            return result
        result.append(offset)
        cursor = offset + 1


def find_statistics_finalization_constructors(image: bytes) -> list[int]:
    return [
        offset - _FINALIZATION_SIGNATURE_OFFSET
        for offset in _all_occurrences(
            image, _FINALIZATION_CONSTRUCTOR_SIGNATURE
        )
        if offset >= _FINALIZATION_SIGNATURE_OFFSET
    ]


def _decode_mov_immediate(
    image: bytes, offset: int, register: int
) -> tuple[int, int] | None:
    if image[offset:offset + 2] == bytes((0xE6, 0xF0 | register)):
        if offset + 4 > len(image):
            return None
        return int.from_bytes(image[offset + 2:offset + 4], "little"), offset + 4
    if (
        offset + 2 <= len(image)
        and image[offset] == 0xE0
        and image[offset + 1] & 0x0F == register
    ):
        return image[offset + 1] >> 4, offset + 2
    return None


def _decode_far_pair(
    image: bytes,
    offset: int,
    low_register: int,
    high_register: int,
) -> tuple[int, int] | None:
    low = _decode_mov_immediate(image, offset, low_register)
    if low is None:
        return None
    low_value, cursor = low
    high = _decode_mov_immediate(image, cursor, high_register)
    if high is None:
        return None
    high_value, cursor = high
    expected_pushes = bytes((
        0x88, high_register << 4,
        0x88, low_register << 4,
    ))
    if image[cursor:cursor + 4] != expected_pushes:
        return None
    return (high_value << 16) | low_value, cursor + 4


def find_flash_configuration_records(
    image: bytes, initializer_address: int
) -> list[FlashConfigurationRecord]:
    records = []
    for site in _all_occurrences(image, bytes((0xE6, 0xF1))):
        finalization = _decode_far_pair(image, site, 1, 2)
        if finalization is None:
            continue
        finalization_address, cursor = finalization
        statistics = _decode_far_pair(image, cursor, 3, 4)
        if statistics is None:
            continue
        statistic_address, cursor = statistics
        callsite = None
        for candidate in range(cursor, min(cursor + 0x48, len(image) - 3), 2):
            if image[candidate] != 0xDA:
                continue
            target = (
                (image[candidate + 1] << 16)
                | int.from_bytes(image[candidate + 2:candidate + 4], "little")
            )
            if target == initializer_address:
                callsite = candidate
                break
        if callsite is not None:
            records.append(FlashConfigurationRecord(
                site, callsite, statistic_address, finalization_address
            ))
    return records


def describe_firmware_payloads(
    data: bytes, transport: bytes
) -> tuple[list[dict[str, Any]], list[tuple[int, bytes, Any]]]:
    try:
        payloads = extract_exe(data)
    except FirmwareError as exc:
        return ([{
            "index": None,
            "status": "unresolved",
            "error": str(exc),
        }], [])

    descriptions: list[dict[str, Any]] = []
    parsed_payloads: list[tuple[int, bytes, Any]] = []
    for index, payload in enumerate(payloads):
        base: dict[str, Any] = {
            "index": index,
            "size": len(payload),
            "sha256": _sha256(payload),
        }
        if not is_xbi(payload):
            base.update({
                "kind": "unknown",
                "status": "unrecognized",
                "raw_updater_stream_occurrences": _all_occurrences(
                    payload, transport
                ),
            })
            descriptions.append(base)
            continue
        try:
            info = parse_xbi(payload)
        except FirmwareError as exc:
            base.update({
                "kind": "xbi",
                "status": "unresolved",
                "error": str(exc),
                "raw_updater_stream_occurrences": _all_occurrences(
                    payload, transport
                ),
            })
            descriptions.append(base)
            continue
        base.update({
            "kind": firmware_extension(info),
            "status": "confirmed",
            "model": info.get("model"),
            "software_version": info.get("svn"),
            "cpu": info.get("cpu_type_name"),
            "cpu_id": info.get("cpu_type"),
            "flash_size": info.get("flash_size"),
            "statistic_addr": info.get("statistic_addr"),
            "split_info": info.get("split_info"),
            "raw_updater_stream_occurrences": _all_occurrences(
                payload, transport
            ),
        })
        descriptions.append(base)
        parsed_payloads.append((index, payload, info))
    return descriptions, parsed_payloads


def match_semantic_profile(
    extraction: Any, payloads: list[dict[str, Any]]
) -> tuple[UpdaterSemanticProfile | None, str | None]:
    transport_hash = _sha256(extraction.stream)
    image_hash = _sha256(extraction.image)
    hash_matches = [
        profile for profile in UPDATER_PROFILES
        if profile.transport_sha256 == transport_hash
        and profile.image_sha256 == image_hash
    ]
    if not hash_matches:
        return None, "no exact transport/image profile"
    models = {
        payload.get("model") for payload in payloads
        if payload.get("status") == "confirmed"
        and isinstance(payload.get("model"), str)
    }
    compatible = [
        profile for profile in hash_matches
        if models and models.issubset(profile.models)
    ]
    if len(compatible) == 1:
        return compatible[0], None
    expected = sorted({model for profile in hash_matches for model in profile.models})
    return None, (
        f"profile model conflict: payload models {sorted(models)!r}, "
        f"expected one of {expected!r}"
    )


def _resolve_finalization_destination(
    profile: UpdaterSemanticProfile,
    image: bytes,
    payloads: list[dict[str, Any]],
) -> tuple[dict[str, Any] | None, str | None]:
    records = find_flash_configuration_records(
        image, profile.configuration_initializer_address
    )
    resolutions = []
    for payload in payloads:
        if payload.get("status") != "confirmed":
            continue
        model = payload.get("model")
        flash_size = payload.get("flash_size")
        statistic_address = payload.get("statistic_addr")
        if (
            model not in profile.models
            or not isinstance(model, str)
            or not isinstance(flash_size, int)
            or not isinstance(statistic_address, int)
        ):
            continue
        try:
            locations = resolve_statistics_locations(
                model, flash_size, statistic_address
            )
        except FirmwareError as exc:
            return None, str(exc)
        matching = [
            record for record in records
            if record.statistic_address & ~0xFF
            == statistic_address & ~0xFF
        ]
        destinations = {record.finalization_address for record in matching}
        if len(destinations) != 1:
            return None, (
                f"statistics address 0x{statistic_address:x} maps to "
                f"finalization destinations "
                f"{[hex(value) for value in sorted(destinations)]}"
            )
        destination = next(iter(destinations))
        if destination != locations.entry_transfer_offset:
            return None, (
                f"updater finalization address 0x{destination:x} does not "
                f"match layout {locations.layout_name!r} entry transfer "
                f"offset 0x{locations.entry_transfer_offset:x}"
            )
        resolutions.append({
            "model": model,
            "layout": locations.layout_name,
            "statistics_address": statistic_address,
            "destination_address": destination,
            "configuration_sites": [
                {
                    "address": record.site_address,
                    "initializer_callsite_address":
                        record.initializer_callsite_address,
                    "statistics_reference_address":
                        record.statistic_address,
                }
                for record in matching
            ],
        })
    if not resolutions:
        return None, "no compatible parsed firmware payload"
    destinations = {
        resolution["destination_address"] for resolution in resolutions
    }
    if len(destinations) != 1:
        return None, (
            "contained payloads resolve different finalization destinations: "
            + ", ".join(hex(value) for value in sorted(destinations))
        )
    low_field, high_field = profile.destination_field_offsets
    return {
        "status": "confirmed",
        "address": next(iter(destinations)),
        "coordinate_system": "logical-fullflash-offset",
        "source": {
            "kind": "updater-configuration-and-maintained-layout",
            "configuration_initializer_address":
                profile.configuration_initializer_address,
            "loader_address": profile.destination_loader_address,
            "fields": [
                {"offset": low_field, "width": 2, "role": "address-low16"},
                {"offset": high_field, "width": 2, "role": "address-high16"},
            ],
            "resolutions": resolutions,
        },
    }, None


def _decoded_finalization_instruction() -> dict[str, Any]:
    segment = _FINALIZATION_BYTES[1]
    offset = int.from_bytes(_FINALIZATION_BYTES[2:4], "little")
    return {
        "kind": "jmps",
        "target": (segment << 16) | offset,
        "segment": segment,
        "offset": offset,
    }


def describe_generation_and_semantics(
    extraction: Any, payloads: list[dict[str, Any]]
) -> tuple[dict[str, Any], dict[str, Any]]:
    transport_hash = _sha256(extraction.stream)
    image_hash = _sha256(extraction.image)
    profile, reason = match_semantic_profile(extraction, payloads)
    generation = {
        "transport_sha256": transport_hash,
        "image_sha256": image_hash,
        "profile_id": profile.profile_id if profile is not None else None,
        "status": "confirmed" if profile is not None else "unrecognized",
    }
    if profile is None:
        return generation, {
            "status": "unrecognized",
            "reason": reason,
            "statistics_finalization_write": {"status": "unrecognized"},
            "statistics": {"status": "unrecognized"},
        }

    constructors = find_statistics_finalization_constructors(extraction.image)
    if constructors != [profile.constructor_address]:
        return generation, {
            "status": "unresolved",
            "reason": (
                f"expected finalization constructor "
                f"0x{profile.constructor_address:x}, found "
                f"{[hex(item) for item in constructors]}"
            ),
            "statistics_finalization_write": {"status": "unresolved"},
            "statistics": {"status": "confirmed"},
        }
    destination, destination_error = _resolve_finalization_destination(
        profile, extraction.image, payloads
    )
    if destination is None:
        return generation, {
            "status": "unresolved",
            "reason": destination_error,
            "profile_id": profile.profile_id,
            "statistics_finalization_write": {
                "status": "unresolved",
                "reason": destination_error,
            },
            "statistics": {"status": "confirmed"},
        }
    return generation, {
        "status": "confirmed",
        "profile_id": profile.profile_id,
        "statistics_finalization_write": {
            "status": "confirmed",
            "command": 0x04,
            "constructor_address": profile.constructor_address,
            "constructor_callsite_address":
                profile.constructor_callsite_address,
            "flash_writer_address": profile.flash_writer_address,
            "destination": destination,
            "write": {
                "size": len(_FINALIZATION_BYTES),
                "bytes": _FINALIZATION_BYTES.hex(),
                "decoded_instruction": _decoded_finalization_instruction(),
            },
            "guard": {
                "status": "confirmed",
                "condition": "statistics range checksum valid",
            },
        },
        "statistics": {
            "status": "confirmed",
            "command": 0x04,
            "command_handler_address": profile.command_handler_address,
            "checksum_function_address": profile.checksum_function_address,
            "body_function_address": profile.body_function_address,
            "checksum": {
                "status": "confirmed",
                "algorithm": "xor16-and-sum16-of-little-endian-words",
                "xor16_offset": 0x10,
                "sum16_offset": 0x12,
                "range_table_offset": 0x80,
                "range_table_format":
                    "terminated little-endian dword start/end pairs",
                "range_end": "exclusive",
            },
            "mutable_body": {
                "status": "confirmed",
                "start_offset": 0x18,
                "end_offset": 0x80,
                "size": 0x68,
            },
            "success_response": {
                "status": "confirmed",
                "kind": "statistics-bytes",
                "start_offset": 0,
                "end_offset": 0x26,
            },
            "error_response": {
                "status": "confirmed",
                "code": 0x86,
            },
        },
    }


def find_relocated_updater_matches(
    extraction: Any, flash: bytes, write_mask: bytes
) -> list[int]:
    if len(flash) != len(write_mask):
        raise ValueError("flash and write mask lengths differ")
    if not extraction.mapped_ranges:
        return []
    anchor = max(extraction.mapped_ranges, key=lambda item: item.end - item.start)
    anchor_bytes = extraction.image[anchor.start:anchor.end]
    matches = []
    for occurrence in _all_occurrences(flash, anchor_bytes):
        relocation = occurrence - anchor.start
        valid = True
        for item in extraction.mapped_ranges:
            start = relocation + item.start
            end = relocation + item.end
            if start < 0 or end > len(flash):
                valid = False
                break
            if flash[start:end] != extraction.image[item.start:item.end]:
                valid = False
                break
            if any(byte != 1 for byte in write_mask[start:end]):
                valid = False
                break
        if valid:
            matches.append(relocation)
    return matches


def compare_updater_with_xbi(
    extraction: Any, payload: bytes, index: int | None = None
) -> dict[str, Any]:
    result: dict[str, Any] = {
        "payload_index": index,
        "payload_sha256": _sha256(payload),
    }
    if not is_xbi(payload):
        result.update({"status": "unrecognized", "matches": []})
        return result
    try:
        info = parse_xbi(payload)
        materialized = materialize_xbi(payload, info)
        matches = find_relocated_updater_matches(
            extraction, materialized.flash, materialized.write_mask
        )
    except FirmwareError as exc:
        result.update({
            "status": "unresolved",
            "error": str(exc),
            "matches": [],
        })
        return result
    result.update({
        "status": "confirmed",
        "model": info.get("model"),
        "software_version": info.get("svn"),
        "matches": [
            {
                "relocation": relocation,
                "mapped_start": relocation + extraction.mapped_ranges[0].start,
                "mapped_end": relocation + extraction.mapped_ranges[-1].end,
                "all_mapped_bytes_package_owned": True,
            }
            for relocation in matches
        ],
    })
    return result


def analyze_updater_firmware(
    data: bytes, extraction: Any, compare_firmware: bool
) -> dict[str, Any]:
    payloads, parsed_payloads = describe_firmware_payloads(
        data, extraction.stream
    )
    generation, semantics = describe_generation_and_semantics(
        extraction, payloads
    )
    comparison: dict[str, Any] = {
        "raw_xbz_updater_stream": {
            "status": "confirmed",
            "payloads": [
                {
                    "index": payload.get("index"),
                    "occurrences":
                        payload.get("raw_updater_stream_occurrences", []),
                }
                for payload in payloads
            ],
        },
        "relocated_image": {
            "status": "not-run",
            "reason": "use --compare-firmware",
        },
    }
    if compare_firmware:
        compared = [
            compare_updater_with_xbi(extraction, payload, index)
            for index, payload, _info in parsed_payloads
        ]
        if not compared:
            status = "unrecognized"
        elif all(item["status"] == "confirmed" for item in compared):
            status = "confirmed"
        else:
            status = "unresolved"
        comparison["relocated_image"] = {
            "status": status,
            "payloads": compared,
        }
    return {
        "payloads": payloads,
        "generation": generation,
        "semantics": semantics,
        "comparison": comparison,
    }
