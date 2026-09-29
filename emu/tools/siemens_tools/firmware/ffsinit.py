from __future__ import annotations

import hashlib
import io
import re
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from .containers import detect_exe_type, extract_exe
from .xbi import FirmwareError, firmware_extension, is_xbi, parse_xbi


_FFSINIT_NAME = re.compile(
    r"^FFSInit_(?P<model>[^_]+)_.+_(?P<software>\d{1,3})_\d+$",
    re.IGNORECASE,
)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class FfsInitIdentity:
    model: str
    software_version: int
    zip_sha256: str
    zip_size: int
    tree_sha256: str
    entry_order_sha256: str

    @property
    def key(self) -> tuple[str, int, str]:
        return self.model.casefold(), self.software_version, self.zip_sha256

    @property
    def tree_key(self) -> tuple[str, int, str]:
        return self.model.casefold(), self.software_version, self.tree_sha256


@dataclass(frozen=True)
class FfsInitReference:
    identity: FfsInitIdentity
    xfs: bytes
    xfs_sha256: str
    exe_path: Path
    xfs_path: Path


@dataclass(frozen=True)
class FfsInitRecovery:
    identity: FfsInitIdentity
    reference: FfsInitReference
    confidence: str
    match_kind: str


def _name_identity(path: Path) -> tuple[str, int]:
    match = _FFSINIT_NAME.fullmatch(path.stem)
    if match is None:
        raise FirmwareError(
            f"FFSInit filename has no model/software identity: {path.name}"
        )
    return match.group("model"), int(match.group("software"), 10)


def _zip_payload(data: bytes) -> bytes:
    if detect_exe_type(data) != "update":
        raise FirmwareError("input is not a Siemens FFSInit executable")
    payloads = extract_exe(data)
    if len(payloads) != 1 or not payloads[0].startswith(b"PK\x03\x04"):
        raise FirmwareError("FFSInit executable does not contain one ZIP payload")
    try:
        with zipfile.ZipFile(io.BytesIO(payloads[0])) as archive:
            corrupt = archive.testzip()
            if corrupt is not None:
                raise FirmwareError(
                    f"FFSInit ZIP payload has a corrupt member: {corrupt}"
                )
    except zipfile.BadZipFile as exc:
        raise FirmwareError(f"invalid FFSInit ZIP payload: {exc}") from exc
    return payloads[0]

def _zip_tree_hashes(payload: bytes) -> tuple[str, str]:
    ordered: list[tuple[str, str, int, str]] = []
    seen: set[str] = set()
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        for info in archive.infolist():
            name = info.filename.replace("\\", "/").rstrip("/")
            if not name:
                continue
            parts = name.split("/")
            if name.startswith("/") or any(
                part in ("", ".", "..") for part in parts
            ):
                raise FirmwareError(
                    f"unsafe FFSInit ZIP member: {info.filename}"
                )
            folded = name.casefold()
            if folded in seen:
                raise FirmwareError(
                    f"duplicate FFSInit ZIP member: {info.filename}"
                )
            seen.add(folded)
            data = b"" if info.is_dir() else archive.read(info)
            ordered.append((
                "directory" if info.is_dir() else "file",
                folded,
                len(data),
                _sha256(data),
            ))

    def digest(rows: Iterable[tuple[str, str, int, str]]) -> str:
        result = hashlib.sha256()
        for row in rows:
            for value in row:
                encoded = str(value).encode("utf-8")
                result.update(len(encoded).to_bytes(4, "little"))
                result.update(encoded)
        return result.hexdigest()

    return digest(sorted(ordered)), digest(ordered)


def identify_ffsinit(path: Path, data: bytes | None = None) -> FfsInitIdentity:
    model, software_version = _name_identity(path)
    payload = _zip_payload(path.read_bytes() if data is None else data)
    tree_sha256, entry_order_sha256 = _zip_tree_hashes(payload)
    return FfsInitIdentity(
        model=model,
        software_version=software_version,
        zip_sha256=_sha256(payload),
        zip_size=len(payload),
        tree_sha256=tree_sha256,
        entry_order_sha256=entry_order_sha256,
    )


def is_ffsinit(path: Path, data: bytes | None = None) -> bool:
    if _FFSINIT_NAME.fullmatch(path.stem) is None:
        return False
    try:
        identify_ffsinit(path, data)
    except FirmwareError:
        return False
    return True


def _candidate_paths(roots: Iterable[Path]) -> list[Path]:
    result: set[Path] = set()
    for root in roots:
        if not root.exists():
            raise FirmwareError(f"FFSInit reference root does not exist: {root}")
        paths = [root] if root.is_file() else root.rglob("*")
        result.update(path for path in paths if path.is_file())
    return sorted(result, key=lambda path: path.as_posix())


def build_ffsinit_reference_index(
    paths: Iterable[Path],
) -> dict[tuple[str, int, str], FfsInitReference]:
    candidates = list(paths)
    by_location = {
        (path.parent.resolve(), path.name.casefold()): path
        for path in candidates
    }
    references: dict[tuple[str, int, str], FfsInitReference] = {}
    for exe_path in sorted(candidates, key=lambda path: path.as_posix()):
        if not exe_path.name.casefold().startswith("ffsinit_"):
            continue
        xfs_name = exe_path.name[len("FFSInit_"):]
        xfs_path = by_location.get((
            exe_path.parent.resolve(),
            Path(xfs_name).with_suffix(".xfs").name.casefold(),
        ))
        if xfs_path is None:
            continue
        identity = identify_ffsinit(exe_path)
        xfs = xfs_path.read_bytes()
        if not is_xbi(xfs):
            raise FirmwareError(f"FFSInit reference is not XBI firmware: {xfs_path}")
        info = parse_xbi(xfs)
        if firmware_extension(info) != "xfs":
            raise FirmwareError(f"FFSInit reference is not XFS firmware: {xfs_path}")
        model = info.get("model")
        software = info.get("svn")
        if (
            not isinstance(model, str)
            or model.casefold() != identity.model.casefold()
            or not isinstance(software, (int, float))
            or int(software) != identity.software_version
        ):
            raise FirmwareError(
                f"FFSInit reference identity mismatch: {exe_path} / {xfs_path}"
            )
        reference = FfsInitReference(
            identity=identity,
            xfs=xfs,
            xfs_sha256=_sha256(xfs),
            exe_path=exe_path,
            xfs_path=xfs_path,
        )
        previous = references.get(identity.key)
        if previous is not None and previous.xfs_sha256 != reference.xfs_sha256:
            raise FirmwareError(
                "conflicting byte-exact FFSInit references for "
                f"{identity.model} SW{identity.software_version} "
                f"ZIP {identity.zip_sha256}: "
                f"{previous.xfs_path} and {xfs_path}"
            )
        if previous is None or xfs_path.as_posix() < previous.xfs_path.as_posix():
            references[identity.key] = reference
    return references


def discover_ffsinit_references(
    roots: Iterable[Path],
) -> dict[tuple[str, int, str], FfsInitReference]:
    return build_ffsinit_reference_index(_candidate_paths(roots))


def recover_ffsinit_xfs(
    path: Path,
    data: bytes,
    references: dict[tuple[str, int, str], FfsInitReference],
) -> FfsInitReference:
    identity = identify_ffsinit(path, data)
    reference = references.get(identity.key)
    if reference is None:
        raise FirmwareError(
            "no byte-exact FFSInit reference for "
            f"{identity.model} SW{identity.software_version} "
            f"ZIP {identity.zip_sha256}"
        )
    if _sha256(reference.xfs) != reference.xfs_sha256:
        raise FirmwareError(
            f"FFSInit reference hash changed: {reference.xfs_path}"
        )
    return reference


def recover_ffsinit_bin_reference(
    path: Path,
    data: bytes,
    references: dict[tuple[str, int, str], FfsInitReference],
) -> FfsInitRecovery:
    identity = identify_ffsinit(path, data)
    reference = references.get(identity.key)
    if reference is not None:
        recovery = FfsInitRecovery(
            identity=identity,
            reference=reference,
            confidence="byte-exact",
            match_kind="zip-sha256",
        )
    else:
        candidates = {
            item.xfs_sha256: item
            for item in references.values()
            if item.identity.tree_key == identity.tree_key
        }
        if len(candidates) != 1:
            reason = "no" if not candidates else "ambiguous"
            raise FirmwareError(
                f"{reason} semantic FFSInit reference for "
                f"{identity.model} SW{identity.software_version} "
                f"tree {identity.tree_sha256}"
            )
        reference = next(iter(candidates.values()))
        recovery = FfsInitRecovery(
            identity=identity,
            reference=reference,
            confidence="heuristic",
            match_kind="canonical-file-tree",
        )
    if _sha256(recovery.reference.xfs) != recovery.reference.xfs_sha256:
        raise FirmwareError(
            f"FFSInit reference hash changed: {recovery.reference.xfs_path}"
        )
    return recovery
