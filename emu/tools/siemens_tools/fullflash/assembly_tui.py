from __future__ import annotations

import importlib
import re
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Iterable

from ..firmware.xbi import FirmwareError
from .assembly import (
    AssemblyContext,
    AssemblyRequest,
    Baseline,
    MobSwSelection,
    RegionCandidate,
    _baseline_artifacts,
    _candidate_payload,
    _canonical_role,
    _compatibility_warnings,
    _effective_scope,
    _eeprom_conflicts,
    _erased_candidate,
    _imei_serial,
    _load_baseline,
    _mobsw_selections,
    _personalization_conflicts,
    _resolve_explicit_regions,
    _resolve_mobsw,
    _role_names,
    _selection_key,
    _short_hashes,
    _tuple_key,
    load_assembly_context,
    parse_bootkey,
    parse_flash_id,
    region_candidates,
)
from .bcore_key import derive_bcore_key, hash_bootkey, parse_fsn, parse_skey


@dataclass(frozen=True)
class TuiResult:
    request: AssemblyRequest
    output: Path | None
    force: bool


@dataclass(frozen=True)
class PickerItem:
    value: Any
    label: str
    enabled: bool = True
    current: bool = False


class _Cancelled(Exception):
    pass


_CANCEL = object()
_SPLIT_IDS = re.compile(r"[\s,]+")


def parse_flash_ids(value: str) -> tuple[tuple[int, int], ...]:
    text = value.strip()
    if not text or text.casefold() in ("baseline", "preserve"):
        return ()
    fields = [field for field in _SPLIT_IDS.split(text) if field]
    if not 1 <= len(fields) <= 2:
        raise ValueError("enter one or two MFR:DEVICE values")
    return tuple(parse_flash_id(field) for field in fields)


def parse_entry_target(value: str) -> int | None:
    text = value.strip()
    if not text or text.casefold() in ("preserve", "baseline"):
        return None
    try:
        target = int(text, 0)
    except ValueError as exc:
        raise ValueError("entry target must be an integer") from exc
    if not 0 <= target <= 0xFFFFFF:
        raise ValueError("entry target must fit in 24 bits")
    return target


def parse_fsn_skey(value: str) -> tuple[bytes, dict[str, Any]]:
    fields = [field for field in _SPLIT_IDS.split(value.strip()) if field]
    if len(fields) != 2:
        raise ValueError("enter FSN and SKey separated by whitespace")
    result = derive_bcore_key(parse_fsn(fields[0]), parse_skey(fields[1]))
    return result.bcore_hash, {"kind": "fsn-skey", **result.as_dict()}


def parse_raw_bootkey(value: str) -> tuple[bytes, dict[str, Any]]:
    bootkey = parse_bootkey(value.strip())
    return hash_bootkey(bootkey), {"kind": "bootkey", "bootkey": bootkey.hex()}


def output_error(output: Path | None) -> str | None:
    if output is not None and output.with_suffix(".json") == output:
        return "binary output path must not use the .json suffix"
    return None


def adjust_scroll(cursor: int, scroll: int, capacity: int, count: int) -> int:
    if capacity <= 0 or count <= capacity:
        return 0
    maximum = max(0, count - capacity)
    if cursor < scroll:
        return cursor
    if cursor >= scroll + capacity:
        return min(maximum, cursor - capacity + 1)
    return min(scroll, maximum)


def numeric_activation(buffer: str, key: int, count: int) -> tuple[str, int | None]:
    if ord("0") <= key <= ord("9"):
        return buffer + chr(key), None
    if key in (10, 13) and buffer:
        value = int(buffer, 10)
        return "", value - 1 if 1 <= value <= count else None
    return "", None


def _baseline_label(artifact: dict[str, Any]) -> str:
    firmware = (artifact.get("metadata") or {}).get("firmware") or {}
    sw = firmware.get("software_version", "?")
    lg = firmware.get("langpack", "?")
    return f"{str(artifact.get('sha256'))[:12]} SW{sw} {lg} {artifact.get('path', '')}"


def _mobsw_label(item: MobSwSelection) -> str:
    sw = item.scope.get("software_version", "?")
    lg = item.scope.get("langpack", "?")
    t9 = item.scope.get("t9_version", "?")
    return f"{item.sha256[:12]} SW{sw}/{lg}/T9-{t9} {item.recipe_path}"


def _flash_ids_label(values: tuple[tuple[int, int], ...]) -> str:
    if not values:
        return "baseline"
    return ", ".join(f"0x{mfr:04X}:0x{device:04X}" for mfr, device in values)


class AssemblyTuiState:
    def __init__(
        self,
        request: AssemblyRequest,
        *,
        output: Path | None,
        force: bool,
        context: AssemblyContext | None = None,
    ) -> None:
        self.context = context or load_assembly_context(
            request.model,
            request.community_catalog,
            request.official_catalog,
            request.expected_catalog_hashes,
        )
        self.original = request
        self.roles = _role_names(self.context)
        self.baseline: Baseline | None = None
        self.baseline_text = request.baseline
        self.regions: dict[str, RegionCandidate | None] = {
            role: None for role in self.roles
        }
        self.pending_regions = list(request.region_overrides)
        self.mob_sw: MobSwSelection | None = None
        self.flash_ids = request.flash_ids
        self.bootkey_action = request.bootkey_action
        self.bootkey_hash = request.bootkey_hash
        self.bootkey_source = request.bootkey_source
        self.entry_target = request.entry_target
        self.output = output
        self.force = force
        self.remembered_tuple: str | None = None
        self.errors: dict[str, str] = {}
        self.identity_cache: dict[str, str | None] = {}
        if request.mob_sw:
            try:
                self.mob_sw = _resolve_mobsw(self.context, request.mob_sw)
            except FirmwareError as exc:
                self.errors["MobSw"] = str(exc)
        if request.baseline:
            self.select_baseline(request.baseline)

    def select_baseline(self, identifier: str) -> None:
        try:
            baseline = _load_baseline(self.context, identifier)
        except FirmwareError as exc:
            self.baseline = None
            self.baseline_text = identifier
            self.errors["Baseline"] = str(exc)
            return
        self.baseline = baseline
        self.baseline_text = baseline.sha256
        self.errors.pop("Baseline", None)
        if self.pending_regions:
            pending = self.pending_regions
            self.pending_regions = []
            by_role: dict[str, tuple[str, str, str]] = {}
            for raw_role, source, selected in pending:
                try:
                    role = _canonical_role(self.context, raw_role)
                except FirmwareError as exc:
                    self.errors[f"Region:{raw_role}"] = str(exc)
                    continue
                if role in by_role:
                    self.errors[f"Region:{role}"] = (
                        f"region selected more than once: {role}"
                    )
                    continue
                by_role[role] = (role, source, selected)
            for role in self.roles:
                if role not in by_role:
                    continue
                try:
                    resolved = _resolve_explicit_regions(
                        self.context, baseline, (by_role[role],)
                    )[0]
                except (FirmwareError, IndexError) as exc:
                    self.errors[f"Region:{role}"] = str(exc)
                    continue
                self.regions[role] = resolved
                self.errors.pop(f"Region:{role}", None)
                remembered = _tuple_key(self.context, resolved)
                if remembered:
                    self.remembered_tuple = remembered

    def select_region(self, role: str, candidate: RegionCandidate | None) -> None:
        self.regions[role] = candidate
        self.errors.pop(f"Region:{role}", None)
        if candidate is not None:
            remembered = _tuple_key(self.context, candidate)
            if remembered:
                self.remembered_tuple = remembered

    def selected_regions(self) -> tuple[RegionCandidate, ...]:
        return tuple(
            candidate for role in self.roles
            if (candidate := self.regions[role]) is not None
        )

    def validation_errors(self) -> tuple[str, ...]:
        errors = list(self.errors.values())
        if self.baseline is None:
            errors.append("select a baseline")
        if self.pending_regions:
            errors.append("CLI region selections are pending a valid baseline")
        if not 0 <= len(self.flash_ids) <= 2:
            errors.append("statistics accept at most two flash IDs")
        if self.bootkey_action == "write-hash" and (
            self.bootkey_hash is None or len(self.bootkey_hash) != 16
        ):
            errors.append("BOOTKEY digest must be exactly 16 bytes")
        if self.bootkey_action not in ("preserve", "erase", "write-hash"):
            errors.append(f"unsupported BOOTKEY action: {self.bootkey_action}")
        if self.entry_target is not None and not 0 <= self.entry_target <= 0xFFFFFF:
            errors.append("entry target must fit in 24 bits")
        path_error = output_error(self.output)
        if path_error:
            errors.append(path_error)
        errors.extend(_personalization_conflicts(
            self.context,
            self.selected_regions(),
            self.mob_sw,
            self.flash_ids,
            self.bootkey_action,
            self.entry_target,
        ))
        errors.extend(_eeprom_conflicts(
            self.selected_regions(), self.original.eeprom_map,
        ))
        if self.baseline is not None and not self.errors:
            try:
                _effective_scope(
                    self.context, self.baseline, self.selected_regions()
                )
            except FirmwareError as exc:
                errors.append(str(exc))
        return tuple(dict.fromkeys(errors))

    @property
    def build_enabled(self) -> bool:
        return not self.validation_errors()

    def warnings(self) -> tuple[str, ...]:
        if self.baseline is None or self.errors:
            return ()
        try:
            scope = _effective_scope(
                self.context, self.baseline, self.selected_regions()
            )
        except FirmwareError:
            return ()
        return tuple(_compatibility_warnings(
            scope, self.selected_regions(), self.mob_sw
        ))

    def canonical_request(self) -> AssemblyRequest:
        errors = self.validation_errors()
        if errors or self.baseline is None:
            raise FirmwareError(errors[0] if errors else "select a baseline")
        return replace(
            self.original,
            model=self.context.model,
            community_catalog=self.context.community.path,
            official_catalog=self.context.official.path,
            baseline=self.baseline.sha256,
            region_overrides=tuple(
                (
                    item.role,
                    item.source,
                    "all-ff" if item.source == "erased" else item.sha256,
                )
                for item in self.selected_regions()
            ),
            mob_sw=self.mob_sw.sha256 if self.mob_sw else None,
            flash_ids=self.flash_ids,
            bootkey_action=self.bootkey_action,
            bootkey_hash=self.bootkey_hash,
            bootkey_source=self.bootkey_source,
            entry_target=self.entry_target,
            interactive=False,
            expected_catalog_hashes={
                "community": self.context.community.sha256,
                "official": self.context.official.sha256,
            },
        )

    def result(self) -> TuiResult:
        return TuiResult(self.canonical_request(), self.output, self.force)

    def candidate_identity(self, candidate: RegionCandidate) -> str | None:
        if candidate.role != "EEPROM" or candidate.erased:
            return None
        if candidate.sha256 not in self.identity_cache:
            try:
                serial, _source = _imei_serial(
                    _candidate_payload(self.context, candidate)
                )
            except (FirmwareError, OSError, ValueError):
                serial = None
            self.identity_cache[candidate.sha256] = serial
        return self.identity_cache[candidate.sha256]

    def candidate_label(self, candidate: RegionCandidate) -> str:
        tuple_key = _tuple_key(self.context, candidate) or "partial"
        path = candidate.payload_path or "synthetic all-FF"
        identity = self.candidate_identity(candidate)
        suffix = f" IMEI..{identity}" if identity else ""
        return (
            f"{candidate.source} {candidate.sha256[:12]} "
            f"{tuple_key} {path}{suffix}"
        )

    def row_value(self, key: str) -> str:
        if key == "Baseline":
            if self.baseline is not None:
                return _baseline_label(self.baseline.artifact)
            return self.baseline_text or "not selected"
        if key.startswith("Region:"):
            role = key.split(":", 1)[1]
            candidate = self.regions[role]
            return "baseline" if candidate is None else self.candidate_label(candidate)
        if key == "MobSw":
            return "none" if self.mob_sw is None else _mobsw_label(self.mob_sw)
        if key == "Flash IDs":
            return _flash_ids_label(self.flash_ids)
        if key == "BOOTKEY":
            if self.bootkey_action == "preserve":
                return "preserve"
            if self.bootkey_action == "erase":
                return "erase"
            source = (self.bootkey_source or {}).get("kind", "raw")
            digest = self.bootkey_hash.hex()[:12] if self.bootkey_hash else "invalid"
            return f"{source} -> {digest}"
        if key == "Entry Target":
            return "preserve" if self.entry_target is None else f"0x{self.entry_target:06X}"
        if key == "EEPROM Plan":
            profile = self.original.eeprom_profile or "factory map only"
            identity = (
                f" IMEI {self.original.eeprom_imei} FSN {self.original.eeprom_fsn:08X}"
                if self.original.eeprom_imei is not None
                and self.original.eeprom_fsn is not None else ""
            )
            donor = (
                f" donor {self.original.eeprom_donor}"
                if self.original.eeprom_donor is not None else ""
            )
            return f"{profile}: {self.original.eeprom_map}{donor}{identity}"
        if key == "Output":
            return "automatic name" if self.output is None else str(self.output)
        if key == "Force":
            return "yes" if self.force else "no"
        raise KeyError(key)


class AssemblyTui:
    def __init__(self, screen: Any, curses_module: Any, state: AssemblyTuiState) -> None:
        self.screen = screen
        self.curses = curses_module
        self.state = state
        self.cursor = 0
        self.scroll = 0
        self.number_buffer = ""
        self.message = ""
        self.keys = ["Baseline"] + [
            f"Region:{role}" for role in state.roles
        ] + ["MobSw", "Flash IDs", "BOOTKEY", "Entry Target"]
        if state.original.eeprom_map is not None:
            self.keys.append("EEPROM Plan")
        self.keys += ["Output", "Force"]

    def run(self) -> TuiResult:
        try:
            self.curses.curs_set(0)
        except self.curses.error:
            pass
        self.screen.keypad(True)
        while True:
            self._draw_main()
            key = self.screen.getch()
            result = self._handle_main_key(key)
            if result is not None:
                return result

    def _put(self, y: int, x: int, text: str, attr: int = 0) -> None:
        height, width = self.screen.getmaxyx()
        if y < 0 or y >= height or x >= width or width <= 0:
            return
        try:
            self.screen.addnstr(y, max(0, x), text, max(0, width - max(0, x) - 1), attr)
        except self.curses.error:
            pass

    def _draw_main(self) -> None:
        self.screen.erase()
        height, width = self.screen.getmaxyx()
        self._put(0, 0, f"Fullflash Assemble: {self.state.context.model}", self.curses.A_BOLD)
        if height < 8 or width < 36:
            build_index = len(self.keys)
            build_attr = self.curses.A_DIM if not self.state.build_enabled else 0
            self._put(
                1, 0,
                f"{build_index + 1}. Build "
                + ("ready" if self.state.build_enabled else "disabled"),
                build_attr,
            )
            self._put(2, 0, "Resize terminal (minimum 36x8)", self.curses.A_BOLD)
            self.screen.refresh()
            return
        self._put(1, 0, "Configure the image, then activate Build")
        capacity = max(1, height - 7)
        config_count = len(self.keys)
        config_cursor = min(self.cursor, max(0, config_count - 1))
        self.scroll = adjust_scroll(config_cursor, self.scroll, capacity, config_count)
        for line, index in enumerate(
            range(self.scroll, min(config_count, self.scroll + capacity)), start=2
        ):
            key = self.keys[index]
            label = key.split(":", 1)[1] if key.startswith("Region:") else key
            prefix = f"{index + 1:2d}. {label:<14} "
            attr = self.curses.A_REVERSE if self.cursor == index else 0
            self._put(line, 0, prefix + self.state.row_value(key), attr)
        build_index = len(self.keys)
        build_attr = self.curses.A_REVERSE if self.cursor == build_index else 0
        if not self.state.build_enabled:
            build_attr |= self.curses.A_DIM
        self._put(
            max(2, height - 5), 0,
            f"{build_index + 1:2d}. Build          "
            + ("ready" if self.state.build_enabled else "disabled"),
            build_attr,
        )
        errors = self.state.validation_errors()
        warnings = self.state.warnings()
        status = self.message
        if errors:
            status = "Error: " + errors[0]
        elif warnings:
            status = "Warning: " + warnings[0]
        if self.number_buffer:
            status = f"Row: {self.number_buffer}"
        self._put(max(3, height - 3), 0, status)
        self._put(
            max(4, height - 2), 0,
            "Arrows move  Enter/Right edit  PgUp/PgDn scroll  number+Enter activate  q cancel",
            self.curses.A_DIM,
        )
        self.screen.refresh()

    def _handle_main_key(self, key: int) -> TuiResult | None:
        count = len(self.keys) + 1
        buffer, activated = numeric_activation(self.number_buffer, key, count)
        if ord("0") <= key <= ord("9"):
            self.number_buffer = buffer
            return None
        if key in (10, 13) and self.number_buffer:
            self.number_buffer = ""
            if activated is not None:
                self.cursor = activated
                return self._activate()
            self.message = "No such row"
            return None
        self.number_buffer = ""
        if key in (ord("q"), ord("Q")):
            raise _Cancelled()
        if key == self.curses.KEY_UP:
            self.cursor = (self.cursor - 1) % count
        elif key == self.curses.KEY_DOWN:
            self.cursor = (self.cursor + 1) % count
        elif key == self.curses.KEY_PPAGE:
            self.cursor = max(0, self.cursor - max(1, self.screen.getmaxyx()[0] - 7))
        elif key == self.curses.KEY_NPAGE:
            self.cursor = min(count - 1, self.cursor + max(1, self.screen.getmaxyx()[0] - 7))
        elif key in (10, 13, self.curses.KEY_RIGHT):
            return self._activate()
        elif key in (27, self.curses.KEY_LEFT, self.curses.KEY_RESIZE):
            self.message = "" if key == self.curses.KEY_RESIZE else "Use q to cancel"
        return None

    def _activate(self) -> TuiResult | None:
        if self.cursor == len(self.keys):
            if self.state.build_enabled:
                return self.state.result()
            self.message = self.state.validation_errors()[0]
            return None
        key = self.keys[self.cursor]
        if key == "Baseline":
            self._edit_baseline()
        elif key.startswith("Region:"):
            self._edit_region(key.split(":", 1)[1])
        elif key == "MobSw":
            self._edit_mobsw()
        elif key == "Flash IDs":
            self._edit_flash_ids()
        elif key == "BOOTKEY":
            self._edit_bootkey()
        elif key == "Entry Target":
            self._edit_entry_target()
        elif key == "EEPROM Plan":
            self.message = "EEPROM plan is configured by command-line options"
        elif key == "Output":
            self._edit_output()
        elif key == "Force":
            self.state.force = not self.state.force
        return None

    def _picker(self, title: str, items: list[PickerItem]) -> Any:
        if not items:
            self.message = "No choices are available"
            return _CANCEL
        current = next((i for i, item in enumerate(items) if item.current), 0)
        scroll = 0
        number = ""
        while True:
            self.screen.erase()
            height, _width = self.screen.getmaxyx()
            self._put(0, 0, title, self.curses.A_BOLD)
            capacity = max(1, height - 4)
            scroll = adjust_scroll(current, scroll, capacity, len(items))
            for line, index in enumerate(
                range(scroll, min(len(items), scroll + capacity)), start=1
            ):
                item = items[index]
                attr = self.curses.A_REVERSE if index == current else 0
                if not item.enabled:
                    attr |= self.curses.A_DIM
                marker = "*" if item.current else " "
                self._put(line, 0, f"{index + 1:2d}{marker} {item.label}", attr)
            status = f"Choice: {number}" if number else "Esc/Left returns; q cancels assembly"
            self._put(max(1, height - 2), 0, status, self.curses.A_DIM)
            self.screen.refresh()
            key = self.screen.getch()
            buffer, activated = numeric_activation(number, key, len(items))
            if ord("0") <= key <= ord("9"):
                number = buffer
                continue
            if key in (10, 13) and number:
                number = ""
                if activated is not None:
                    current = activated
                    if items[current].enabled:
                        return items[current].value
                continue
            number = ""
            if key in (ord("q"), ord("Q")):
                raise _Cancelled()
            if key in (27, self.curses.KEY_LEFT):
                return _CANCEL
            if key == self.curses.KEY_UP:
                current = (current - 1) % len(items)
            elif key == self.curses.KEY_DOWN:
                current = (current + 1) % len(items)
            elif key == self.curses.KEY_PPAGE:
                current = max(0, current - max(1, capacity))
            elif key == self.curses.KEY_NPAGE:
                current = min(len(items) - 1, current + max(1, capacity))
            elif key in (10, 13, self.curses.KEY_RIGHT) and items[current].enabled:
                return items[current].value

    def _text_editor(self, title: str, initial: str) -> str | object:
        value = initial
        try:
            self.curses.curs_set(1)
        except self.curses.error:
            pass
        try:
            while True:
                self.screen.erase()
                height, width = self.screen.getmaxyx()
                self._put(0, 0, title, self.curses.A_BOLD)
                visible = value[-max(1, width - 2):]
                self._put(2, 0, visible)
                self._put(max(3, height - 2), 0, "Enter accepts; Esc returns", self.curses.A_DIM)
                try:
                    self.screen.move(2, min(len(visible), max(0, width - 2)))
                except self.curses.error:
                    pass
                self.screen.refresh()
                key = self.screen.get_wch()
                if isinstance(key, str):
                    if key in ("\n", "\r"):
                        return value
                    if key == "\x1b":
                        return _CANCEL
                    if key in ("\b", "\x7f"):
                        value = value[:-1]
                    elif key.isprintable():
                        value += key
                elif key in (self.curses.KEY_BACKSPACE, 127, 8):
                    value = value[:-1]
                elif key == self.curses.KEY_RESIZE:
                    continue
        finally:
            try:
                self.curses.curs_set(0)
            except self.curses.error:
                pass

    def _edit_baseline(self) -> None:
        artifacts = _baseline_artifacts(self.state.context)
        selected = self._picker("Select community baseline", [
            PickerItem(
                artifact,
                _baseline_label(artifact),
                current=(
                    self.state.baseline is not None
                    and artifact.get("sha256") == self.state.baseline.sha256
                ),
            )
            for artifact in artifacts
        ])
        if selected is not _CANCEL:
            self.state.select_baseline(str(selected["sha256"]))

    def _edit_region(self, role: str) -> None:
        current = self.state.regions[role]
        available = {
            source: any(
                not candidate.erased
                for candidate in region_candidates(self.state.context, source, role)
            )
            for source in ("official", "community")
        }
        action = self._picker(f"Select action for {role}", [
            PickerItem("baseline", "baseline bytes", current=current is None),
            PickerItem(
                "official", "official catalog payload",
                enabled=available["official"],
                current=current is not None and current.source == "official",
            ),
            PickerItem(
                "community", "community catalog payload",
                enabled=available["community"],
                current=current is not None and current.source == "community",
            ),
            PickerItem(
                "erase", "synthetic all-FF payload",
                current=current is not None and current.source == "erased",
            ),
        ])
        if action is _CANCEL:
            return
        if action == "baseline":
            self.state.select_region(role, None)
        elif action == "erase":
            self.state.select_region(role, _erased_candidate(self.state.context, role))
        else:
            candidate = self._pick_source_candidate(action, role, current)
            if candidate is not _CANCEL:
                self.state.select_region(role, candidate)

    def _pick_source_candidate(
        self,
        source: str,
        role: str,
        current: RegionCandidate | None,
    ) -> RegionCandidate | object:
        candidates = [
            item for item in region_candidates(self.state.context, source, role)
            if not item.erased
        ]
        groups: dict[str, list[RegionCandidate]] = {}
        for candidate in candidates:
            groups.setdefault(_selection_key(self.state.context, candidate), []).append(candidate)
        keys = sorted(groups, key=lambda value: (
            0 if len(value) == 6 and value.isdigit() else 1, value,
        ))
        current_key = (
            _selection_key(self.state.context, current)
            if current is not None and current.source == source else None
        )
        default_key = current_key or (
            self.state.remembered_tuple if self.state.remembered_tuple in groups else None
        )
        selected_key = self._picker(f"Select {source} {role} tuple", [
            PickerItem(
                key,
                f"{key}  " + (
                    f"{len({item.sha256 for item in groups[key]})} payloads"
                    if len({item.sha256 for item in groups[key]}) > 1
                    else self.state.candidate_label(groups[key][0])
                ),
                current=key == default_key,
            )
            for key in keys
        ])
        if selected_key is _CANCEL:
            return _CANCEL
        selected = groups[selected_key]
        payloads = sorted({item.sha256 for item in selected})
        if len(payloads) > 1:
            prefixes = _short_hashes(selected)
            digest = self._picker(
                f"Select {source} {role} payload for {selected_key}",
                [
                    PickerItem(
                        digest,
                        f"{prefixes[digest]}  " + self.state.candidate_label(next(
                            item for item in selected if item.sha256 == digest
                        )),
                        current=(current is not None and current.sha256 == digest),
                    )
                    for digest in payloads
                ],
            )
            if digest is _CANCEL:
                return _CANCEL
            selected = [item for item in selected if item.sha256 == digest]
        return sorted(selected, key=lambda item: (item.sha256, item.payload_path or ""))[0]

    def _edit_mobsw(self) -> None:
        selected = self._picker("Select official MobSw package", [
            PickerItem(None, "none", current=self.state.mob_sw is None),
            *[
                PickerItem(
                    item, _mobsw_label(item),
                    current=(
                        self.state.mob_sw is not None
                        and item.sha256 == self.state.mob_sw.sha256
                    ),
                )
                for item in _mobsw_selections(self.state.context)
            ],
        ])
        if selected is not _CANCEL:
            self.state.mob_sw = selected
            self.state.errors.pop("MobSw", None)

    def _edit_flash_ids(self) -> None:
        action = self._picker("Flash ID mode", [
            PickerItem("baseline", "baseline descriptors", current=not self.state.flash_ids),
            PickerItem("custom", "one or two MFR:DEVICE values", current=bool(self.state.flash_ids)),
        ])
        if action is _CANCEL:
            return
        if action == "baseline":
            self.state.flash_ids = ()
            self.state.errors.pop("Flash IDs", None)
            self.message = ""
            return
        value = self._text_editor("Flash IDs (comma or space separated)", _flash_ids_label(
            self.state.flash_ids
        ) if self.state.flash_ids else "")
        if value is _CANCEL:
            return
        try:
            self.state.flash_ids = parse_flash_ids(str(value))
            self.message = ""
        except ValueError as exc:
            self.state.errors["Flash IDs"] = str(exc)
            self.message = str(exc)
        else:
            self.state.errors.pop("Flash IDs", None)

    def _edit_bootkey(self) -> None:
        action = self._picker("BOOTKEY mode", [
            PickerItem("preserve", "preserve baseline field", current=self.state.bootkey_action == "preserve"),
            PickerItem("erase", "erase digest field", current=self.state.bootkey_action == "erase"),
            PickerItem("raw", "raw 16-byte BOOTKEY", current=(self.state.bootkey_source or {}).get("kind") == "bootkey"),
            PickerItem("fsn", "derive from FSN and SKey", current=(self.state.bootkey_source or {}).get("kind") == "fsn-skey"),
        ])
        if action is _CANCEL:
            return
        if action == "preserve":
            self.state.errors.pop("BOOTKEY", None)
            self.state.bootkey_action = "preserve"
            self.state.bootkey_hash = None
            self.state.bootkey_source = None
        elif action == "erase":
            self.state.errors.pop("BOOTKEY", None)
            self.state.bootkey_action = "erase"
            self.state.bootkey_hash = None
            self.state.bootkey_source = {"kind": "erased"}
        elif action == "raw":
            initial = str((self.state.bootkey_source or {}).get("bootkey", ""))
            value = self._text_editor("BOOTKEY (32 hexadecimal digits)", initial)
            if value is _CANCEL:
                return
            try:
                digest, source = parse_raw_bootkey(str(value))
            except ValueError as exc:
                self.state.errors["BOOTKEY"] = str(exc)
                self.message = str(exc)
                return
            self.state.errors.pop("BOOTKEY", None)
            self.state.bootkey_action = "write-hash"
            self.state.bootkey_hash = digest
            self.state.bootkey_source = source
        else:
            source = self.state.bootkey_source or {}
            initial = ""
            if source.get("kind") == "fsn-skey":
                initial = f"{source.get('fsn', '')} {source.get('skey', '')}"
            value = self._text_editor("FSN SKey", initial)
            if value is _CANCEL:
                return
            try:
                digest, source = parse_fsn_skey(str(value))
            except ValueError as exc:
                self.state.errors["BOOTKEY"] = str(exc)
                self.message = str(exc)
                return
            self.state.errors.pop("BOOTKEY", None)
            self.state.bootkey_action = "write-hash"
            self.state.bootkey_hash = digest
            self.state.bootkey_source = source
        self.state.errors.pop("BOOTKEY", None)
        self.message = ""

    def _edit_entry_target(self) -> None:
        action = self._picker("Entry target mode", [
            PickerItem("preserve", "preserve baseline JMPS", current=self.state.entry_target is None),
            PickerItem("custom", "write a 24-bit C166 target", current=self.state.entry_target is not None),
        ])
        if action is _CANCEL:
            return
        if action == "preserve":
            self.state.entry_target = None
            self.state.errors.pop("Entry Target", None)
            self.message = ""
            return
        initial = "" if self.state.entry_target is None else f"0x{self.state.entry_target:06X}"
        value = self._text_editor("Entry target", initial)
        if value is _CANCEL:
            return
        try:
            self.state.entry_target = parse_entry_target(str(value))
            self.message = ""
        except ValueError as exc:
            self.state.errors["Entry Target"] = str(exc)
            self.message = str(exc)
        else:
            self.state.errors.pop("Entry Target", None)

    def _edit_output(self) -> None:
        action = self._picker("Output path mode", [
            PickerItem("auto", "automatic deterministic name", current=self.state.output is None),
            PickerItem("custom", "custom binary path", current=self.state.output is not None),
        ])
        if action is _CANCEL:
            return
        if action == "auto":
            self.state.output = None
            self.state.errors.pop("Output", None)
            self.message = ""
            return
        value = self._text_editor(
            "Binary output path", "" if self.state.output is None else str(self.state.output)
        )
        if value is _CANCEL:
            return
        if not str(value).strip():
            self.state.errors["Output"] = "output path must not be empty"
            self.message = self.state.errors["Output"]
            return
        self.state.output = Path(str(value)).expanduser()
        error = output_error(self.state.output)
        if error:
            self.state.errors["Output"] = error
        else:
            self.state.errors.pop("Output", None)
        self.message = error or ""


def run_assembly_tui(
    request: AssemblyRequest,
    *,
    output: Path | None,
    force: bool,
) -> TuiResult:
    state = AssemblyTuiState(request, output=output, force=force)
    try:
        curses = importlib.import_module("curses")
    except ImportError as exc:
        raise FirmwareError(
            "curses is unavailable; rerun with --non-interactive and --baseline"
        ) from exc
    try:
        return curses.wrapper(lambda screen: AssemblyTui(screen, curses, state).run())
    except _Cancelled:
        raise FirmwareError("assembly cancelled") from None
    except curses.error as exc:
        raise FirmwareError(
            "cannot initialize the terminal UI; rerun with --non-interactive "
            "and --baseline"
        ) from exc
