from __future__ import annotations

from typing import TYPE_CHECKING

from .security import (
    B5008_LEN,
    B5009_LEN,
    B5077_LEN,
    BUNDLE_PROFILE,
    BUNDLE_PROFILE_VERSION,
    BUNDLE_SCHEMA,
    BUNDLE_SCHEMA_VERSION,
    COD00_XOR_KEY_A50,
    CODE_TABLE04,
    CODE_TABLE05,
    MAIN_BODY_5008_TEMPLATE,
    MAIN_BODY_5077_TEMPLATE,
    MODEL_ID7,
    PARS_A50,
    SEC_MAINBODY5008_CRC_LEN,
    SEC_MAINBODY5077_CRC_LEN,
    STRANGE_HEADER_LEN,
    STRANGE_HEADER_TEMPLATE,
    convert_to_bcd,
    encrypt_5008,
    encrypt_5077,
    validate_imei,
)

if TYPE_CHECKING:
    from .battery import BatteryCalibration


def crc_check_bytes(buf: bytes, length: int) -> tuple[int, int]:
    """Port of SEC.C:870 CRCBuffer: 8-bit running sum, then running XOR, over length bytes."""
    s = 0
    x = 0
    for i in range(length):
        s = (s + buf[i]) & 0xFF
        x ^= buf[i]
    return s, x


def verify_checks(dec5008: bytes, dec5077: bytes) -> list[tuple[str, int, int]]:
    """Recompute the three DD2476 sum/XOR pairs from decrypted plaintext.

    Region 1: 5008 StrangeHeader (offset 8, len 0x16 = header minus its 2 check bytes).
    Region 2: 5008 MainBody (offset 8+0x18+8 = 0x28, len 0xB0).
    Region 3: 5077 MainBody (offset 8, len 0xD8).
    """
    h = STRANGE_HEADER_LEN
    header = dec5008[8:8 + h]
    body5008 = dec5008[h + 16:]
    body5077 = dec5077[8:]
    return [
        ("5008.header", *crc_check_bytes(header, h - 2)),
        ("5008.body", *crc_check_bytes(body5008, SEC_MAINBODY5008_CRC_LEN)),
        ("5077.body", *crc_check_bytes(body5077, SEC_MAINBODY5077_CRC_LEN)),
    ]


def get_code_table(idx_hi: int, idx_lo: int) -> int:
    """Port of the A50 branch of SEC.C:2897 GetCodeTable (CodeTable04/05)."""
    return ((CODE_TABLE04[idx_hi & 0x0F] << 4) | CODE_TABLE05[idx_lo & 0x0F]) & 0xFF


def do_tricky(vls: int, arr: bytearray) -> None:
    """Port of SEC.C DoSomeTrickyCalculations — one forward scramble round."""
    half = B5009_LEN // 2
    src = bytes(arr)
    for i in range(half):
        b = arr[i]
        val = get_code_table((b >> 4) & 0x0F, b & 0x0F)
        if not (i & 1):
            arr[i] = (val ^ vls ^ src[i + half]) & 0xFF
        else:
            arr[i] = ((~val & 0xFF) ^ vls ^ src[i + half]) & 0xFF
        arr[i + half] = src[i]


def imei_check_digit(imei: str) -> int:
    """Freia DoLikeProcedure0323 result (the standard IMEI/Luhn check digit)."""
    validate_imei(imei)
    total = 0
    for i, char in enumerate(imei):
        value = int(char) * (2 if i & 1 else 1)
        total += value // 10 + value % 10
    return (-total) % 10


def create_imei_records(imei: str,
                        pars: list[int] = PARS_A50) -> tuple[bytes, bytes]:
    """Port SEC_CreateBlocks5009And76 for the model-ID-7 A50/C55 path."""
    validate_imei(imei)
    common = bytearray(10)
    for i in range(0, 14, 2):
        common[i // 2] = int(imei[i]) | (int(imei[i + 1]) << 4)
    common[7] = imei_check_digit(imei) << 4

    bp0a = 0
    bp0b = 0
    for i, value in enumerate(common[:8]):
        if i & 1:
            bp0a ^= value
        else:
            bp0b ^= value
    bp0c = (~(bp0a ^ bp0b)) & 0xFF

    b5009 = bytearray(common)
    b76 = bytearray(common)
    b5009[8], b5009[9] = bp0a, bp0c
    b76[8], b76[9] = bp0b, bp0c
    for i, value in enumerate(pars):
        do_tricky(value, b5009 if not (i & 1) else b76)
    return bytes(b5009), bytes(b76)


def create_unlocked_plaintexts() -> tuple[bytes, bytes]:
    """Build Freia 15's deterministic unlocked model-ID-7 5008/5077 plaintext."""
    header = bytearray(STRANGE_HEADER_TEMPLATE)
    body5008 = bytearray(MAIN_BODY_5008_TEMPLATE)
    body5077 = bytearray(MAIN_BODY_5077_TEMPLATE)
    if (len(header), len(body5008), len(body5077)) != (24, 184, 224):
        raise AssertionError("Freia template geometry changed")
    header[-2:] = bytes(crc_check_bytes(header, len(header) - 2))
    body5008[SEC_MAINBODY5008_CRC_LEN:SEC_MAINBODY5008_CRC_LEN + 2] = bytes(
        crc_check_bytes(body5008, SEC_MAINBODY5008_CRC_LEN)
    )
    body5077[SEC_MAINBODY5077_CRC_LEN:SEC_MAINBODY5077_CRC_LEN + 2] = bytes(
        crc_check_bytes(body5077, SEC_MAINBODY5077_CRC_LEN)
    )
    plain5008 = COD00_XOR_KEY_A50 + header + COD00_XOR_KEY_A50 + body5008
    plain5077 = COD00_XOR_KEY_A50 + body5077
    if len(plain5008) != B5008_LEN or len(plain5077) != B5077_LEN:
        raise AssertionError("generated security block geometry is invalid")
    return plain5008, plain5077


def generate_identity_bundle(
    imei: str,
    fsn: int,
) -> dict:
    """Return the four deterministic model-ID-7 identity records."""
    validate_imei(imei)
    if not 0 <= fsn <= 0xFFFFFFFF:
        raise ValueError(f"FSN must fit in 32 bits, got {fsn!r}")
    b5009, b76 = create_imei_records(imei)
    plain5008, plain5077 = create_unlocked_plaintexts()
    bcd = convert_to_bcd(imei)
    b5008 = bytes(encrypt_5008(plain5008, fsn, bcd))
    b5077 = bytes(encrypt_5077(plain5077, fsn, bcd))
    return {
        "schema": BUNDLE_SCHEMA,
        "schema_version": BUNDLE_SCHEMA_VERSION,
        "profile": BUNDLE_PROFILE,
        "profile_version": BUNDLE_PROFILE_VERSION,
        "crypto_model_id": MODEL_ID7,
        "imei": imei,
        "fsn": f"{fsn:08X}",
        "blocks": {
            "76": b76.hex().upper(),
            "5008": b5008.hex().upper(),
            "5009": b5009.hex().upper(),
            "5077": b5077.hex().upper(),
        },
    }


def generate_overlay_bundle(
    imei: str,
    fsn: int,
    battery_calibration: BatteryCalibration | None = None,
) -> dict:
    """Return a runtime overlay with identity and battery-calibration records."""
    from .battery import (
        CEMU_DEFAULT_BATTERY_CALIBRATION,
        encode_battery_calibration,
    )

    if battery_calibration is None:
        battery_calibration = CEMU_DEFAULT_BATTERY_CALIBRATION
    bundle = generate_identity_bundle(imei, fsn)
    bundle["blocks"] = {
        "67": encode_battery_calibration(battery_calibration).hex().upper(),
        **bundle["blocks"],
    }
    return bundle


def inv_tricky(vls: int, arr: bytearray) -> None:
    """Port of SEC.C:2937 InvDoSomeTrickyCalculations — one inverse scramble round.

    Operates in place on a 10-byte block; uses only the model S-box, so it needs
    no FSN. Reverses one round of the 5009/0001 obfuscation.
    """
    half = B5009_LEN // 2
    src = bytes(arr)
    for i in range(half):
        b = arr[i + half]
        val = get_code_table((b >> 4) & 0x0F, b & 0x0F)
        if not (i & 1):
            arr[i + half] = (val ^ vls ^ src[i]) & 0xFF
        else:
            arr[i + half] = ((~val & 0xFF) ^ vls ^ src[i]) & 0xFF
        arr[i] = src[i + half]


def recreate_imei(b5009: bytes, pars: list[int] = PARS_A50) -> tuple[str, bool]:
    """Port of SEC.C:3122 SEC_RecreateIMEI (non-C30 branch) -> (imei, is_empty).

    Runs the even-round inverse scramble over the 10-byte 5009 block, then decodes
    the BCD IMEI (SEC_BCDIMEIToNormalIMEI, SEC.C). A55 and C55 both use Pars[7].
    Returns ("", True) when the block is blank (all 0xFF).
    """
    buf = bytearray(b5009)
    for i in range(7, -1, -1):
        if not (i & 1):
            inv_tricky(pars[i], buf)
    return bcd_imei_to_normal(bytes(buf))


def recreate_imei_companion(b76: bytes,
                            pars: list[int] = PARS_A50) -> tuple[str, bool]:
    """Decode block 76, the odd-round companion to block 5009.

    SEC_RecreateBlocks5009And76 applies inverse even rounds to 5009 and inverse
    odd rounds to 76. Both recover the same BCD IMEI bytes, but their byte 8
    checks differ (BP0A versus BP0B).
    """
    buf = bytearray(b76)
    for i in range(7, -1, -1):
        if i & 1:
            inv_tricky(pars[i], buf)
    return bcd_imei_to_normal(bytes(buf))


def bcd_imei_to_normal(b5009: bytes) -> tuple[str, bool]:
    """Port of SEC.C SEC_BCDIMEIToNormalIMEI: 7 BCD bytes -> 14-digit IMEI.

    An all-0xFF (sum == 7*255) block means "empty". Low nibble is the earlier
    digit of each pair. Raises ValueError on a non-decimal nibble.
    """
    if sum(b5009[:7]) == 7 * 255:
        return "", True
    digits = []
    for i in range(7):
        lo = b5009[i] & 0x0F
        hi = (b5009[i] >> 4) & 0x0F
        if lo > 9 or hi > 9:
            raise ValueError(f"invalid decoded IMEI nibble at byte {i}: {b5009[i]:#04x}")
        digits.append(str(lo))
        digits.append(str(hi))
    return "".join(digits), False


def hex_dump(data: bytes, base: int = 0) -> str:
    lines = []
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        asciipart = "".join(chr(b) if 0x20 <= b <= 0x7E else "." for b in chunk)
        lines.append(f"{base + off:08x}: {hexpart:<47}  {asciipart}")
    return "\n".join(lines)
