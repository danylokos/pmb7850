"""Raw Siemens fullflash inspection and assembly."""

from .audit import audit_fullflash
from .bcore_key import (
    BCoreKeyResult,
    build_seed,
    derive_bcore_key,
    hash_bootkey,
    recover_bcore_key,
)
from .corpus import build_corpus, discover_sources, render_report
from .compose import apply_official_recipe, compose_manifest
from .eeprom_packing import (
    pack_a52_eeprom,
    pack_a55_eeprom,
    pack_a60_eeprom,
    pack_a62_eeprom,
    pack_a65_eeprom,
    pack_c55_eeprom,
    pack_cf62_eeprom,
    pack_c60_eeprom,
    pack_m55_eeprom,
    pack_mc60_eeprom,
    pack_sl55_eeprom,
    pack_s55_eeprom,
)
from .eeprom_composition import (
    EepromCompositionResult,
    compose_a52_eeprom,
    compose_a55_eeprom,
    compose_a60_eeprom,
    compose_a62_eeprom,
    compose_a65_eeprom,
    compose_c55_eeprom,
    compose_c60_eeprom,
    compose_cf62_eeprom,
    compose_eeprom,
    compose_m55_eeprom,
    compose_mc60_eeprom,
    compose_sl55_eeprom,
    compose_s55_eeprom,
)
from .official_corpus import build_official_corpus, discover_official_packages
from .reconstruct import reconstruct_recipe
from .split import plan_split

__all__ = [
    "BCoreKeyResult",
    "apply_official_recipe",
    "audit_fullflash",
    "build_seed",
    "build_corpus",
    "build_official_corpus",
    "compose_manifest",
    "compose_a52_eeprom",
    "compose_a55_eeprom",
    "compose_a60_eeprom",
    "compose_a62_eeprom",
    "compose_a65_eeprom",
    "compose_c55_eeprom",
    "compose_c60_eeprom",
    "compose_cf62_eeprom",
    "compose_eeprom",
    "compose_m55_eeprom",
    "compose_mc60_eeprom",
    "compose_sl55_eeprom",
    "compose_s55_eeprom",
    "discover_official_packages",
    "discover_sources",
    "derive_bcore_key",
    "EepromCompositionResult",
    "hash_bootkey",
    "pack_a52_eeprom",
    "pack_a55_eeprom",
    "pack_a60_eeprom",
    "pack_a62_eeprom",
    "pack_a65_eeprom",
    "pack_c55_eeprom",
    "pack_cf62_eeprom",
    "pack_c60_eeprom",
    "pack_m55_eeprom",
    "pack_mc60_eeprom",
    "pack_sl55_eeprom",
    "pack_s55_eeprom",
    "plan_split",
    "render_report",
    "reconstruct_recipe",
    "recover_bcore_key",
]
