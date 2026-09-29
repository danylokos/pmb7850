from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from .identity import recreate_imei, recreate_imei_companion
from .security import (
    B5009_LEN,
    MASK32,
    MODEL_KEY1,
    UNKNOWN_KEY1_C55,
    convert_to_bcd,
    decrypt_5008,
    decrypt_5077,
)


BLOCKHEADER_LEN = 12
EEPROM_NAME_OFS = 0x12
ACTIVE_END_IDS = (0xFC00, 0xF800)
FALLBACK_END_ID = 0xF000
VALID_END_IDS = ACTIVE_END_IDS + (FALLBACK_END_ID,)
DEFAULT_REGION_LINEAR_BASE = 0x00FA0000
DEFAULT_REGION_SIZE = 0x60000
EEPROM_BANK_STRIDES = (0x20000, 0x10000)
EEPROM_MIRROR_OFFSETS = (
    (0x20000, 0x40000),
    (0x10000, 0x20000),
    (0x10000, 0x12000),
)


@dataclass(frozen=True)
class BlockEntry:
    """A resolved EEPROM directory record."""
    block_id: int
    linear: int
    file_off: int
    length: int
    seg: int
    startid: int
    endid: int
    dir_off: int  # file offset of the descriptor itself
    version: int
    active: bool


@dataclass(frozen=True)
class EepromRegion:
    """Located EEPROM region: how to map a block's linear address to a file offset."""
    region_file_base: int      # segment-aligned file offset of the EEPROM region
    region_linear_base: int    # linear address of the region start (model-fixed)
    size: int

    def linear_to_file(self, linear: int) -> int:
        return linear - self.region_linear_base + self.region_file_base


@dataclass(frozen=True)
class Block:
    """One EEPROM block from either source, keyed by the shared integer id.

    A binary dump gives ``payload`` = the on-flash bytes at a resolved ``linear``
    (CPU) address and ``file_off`` (byte offset in the dump file, the ``xxd -s``
    value); a .map gives ``payload`` = the factory-default bytes with neither.
    ``length`` == ``len(payload)``. This is the source-agnostic unit the list /
    extract / diff paths all operate on.
    """
    block_id: int
    payload: bytes
    length: int
    source_kind: str             # "map" | "dump"
    name: str = ""               # .map comment/name, else ""
    linear: int | None = None    # dump only: CPU/linear address
    file_off: int | None = None  # dump only: byte offset in the dump file
    memory_class: int | None = None
    version: int | None = None
    marker: int | None = None
    dir_off: int | None = None


def _derive_linear_base(flash: bytes, region_file_base: int, size: int) -> int | None:
    """Infer the region's linear base from its directory records.

    Every directory record carries the block's segment (linear >> 16). Blocks are
    packed by descending address from the region top, so the *smallest* segment
    seen marks the region's linear base (base = min_seg << 16). Returns None if no
    plausible records are found.
    """
    segs: list[int] = []
    end = min(len(flash), region_file_base + size) - BLOCKHEADER_LEN
    for off in range(region_file_base, end + 1):
        startid, length, _boffs, bseg, _bid, endid = struct.unpack_from("<HHHHHH", flash, off)
        if endid in VALID_END_IDS and (startid & 0xFF) in (0xFC, 0xF8, 0xF0) \
                and 1 <= length <= 0x600 and bseg <= 0xFF:
            segs.append(bseg)
    if not segs:
        return None
    return min(segs) << 16


def find_eeprom_region(flash: bytes, base: int | None = None,
                       region_linear_base: int | None = None,
                       size: int = DEFAULT_REGION_SIZE) -> EepromRegion:
    """Locate the EEPROM region in any dump (device-generic).

    C55-family banks hold "EELITE" at +0x12; M55's 0x70-byte wrapper moves it
    to +0x82. S55 uses the same wrapper, then places its first EEFULL bank one
    segment later and additional copies every 0x2000 bytes. Detection finds
    "EELITE" with one of the known two-mirror offset pairs; this rejects stray
    strings elsewhere in flash. The region's *linear* base is then derived from
    the directory records themselves (min segment << 16), so devices whose
    EEPROM is not at 0xFA0000 work without a table.

    ``base`` forces the region file base; ``region_linear_base`` forces the linear
    base (else derived, else the C55/A55 default).
    """
    if base is not None:
        rfb = base
        rlb = region_linear_base
        if rlb is None:
            rlb = _derive_linear_base(flash, rfb, size)
        return EepromRegion(rfb, rlb if rlb is not None else DEFAULT_REGION_LINEAR_BASE, size)

    magic = flash.find(b"EELITE")
    while magic != -1:
        candidate = magic - EEPROM_NAME_OFS
        if candidate >= 0 and flash[magic - 2:magic] == b"\xfe\xfe":
            for first, second in EEPROM_MIRROR_OFFSETS:
                f1 = magic + first
                f2 = magic + second
                if flash[f1:f1 + 6] == b"EEFULL" and flash[f2:f2 + 6] == b"EEFULL":
                    # EEPROM linear bases are segment-aligned. Wrapped M55/S55
                    # banks place the magic at +0x82 rather than +0x12.
                    rfb = candidate & ~0xFFFF
                    rlb = region_linear_base
                    if rlb is None:
                        rlb = _derive_linear_base(flash, rfb, size)
                    if rlb is not None:
                        return EepromRegion(rfb, rlb, size)
        magic = flash.find(b"EELITE", magic + 1)
    raise ValueError(
        "could not locate EEPROM region: no 'EELITE' header with EEFULL mirrors "
        "found; pass --eeprom-base"
    )


def parse_directory_records(flash: bytes, region: EepromRegion) -> list[BlockEntry]:
    """Walk the tCOMM_BLOCKHEADER directory and return every valid record.

    Scans the region byte-by-byte (as Freia's COMM_CopyBlock does), accepting a
    12-byte record whose terminator is valid, whose startid marker byte matches,
    and whose segment lands inside the region. Records remain in physical scan
    order so callers can inspect active and historical copies.
    """
    seg_lo = region.region_linear_base >> 16
    seg_hi = (region.region_linear_base + region.size) >> 16
    start = region.region_file_base
    end = min(len(flash), start + region.size) - BLOCKHEADER_LEN
    out: list[BlockEntry] = []
    for off in range(start, end + 1):
        startid, length, boffs, bseg, bid, endid = struct.unpack_from("<HHHHHH", flash, off)
        if endid not in VALID_END_IDS:
            continue
        if (startid & 0xFF) not in (0xFC, 0xF8, 0xF0):
            continue
        if not (seg_lo <= bseg <= seg_hi):
            continue
        if length == 0 or length > region.size:
            continue
        linear = bseg * 0x10000 + boffs
        file_off = region.linear_to_file(linear)
        if file_off < 0 or file_off + length > len(flash):
            continue
        out.append(BlockEntry(
            bid, linear, file_off, length, bseg, startid, endid, off,
            startid >> 8, endid in ACTIVE_END_IDS,
        ))
    return out


def parse_directory(flash: bytes, region: EepromRegion) -> dict[int, BlockEntry]:
    """Resolve one record per ID using the firmware/Freia marker priority.

    Firmware first searches for active ``FC00``/``F800`` descriptors and only
    falls back to ``F000`` history when no active descriptor exists. Within
    either class, the first physically encountered valid record wins.
    """
    records = parse_directory_records(flash, region)
    out: dict[int, BlockEntry] = {}
    for active in (True, False):
        for entry in records:
            if entry.active == active and entry.block_id not in out:
                out[entry.block_id] = entry
    return out


# --------------------------------------------------------------------------- #
# .map factory-default source (text INI)
#
# A Siemens .map file lists each block's DEFAULT payload as text, keyed by the
# same integer id the binary directory uses (a [5009] section == block 0x1391).
# It carries no directory/addresses — just id -> default bytes. Structure:
#   [MapFileInfo]              header (Product/Provider/SWVersion/…)
#   [<decimal id>]  ; <name>   Offset/Memory/PStufe/Service/Version/DataSize +
#   Data { 0xNN 0xNN … }       DataSize hex bytes (space-separated, ~10/line)
#   [CheckSum]                 trailing Key = <dec> ; CheckByte: 0xNN
# See docs/reference/eeprom-block-packing.md.
# --------------------------------------------------------------------------- #

_MAP_SECTION_RE = re.compile(
    r"^\[(?P<id>\d+)\](?P<name>[^\r\n]*)(?P<body>.*?)(?=^\[|\Z)", re.M | re.S)
_MAP_DATA_RE = re.compile(r"Data\s*\{(?P<data>.*?)\}", re.S)
_MAP_BYTE_RE = re.compile(r"0x([0-9A-Fa-f]{2})")


def _map_int(body: str, field: str) -> int | None:
    match = re.search(rf"^{re.escape(field)}\s*=\s*(\d+)", body, re.M)
    return int(match.group(1)) if match is not None else None


def looks_like_map(raw: bytes) -> bool:
    """A .map is text starting with the [MapFileInfo] header."""
    return raw[:64].lstrip().startswith(b"[MapFileInfo]")


def parse_map_file(path: Path) -> dict[int, Block]:
    """Parse a .map factory-defaults file into {id: Block} (source_kind='map').

    Skips the [MapFileInfo] / [CheckSum] sections. Decodes each block's
    ``Data { 0xNN … }`` payload; a DataSize/payload-length mismatch is warned to
    stderr, not fatal (the actual bytes win). Latin-1 to tolerate German names.
    """
    txt = path.read_text(encoding="latin-1")
    out: dict[int, Block] = {}
    for m in _MAP_SECTION_RE.finditer(txt):
        block_id = int(m.group("id"))
        body = m.group("body")
        data = _MAP_DATA_RE.search(body)
        if data is None:
            continue  # [MapFileInfo]/[CheckSum] have no Data block
        payload = bytes(int(b, 16) for b in _MAP_BYTE_RE.findall(data.group("data")))
        ds = re.search(r"DataSize\s*=\s*(\d+)", body)
        if ds is not None and int(ds.group(1)) != len(payload):
            print(f"warning: block {block_id} DataSize={ds.group(1)} but "
                  f"{len(payload)} bytes in Data{{}}", file=sys.stderr)
        name = m.group("name").strip().lstrip(";").strip()
        out[block_id] = Block(
            block_id, payload, len(payload), "map", name,
            memory_class=_map_int(body, "Memory"),
            version=_map_int(body, "Version"),
        )
    return out


def load_dump_blocks(flash: bytes, region: EepromRegion) -> dict[int, Block]:
    """Turn a parsed binary directory into the shared {id: Block} inventory."""
    full_magic = flash.find(
        b"EEFULL",
        region.region_file_base,
        min(len(flash), region.region_file_base + region.size),
    )
    full_bank_file_base = (
        full_magic - EEPROM_NAME_OFS if full_magic >= 0 else None
    )
    out: dict[int, Block] = {}
    for bid, e in parse_directory(flash, region).items():
        payload = flash[e.file_off:e.file_off + e.length]
        memory_class = (
            2 if full_bank_file_base is not None
            and e.file_off < full_bank_file_base else 8
        )
        out[bid] = Block(
            bid, payload, e.length, "dump", "", e.linear, e.file_off,
            memory_class, e.version, e.endid, e.dir_off,
        )
    return out


def load_eeprom_source(path: Path, base: int | None = None,
                       region_linear_base: int | None = None) -> tuple[str, dict[int, Block]]:
    """Read either a .map or a binary dump into (kind, {id: Block}).

    Detects a .map by its text header; anything else is treated as a binary
    dump/slice and located via find_eeprom_region (honoring --eeprom-base /
    --region-linear-base).
    """
    raw = path.read_bytes()
    if looks_like_map(raw):
        return "map", parse_map_file(path)
    region = find_eeprom_region(raw, base=base, region_linear_base=region_linear_base)
    return "dump", load_dump_blocks(raw, region)


# Transform class per block id. Everything not listed is treated as plaintext.
CIPHER_IDS = {0x1390: "5008", 0x13D5: "5077"}   # FSN+IMEI-keyed stream cipher
SCRAMBLE_IDS = {0x1391}                           # 5009: even model-S-box rounds
IMEI_COMPANION_IDS = {0x004C}                    # 76: odd model-S-box rounds
# SIMLOCK signature words that mark a lock block (Joker inject / Freia).
SIMLOCK_SIG = (0x4545FEFE, 0x4554494C)


def block_class(block_id: int) -> str:
    if block_id in CIPHER_IDS:
        return "cipher"
    if block_id in SCRAMBLE_IDS:
        return "scramble"
    if block_id in IMEI_COMPANION_IDS:
        return "imei-companion"
    return "plaintext"


def serialize_block(block: Block) -> dict:
    """Serialize one EEPROM block for stable CLI and fullflash JSON output."""
    return {
        "id": block.block_id,
        "id_hex": f"{block.block_id:#06x}",
        "linear": f"{block.linear:#x}" if block.linear is not None else None,
        "file_off": (
            f"{block.file_off:#x}" if block.file_off is not None else None
        ),
        "length": block.length,
        "class": block_class(block.block_id),
        "memory_class": block.memory_class,
        "version": block.version,
        "marker": (
            f"{block.marker:#06x}" if block.marker is not None else None
        ),
        "dir_off": (
            f"{block.dir_off:#x}" if block.dir_off is not None else None
        ),
        "name": block.name or None,
    }


def _resolve_block_id(name: str) -> int:
    """Accept a decimal EEP number (5008), a hex id (0x1390), or a bare int."""
    n = int(name, 0)
    # EEP block numbers (>=1000 decimal) map to id = number encoded directly:
    # 5008 -> 0x1390. Freia ids are the decimal number in hex-of-decimal? No —
    # id 0x1390 == 5008 decimal, so a decimal 5008 IS the id value 5008.
    return n


def extract_block(inventory: dict[int, Block], block_id: int,
                  fsn: int | None = None, imei: str | None = None,
                  key1: list[int] | None = None) -> dict:
    """Extract one block by id from a {id: Block} inventory, dispatching on class.

    Works on both map and dump inventories. Returns a dict with the block's
    metadata plus the recovered bytes/value:
      - plaintext      : {'class':'plaintext','raw_hex':..., 'simlock': bool}
      - scramble       : {'class':'scramble','raw_hex':..., 'imei':..., 'empty':bool}
      - imei-companion : same recovered fields, using block 76's odd rounds
      - cipher         : {'class':'cipher','plaintext_hex':...} (needs fsn+imei)
    """
    if block_id not in inventory:
        raise KeyError(f"block id {block_id:#06x} ({block_id}) not in EEPROM inventory")
    block = inventory[block_id]
    raw = block.payload
    info = {
        "id": block_id,
        "id_hex": f"{block_id:#06x}",
        "linear": f"{block.linear:#x}" if block.linear is not None else None,
        "file_off": f"{block.file_off:#x}" if block.file_off is not None else None,
        "length": block.length,
        "class": block_class(block_id),
        "memory_class": block.memory_class,
        "version": block.version,
        "marker": f"{block.marker:#06x}" if block.marker is not None else None,
        "dir_off": f"{block.dir_off:#x}" if block.dir_off is not None else None,
    }
    cls = info["class"]
    if cls == "plaintext":
        info["raw_hex"] = raw.hex()
        sig = struct.unpack_from("<I", raw, 0)[0] if len(raw) >= 4 else None
        info["simlock"] = sig in SIMLOCK_SIG
        return info
    if cls in ("scramble", "imei-companion"):
        info["raw_hex"] = raw.hex()
        try:
            decoder = recreate_imei if cls == "scramble" else recreate_imei_companion
            imei_val, empty = decoder(raw[:B5009_LEN])
            info["imei"] = imei_val
            info["empty"] = empty
        except ValueError as exc:
            info["imei_error"] = str(exc)
        return info
    # cipher
    if fsn is None or imei is None:
        raise ValueError(f"block {block_id:#06x} is cipher-class; --fsn and --imei are required")
    bcd = convert_to_bcd(imei)
    k1 = key1 if key1 is not None else UNKNOWN_KEY1_C55
    if block_id == 0x1390:
        info["plaintext_hex"] = bytes(decrypt_5008(raw, fsn, bcd, k1)).hex()
    else:  # 0x13D5 == 5077
        info["plaintext_hex"] = bytes(decrypt_5077(raw, fsn, bcd, k1)).hex()
    return info


def _parse_int(text: str) -> int:
    return int(text, 0)


def _parse_key1(text: str) -> list[int]:
    """Parse a --key1 override: a model name, or 16 comma/space-separated u32 words."""
    if text in MODEL_KEY1:
        return MODEL_KEY1[text]
    parts = [p for p in text.replace(",", " ").split() if p]
    if len(parts) != 16:
        raise argparse.ArgumentTypeError(
            f"--key1 must be a known model {sorted(MODEL_KEY1)} or 16 u32 words, got {len(parts)}"
        )
    return [int(p, 0) & MASK32 for p in parts]


# Friendly --block aliases -> id.
_BLOCK_ALIASES = {"5008": 0x1390, "5077": 0x13D5, "5009": 0x1391}


def _resolve_want(block_arg, inventory: dict[int, Block], verify: bool) -> list[int]:
    """Map --block to a list of block ids to extract."""
    if block_arg in (None, "cipher"):
        want = [0x1390, 0x13D5]
    elif block_arg == "all":
        want = sorted(inventory)
    elif block_arg in _BLOCK_ALIASES:
        want = [_BLOCK_ALIASES[block_arg]]
    else:
        want = [_resolve_block_id(block_arg)]
    if verify:  # a full DD2476 verify needs both cipher blocks (region 3 is in 5077)
        for bid in (0x1390, 0x13D5):
            if bid not in want:
                want.append(bid)
    return want
