#!/usr/bin/env python3
"""Focused tests for tools.siemens_tools.eeprom."""

from __future__ import annotations

import hashlib
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

import struct  # noqa: E402

from . import REPO_ROOT
from tools.siemens_tools.eeprom import (  # noqa: E402
    B5008_LEN,
    B5009_LEN,
    B5077_LEN,
    DEFAULT_REGION_LINEAR_BASE,
    MODEL_ID7_KEY1_ALIASES,
    MODEL_KEY1,
    UNKNOWN_KEY1_C55,
    BUNDLE_PROFILE,
    BUNDLE_SCHEMA,
    Block,
    EepromRegion,
    bcd_imei_to_normal,
    block_class,
    convert_to_bcd,
    create_imei_records,
    create_unlocked_plaintexts,
    crc_check_bytes,
    decrypt_5008,
    decrypt_5077,
    diff_sources,
    encrypt_5008,
    encrypt_5077,
    extract_block,
    find_eeprom_region,
    generate_identity_bundle,
    generate_overlay_bundle,
    imei_check_digit,
    load_dump_blocks,
    load_eeprom_source,
    parse_directory,
    parse_directory_records,
    parse_map_file,
    parse_fsn,
    recreate_imei,
    recreate_imei_companion,
    render_block_bytediff,
    verify_checks,
)
from tools.siemens_tools.eeprom.cli import main
from tools.siemens_tools.eeprom.storage import _parse_key1 as parse_key1



from tools.bundled_firmware import image as bundled_image


# Bundled C55 SW24/LG1; required inputs are verified by the manifest selector.
FLASH = bundled_image("c55")
STOCK_FLASH = bundled_image("c55", langpack=91)
CUST_FSN = 0x1234ABCD
CUST_IMEI = "11223344556677"
# Known-answer C55 v24 record linear addresses (were tool constants; test-local now).
C55_LINEAR_5008 = 0x00FC01AE
C55_LINEAR_5077 = 0x00FC24F4
C55_LINEAR_5009 = 0x00FC028E
# Known-answer sum/xor pairs for the bundled identity records.
EXPECTED_CHECKS = [
    ("5008.header", 0x62, 0x64),
    ("5008.body", 0x70, 0xE0),
    ("5077.body", 0xEF, 0x3D),
]



A55_FLASH = bundled_image("a55")
A55_LINEAR_5009 = 0xFC028E
M55_FLASH = bundled_image("m55")


def blockheader(startid: int, length: int, offs: int, seg: int, bid: int, endid: int) -> bytes:
    return struct.pack("<HHHHHH", startid, length, offs, seg, bid, endid)
