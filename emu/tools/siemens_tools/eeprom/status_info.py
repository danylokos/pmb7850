from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import date
from pathlib import Path

from .storage import Block, load_eeprom_source


STATUS_INFO_BLOCK = 5005
STATUS_INFO_LENGTH = 64


@dataclass(frozen=True)
class StatusInfo:
    production_date: date | None
    day_raw: int
    month_raw: int
    year_digit_raw: int
    standard_sw: int | None
    standard_map: int | None
    d_map_provider: int | None
    d_map: int | None
    variant: str
    variant_letter_index: int
    variant_suffix_nibbles: tuple[int, int, int]
    reserved_0d: int
    filler_14_low: int

    @property
    def production_date_text(self) -> str | None:
        return self.production_date.isoformat() if self.production_date else None

    @property
    def standard_map_sw(self) -> str:
        if self.standard_map is None or self.standard_sw is None:
            return "-/-"
        return f"{self.standard_map}/{self.standard_sw}"

    @property
    def d_map_provider_text(self) -> str:
        if self.d_map is None or self.d_map_provider is None:
            return "-/-"
        return f"{self.d_map}/{self.d_map_provider}"

    def as_dict(self) -> dict[str, object]:
        result = asdict(self)
        result["production_date"] = self.production_date_text
        result["standard_map_sw"] = self.standard_map_sw
        result["d_map_provider_text"] = self.d_map_provider_text
        result["variant_suffix_nibbles"] = list(self.variant_suffix_nibbles)
        return result


def _validate_payload(payload: bytes) -> None:
    if len(payload) != STATUS_INFO_LENGTH:
        raise ValueError(
            f"EEPROM block 5005 length is {len(payload)}, expected {STATUS_INFO_LENGTH}"
        )


def decode_status_info(payload: bytes) -> StatusInfo:
    """Decode the bytes consumed by the x55 live Status-page renderer."""
    _validate_payload(payload)
    day = payload[0x0B]
    month = payload[0x0C] >> 4
    year_digit = payload[0x0C] & 0x0F
    try:
        production_date = date(2000 + year_digit, month, day)
    except ValueError:
        production_date = None

    standard_sw = None if payload[0x0E] == 0xFF else payload[0x0E]
    standard_map = None if payload[0x0F] == 0xFF else payload[0x0F]
    provider = None if payload[0x10] == 0xFF else payload[0x10]
    d_map = None if payload[0x11] == 0xFF else payload[0x11]
    letter_index = payload[0x12]
    letter = chr(ord("A") + letter_index - 1) if 1 <= letter_index <= 25 else "?"
    suffix = (payload[0x13] >> 4, payload[0x13] & 0x0F, payload[0x14] >> 4)
    suffix_text = "".join(f"{n:X}" for n in suffix)
    return StatusInfo(
        production_date=production_date,
        day_raw=day,
        month_raw=month,
        year_digit_raw=year_digit,
        standard_sw=standard_sw,
        standard_map=standard_map,
        d_map_provider=provider,
        d_map=d_map,
        variant=f"{letter}{suffix_text}",
        variant_letter_index=letter_index,
        variant_suffix_nibbles=suffix,
        reserved_0d=payload[0x0D],
        filler_14_low=payload[0x14] & 0x0F,
    )


def encode_status_info(
    template: bytes,
    *,
    production_date: date,
    standard_sw: int,
    standard_map: int,
    d_map_provider: int,
    d_map: int,
    variant: str,
) -> bytes:
    """Encode resolved fields while preserving every opaque/reserved bit."""
    _validate_payload(template)
    if not 2000 <= production_date.year <= 2009:
        raise ValueError("block 5005 Status year must be in 2000..2009")
    for name, value in (
        ("standard SW", standard_sw),
        ("standard map", standard_map),
        ("D-Map provider", d_map_provider),
        ("D-Map", d_map),
    ):
        if not 0 <= value <= 0xFE:
            raise ValueError(f"block 5005 {name} must be in 0..254")
    if len(variant) != 4 or not "A" <= variant[0] <= "Y" or any(
        ch not in "0123456789ABCDEF" for ch in variant[1:].upper()
    ):
        raise ValueError("block 5005 variant must be A..Y plus three nibbles")

    out = bytearray(template)
    out[0x0B] = production_date.day
    out[0x0C] = (production_date.month << 4) | (production_date.year - 2000)
    out[0x0E] = standard_sw
    out[0x0F] = standard_map
    out[0x10] = d_map_provider
    out[0x11] = d_map
    out[0x12] = ord(variant[0]) - ord("A") + 1
    digits = tuple(int(ch, 16) for ch in variant[1:])
    out[0x13] = (digits[0] << 4) | digits[1]
    out[0x14] = (digits[2] << 4) | (out[0x14] & 0x0F)
    return bytes(out)


def load_status_info(
    source: Path,
    *,
    eeprom_base: int | None = None,
    region_linear_base: int | None = None,
) -> tuple[str, Block, StatusInfo]:
    kind, blocks = load_eeprom_source(
        source, base=eeprom_base, region_linear_base=region_linear_base
    )
    if STATUS_INFO_BLOCK not in blocks:
        raise ValueError("EEPROM block 5005 is missing")
    block = blocks[STATUS_INFO_BLOCK]
    return kind, block, decode_status_info(block.payload)


def replace_status_info_payload(source: Path, output: Path, replacement: bytes) -> Block:
    """Copy a dump while replacing only its resolved active block-5005 payload."""
    _validate_payload(replacement)
    kind, blocks = load_eeprom_source(source)
    if STATUS_INFO_BLOCK not in blocks:
        raise ValueError("EEPROM block 5005 is missing")
    block = blocks[STATUS_INFO_BLOCK]
    if kind != "dump" or block.file_off is None:
        raise ValueError("block 5005 replacement requires an initialized binary dump")
    if block.length != STATUS_INFO_LENGTH:
        _validate_payload(block.payload)
    raw = bytearray(source.read_bytes())
    raw[block.file_off:block.file_off + STATUS_INFO_LENGTH] = replacement
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(raw)
    return block


def status_info_report(source: Path, kind: str, block: Block, info: StatusInfo) -> dict:
    payload = block.payload
    return {
        "source": str(source),
        "kind": kind,
        "block": {
            "id": block.block_id,
            "length": block.length,
            "linear": block.linear,
            "file_offset": block.file_off,
            "directory_offset": block.dir_off,
            "memory_class": block.memory_class,
            "version": block.version,
            "marker": block.marker,
        },
        "validation": {
            "length_valid": len(payload) == STATUS_INFO_LENGTH,
            "production_date_valid": info.production_date is not None,
            "standard_map_sw_valid": (
                info.standard_map is not None and info.standard_sw is not None
            ),
            "d_map_provider_valid": (
                info.d_map is not None and info.d_map_provider is not None
            ),
            "variant_letter_valid": 1 <= info.variant_letter_index <= 25,
            "variant_suffix_decimal": all(n <= 9 for n in info.variant_suffix_nibbles),
        },
        "fields": info.as_dict(),
        "raw_hex": payload.hex(),
        "consumed_range": {"start": 0x0B, "end_exclusive": 0x15,
                           "raw_hex": payload[0x0B:0x15].hex()},
        "opaque_ranges": [
            {"start": 0, "end_exclusive": 0x0B, "raw_hex": payload[:0x0B].hex()},
            {"start": 0x15, "end_exclusive": STATUS_INFO_LENGTH,
             "raw_hex": payload[0x15:].hex()},
        ],
        "ignored_within_consumed_range": [
            {"offset": 0x0D, "raw": payload[0x0D]},
            {"offset": 0x14, "mask": 0x0F, "raw": payload[0x14] & 0x0F},
        ],
    }
