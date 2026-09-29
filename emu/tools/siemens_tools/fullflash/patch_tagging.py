from __future__ import annotations

import copy
import hashlib
import json
import os
import re
import zipfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

from .catalog_backing import validate_official_backing
from .corpus import SCHEMA as COMMUNITY_SCHEMA
from .corpus import SCHEMA_VERSION as COMMUNITY_VERSION
from .corpus import _atomic_output
from .corpus_scope import atomic_relative_symlink, payload_paths
from .official_corpus import SCHEMA as OFFICIAL_SCHEMA
from .official_corpus import SCHEMA_VERSION as OFFICIAL_VERSION
from .official_corpus import RECIPE_SCHEMA as OFFICIAL_RECIPE_SCHEMA
from .official_corpus import RECIPE_SCHEMA_VERSION as OFFICIAL_RECIPE_VERSION
from .normalization import restore_normalization
from .reconstruct import SCHEMA as DUMP_RECIPE_SCHEMA
from .reconstruct import SCHEMA_VERSION as DUMP_RECIPE_VERSION
from ..firmware.xbi import FirmwareError


RECORD_RE = re.compile(
    r"^\s*([0-9A-Fa-f]{1,8})\s*:\s*"
    r"([0-9A-Fa-f]+)\s+([0-9A-Fa-f?Xx]+)(?:\s*;.*)?\s*$"
)
RECORD_PREFIX_RE = re.compile(r"^\s*[0-9A-Fa-f]{1,8}\s*:")
HEX_RE = re.compile(r"[0-9A-Fa-f]+")


@dataclass(frozen=True)
class VkpRecord:
    line: int
    address: int
    old: bytes
    new: tuple[int | None, ...]


@dataclass(frozen=True)
class PatchOperation:
    old_address: int
    new_address: int
    old: bytes
    new: tuple[frozenset[int], ...]
    require_destination_old: bool = False


@dataclass(frozen=True)
class PatchDefinition:
    model: str
    software_version: int
    patch_id: int
    member: str
    alternatives: tuple[tuple[int, tuple[int, ...]], ...] = ()
    override: str | None = None

    @property
    def archive_key(self) -> str:
        return f"{self.model}v{self.software_version}"

    @property
    def scoped_id(self) -> str:
        return f"{self.archive_key}/{self.patch_id}"


@dataclass(frozen=True)
class LoadedPatch:
    definition: PatchDefinition
    records: tuple[VkpRecord, ...]
    operations: tuple[PatchOperation, ...]
    title: str


@dataclass(frozen=True)
class PatchMatch:
    fullflash_sha256: str
    source_path: str
    patch: LoadedPatch
    official_packages: tuple[str, ...]


@dataclass(frozen=True)
class PatchRejection:
    fullflash_sha256: str
    source_path: str
    patch: LoadedPatch
    reason: str


@dataclass(frozen=True)
class PayloadMove:
    old_path: str
    new_path: str
    kind: str
    target_path: str | None


@dataclass(frozen=True)
class ReferenceChange:
    path: Path
    pointer: str
    old_value: str
    new_value: str


@dataclass
class PatchTagPlan:
    model: str
    dump_catalog_path: Path
    official_catalog_path: Path
    archive_path: Path
    matches: list[PatchMatch]
    rejections: list[PatchRejection]
    affected_fullflashes: list[str]
    analyses: list[dict[str, Any]]
    payload_moves: list[PayloadMove]
    reference_changes: list[ReferenceChange]
    dump_catalog: dict[str, Any]
    recipes: dict[Path, dict[str, Any]]
    ignored_archive_patches: int


def _definition(
    model: str,
    version: int,
    patch_id: int,
    filename: str,
    **kwargs: Any,
) -> PatchDefinition:
    return PatchDefinition(
        model, version, patch_id, f"{model}v{version}/{filename}", **kwargs,
    )


_DATE_SEPARATORS = (
    0x20, 0x2F, 0x2D, 0x2E, 0x2C, 0x3A, 0x3D,
    0x3E, 0x11, 0x23, 0x27, 0x2A, 0x24,
)
_DATE_CASE_OPTIONS = (
    (0x21A3EC, (0x2F, 0x4F)), (0x21A3F0, (0x24, 0x44)),
    (0x21A3F4, (0x1C, 0x3C)), (0x21A3F8, (0x10, 0x30)),
    (0x21A3FC, (0x1C, 0x3C)), (0x21A400, (0x18, 0x38)),
    (0x21A404, (0x18, 0x38)), (0x21A408, (0x10, 0x30)),
    (0x21A40C, (0x21, 0x41)), (0x21A410, (0x1E, 0x3E)),
    (0x21A414, (0x1D, 0x3D)), (0x21A418, (0x14, 0x34)),
)
_DEFINITIONS = (
    _definition("C55", 24, 16, "16-CRC_check_off_C55v24.vkp"),
    _definition(
        "C55", 24, 160,
        "160-Speaker_volume_check_off__55v24.vkp",
    ),
    _definition(
        "C55", 24, 2401,
        "2401-Enable_Developer_Menu.vkp",
    ),
    _definition(
        "C55", 24, 2406,
        "2406-Black_list_v1_1.vkp",
        override="c55-black-list-relocation",
    ),
    _definition(
        "C55", 24, 2514,
        "2514-Sending_of_Russian_Flash_SMS_from_p.vkp",
        alternatives=((0x0B6E56, (0x20, 0x21, 0x23, 0x24, 0x25, 0x3A, 0x3F)),),
    ),
    _definition(
        "C55", 24, 4301,
        "4301-Disable_Java_classes_verification.vkp",
    ),
    _definition("C55", 24, 5333, "5333-Font_by_LS_D.vkp"),
    _definition("S55", 20, 4763, "4763-Switch_On_by_Alarmclock.vkp"),
    _definition(
        "S55", 20, 4764,
        "4764-Change_keys_for_incoming_call.vkp",
    ),
    _definition(
        "S55", 20, 4771,
        "4771-Turn_GPRS_onoff_without_confirm.vkp",
    ),
    _definition("S55", 20, 4772, "4772-Remove_coma.vkp"),
    _definition(
        "S55", 20, 4774,
        "4774-Handsfree_no_confirmation.vkp",
    ),
    _definition(
        "S55", 20, 4777,
        "4777-Date_on_mainscreen_as_Mon_08_Dec.vkp",
        alternatives=(
            (0x21A350, _DATE_SEPARATORS),
            (0x21A38C, _DATE_SEPARATORS),
            *_DATE_CASE_OPTIONS,
        ),
    ),
    _definition(
        "S55", 20, 4781,
        "4781-Fix_line_select_bug.vkp",
    ),
    _definition(
        "S55", 20, 4784,
        "4784-Change_calculator_softkeys.vkp",
    ),
    _definition(
        "S55", 20, 4785,
        "4785-Changing_illumination_by_5_percent.vkp",
    ),
    _definition("S55", 20, 4790, "4790-Change_UpArrowAction.vkp"),
    _definition(
        "S55", 20, 4822,
        "4822-Change_long_up_arrow_action.vkp",
    ),
    _definition(
        "S55", 20, 4823,
        "4823-Replace_left_and_right_softkeys_v2.vkp",
    ),
    _definition(
        "S55", 20, 4832,
        "4832-Modify_____Right_SoftKey_from_View_.vkp",
    ),
    _definition("S55", 91, 1288, "1288-netmonitor.vkp"),
)


def _read_json(path: Path, description: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise FirmwareError(f"invalid {description} JSON {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise FirmwareError(f"{description} JSON is not an object: {path}")
    return value


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _safe_relative(value: object, description: str) -> str:
    if not isinstance(value, str):
        raise FirmwareError(f"{description} is not a path")
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or any(
        part in ("", ".", "..") for part in path.parts
    ):
        raise FirmwareError(f"unsafe {description}: {value!r}")
    return value


def parse_vkp(data: bytes, member: str = "<vkp>") -> tuple[VkpRecord, ...]:
    records: list[VkpRecord] = []
    for number, raw in enumerate(data.splitlines(), 1):
        line = raw.decode("latin-1")
        match = RECORD_RE.fullmatch(line)
        if match is None:
            if RECORD_PREFIX_RE.match(line):
                raise FirmwareError(
                    f"malformed VKP record {member}:{number}: {line.strip()!r}"
                )
            continue
        old_text, new_text = match.group(2), match.group(3)
        if len(old_text) != len(new_text) or len(old_text) % 2:
            raise FirmwareError(f"unequal or odd VKP bytes {member}:{number}")
        new: list[int | None] = []
        for offset in range(0, len(new_text), 2):
            byte = new_text[offset:offset + 2]
            if HEX_RE.fullmatch(byte):
                new.append(int(byte, 16))
            elif byte in ("??", "XX", "xx"):
                new.append(None)
            else:
                raise FirmwareError(
                    f"malformed VKP byte {member}:{number}: {byte!r}"
                )
        records.append(VkpRecord(
            number, int(match.group(1), 16),
            bytes.fromhex(old_text), tuple(new),
        ))
    if not records:
        raise FirmwareError(f"VKP patch has no active records: {member}")
    return tuple(records)


def _validate_definitions(
    definitions: Iterable[PatchDefinition],
) -> tuple[PatchDefinition, ...]:
    result = tuple(definitions)
    keys: set[tuple[str, int, int]] = set()
    for item in result:
        key = item.model, item.software_version, item.patch_id
        if key in keys:
            raise FirmwareError(f"duplicate curated patch definition: {key}")
        keys.add(key)
        if not item.member.startswith(f"{item.archive_key}/"):
            raise FirmwareError(f"curated patch member has wrong scope: {item.member}")
        _safe_relative(item.member, "curated patch member")
    return result


def _fixed_new(data: bytes) -> tuple[frozenset[int], ...]:
    return tuple(frozenset((value,)) for value in data)


def _operations(
    definition: PatchDefinition,
    records: tuple[VkpRecord, ...],
) -> tuple[PatchOperation, ...]:
    alternatives = {
        address: frozenset(values)
        for address, values in definition.alternatives
    }
    seen: set[int] = set()
    operations: list[PatchOperation] = []
    for record in records:
        choices: list[frozenset[int]] = []
        for offset, value in enumerate(record.new):
            address = record.address + offset
            allowed = alternatives.get(address)
            if allowed is not None:
                if not allowed or (value is not None and value not in allowed):
                    raise FirmwareError(
                        f"invalid alternative policy for {definition.member} "
                        f"at 0x{address:X}"
                    )
                choices.append(allowed)
                seen.add(address)
            elif value is None:
                raise FirmwareError(
                    f"unbounded configurable VKP record in "
                    f"{definition.member}:{record.line}"
                )
            else:
                choices.append(frozenset((value,)))
        operations.append(PatchOperation(
            record.address, record.address, record.old, tuple(choices)
        ))
    if set(alternatives) != seen:
        raise FirmwareError(
            f"alternative policy does not name VKP bytes in {definition.member}"
        )
    if definition.override is None:
        return tuple(operations)
    if definition.override != "c55-black-list-relocation":
        raise FirmwareError(f"unknown patch override: {definition.override}")
    if len(records) != 2:
        raise FirmwareError("C55 Black List override requires two records")
    body, hook = records
    body_bytes = bytes.fromhex(
        "46FC09002D0746FC0A002D068880F08CFA94E693FA84180DDB00"
    )
    if (
        body.address, body.old, bytes(value for value in body.new if value is not None),
        hook.address, hook.old, bytes(value for value in hook.new if value is not None),
    ) != (
        0x612092, b"\xFF" * 26, body_bytes,
        0x1493E2, bytes.fromhex("8880F08C"), bytes.fromhex("FAE19220"),
    ):
        raise FirmwareError("C55 Black List archive records changed")
    return (
        PatchOperation(
            0x0B6EC0, 0x0B6EC0, b"\xFF" * 26,
            _fixed_new(body_bytes),
        ),
        PatchOperation(
            0x1493E2, 0x1493E2, bytes.fromhex("8880F08C"),
            _fixed_new(bytes.fromhex("FA8BC06E")),
        ),
    )


def _load_patches(
    archive_path: Path,
    model: str,
    definitions: tuple[PatchDefinition, ...],
) -> tuple[list[LoadedPatch], int]:
    selected = [item for item in definitions if item.model == model]
    try:
        archive = zipfile.ZipFile(archive_path)
    except (OSError, zipfile.BadZipFile) as exc:
        raise FirmwareError(f"invalid patch archive {archive_path}: {exc}") from exc
    with archive:
        try:
            index = json.loads(archive.read("patches/index.json").decode("utf-8"))
        except (KeyError, UnicodeError, json.JSONDecodeError) as exc:
            raise FirmwareError(f"invalid patch archive index: {exc}") from exc
        if not isinstance(index, dict):
            raise FirmwareError("patch archive index is not an object")
        selected_keys = {
            (item.archive_key, str(item.patch_id)) for item in selected
        }
        ignored = 0
        for archive_key, entries in index.items():
            if not isinstance(entries, dict):
                raise FirmwareError(f"invalid patch index scope: {archive_key!r}")
            if archive_key.startswith(f"{model}v"):
                ignored += sum(
                    (archive_key, str(patch_id)) not in selected_keys
                    for patch_id in entries
                )
        result: list[LoadedPatch] = []
        for definition in selected:
            entries = index.get(definition.archive_key)
            entry = entries.get(str(definition.patch_id)) if isinstance(
                entries, dict
            ) else None
            if (
                not isinstance(entry, dict)
                or entry.get("id") != definition.patch_id
                or entry.get("model") != definition.archive_key
                or entry.get("file") != definition.member
            ):
                raise FirmwareError(
                    f"curated patch index mismatch: "
                    f"{definition.archive_key}/{definition.patch_id}"
                )
            member = f"patches/{definition.member}"
            try:
                records = parse_vkp(archive.read(member), member)
            except KeyError as exc:
                raise FirmwareError(f"curated patch member is missing: {member}") from exc
            titles = entry.get("title", {})
            title = titles.get("en", "") if isinstance(titles, dict) else ""
            if not isinstance(title, str):
                raise FirmwareError(f"invalid patch title: {member}")
            result.append(LoadedPatch(
                definition, records, _operations(definition, records), title
            ))
    return result, ignored


def _recipe_slice(
    recipe: dict[str, Any], address: int, size: int,
) -> dict[str, Any] | None:
    end = address + size
    for item in recipe.get("slices", []):
        span = item.get("source_range", {})
        if (
            isinstance(span, dict)
            and span.get("from", -1) <= address
            and span.get("to_exclusive", -1) >= end
        ):
            return item
    return None


def _occurrence(
    catalog: dict[str, Any], source_id: str, item: dict[str, Any],
) -> dict[str, Any] | None:
    return next((
        occurrence for occurrence in catalog.get("occurrences", [])
        if occurrence.get("source_id") == source_id
        and occurrence.get("role") == item.get("role")
        and occurrence.get("sha256") == item.get("sha256")
    ), None)


def _scope_fields(role: str) -> tuple[str, ...]:
    return (
        ("software_version",)
        if role in ("BCORE", "FFS(A)")
        else ("software_version", "langpack", "t9_version")
    )


def _scope_problem(
    catalog: dict[str, Any],
    source_id: str,
    recipe: dict[str, Any],
    operations: Iterable[PatchOperation],
) -> str | None:
    checked: set[tuple[str, str]] = set()
    for operation in operations:
        for address, size in (
            (operation.old_address, len(operation.old)),
            (operation.new_address, len(operation.new)),
        ):
            item = _recipe_slice(recipe, address, size)
            if item is None or item.get("erased"):
                return f"patch range 0x{address:X}+0x{size:X} has no stored slice"
            key = item.get("role"), item.get("sha256")
            if key in checked:
                continue
            checked.add(key)
            occurrence = _occurrence(catalog, source_id, item)
            scope = (
                occurrence.get("resolved_scope") or occurrence.get("scope")
                if occurrence else None
            )
            if not isinstance(scope, dict):
                return f"patch slice {key[0]} has no occurrence scope"
            unknown = [
                name for name in _scope_fields(key[0])
                if scope.get(name) is None
            ]
            if unknown:
                return f"unknown community scope for {key[0]}: {', '.join(unknown)}"
    return None


def _load_recipe(path: Path, schema: str) -> dict[str, Any]:
    value = _read_json(path, "recipe")
    versions = {
        DUMP_RECIPE_SCHEMA: DUMP_RECIPE_VERSION,
        OFFICIAL_RECIPE_SCHEMA: OFFICIAL_RECIPE_VERSION,
    }
    if (
        value.get("schema") != schema
        or value.get("schema_version") != versions.get(schema)
    ):
        raise FirmwareError(f"unexpected recipe schema: {path}")
    return value


def _payload(root: Path, relative: object, size: int, digest: str) -> bytes:
    relative = _safe_relative(relative, "payload path")
    path = root / relative
    if not path.is_file():
        raise FirmwareError(f"payload does not exist: {path}")
    data = path.read_bytes()
    if len(data) != size or _sha256(data) != digest:
        raise FirmwareError(f"payload size or hash mismatch: {path}")
    return data


def _dump_bytes(
    root: Path, recipe: dict[str, Any], address: int, size: int,
) -> bytes | None:
    item = _recipe_slice(recipe, address, size)
    if item is None:
        return None
    if item.get("erased"):
        return b"\xFF" * size
    data = _payload(root, item.get("payload_path"), item["size"], item["sha256"])
    normalization = item.get("normalization")
    if normalization is not None:
        data = restore_normalization(
            data, normalization, context="patch-tagging dump slice"
        )
        if _sha256(data) != item.get("source_sha256"):
            raise FirmwareError("patch-tagging restored dump hash mismatch")
    offset = address - item["source_range"]["from"]
    return data[offset:offset + size]


def _dump_matches_new(root: Path, recipe: dict[str, Any], patch: LoadedPatch) -> bool:
    constraints: dict[int, frozenset[int]] = {}
    fixed_change = False
    for operation in patch.operations:
        for offset, choices in enumerate(operation.new):
            constraints[operation.new_address + offset] = choices
            if len(choices) == 1 and choices != frozenset((operation.old[offset],)):
                fixed_change = True
    if not fixed_change:
        return False
    for address, choices in constraints.items():
        value = _dump_bytes(root, recipe, address, 1)
        if value is None or value[0] not in choices:
            return False
    return True


@dataclass
class _OfficialOperation:
    start: int
    end: int
    role: str
    sha256: str
    data: bytes


@dataclass
class _OfficialPackage:
    document: dict[str, Any]
    operations: list[_OfficialOperation]
    scopes: dict[tuple[str, str], dict[str, Any]]

    def operation(self, address: int, size: int) -> _OfficialOperation | None:
        end = address + size
        candidates = [
            item for item in self.operations
            if item.start <= address and item.end >= end
        ]
        return candidates[-1] if candidates else None


def _official_packages(
    catalog: dict[str, Any], root: Path,
) -> list[_OfficialPackage]:
    variant_scopes: dict[tuple[str, str, str], dict[str, Any]] = {}
    for region in catalog.get("regions", []):
        role = region.get("role")
        for variant in region.get("variants", []):
            digest = variant.get("sha256")
            for occurrence in variant.get("occurrences", []):
                package = occurrence.get("package_sha256")
                scope = occurrence.get("scope")
                if all(isinstance(value, str) for value in (role, digest, package)) \
                        and isinstance(scope, dict):
                    variant_scopes[(package, role, digest)] = scope
    result: list[_OfficialPackage] = []
    for package in catalog.get("packages", []):
        package_digest = package.get("sha256")
        recipe_relative = package.get("recipe", {}).get("path")
        if not isinstance(package_digest, str) or not isinstance(recipe_relative, str):
            raise FirmwareError("official package has invalid recipe identity")
        recipe = _load_recipe(
            root / _safe_relative(recipe_relative, "official recipe path"),
            "siemens-official-package-recipe",
        )
        operations: list[_OfficialOperation] = []
        scopes: dict[tuple[str, str], dict[str, Any]] = {}
        for operation in recipe.get("operations", []):
            if operation.get("action") != "replace":
                continue
            span = operation.get("range", {})
            start, end = span.get("from"), span.get("to_exclusive")
            size = operation.get("size")
            role, digest = operation.get("role"), operation.get("sha256")
            if (
                not isinstance(start, int) or not isinstance(end, int)
                or not isinstance(size, int) or end - start != size
                or not isinstance(role, str) or not isinstance(digest, str)
            ):
                raise FirmwareError(f"invalid official operation: {recipe_relative}")
            data = (
                b"\xFF" * size if operation.get("erased")
                else _payload(root, operation.get("payload_path"), size, digest)
            )
            normalization = operation.get("normalization")
            if normalization is not None:
                data = restore_normalization(
                    data, normalization,
                    context="patch-tagging official operation",
                )
                if _sha256(data) != operation.get("source_sha256"):
                    raise FirmwareError(
                        "patch-tagging restored official hash mismatch"
                    )
            operations.append(_OfficialOperation(start, end, role, digest, data))
            scope = variant_scopes.get((package_digest, role, digest))
            if scope is not None:
                scopes[(role, digest)] = scope
        result.append(_OfficialPackage(package, operations, scopes))
    return result


def _same_scope(
    dump_scope: dict[str, Any],
    official_scope: dict[str, Any],
    role: str,
) -> bool:
    for name in _scope_fields(role):
        left, right = dump_scope.get(name), official_scope.get(name)
        if left is None or right is None:
            return False
        if isinstance(left, str):
            left = left.casefold()
        if isinstance(right, str):
            right = right.casefold()
        if left != right:
            return False
    return True


def _official_matches(
    catalog: dict[str, Any],
    source_id: str,
    dump_recipe: dict[str, Any],
    dump_root: Path,
    patch: LoadedPatch,
    packages: Iterable[_OfficialPackage],
) -> tuple[str, ...]:
    matches: list[str] = []
    for package in packages:
        overlay: dict[int, int] = {}
        for patch_op in patch.operations:
            official_op = package.operation(patch_op.old_address, len(patch_op.old))
            dump_item = _recipe_slice(
                dump_recipe, patch_op.old_address, len(patch_op.old)
            )
            if official_op is None or dump_item is None:
                break
            occurrence = _occurrence(catalog, source_id, dump_item)
            official_scope = package.scopes.get((official_op.role, official_op.sha256))
            if (
                occurrence is None or official_scope is None
                or official_op.role != dump_item.get("role")
                or not _same_scope(
                    occurrence["scope"], official_scope, official_op.role
                )
            ):
                break
            current = bytes(
                overlay.get(
                    patch_op.old_address + offset,
                    official_op.data[patch_op.old_address + offset - official_op.start],
                )
                for offset in range(len(patch_op.old))
            )
            if current != patch_op.old:
                break
            if patch_op.require_destination_old \
                    and patch_op.new_address != patch_op.old_address:
                destination = package.operation(
                    patch_op.new_address, len(patch_op.old)
                )
                dump_destination = _recipe_slice(
                    dump_recipe, patch_op.new_address, len(patch_op.old)
                )
                if destination is None or dump_destination is None:
                    break
                destination_occurrence = _occurrence(
                    catalog, source_id, dump_destination
                )
                destination_scope = package.scopes.get(
                    (destination.role, destination.sha256)
                )
                start = patch_op.new_address - destination.start
                if (
                    destination.data[start:start + len(patch_op.old)] != patch_op.old
                    or destination_occurrence is None or destination_scope is None
                    or destination.role != dump_destination.get("role")
                    or not _same_scope(
                        destination_occurrence["scope"],
                        destination_scope,
                        destination.role,
                    )
                ):
                    break
            selected = _dump_bytes(
                dump_root, dump_recipe, patch_op.new_address, len(patch_op.new)
            )
            if selected is None or any(
                value not in choices for value, choices in zip(selected, patch_op.new)
            ):
                break
            overlay.update({
                patch_op.new_address + offset: value
                for offset, value in enumerate(selected)
            })
        else:
            digest = package.document.get("sha256")
            if isinstance(digest, str):
                matches.append(digest)
    return tuple(sorted(matches))


def patched_payload_path(
    relative: str, patch_count: int, digest: str, *,
    name_max: int = 255,
) -> str:
    relative = _safe_relative(relative, "payload path")
    path = PurePosixPath(relative)
    if path.suffix != ".bin":
        raise FirmwareError(f"payload does not have .bin suffix: {relative}")
    if not isinstance(patch_count, int) or patch_count < 1:
        raise FirmwareError(f"invalid payload patch count: {patch_count!r}")
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        raise FirmwareError(f"invalid payload SHA-256: {digest!r}")
    filename = f"patched-{patch_count}-{digest[:12]}.bin"
    if len(os.fsencode(filename)) > name_max:
        raise FirmwareError(
            f"patched payload name exceeds {name_max} bytes: {filename}"
        )
    return str(path.with_name(filename))


def untagged_payload_path(relative: str, digest: str) -> str:
    relative = _safe_relative(relative, "payload path")
    path = PurePosixPath(relative)
    match = re.fullmatch(r"patched-\d+-([0-9a-f]{12,64})\.bin", path.name)
    if match is None:
        return relative
    prefix = match.group(1)
    if not digest.startswith(prefix):
        raise FirmwareError(f"patched payload hash prefix mismatch: {relative}")
    return str(path.with_name(f"{prefix}.bin"))
PERSISTENT_ROLES = {"BCORE", "EE_FS", "EEPROM", "FFS(A)"}




def _range(start: int, end: int) -> dict[str, int]:
    return {"from": start, "to_exclusive": end, "length": end - start}


def _range_metadata(
    item: dict[str, Any], occurrence: dict[str, Any],
    start: int, end: int,
) -> dict[str, Any]:
    source = item["source_range"]
    payload_start = start - source["from"]
    payload_end = end - source["from"]

    def translated(name: str) -> dict[str, int]:
        span = occurrence.get(name, source)
        translated_start = span["from"] + payload_start
        return _range(translated_start, translated_start + end - start)

    return {
        "file_range": _range(start, end),
        "layout_range": translated("layout_range"),
        "native_range": translated("native_range"),
        "payload_range": _range(payload_start, payload_end),
    }


def _byte_metadata(old: bytes, new: bytes) -> dict[str, Any]:
    result: dict[str, Any] = {}
    if len(old) <= 64:
        result.update({"old_bytes": old.hex(), "new_bytes": new.hex()})
    else:
        result.update({
            "old_prefix": old[:32].hex(),
            "old_suffix": old[-32:].hex(),
            "new_prefix": new[:32].hex(),
            "new_suffix": new[-32:].hex(),
        })
    return result


def _comparison_spans(
    catalog: dict[str, Any], source_id: str,
    recipe: dict[str, Any], dump_root: Path,
    package: _OfficialPackage,
) -> list[tuple[dict[str, Any], dict[str, Any], _OfficialOperation, bytes]]:
    result = []
    for item in recipe.get("slices", []):
        role = item.get("role")
        if item.get("erased") or role in PERSISTENT_ROLES:
            continue
        span = item.get("source_range", {})
        start, end = span.get("from"), span.get("to_exclusive")
        size = item.get("size")
        if not isinstance(start, int) or not isinstance(end, int) \
                or not isinstance(size, int) or end - start != size:
            raise FirmwareError("fullflash recipe has an invalid slice range")
        official = package.operation(start, size)
        occurrence = _occurrence(catalog, source_id, item)
        if official is None or occurrence is None:
            continue
        official_scope = package.scopes.get((official.role, official.sha256))
        if (
            official.start != start or official.end != end
            or official.role != role or official_scope is None
            or not _same_scope(occurrence["scope"], official_scope, role)
        ):
            continue
        data = _payload(
            dump_root, item.get("payload_path"), size, item["sha256"]
        )
        result.append((item, occurrence, official, data))
    return result


def _select_baseline(
    catalog: dict[str, Any], source_id: str,
    recipe: dict[str, Any], dump_root: Path,
    matches: list[PatchMatch], packages: dict[str, _OfficialPackage],
) -> tuple[_OfficialPackage, tuple[str, ...], list[
    tuple[dict[str, Any], dict[str, Any], _OfficialOperation, bytes]
]]:
    candidate_sets = [set(match.official_packages) for match in matches]
    candidates = set.intersection(*candidate_sets)
    if not candidates:
        raise FirmwareError(
            f"matched patches for {matches[0].fullflash_sha256} "
            "do not share one official baseline"
        )
    selected: _OfficialPackage | None = None
    selected_spans = []
    signature = None
    for digest in sorted(candidates):
        package = packages.get(digest)
        if package is None:
            raise FirmwareError(f"missing matched official package {digest}")
        spans = _comparison_spans(
            catalog, source_id, recipe, dump_root, package
        )
        current = tuple(
            (item["role"], official.start, official.end, _sha256(official.data))
            for item, _occurrence_row, official, _data in spans
        )
        if not current:
            raise FirmwareError(
                f"official baseline {digest} has no complete comparable regions"
            )
        if signature is None:
            signature = current
            selected, selected_spans = package, spans
        elif current != signature:
            raise FirmwareError(
                f"ambiguous non-byte-equivalent official baselines for "
                f"{matches[0].fullflash_sha256}"
            )
    assert selected is not None
    return selected, tuple(sorted(candidates)), selected_spans


def _artifact_patch_analysis(
    *,
    catalog: dict[str, Any], source_id: str,
    recipe: dict[str, Any], dump_root: Path,
    matches: list[PatchMatch], packages: dict[str, _OfficialPackage],
    archive_path: Path, archive_sha256: str,
    official_catalog_sha256: str,
) -> tuple[dict[str, Any], dict[tuple[str, str], dict[str, set[str]]]]:
    baseline, equivalent, spans = _select_baseline(
        catalog, source_id, recipe, dump_root, matches, packages
    )
    baseline_digest = baseline.document["sha256"]
    claimed: dict[int, str] = {}
    payload_refs: dict[tuple[str, str], dict[str, set[str]]] = {}
    identified = []

    for match in sorted(
        matches,
        key=lambda item: (
            item.patch.definition.archive_key,
            item.patch.definition.patch_id,
        ),
    ):
        patch = match.patch
        scoped_id = patch.definition.scoped_id
        records = []
        for index, operation in enumerate(patch.operations):
            item = _recipe_slice(
                recipe, operation.new_address, len(operation.new)
            )
            if item is None or item.get("erased"):
                raise FirmwareError("matched patch target has no payload slice")
            occurrence = _occurrence(catalog, source_id, item)
            official = baseline.operation(
                operation.new_address, len(operation.new)
            )
            selected = _dump_bytes(
                dump_root, recipe, operation.new_address, len(operation.new)
            )
            if occurrence is None or official is None or selected is None:
                raise FirmwareError("matched patch target has no baseline mapping")
            offset = operation.new_address - official.start
            old = official.data[offset:offset + len(operation.new)]
            changed = [
                address for address, old_byte, new_byte in zip(
                    range(operation.new_address, operation.new_address + len(selected)),
                    old, selected,
                ) if old_byte != new_byte
            ]
            for address in changed:
                prior = claimed.setdefault(address, scoped_id)
                if prior != scoped_id:
                    raise FirmwareError(
                        f"patch collision at 0x{address:X}: "
                        f"{prior} and {scoped_id}"
                    )
            if changed:
                key = item["role"], item["sha256"]
                payload_refs.setdefault(key, {
                    "patches": set(), "unknown": set(),
                })["patches"].add(scoped_id)
            record = {
                "vkp_line": patch.records[index].line,
                "vkp_address": patch.records[index].address,
                "role": item["role"],
                "payload_sha256": item["sha256"],
                "payload_path": item.get("payload_path"),
                **_range_metadata(
                    item, occurrence, operation.new_address,
                    operation.new_address + len(selected),
                ),
                **_byte_metadata(old, selected),
            }
            records.append(record)
        definition = patch.definition
        identified.append({
            "archive_key": definition.archive_key,
            "patch_id": definition.patch_id,
            "title": patch.title,
            "member": f"patches/{definition.member}",
            "software_version": definition.software_version,
            "records": records,
        })

    unknown = []
    for item, occurrence, official, data in spans:
        changed = [
            index for index, (old, new) in enumerate(zip(official.data, data))
            if old != new and official.start + index not in claimed
        ]
        runs: list[tuple[int, int]] = []
        for index in changed:
            if not runs or index != runs[-1][1]:
                runs.append((index, index + 1))
            else:
                runs[-1] = runs[-1][0], index + 1
        for start_offset, end_offset in runs:
            start, end = official.start + start_offset, official.start + end_offset
            old = official.data[start_offset:end_offset]
            new = data[start_offset:end_offset]
            unknown.append({
                "role": item["role"],
                "payload_sha256": item["sha256"],
                "payload_path": item.get("payload_path"),
                "official_package_sha256": baseline_digest,
                "length": end - start,
                **_range_metadata(item, occurrence, start, end),
                **_byte_metadata(old, new),
            })
    for index, region in enumerate(unknown, 1):
        region["id"] = f"unknown-{index:03d}"
        key = region["role"], region["payload_sha256"]
        payload_refs.setdefault(key, {
            "patches": set(), "unknown": set(),
        })["unknown"].add(region["id"])

    return {
        "schema_version": 1,
        "patch_archive": {
            "name": archive_path.name,
            "sha256": archive_sha256,
        },
        "baseline": {
            "package_sha256": baseline_digest,
            "equivalent_package_sha256s": list(equivalent),
            "official_catalog_sha256": official_catalog_sha256,
        },
        "identified_patches": identified,
        "unidentified_regions": unknown,
    }, payload_refs


def _pointer(path: tuple[object, ...]) -> str:
    return "".join(
        f"[{part}]" if isinstance(part, int) else f".{part}" for part in path
    ).lstrip(".")


def _replace_references(
    value: Any,
    path: tuple[object, ...],
    replacements: dict[str, str],
    changes: list[tuple[str, str, str]],
) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if isinstance(child, str) and child in replacements:
                value[key] = replacements[child]
                changes.append((_pointer((*path, key)), child, replacements[child]))
            else:
                _replace_references(child, (*path, key), replacements, changes)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _replace_references(child, (*path, index), replacements, changes)


def _validate_model(document: dict[str, Any], model: str, description: str) -> None:
    name = document.get("layout", {}).get("name")
    if not isinstance(name, str) or model not in name.split("/"):
        raise FirmwareError(
            f"{description} layout {name!r} does not match model {model}"
        )


def plan_patch_tags(
    *,
    model: str,
    dump_catalog_path: Path,
    official_catalog_path: Path,
    archive_path: Path,
    definitions: Iterable[PatchDefinition] = _DEFINITIONS,
) -> PatchTagPlan:
    model = model.upper()
    dump_catalog_path = dump_catalog_path.resolve()
    official_catalog_path = official_catalog_path.resolve()
    archive_path = archive_path.resolve()
    dump = _read_json(dump_catalog_path, "community catalog")
    official = _read_json(official_catalog_path, "official catalog")
    if dump.get("schema") != COMMUNITY_SCHEMA \
            or dump.get("schema_version") != COMMUNITY_VERSION:
        raise FirmwareError(
            f"tag-patches requires a schema-{COMMUNITY_VERSION} "
            "community catalog"
        )
    if official.get("schema") != OFFICIAL_SCHEMA \
            or official.get("schema_version") != OFFICIAL_VERSION:
        raise FirmwareError(
            f"tag-patches requires a schema-{OFFICIAL_VERSION} official catalog"
        )
    validate_official_backing(dump, dump_catalog_path)
    dump_layout = dump.get("layout", {})
    official_layout = official.get("layout", {})
    layout_fields = ("name", "base", "length", "catalog_sha256")
    if any(dump_layout.get(key) != official_layout.get(key) for key in layout_fields):
        raise FirmwareError("community and official catalog layouts differ")
    _validate_model(dump, model, "community catalog")
    _validate_model(official, model, "official catalog")
    definitions = _validate_definitions(definitions)
    patches, ignored = _load_patches(archive_path, model, definitions)
    dump_root, official_root = dump_catalog_path.parent, official_catalog_path.parent
    packages = _official_packages(official, official_root) if patches else []
    packages_by_digest = {item.document["sha256"]: item for item in packages}
    matches: list[PatchMatch] = []
    rejections: list[PatchRejection] = []
    recipes_by_hash: dict[str, dict[str, Any]] = {}

    for artifact in dump.get("artifacts", []):
        if artifact.get("kind") != "complete-fullflash":
            continue
        digest, source_id = artifact.get("sha256"), artifact.get("id")
        relative = artifact.get("recipe", {}).get("path")
        source_path = artifact.get("path", "")
        if not all(isinstance(value, str) for value in (
            digest, source_id, relative, source_path,
        )):
            raise FirmwareError("dump artifact has invalid recipe identity")
        recipe = _load_recipe(
            dump_root / _safe_relative(relative, "fullflash recipe path"),
            "siemens-fullflash-recipe",
        )
        recipes_by_hash[digest] = recipe
        versions = {
            occurrence.get("scope", {}).get("software_version")
            for occurrence in dump.get("occurrences", [])
            if occurrence.get("source_id") == source_id
            and occurrence.get("role") not in ("BCORE", "FFS(A)", "EEPROM")
        }
        for patch in patches:
            if patch.definition.software_version not in versions \
                    or not _dump_matches_new(dump_root, recipe, patch):
                continue
            problem = _scope_problem(dump, source_id, recipe, patch.operations)
            if problem is not None:
                rejections.append(PatchRejection(
                    digest, source_path, patch, problem
                ))
                continue
            official_matches = _official_matches(
                dump, source_id, recipe, dump_root, patch, packages
            )
            if official_matches:
                matches.append(PatchMatch(
                    digest, source_path, patch, official_matches
                ))
            else:
                rejections.append(PatchRejection(
                    digest, source_path, patch,
                    "no one scope-compatible official package has all old bytes",
                ))

    matches_by_hash: dict[str, list[PatchMatch]] = {}
    for match in matches:
        matches_by_hash.setdefault(match.fullflash_sha256, []).append(match)

    archive_sha256 = _sha256(archive_path.read_bytes())
    official_catalog_sha256 = _sha256(official_catalog_path.read_bytes())
    analyses_by_hash: dict[str, dict[str, Any]] = {}
    refs_by_hash: dict[
        str, dict[tuple[str, str], dict[str, set[str]]]
    ] = {}
    patches_by_variant: dict[tuple[str, str], frozenset[str]] = {}
    analyses = []
    artifacts_by_hash = {
        artifact["sha256"]: artifact
        for artifact in dump.get("artifacts", [])
        if artifact.get("kind") == "complete-fullflash"
    }
    for digest, artifact_matches in sorted(matches_by_hash.items()):
        artifact = artifacts_by_hash[digest]
        analysis, refs = _artifact_patch_analysis(
            catalog=dump,
            source_id=artifact["id"],
            recipe=recipes_by_hash[digest],
            dump_root=dump_root,
            matches=artifact_matches,
            packages=packages_by_digest,
            archive_path=archive_path,
            archive_sha256=archive_sha256,
            official_catalog_sha256=official_catalog_sha256,
        )
        analyses_by_hash[digest] = analysis
        refs_by_hash[digest] = refs
        analyses.append({"fullflash_sha256": digest, **analysis})
        for key, ref in refs.items():
            patch_ids = frozenset(ref["patches"])
            if not patch_ids:
                continue
            prior = patches_by_variant.setdefault(key, patch_ids)
            if prior != patch_ids:
                raise FirmwareError(
                    f"shared payload has inconsistent patch attribution: "
                    f"{key[0]} {key[1]}"
                )

    try:
        name_max = os.pathconf(dump_root, "PC_NAME_MAX")
    except (OSError, ValueError):
        name_max = 255
    replacements: dict[str, str] = {}
    specs: dict[str, tuple[str, str | None]] = {}
    for region in dump.get("regions", []):
        for variant in region.get("variants", []):
            if variant.get("erased") is True:
                continue
            patch_ids = patches_by_variant.get(
                (region.get("role"), variant.get("sha256"))
            )
            paths = payload_paths(variant)
            if not paths:
                raise FirmwareError("cataloged variant has no payload path")
            digest = variant["sha256"]
            if patch_ids:
                updated_paths = [
                    patched_payload_path(
                        path, len(patch_ids), digest, name_max=name_max
                    )
                    for path in paths
                ]
            else:
                updated_paths = [
                    untagged_payload_path(path, digest) for path in paths
                ]
                if updated_paths == paths:
                    continue
            canonical = updated_paths[0]
            for index, (old, new) in enumerate(zip(paths, updated_paths)):
                if replacements.setdefault(old, new) != new:
                    raise FirmwareError(f"payload rename collision for {old}")
                specs[old] = (
                    "regular" if index == 0 else "symlink",
                    None if index == 0 else canonical,
                )

    destinations: dict[str, str] = {}
    old_paths = set(replacements)
    moves: list[PayloadMove] = []
    for old, new in sorted(replacements.items()):
        if destinations.setdefault(new, old) != old:
            raise FirmwareError(f"two payloads would collide at {new}")
        source, destination = dump_root / old, dump_root / new
        kind, target = specs[old]
        if old != new and (
            not os.path.lexists(source)
            or (os.path.lexists(destination) and new not in old_paths)
        ):
            raise FirmwareError(f"invalid or colliding payload move: {old} -> {new}")
        if kind == "regular" and (source.is_symlink() or not source.is_file()):
            raise FirmwareError(f"canonical payload is not regular: {source}")
        if kind == "symlink" and not source.is_symlink():
            raise FirmwareError(f"payload alias is not a symlink: {source}")
        if old != new:
            moves.append(PayloadMove(old, new, kind, target))

    updated_dump = copy.deepcopy(dump)
    summary = updated_dump.setdefault("summary", {})
    summary.setdefault("official_backed_variants", 0)
    summary.setdefault("official_backed_paths", 0)
    summary.setdefault("official_backed_bytes", 0)
    references: list[ReferenceChange] = []
    catalog_changes: list[tuple[str, str, str]] = []
    _replace_references(updated_dump, (), replacements, catalog_changes)
    references.extend(
        ReferenceChange(dump_catalog_path, pointer, old, new)
        for pointer, old, new in catalog_changes if old != new
    )

    affected: set[str] = set(analyses_by_hash)
    digest_by_source = {}
    analyses = []
    for artifact in updated_dump.get("artifacts", []):
        if artifact.get("kind") != "complete-fullflash":
            continue
        digest = artifact.get("sha256")
        source_id = artifact.get("id")
        if isinstance(digest, str) and isinstance(source_id, str):
            digest_by_source[source_id] = digest
        metadata = artifact.setdefault("metadata", {})
        if not isinstance(metadata, dict):
            raise FirmwareError("fullflash artifact metadata is not an object")
        metadata.pop("patch_analysis", None)
        analysis = analyses_by_hash.get(digest)
        if analysis is not None:
            analysis = copy.deepcopy(analysis)
            _replace_references(analysis, (), replacements, [])
            analyses_by_hash[digest] = analysis
            analyses.append({"fullflash_sha256": digest, **analysis})
            metadata["patch_analysis"] = analysis
    for occurrence in updated_dump.get("occurrences", []):
        occurrence.pop("patch_analysis", None)
        digest = digest_by_source.get(occurrence.get("source_id"))
        refs = refs_by_hash.get(digest, {}).get((
            occurrence.get("role"), occurrence.get("sha256"),
        ))
        if refs is None:
            continue
        occurrence["patch_analysis"] = {
            "identified_patch_ids": sorted(refs["patches"]),
            "unidentified_region_ids": sorted(refs["unknown"]),
        }

    updated_recipes: dict[Path, dict[str, Any]] = {}
    for artifact in updated_dump.get("artifacts", []):
        if artifact.get("kind") != "complete-fullflash":
            continue
        digest = artifact.get("sha256")
        relative = artifact.get("recipe", {}).get("path")
        if not isinstance(digest, str) or not isinstance(relative, str):
            continue
        path = dump_root / relative
        recipe = copy.deepcopy(_load_recipe(path, "siemens-fullflash-recipe"))
        changes: list[tuple[str, str, str]] = []
        _replace_references(recipe, (), replacements, changes)
        changes = [item for item in changes if item[1] != item[2]]
        if changes:
            affected.add(digest)
            updated_recipes[path] = recipe
            references.extend(
                ReferenceChange(path, pointer, old, new)
                for pointer, old, new in changes
            )

    return PatchTagPlan(
        model, dump_catalog_path, official_catalog_path, archive_path,
        sorted(matches, key=lambda item: (
            item.fullflash_sha256,
            item.patch.definition.archive_key,
            item.patch.definition.patch_id,
        )),
        sorted(rejections, key=lambda item: (
            item.fullflash_sha256,
            item.patch.definition.archive_key,
            item.patch.definition.patch_id,
        )),
        sorted(affected), analyses, moves,
        sorted(references, key=lambda item: (
            item.path.as_posix(), item.pointer, item.old_value,
        )),
        updated_dump, updated_recipes, ignored,
    )


def apply_patch_tag_plan(plan: PatchTagPlan) -> None:
    root = plan.dump_catalog_path.parent
    regular = [move for move in plan.payload_moves if move.kind == "regular"]
    aliases = [move for move in plan.payload_moves if move.kind == "symlink"]
    for move in regular:
        destination = root / move.new_path
        destination.parent.mkdir(parents=True, exist_ok=True)
        os.rename(root / move.old_path, destination)
    for move in aliases:
        assert move.target_path is not None
        atomic_relative_symlink(root, move.new_path, move.target_path, False)
    for path, recipe in sorted(plan.recipes.items()):
        _atomic_output(
            path,
            (json.dumps(
                recipe, indent=2, sort_keys=True, ensure_ascii=False
            ) + "\n").encode(),
            True,
        )
    _atomic_output(
        plan.dump_catalog_path,
        (json.dumps(
            plan.dump_catalog, indent=2, sort_keys=True, ensure_ascii=False
        ) + "\n").encode(),
        True,
    )
    for move in aliases:
        (root / move.old_path).unlink()


def _print_plan(plan: PatchTagPlan, dry_run: bool) -> None:
    print(f"mode: {'dry-run' if dry_run else 'apply'}")
    print(f"model: {plan.model}")
    print(f"patch archive: {plan.archive_path}")
    print(f"ignored uncurated archive patches: {plan.ignored_archive_patches}")
    print(f"matched patches: {len(plan.matches)}")
    for match in plan.matches:
        definition = match.patch.definition
        print(
            f"  MATCH fullflash={match.fullflash_sha256} "
            f"patch={definition.scoped_id} "
            f"title={json.dumps(match.patch.title, ensure_ascii=False)} "
            f"member=patches/{definition.member} "
            f"official={','.join(match.official_packages)}"
        )
    print(f"rejected patches: {len(plan.rejections)}")
    for rejection in plan.rejections:
        definition = rejection.patch.definition
        print(
            f"  REJECT fullflash={rejection.fullflash_sha256} "
            f"patch={definition.scoped_id} "
            f"title={json.dumps(rejection.patch.title, ensure_ascii=False)} "
            f"reason={rejection.reason}"
        )
    print(f"affected fullflashes: {len(plan.affected_fullflashes)}")
    for digest in plan.affected_fullflashes:
        print(f"  FULLFLASH {digest}")
    print(f"metadata analyses: {len(plan.analyses)}")
    for analysis in plan.analyses:
        patches = analysis["identified_patches"]
        unknown = analysis["unidentified_regions"]
        print(
            f"  ANALYSIS fullflash={analysis['fullflash_sha256']} "
            f"baseline={analysis['baseline']['package_sha256']} "
            f"patches={len(patches)} unidentified={len(unknown)}"
        )
        for patch in patches:
            print(
                f"    {patch['archive_key']}/{patch['patch_id']}: "
                f"{patch['title']}"
            )
        roles: dict[str, int] = {}
        for region in unknown:
            roles[region["role"]] = roles.get(region["role"], 0) + 1
        for role, count in sorted(roles.items()):
            print(f"    UNKNOWN-SUMMARY role={role} regions={count}")
        if len(unknown) <= 20:
            for region in unknown:
                span = region["layout_range"]
                print(
                    f"    UNKNOWN id={region['id']} role={region['role']} "
                    f"layout=0x{span['from']:X}-0x{span['to_exclusive']:X} "
                    f"length={region['length']}"
                )
        elif unknown:
            print(
                "    UNKNOWN-DETAIL omitted from text output; "
                "use 'fullflash catalog info --catalog ROOT IDENTIFIER --json' "
                "after tagging"
            )
    print(f"payload moves: {len(plan.payload_moves)}")
    for move in plan.payload_moves:
        print(f"  PAYLOAD {move.old_path} -> {move.new_path}")
    print(f"reference changes: {len(plan.reference_changes)}")
    for change in plan.reference_changes:
        print(
            f"  REF {change.path}:{change.pointer} "
            f"{change.old_value} -> {change.new_value}"
        )
    print("validation: ok")
    writes = len(plan.payload_moves) + len(plan.recipes) + 1
    print(f"writes: {0 if dry_run else writes}")


def command_tag_patches(args: Any) -> None:
    plan = plan_patch_tags(
        model=args.model,
        dump_catalog_path=args.catalog / "catalog.json",
        official_catalog_path=args.official_catalog / "catalog.json",
        archive_path=args.patch_archive,
    )
    if not args.dry_run:
        apply_patch_tag_plan(plan)
    _print_plan(plan, args.dry_run)
