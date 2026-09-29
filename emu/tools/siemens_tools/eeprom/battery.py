from __future__ import annotations

import struct
from dataclasses import asdict, dataclass, replace
from pathlib import Path

from .storage import Block, load_eeprom_source


BATTERY_CALIBRATION_BLOCK = 67
BATTERY_CALIBRATION_LENGTH = 20


@dataclass(frozen=True)
class AffineCorrection:
    scale_percent: int
    offset: int

    def apply(self, raw: int) -> int:
        product = raw * self.scale_percent
        scaled = product // 100 if product >= 0 else -((-product) // 100)
        return scaled - self.offset


@dataclass(frozen=True)
class BatteryCalibration:
    low_raw: int
    low_mv: int
    high_raw: int
    high_mv: int
    tbat: AffineCorrection
    tenv: AffineCorrection
    reserved: AffineCorrection

    @property
    def span_mv(self) -> int:
        return self.high_mv - self.low_mv

    def as_dict(self) -> dict[str, object]:
        result = asdict(self)
        result["span_mv"] = self.span_mv
        return result


CEMU_DEFAULT_BATTERY_CALIBRATION = BatteryCalibration(
    low_raw=4499,
    low_mv=3177,
    high_raw=-1628,
    high_mv=4178,
    tbat=AffineCorrection(scale_percent=98, offset=-343),
    tenv=AffineCorrection(scale_percent=98, offset=-217),
    reserved=AffineCorrection(scale_percent=100, offset=0),
)


def _validate_battery_calibration(calibration: BatteryCalibration) -> None:
    signed = (
        ("low raw", calibration.low_raw),
        ("high raw", calibration.high_raw),
        ("TBAT scale", calibration.tbat.scale_percent),
        ("TBAT offset", calibration.tbat.offset),
        ("TENV scale", calibration.tenv.scale_percent),
        ("TENV offset", calibration.tenv.offset),
        ("reserved scale", calibration.reserved.scale_percent),
        ("reserved offset", calibration.reserved.offset),
    )
    for name, value in signed:
        if not -32768 <= value <= 32767:
            raise ValueError(f"EEPROM block 67 {name} must fit a signed 16-bit value")
    if calibration.low_raw <= calibration.high_raw:
        raise ValueError("EEPROM block 67 ADC endpoints are not monotonic")
    if not 0 < calibration.low_mv < calibration.high_mv <= 0xFFFF:
        raise ValueError("EEPROM block 67 voltage endpoints are invalid")


def encode_battery_calibration(calibration: BatteryCalibration) -> bytes:
    _validate_battery_calibration(calibration)
    return struct.pack(
        "<hHhHhhhhhh",
        calibration.low_raw,
        calibration.high_mv,
        calibration.high_raw,
        calibration.span_mv,
        calibration.tbat.scale_percent,
        calibration.tbat.offset,
        calibration.tenv.scale_percent,
        calibration.tenv.offset,
        calibration.reserved.scale_percent,
        calibration.reserved.offset,
    )


def decode_battery_calibration(payload: bytes) -> BatteryCalibration:
    if len(payload) != BATTERY_CALIBRATION_LENGTH:
        raise ValueError(
            f"EEPROM block 67 length is {len(payload)}, expected "
            f"{BATTERY_CALIBRATION_LENGTH}"
        )
    values = struct.unpack("<hHhHhhhhhh", payload)
    calibration = BatteryCalibration(
        low_raw=values[0],
        low_mv=values[1] - values[3],
        high_raw=values[2],
        high_mv=values[1],
        tbat=AffineCorrection(values[4], values[5]),
        tenv=AffineCorrection(values[6], values[7]),
        reserved=AffineCorrection(values[8], values[9]),
    )
    _validate_battery_calibration(calibration)
    return calibration


def load_battery_calibration(
    source: Path,
    *,
    eeprom_base: int | None = None,
    region_linear_base: int | None = None,
) -> tuple[str, Block, BatteryCalibration]:
    kind, blocks = load_eeprom_source(
        source, base=eeprom_base, region_linear_base=region_linear_base
    )
    if BATTERY_CALIBRATION_BLOCK not in blocks:
        raise ValueError("EEPROM block 67 is missing")
    block = blocks[BATTERY_CALIBRATION_BLOCK]
    return kind, block, decode_battery_calibration(block.payload)


def synthesize_battery_calibration(
    source: Path,
    output: Path,
    *,
    low_raw: int,
    low_mv: int,
    high_raw: int,
    high_mv: int,
    eeprom_base: int | None = None,
    region_linear_base: int | None = None,
) -> tuple[Block, BatteryCalibration]:
    kind, block, existing = load_battery_calibration(
        source,
        eeprom_base=eeprom_base,
        region_linear_base=region_linear_base,
    )
    if kind != "dump" or block.file_off is None:
        raise ValueError(
            "battery synthesis requires an initialized binary dump template"
        )
    replacement = encode_battery_calibration(
        replace(
            existing,
            low_raw=low_raw,
            low_mv=low_mv,
            high_raw=high_raw,
            high_mv=high_mv,
        )
    )
    raw = bytearray(source.read_bytes())
    raw[block.file_off:block.file_off + block.length] = replacement
    output.write_bytes(raw)
    calibration = decode_battery_calibration(
        bytes(raw[block.file_off:block.file_off + block.length])
    )
    return block, calibration
