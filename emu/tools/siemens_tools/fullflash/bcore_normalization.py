from __future__ import annotations

import hashlib
from dataclasses import dataclass
from typing import Any

from ..firmware.xbi import FirmwareError, FlashLayout


BCORE_FIELD_OFFSET = 0x330
BCORE_FIELD_SIZE = 0x10
BCORE_ERASED_VALUE = b"\xFF" * BCORE_FIELD_SIZE
NORMALIZATION_KIND = "pmb7850-bcore-field-330-erased"

# These profiles have target evidence for a PMB7850 / E-GOLD+ V3 BCORE.
# A50, E-GOLD Lite models, and unresolved later profiles are intentionally absent.
VALIDATED_PMB7850_LAYOUTS = frozenset({
    "A51/A52",
    "A55/A56/A57",
    "C55/C56/CT56",
    "M55/M56",
    "S55/S56/S57",
    "SL55/SL56",
    "A60/A62",
    "A65/C60",
    "MC60",
    "CF62",
})
VALIDATED_PMB7850_MODELS = frozenset({
    "A52",
    "A55",
    "A57",
    "A60",
    "A62",
    "A65",
    "C55",
    "C60",
    "CF62",
    "M55",
    "MC60",
    "S55",
    "S56",
    "SL55",
    "SL56",
})


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _validated_model(model: object) -> bool:
    return (
        isinstance(model, str)
        and model.upper() in VALIDATED_PMB7850_MODELS
    )


@dataclass(frozen=True)
class NormalizedBCore:
    payload: bytes
    metadata: dict[str, Any]


def normalize_bcore(
    payload: bytes,
    layout: FlashLayout,
    bcore_metadata: dict[str, Any] | None,
    *,
    full_boundary_present: bool,
) -> NormalizedBCore | None:
    """Erase the personalized field only for a validated complete PMB7850 BCORE."""
    if (
        layout.name not in VALIDATED_PMB7850_LAYOUTS
        or not full_boundary_present
        or len(payload) < BCORE_FIELD_OFFSET + BCORE_FIELD_SIZE
        or not isinstance(bcore_metadata, dict)
        or not _validated_model(bcore_metadata.get("model"))
    ):
        return None

    fingerprints = bcore_metadata.get("fingerprints")
    if not isinstance(fingerprints, dict):
        return None
    source_sha256 = _sha256(payload)
    if fingerprints.get("raw_sha256") != source_sha256:
        return None
    stored_value = payload[
        BCORE_FIELD_OFFSET:BCORE_FIELD_OFFSET + BCORE_FIELD_SIZE
    ]
    if fingerprints.get("field_330_raw") != stored_value.hex():
        return None

    normalized = bytearray(payload)
    normalized[
        BCORE_FIELD_OFFSET:BCORE_FIELD_OFFSET + BCORE_FIELD_SIZE
    ] = BCORE_ERASED_VALUE
    normalized_payload = bytes(normalized)
    normalized_sha256 = _sha256(normalized_payload)
    return NormalizedBCore(normalized_payload, {
        "kind": NORMALIZATION_KIND,
        "offset": BCORE_FIELD_OFFSET,
        "length": BCORE_FIELD_SIZE,
        "stored_value": stored_value.hex(),
        "source_sha256": source_sha256,
        "normalized_sha256": normalized_sha256,
        "restored_sha256": source_sha256,
    })


def restore_bcore(
    payload: bytes,
    metadata: object,
    *,
    context: str = "BCORE normalization",
) -> bytes:
    """Validate a normalized payload and restore its captured 16-byte field."""
    if not isinstance(metadata, dict):
        raise FirmwareError(f"{context} metadata is invalid")
    required = {
        "kind": NORMALIZATION_KIND,
        "offset": BCORE_FIELD_OFFSET,
        "length": BCORE_FIELD_SIZE,
    }
    if any(metadata.get(key) != value for key, value in required.items()):
        raise FirmwareError(f"{context} metadata is invalid")
    stored_hex = metadata.get("stored_value")
    source_sha256 = metadata.get("source_sha256")
    normalized_sha256 = metadata.get("normalized_sha256")
    restored_sha256 = metadata.get("restored_sha256")
    try:
        stored_value = bytes.fromhex(stored_hex)
    except (TypeError, ValueError) as exc:
        raise FirmwareError(f"{context} stored value is invalid") from exc
    if (
        len(stored_value) != BCORE_FIELD_SIZE
        or not isinstance(source_sha256, str)
        or len(source_sha256) != 64
        or not isinstance(normalized_sha256, str)
        or len(normalized_sha256) != 64
        or restored_sha256 != source_sha256
    ):
        raise FirmwareError(f"{context} metadata is invalid")
    if _sha256(payload) != normalized_sha256:
        raise FirmwareError(f"{context} normalized hash mismatch")
    end = BCORE_FIELD_OFFSET + BCORE_FIELD_SIZE
    if payload[BCORE_FIELD_OFFSET:end] != BCORE_ERASED_VALUE:
        raise FirmwareError(f"{context} field is not erased")

    restored = bytearray(payload)
    restored[BCORE_FIELD_OFFSET:end] = stored_value
    result = bytes(restored)
    if _sha256(result) != source_sha256:
        raise FirmwareError(f"{context} restored hash mismatch")
    return result
