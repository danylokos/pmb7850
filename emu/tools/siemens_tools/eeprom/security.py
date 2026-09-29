#!/usr/bin/env python3
"""List, extract, and diff Siemens x55 EEPROM parameter blocks.

EEPROM blocks live in two forms on disk, both keyed by the same integer id (the
decimal EEP number equals the binary block id: 5009 == 0x1391):
  * compiled binary  — the packed directory inside a full-flash dump or EEPROM
    slice. Block byte-offsets are NOT fixed; they are read from a table of 12-byte
    tCOMM_BLOCKHEADER records (Freia COMMLOC.H:114), anchored by an "EELITE"/
    "EEFULL" magic. We locate the region (auto, or via --eeprom-base) and walk it.
  * text .map source — Siemens factory-default files (fw/<dev>/<ver>/map/*.map),
    an INI listing of each block's default payload. No directory/addresses, just
    id -> default bytes.
``load_eeprom_source`` reads either transparently. See
docs/reference/eeprom-block-packing.md for the on-flash format.

Four subcommands:
    list    <source>            enumerate every block (id, len, class, name/addr)
    extract <source> --block …  recover one/some blocks' bytes
    diff    <a> <b>             compare two sources; --block <id> = byte-level view
    generate                    create a deterministic unlocked identity overlay

Extraction is by transform CLASS, not device:
  * plaintext      (most blocks) -> raw slice; no key needed.
  * scramble       (5009)        -> even-round model S-box de-scramble -> IMEI.
  * imei-companion (76)          -> odd-round model S-box de-scramble -> IMEI.
  * cipher         (5008/5077)    -> FSN+IMEI+model-keyed stream cipher.
Block 67 is plaintext temperature/voltage adjustment data, not an IMEI record.
Only cipher blocks need a model key row. Freia 15 maps the EEPROM-bearing
A50/A52/A55/A60/C55/C60/M55/MC60/S55/SL55/SX1/1168/2128 records to crypto
model ID 7, so they share one row. Select it with --key1 (a model name or 16
u32 words). Personalized M55/S55 known-answer decryption remains unverified
because the repository has no suitable IMEI/FSN ground truth for them.

The cipher is a faithful port of Freia's C45/C55 recreate path
(refs/freia/siemens_source/SEC.C + INCLUDE/SECLOC.H):

    ModifyinputArray1/2 -> CreateCodE2 (key schedule, MD-shaped)
    GosubC7D524          -> RC4-style key-scheduling (KSA), 256-byte state
    GosubC7D5CE          -> RC4-style keystream (PRGA), XORed over each segment

Because the cipher key is the 32-bit FSN, pass the FSN directly. The hardware
identity fold is lossy and documented separately in docs/ANALYSIS.md; recover
the FSN there (or with tools/recover_fsn.c) and feed
it here.

The tool can VERIFY its own decrypt: before encryption Freia appends 8-bit
sum+XOR check bytes (CRCBuffer) over three regions — the 5008 StrangeHeader, the
5008 body, and the 5077 body — which are exactly the three blocks firmware
validator DD2476 checks. --verify recomputes those pairs so a correct decrypt is
provable against known-good firmware values.

"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path


MASK32 = 0xFFFFFFFF

# UnknownKey1 row for the A50/C55 model (SECLOC.H:284-289). Words 12..15 are the
# per-handset slots CreateCodE2 overwrites with FSN (word 12) and IMEI-BCD
# (words 14..15) before the key schedule runs.
UNKNOWN_KEY1_C55 = [
    0xE5EE0A14, 0xF8FFFBD7, 0x03C64D76, 0xB2F9B2FD,
    0xA266CDB4, 0xA410F726, 0x43C2113E, 0xAFF651F4,
    0x8C986DDC, 0x8E588F08, 0x8EB84FDC, 0x4128E208,
    0x0, 0x08555555, 0x0, 0x0,
]

# UnknownKey2 seed state (SECLOC.H:287).
UNKNOWN_KEY2 = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0x200, 0x0]

# EEPROM-bearing Freia 15 phone records whose crypto model ID is 7. The three
# boot-only records (C60 BOOT, M55 BOOT, MC60 BOOT) also carry ID 7 but have no
# personalized EEPROM region, so they are deliberately not CLI aliases.
MODEL_ID7_KEY1_ALIASES = (
    "A50", "A52", "A55", "A60", "1168", "C55", "2128",
    "C60", "S55", "SX1", "M55", "MC60", "SL55",
)

# Freia 15 g_UnknownKey1[7], byte-identical to the existing C55/A50 constant.
# Keep UNKNOWN_KEY1_C55 as the stable programmatic API used by existing callers.
MODEL_KEY1 = {name: UNKNOWN_KEY1_C55 for name in MODEL_ID7_KEY1_ALIASES}

# Block geometry for C45/C55 (SECLOC.H B08Sz/B77Sz last entry; C55.A66 sizes).
B5008_LEN = 0xE0
B5077_LEN = 0xE8
STRANGE_HEADER_LEN = 0x18  # sizeof(StrangeHeader)

# CRCBuffer body lengths that DD2476 checks (SECLOC.H:33-34).
SEC_MAINBODY5008_CRC_LEN = 0xB0
SEC_MAINBODY5077_CRC_LEN = 0xD8

# IMEI block (5009/0001) geometry and A50/C55 obfuscation tables. Unlike 5008/5077
# this layer is NOT FSN-keyed — it is a model-scrambled BCD IMEI, so the IMEI
# decodes from the dump alone. Freia 15 routes the listed model-ID-7 aliases to
# the same A50 tables.
B5009_LEN = 0x0A  # FREIA_5009_AND_0001_LEN (FREIAPUB.H:55)

# Deterministic Freia-15 model-ID-7 unlocked profile. CreateIMEISpecificBlocks
# starts each cipher segment from a zero random seed XORed with Cod00XORKey[7].
# The templates are SECLOC.H's unlocked StrangeHeader/MainBody5008/MainBody5077
# globals. Their three trailing sum/XOR pairs are filled before encryption.
MODEL_ID7 = 7
BUNDLE_SCHEMA = "cemu.eeprom-identity-overlay"
BUNDLE_SCHEMA_VERSION = 2
BUNDLE_PROFILE = "freia15-model7-unlocked"
BUNDLE_PROFILE_VERSION = 1
COD00_XOR_KEY_A50 = bytes.fromhex("8f1d74f4718a0e35")
STRANGE_HEADER_TEMPLATE = bytes.fromhex(
    "000300000067000000000000ff00ffffffffffff00ff6264"
)
MAIN_BODY_5008_TEMPLATE = bytes.fromhex(
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff1fffffffffffffffffffffffffffffffffffffffff70e0ffffffffffff"
)
MAIN_BODY_5077_TEMPLATE = bytes.fromhex(
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff1fffffffffffffffffffffffffffffffffffffffffffff13883d1141073d110000000000000000000000000000000000000000000000000000000000000000ef3d000000000000"
)

# Pars[7] "A50/C55" per-round scramble constants (SECLOC.H:76).
PARS_A50 = [0xE3, 0xB7, 0x5C, 0x13, 0xB0, 0xD2, 0xC4, 0x19]
# CodeTable04/05 nibble S-boxes for the A50 GetCodeTable branch (SECLOC.H:135-143).
CODE_TABLE04 = [0x03, 0x0F, 0x01, 0x0D, 0x05, 0x0B, 0x0D, 0x09,
                0x0D, 0x07, 0x06, 0x00, 0x0E, 0x06, 0x0B, 0x08]
CODE_TABLE05 = [0x0A, 0x0E, 0x01, 0x05, 0x03, 0x06, 0x02, 0x0F,
                0x0B, 0x0A, 0x03, 0x05, 0x06, 0x05, 0x04, 0x02]


def _rol32(value: int, count: int) -> int:
    value &= MASK32
    return ((value << count) | (value >> (32 - count))) & MASK32


def validate_imei(imei: str) -> str:
    """Require the generator's public input form: exactly 14 decimal digits."""
    if not re.fullmatch(r"[0-9]{14}", imei or ""):
        raise ValueError(f"IMEI must be exactly 14 decimal digits, got {imei!r}")
    return imei


def parse_fsn(text: str) -> int:
    """Parse the public eight-hex-digit FSN representation."""
    value = text[2:] if text.startswith(("0x", "0X")) else text
    if not re.fullmatch(r"[0-9A-Fa-f]{8}", value):
        raise ValueError(f"FSN must be exactly eight hexadecimal digits, got {text!r}")
    return int(value, 16)


def convert_to_bcd(imei: str) -> bytes:
    """Port of SEC.C:146 ConvertToBCD: 14 IMEI digits -> 8 BCD bytes.

    Byte 0 low nibble is a fixed 0x0A type tag; digits pack high-then-low nibble.
    """
    digits = [d for d in imei if d.isdigit()]
    if len(digits) < 14:
        raise ValueError(f"IMEI needs >=14 digits, got {len(digits)}: {imei!r}")
    d = [int(c) for c in digits[:14]]
    bcd = bytearray(8)
    bcd[0] = 0x0A
    i = 0
    k = 0
    # Mirrors the C loop: bcd[i] |= (digit<<4); i++; bcd[i] = digit; (7 iterations)
    src = 0
    while i < 7:
        bcd[i] |= (d[src] << 4) & 0xF0
        src += 1
        i += 1
        bcd[i] = d[src]
        src += 1
    return bytes(bcd)


def modify_input_array1(arr64: list[int], arr4c: list[int]) -> None:
    """Port of SEC.C:357-451 ModifyinputArray1 — in-place mix of arr64[0..3]."""
    a0, a1, a2, a3 = arr64[0], arr64[1], arr64[2], arr64[3]

    for i in range(4):
        a0 = _rol32(a0 + (arr4c[i * 4] + (((a2 ^ a3) & a1) ^ a3)), 3)
        a3 = _rol32(a3 + (arr4c[i * 4 + 1] + (((a1 ^ a2) & a0) ^ a2)), 7)
        a2 = _rol32(a2 + (arr4c[i * 4 + 2] + (((a0 ^ a1) & a3) ^ a1)), 11)
        a1 = _rol32(a1 + (arr4c[i * 4 + 3] + (((a3 ^ a0) & a2) ^ a0)), 19)

    for i in range(4):
        a0 = _rol32(a0 + (((a1 & a2) | (a1 & a3) | (a2 & a3)) + arr4c[i] + 0x5A827999), 3)
        a3 = _rol32(a3 + (((a0 & a1) | (a0 & a2) | (a1 & a2)) + arr4c[i + 4] + 0x5A827999), 5)
        a2 = _rol32(a2 + (((a3 & a0) | (a3 & a1) | (a0 & a1)) + arr4c[i + 8] + 0x5A827999), 9)
        a1 = _rol32(a1 + (((a2 & a3) | (a2 & a0) | (a3 & a0)) + arr4c[i + 12] + 0x5A827999), 13)

    reorder = (0, 2, 1, 3)
    for i in range(4):
        j = reorder[i]
        a0 = _rol32(a0 + (arr4c[j] + (a1 ^ a2 ^ a3) + 0x6ED9EBA1), 3)
        a3 = _rol32(a3 + (arr4c[j + 8] + (a0 ^ a1 ^ a2) + 0x6ED9EBA1), 9)
        a2 = _rol32(a2 + (arr4c[j + 4] + (a3 ^ a0 ^ a1) + 0x6ED9EBA1), 11)
        a1 = _rol32(a1 + (arr4c[j + 12] + (a2 ^ a3 ^ a0) + 0x6ED9EBA1), 15)

    arr64[0] = (arr64[0] + a0) & MASK32
    arr64[1] = (arr64[1] + a1) & MASK32
    arr64[2] = (arr64[2] + a2) & MASK32
    arr64[3] = (arr64[3] + a3) & MASK32


def modify_input_array2(arr64: list[int]) -> list[int]:
    """Port of SEC.C:453-471 ModifyinputArray2 — finalize into CodE2[0..3]."""
    arr50 = [0] * 16
    arr50[0] = 0x80
    arr50[14] = arr64[4]
    arr50[15] = arr64[5]
    modify_input_array1(arr64, arr50)
    return [arr64[i] & MASK32 for i in range(4)]


def create_cod_e2(fsn: int, bcd: bytes, key1: list[int]) -> list[int]:
    """Port of SEC.C CreateCodE2 with the SEC.C:2503-2505 key injection.

    UnknownKey1[model][12] = FSN; [14..15] = IMEI-BCD (little-endian words).
    """
    arr4c = list(key1)
    arr4c[12] = fsn & MASK32
    arr4c[14] = int.from_bytes(bcd[0:4], "little")
    arr4c[15] = int.from_bytes(bcd[4:8], "little")
    arr64 = list(UNKNOWN_KEY2)
    modify_input_array1(arr64, arr4c)
    return modify_input_array2(arr64)


def gosub_c7d524(cod_e2: list[int]) -> bytearray:
    """Port of SEC.C:761-793 GosubC7D524 (RC4-style KSA), Num=0x80.

    Returns the 259-byte Cods08 state: [0..2] are the PRGA counters (0,0,0),
    [3..258] is the 256-byte permutation.
    """
    num = 0x80 >> 3  # = 16
    cods08 = bytearray(259)
    for i in range(256):
        cods08[i + 3] = i

    cd01 = bytearray(16)
    for i in range(4):
        cd01[i * 4 + 0] = cod_e2[i] & 0xFF
        cd01[i * 4 + 1] = (cod_e2[i] >> 8) & 0xFF
        cd01[i * 4 + 2] = (cod_e2[i] >> 16) & 0xFF
        cd01[i * 4 + 3] = (cod_e2[i] >> 24) & 0xFF

    nc01 = 0
    for i in range(256):
        nc01 = (nc01 + cd01[i % num] + cods08[i + 3]) & 0xFF
        cods08[i + 3], cods08[nc01 + 3] = cods08[nc01 + 3], cods08[i + 3]
    return cods08


def gosub_c7d5ce(decrypt: bool, cods08: bytearray, out: bytearray,
                 idx: int, src: bytes, num: int) -> None:
    """Port of SEC.C:816-847 GosubC7D5CE (RC4-style PRGA), in-place into out[idx:].

    decrypt=True: out[i] = src[i] ^ keystream (recover plaintext from ciphertext).
    decrypt=False: same XOR, other direction (encrypt). Counters persist in
    cods08[0..2] across calls within a segment schedule.

    The carried counter Rl1 (cods08[2]) holds the CIPHERTEXT byte in both
    directions — the encrypt branch stores its output (ciphertext) and the
    decrypt branch stores its input (also ciphertext) — which keeps the keystream
    feedback identical, so decrypt exactly inverts encrypt (SEC.C:830-842).
    """
    rl6 = cods08[0]
    rl3 = cods08[1]
    rl1 = cods08[2]
    for i in range(num):
        rl6 = (rl6 + 1) & 0xFF
        rl1 = (rl1 + cods08[rl6 + 3]) & 0xFF
        rl3 = (rl3 + rl1) & 0xFF
        rl7 = (rl1 + cods08[rl3 + 3]) & 0xFF
        cods08[rl6 + 3], cods08[rl3 + 3] = cods08[rl3 + 3], rl1
        if decrypt:
            rl1 = src[i]
            out[i + idx] = rl1 ^ cods08[rl7 + 3]
        else:
            rl1 = src[i] ^ cods08[rl7 + 3]
            out[i + idx] = rl1
    cods08[0] = rl6
    cods08[1] = rl3
    cods08[2] = rl1


def _crypt_5008(direction: bool, cipher: bytes, fsn: int, bcd: bytes,
                key1: list[int]) -> bytearray:
    """5008 segment/re-seed schedule from RecreateIMEISpecificBlocksC45 (SEC.C:2513-2531)."""
    cod_e2 = create_cod_e2(fsn, bcd, key1)
    out = bytearray(len(cipher))
    h = STRANGE_HEADER_LEN

    cods08 = gosub_c7d524(cod_e2)
    gosub_c7d5ce(direction, cods08, out, 0x00, cipher[0x00:0x08], 0x08)
    gosub_c7d5ce(direction, cods08, out, 0x08, cipher[0x08:0x08 + h], h)

    cods08 = gosub_c7d524(cod_e2)
    gosub_c7d5ce(direction, cods08, out, h + 8, cipher[h + 8:h + 16], 0x08)
    body_len = B5008_LEN - (h + 16)
    gosub_c7d5ce(direction, cods08, out, h + 16, cipher[h + 16:h + 16 + body_len], body_len)
    return out


def _crypt_5077(direction: bool, cipher: bytes, fsn: int, bcd: bytes,
                key1: list[int]) -> bytearray:
    """5077 segment/re-seed schedule from RecreateIMEISpecificBlocksC45 (SEC.C:2533-2536)."""
    cod_e2 = create_cod_e2(fsn, bcd, key1)
    out = bytearray(len(cipher))
    cods08 = gosub_c7d524(cod_e2)
    gosub_c7d5ce(direction, cods08, out, 0x00, cipher[0x00:0x08], 0x08)
    body_len = B5077_LEN - 8
    gosub_c7d5ce(direction, cods08, out, 0x08, cipher[0x08:0x08 + body_len], body_len)
    return out


def decrypt_5008(cipher: bytes, fsn: int, bcd: bytes,
                 key1: list[int] = UNKNOWN_KEY1_C55) -> bytearray:
    return _crypt_5008(True, cipher, fsn, bcd, key1)


def decrypt_5077(cipher: bytes, fsn: int, bcd: bytes,
                 key1: list[int] = UNKNOWN_KEY1_C55) -> bytearray:
    return _crypt_5077(True, cipher, fsn, bcd, key1)


def encrypt_5008(plain: bytes, fsn: int, bcd: bytes,
                 key1: list[int] = UNKNOWN_KEY1_C55) -> bytearray:
    return _crypt_5008(False, plain, fsn, bcd, key1)


def encrypt_5077(plain: bytes, fsn: int, bcd: bytes,
                 key1: list[int] = UNKNOWN_KEY1_C55) -> bytearray:
    return _crypt_5077(False, plain, fsn, bcd, key1)
