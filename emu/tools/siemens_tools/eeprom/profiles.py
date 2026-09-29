"""Named EEPROM profiles for reproducible firmware assembly."""

from __future__ import annotations

from dataclasses import dataclass

from .battery import (
    CEMU_DEFAULT_BATTERY_CALIBRATION,
    encode_battery_calibration,
)


C55_EMULATOR_MINIMAL_V1 = "c55-emulator-minimal-v1"
M55_EMULATOR_MINIMAL_V1 = "m55-emulator-minimal-v1"
A55_EMULATOR_MINIMAL_V1 = "a55-emulator-minimal-v1"
A52_EMULATOR_MINIMAL_V1 = "a52-emulator-minimal-v1"
MC60_EMULATOR_MINIMAL_V1 = "mc60-emulator-minimal-v1"
CF62_EMULATOR_MINIMAL_V1 = "cf62-emulator-minimal-v1"
A60_EMULATOR_MINIMAL_V1 = "a60-emulator-minimal-v1"
A62_EMULATOR_MINIMAL_V1 = "a62-emulator-minimal-v1"
A65_EMULATOR_MINIMAL_V1 = "a65-emulator-minimal-v1"
C60_EMULATOR_MINIMAL_V1 = "c60-emulator-minimal-v1"
SL55_EMULATOR_MINIMAL_V1 = "sl55-emulator-minimal-v1"
S55_EMULATOR_MINIMAL_V1 = "s55-emulator-minimal-v1"


@dataclass(frozen=True)
class ProfileRecord:
    block_id: int
    payload: bytes
    memory_class: int
    version: int
    payload_kind: str
    observed_bytes: int
    provenance: str


@dataclass(frozen=True)
class EepromProfile:
    name: str
    product: int
    software_versions: tuple[int, ...]
    ui_boot_qualified_software_versions: tuple[int, ...]
    intended_use: str
    warning: str
    records: tuple[ProfileRecord, ...]
    omitted_blocks: tuple[int, ...]
    donor_blocks: tuple[int, ...] = ()
    donor_sha256: str | None = None
    map_sha256: str | None = None
    compatible_map_sha256s: tuple[str, ...] = ()
    compatible_map_software_versions: tuple[tuple[int, int], ...] = ()

    @property
    def observed_payload_bytes(self) -> int:
        return sum(record.observed_bytes for record in self.records)


_DUMP_ONLY_ZERO_PROVENANCE = (
    "same-device donor shape; dump-only versus pinned Standard map; "
    "payload neutralized for manual qualification"
)
_CEMU_BATTERY_PROVENANCE = (
    "CEMU battery calibration v1; emulator calibration policy matching the "
    "shared host voltage curve, not observed target bytes"
)
_CEMU_BATTERY_RECORD = ProfileRecord(
    67,
    encode_battery_calibration(CEMU_DEFAULT_BATTERY_CALIBRATION),
    2,
    2,
    "synthesized-cemu-battery-v1",
    0,
    _CEMU_BATTERY_PROVENANCE,
)
_COMMON_DONOR_BLOCKS = (5005, 5006)

# Record geometry is shared only where the qualified donors agree.  Block 55
# and block 5123 have two observed shapes and therefore remain family-specific.
_COMMON_ZERO_SHAPES: dict[int, tuple[int, int, int]] = {
    1: (348, 2, 1),
    2: (348, 2, 1),
    75: (46, 2, 0),
    167: (348, 2, 1),
    5002: (136, 8, 0),
    **{block_id: (300, 8, 0) for block_id in range(5047, 5057)},
    5057: (180, 8, 1),
    5121: (56, 8, 0),
    5122: (6, 8, 0),
    5165: (60, 8, 0),
    5180: (250, 8, 1),
    5181: (250, 8, 1),
    5223: (1000, 8, 0),
    **{block_id: (234, 8, 1) for block_id in range(5244, 5249)},
    **{block_id: (32, 8, 1) for block_id in range(5255, 5260)},
    5351: (4, 8, 0),
    5352: (564, 8, 0),
    5372: (30, 8, 0),
    5385: (2700, 8, 0),
    5395: (6, 8, 0),
    5436: (20, 8, 1),
    5437: (200, 8, 1),
    5439: (92, 8, 1),
}
_C55_FAMILY_SHAPES = {
    **_COMMON_ZERO_SHAPES,
    55: (14, 2, 0),
    57: (72, 2, 10),
    144: (72, 2, 10),
    5123: (14, 8, 0),
}
_M55_FAMILY_SHAPES = {
    **_COMMON_ZERO_SHAPES,
    55: (24, 2, 12),
    5123: (12, 8, 0),
}
_S55_SHAPES = {**_M55_FAMILY_SHAPES, 55: (14, 2, 0)}


def _zero_records(
    block_ids: tuple[int, ...],
    shapes: dict[int, tuple[int, int, int]],
) -> tuple[ProfileRecord, ...]:
    records = []
    for block_id in block_ids:
        length, memory_class, version = shapes[block_id]
        records.append(ProfileRecord(
            block_id,
            bytes(length),
            memory_class,
            version,
            "synthesized-zero",
            0,
            _DUMP_ONLY_ZERO_PROVENANCE,
        ))
    return tuple(records)


def _profile_records(
    block_ids: tuple[int, ...],
    shapes: dict[int, tuple[int, int, int]],
) -> tuple[ProfileRecord, ...]:
    return tuple(sorted(
        (*_zero_records(block_ids, shapes), _CEMU_BATTERY_RECORD),
        key=lambda record: record.block_id,
    ))


C55_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=C55_EMULATOR_MINIMAL_V1,
    product=130,
    software_versions=(24, 85),
    ui_boot_qualified_software_versions=(24,),
    intended_use="emulator-only C55 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile c55-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for C55 SW24 passed manual stable-GUI validation; analog/RF "
        "calibration and physical-hardware safety are not validated"
    ),
    records=_profile_records((1, 2, 55, 75, 5121, 5122, 5123), _C55_FAMILY_SHAPES),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="bcb893b6b3ae94031f319e2d387e34679055c02e9cd6024d218a514f09954d75",
    map_sha256="d1f1369ffebf573c0a67cc9dbe1652e9f91aa32db1630d3bd56b42c587f0bf23",
    compatible_map_sha256s=(
        "e54531347ae129c05e848c14e03486bf464be5f8788b91b51cfda09a8d1880ef",
    ),
)

M55_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=M55_EMULATOR_MINIMAL_V1,
    product=86,
    software_versions=(10, 11, 91),
    ui_boot_qualified_software_versions=(91,),
    intended_use="emulator-only M55 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile m55-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for M55 SW91 passed manual stable-GUI validation; analog/RF "
        "calibration and physical-hardware safety are not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 75, 167, 5002, 5121, 5122, 5123, 5352),
        _M55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="d274a6cb85651e5ad846a157bc6c18373f0f2db784c88a0b11e59f563975b26d",
    map_sha256="2377acb70713c4478d7fe826284c23b62d58e05d52f156bfd2a231de3be3fd33",
    compatible_map_sha256s=(
        "4b02cea8808c96266510843b870b49fa3b3f92c84f778130f5a6ffeb0ea943ca",
        "5ff573ebb34aced2744b0352d57b2d87adf36096f885c7128c4dfa19192c091b",
    ),
)

A55_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=A55_EMULATOR_MINIMAL_V1,
    product=196,
    software_versions=(9, 11),
    ui_boot_qualified_software_versions=(9,),
    intended_use="emulator-only A55 SW09 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile a55-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for A55 SW09 passed manual stable-GUI validation; analog/RF "
        "calibration and physical-hardware safety are not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 57, 75, 144, 5121, 5122, 5123, 5180, 5181,
         5255, 5256, 5257, 5258, 5259, 5372),
        _C55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="7e7b1eb018dc2eb26dd41fda3565fab0f12d368a833825c0b189bb42394afe61",
    map_sha256="bc5cf567184113db474ab051301e47c62f2554c434c428de2b82dd45770ac784",
    compatible_map_sha256s=(
        "2f88e272baaa11ffb37b5984df11788303fdf21075e55dae4bba839f47b5f04c",
    ),
)

A52_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=A52_EMULATOR_MINIMAL_V1,
    product=226,
    software_versions=(9,),
    ui_boot_qualified_software_versions=(9,),
    intended_use="emulator-only A52 SW09 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile a52-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for A52 SW09 passed manual stable-GUI validation; analog/RF "
        "calibration and physical-hardware safety are not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 57, 75, 144, 5121, 5122, 5123, 5180, 5181,
         5255, 5256, 5257, 5258, 5259, 5372),
        _C55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="1aeb0d3e0329715dbb7ffc57829c14aa7af91fbfd59399e6ec3915d461815a35",
    map_sha256="7c1c2f17480fd4e739295555e7aeaf2e492d35e11ac06e3a874c5cf02afacc66",
)

MC60_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=MC60_EMULATOR_MINIMAL_V1,
    product=132,
    software_versions=(10, 13),
    ui_boot_qualified_software_versions=(13,),
    intended_use="emulator-only MC60 SW13 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile mc60-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for MC60 SW13 passed manual stable-GUI validation; "
        "physical-hardware safety is not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 75, 167, 5057, 5121, 5122, 5123,
         5244, 5245, 5246, 5247, 5248, 5352),
        _M55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="5b50aefc95b6e9730439d929ea3c1f1a08d516f7a1d9be070e5c81cdd7d6010f",
    map_sha256="37a2ab5746bcab4f6246ac97371710431e8d7a786beddc9cdaa05f22af291993",
    compatible_map_sha256s=(
        "7f18448bbcfeba0834a65f0af9f59276ef5aa7812e55be511fe0e83fc6fbe994",
    ),
)

CF62_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=CF62_EMULATOR_MINIMAL_V1,
    product=228,
    software_versions=(7, 24, 95),
    ui_boot_qualified_software_versions=(7,),
    intended_use="emulator-only CF62 SW07 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile cf62-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for CF62 SW07 passed manual stable-GUI validation; "
        "physical-hardware safety is not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 75, 167, 5121, 5122, 5123, 5223,
         5244, 5245, 5246, 5247, 5248, 5352, 5385, 5395),
        _M55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="a3492d48cb065d3102bc0128e7721b8e6d826d5c7e5b1955b46ae14de4c8aab3",
    map_sha256="1db761f099f49273a07271b6a6019fb960b1c9192227369e589f9bba0c82b8d8",
    compatible_map_sha256s=(
        "1cd36c1416bd549061f8b51fc100b1343d1297ce023f4e3dba31e18826b3fabb",
    ),
    compatible_map_software_versions=((24, 95),),
)

A60_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=A60_EMULATOR_MINIMAL_V1,
    product=39,
    software_versions=(27,),
    ui_boot_qualified_software_versions=(27,),
    intended_use="emulator-only A60 SW27 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile a60-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for A60 SW27 passed manual stable-GUI validation; "
        "physical-hardware safety is not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 75, 167, 5057, 5121, 5122, 5123, 5165, 5351, 5352),
        _M55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="ccd8459c2c0af4dc30c323c4135ab36d00b71918e5d3304e284c4b70bb14a20b",
    map_sha256="09f955bc3e3338c4dcbd11fc676cf01ad1dcc822d58f18858779a8b896ab3de6",
)

A62_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=A62_EMULATOR_MINIMAL_V1,
    product=231,
    software_versions=(6, 7),
    ui_boot_qualified_software_versions=(7,),
    intended_use="emulator-only A62 SW07 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile a62-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for A62 SW07 passed manual stable-GUI validation; "
        "physical-hardware safety is not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 75, 167, 5057, 5121, 5122, 5123, 5165,
         5351, 5352, 5436, 5437, 5439),
        _M55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="ed4e093ea7d71e154c0420917b3ff67d66913a9733d968500c87a1f8c79cb677",
    map_sha256="03c0fcf3cad47adac60a4d56915232198e0ae03bbae985545071c195b5e02acc",
    compatible_map_sha256s=(
        "d81f84b952950fa290533f8029b2d3d2af7a0ab1eb0f7fc94c53d7e02bdc1d14",
    ),
)

A65_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=A65_EMULATOR_MINIMAL_V1,
    product=230,
    software_versions=(15, 17, 62),
    ui_boot_qualified_software_versions=(15,),
    intended_use="emulator-only A65 SW15 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile a65-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for A65 SW15 passed manual stable-GUI validation; "
        "physical-hardware safety is not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 75, 167, 5057, 5121, 5122, 5123, 5165, 5351, 5352),
        _M55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=(306, 5005, 5006, 5436, 5437),
    donor_sha256="cead2ffd5f221d198d8094527a815290340b0d59fb0c951a33fe66315e342cf7",
    map_sha256="636b9b55f0c211dd5aa97fbb651caccdf539d5d365c96d3093f0c110f09e4553",
    compatible_map_sha256s=(
        "b0e086e25f3212e9de774d98fc0a3c92f7f8294b6ad91b19fcd7401a37de8ed8",
        "d4ed88e8755adb60bddd85f8b5879e2b092194f3c7071a9fcea056802adadca8",
    ),
)

C60_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=C60_EMULATOR_MINIMAL_V1,
    product=40,
    software_versions=(20, 27),
    ui_boot_qualified_software_versions=(20,),
    intended_use="emulator-only C60 SW20 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile c60-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for C60 SW20 passed manual stable-GUI validation; "
        "physical-hardware safety is not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 75, 167, 5047, 5048, 5049, 5050, 5051, 5052,
         5053, 5054, 5055, 5056, 5057, 5121, 5122, 5123, 5165, 5351, 5352),
        _M55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="c600586e982a14455279c260fd1d553ea473ae5c58acdc04f6b9187eb13a0568",
    map_sha256="ab5a432658dd37f01caeea8d507e49f0dddca4054177904b29e32ffec62afa0b",
    compatible_map_sha256s=(
        "35e253d881c86f2712cb2afeb0b0844a237791281892e2d9161e234baeb22344",
    ),
)

SL55_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=SL55_EMULATOR_MINIMAL_V1,
    product=36,
    software_versions=(7, 20),
    ui_boot_qualified_software_versions=(7,),
    intended_use="emulator-only SL55 SW07 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile sl55-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for SL55 SW07 passed manual stable-GUI validation; "
        "physical-hardware safety is not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 75, 167, 5002, 5121, 5122, 5123, 5352),
        _M55_FAMILY_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="f924f6de23a81447f3c7bf11b5b9596836e8fcb9a5bc943617480ead0adfb8dc",
    map_sha256="dc2a61be27e7182a9bf604f8c98dbc5a5cb6177dfb68424a64c16c92cdfe502b",
    compatible_map_sha256s=(
        "456111dc28e2ff2f43f918ebaabbcc155042ed40062fa383c4ea05c821a3e359",
    ),
)

S55_EMULATOR_MINIMAL_PROFILE = EepromProfile(
    name=S55_EMULATOR_MINIMAL_V1,
    product=84,
    software_versions=(20, 91),
    ui_boot_qualified_software_versions=(91,),
    intended_use="emulator-only S55 SW91 EEPROM reconstruction experiments",
    warning=(
        "EEPROM profile s55-emulator-minimal-v1 is emulator-only; this dump-only "
        "zero profile for S55 SW91 passed manual stable-GUI validation; "
        "physical-hardware safety is not validated"
    ),
    records=_profile_records(
        (1, 2, 55, 75, 167, 5002, 5121, 5122, 5123),
        _S55_SHAPES,
    ),
    omitted_blocks=(),
    donor_blocks=_COMMON_DONOR_BLOCKS,
    donor_sha256="0d48bdf90435520a3957116f35864db24ae1fb3d63209c3bac78ad2ec6ee36b7",
    map_sha256="c08ea9ad4aa603ff4974a8a82ab486a8fd07f63461100afb256b90eafc2d3e17",
    compatible_map_sha256s=(
        "c466ef00d5dd9bc037e4f958a8964c7e2e3eb75ca9fd58f49a24c56253e2681b",
    ),
)

EEPROM_PROFILES = {
    A52_EMULATOR_MINIMAL_PROFILE.name: A52_EMULATOR_MINIMAL_PROFILE,
    A55_EMULATOR_MINIMAL_PROFILE.name: A55_EMULATOR_MINIMAL_PROFILE,
    C55_EMULATOR_MINIMAL_PROFILE.name: C55_EMULATOR_MINIMAL_PROFILE,
    M55_EMULATOR_MINIMAL_PROFILE.name: M55_EMULATOR_MINIMAL_PROFILE,
    MC60_EMULATOR_MINIMAL_PROFILE.name: MC60_EMULATOR_MINIMAL_PROFILE,
    CF62_EMULATOR_MINIMAL_PROFILE.name: CF62_EMULATOR_MINIMAL_PROFILE,
    A60_EMULATOR_MINIMAL_PROFILE.name: A60_EMULATOR_MINIMAL_PROFILE,
    A62_EMULATOR_MINIMAL_PROFILE.name: A62_EMULATOR_MINIMAL_PROFILE,
    A65_EMULATOR_MINIMAL_PROFILE.name: A65_EMULATOR_MINIMAL_PROFILE,
    C60_EMULATOR_MINIMAL_PROFILE.name: C60_EMULATOR_MINIMAL_PROFILE,
    SL55_EMULATOR_MINIMAL_PROFILE.name: SL55_EMULATOR_MINIMAL_PROFILE,
    S55_EMULATOR_MINIMAL_PROFILE.name: S55_EMULATOR_MINIMAL_PROFILE,
}


def get_eeprom_profile(name: str) -> EepromProfile:
    try:
        return EEPROM_PROFILES[name]
    except KeyError as exc:
        available = ", ".join(sorted(EEPROM_PROFILES))
        raise ValueError(
            f"unknown EEPROM profile {name!r}; available: {available}"
        ) from exc
