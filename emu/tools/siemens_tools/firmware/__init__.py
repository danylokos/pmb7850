"""Siemens XBI firmware packages and executable containers."""

from .compression import convert_xbi_to_flash, materialize_xbi
from .containers import detect_exe_type, detect_service_exe_version, extract_exe
from .ffsinit import (
    FfsInitIdentity,
    FfsInitReference,
    FfsInitRecovery,
    build_ffsinit_reference_index,
    discover_ffsinit_references,
    identify_ffsinit,
    is_ffsinit,
    recover_ffsinit_bin_reference,
    recover_ffsinit_xfs,
)
from .xbi import (
    ASSEMBLY_SCHEMA,
    C55_FLASH_PROFILE_RANGES,
    C55_EEPROM_REGION_SIZE,
    FirmwareError,
    FlashLayout,
    LayoutRegion,
    SAG_JK_TRAILER_SIZE,
    SAG_JK_WH,
    SAG_UDT,
    parse_xbi,
)

__all__ = [
    "ASSEMBLY_SCHEMA",
    "C55_FLASH_PROFILE_RANGES",
    "C55_EEPROM_REGION_SIZE",
    "FirmwareError",
    "FfsInitIdentity",
    "FfsInitReference",
    "FfsInitRecovery",
    "FlashLayout",
    "LayoutRegion",
    "SAG_JK_TRAILER_SIZE",
    "SAG_JK_WH",
    "SAG_UDT",
    "build_ffsinit_reference_index",
    "convert_xbi_to_flash",
    "detect_exe_type",
    "detect_service_exe_version",
    "discover_ffsinit_references",
    "extract_exe",
    "identify_ffsinit",
    "is_ffsinit",
    "materialize_xbi",
    "parse_xbi",
    "recover_ffsinit_bin_reference",
    "recover_ffsinit_xfs",
]
