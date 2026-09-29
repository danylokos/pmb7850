from __future__ import annotations

import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from tools.siemens_tools.firmware.xbi import FirmwareError
from tools.siemens_tools.fullflash.assembly import (
    _SUPPORTED_MODELS,
    _baseline_artifacts,
    _erased_candidate,
    _mobsw_selections,
    _role_names,
    _tuple_key,
    AssemblyRequest,
    region_candidates,
)
from tools.siemens_tools.fullflash.assembly_tui import (
    _CANCEL,
    AssemblyTui,
    AssemblyTuiState,
    TuiResult,
    adjust_scroll,
    numeric_activation,
    output_error,
    parse_entry_target,
    parse_flash_ids,
    parse_fsn_skey,
    parse_raw_bootkey,
    run_assembly_tui,
)
from tools.siemens_tools.fullflash.cli import build_parser
from tools.siemens_tools.fullflash import assembly

from .bundled_fixtures import catalogs, context as load_assembly_context, ambiguous_t9
from tools.bundled_firmware import entry as bundled_entry


C55_BASELINE = bundled_entry("c55", langpack=91)["sha256"][:12]
C55_T9 = "06aca91c3667"
C55_MOBSW = "724c44ebaa95"


class FakeScreen:
    def __init__(self, keys: list[object], height: int = 24, width: int = 100) -> None:
        self.keys = list(keys)
        self.height = height
        self.width = width
        self.keypad_enabled = False
        self.refreshes = 0

    def keypad(self, enabled: bool) -> None:
        self.keypad_enabled = enabled

    def getmaxyx(self) -> tuple[int, int]:
        return self.height, self.width

    def erase(self) -> None:
        pass

    def addnstr(self, *_args: object) -> None:
        pass

    def refresh(self) -> None:
        self.refreshes += 1

    def move(self, *_args: object) -> None:
        pass

    def getch(self) -> int:
        value = self.keys.pop(0)
        if isinstance(value, BaseException):
            raise value
        return int(value)

    def get_wch(self) -> object:
        value = self.keys.pop(0)
        if isinstance(value, BaseException):
            raise value
        return value


class FakeCurses:
    class error(Exception):
        pass

    A_BOLD = 1
    A_REVERSE = 2
    A_DIM = 4
    KEY_UP = 1001
    KEY_DOWN = 1002
    KEY_LEFT = 1003
    KEY_RIGHT = 1004
    KEY_PPAGE = 1005
    KEY_NPAGE = 1006
    KEY_RESIZE = 1007
    KEY_BACKSPACE = 1008

    def __init__(self, screen: FakeScreen) -> None:
        self.screen = screen
        self.entered = False
        self.cleaned = False
        self.cursor = 0

    def curs_set(self, value: int) -> None:
        self.cursor = value

    def wrapper(self, function: object) -> object:
        self.entered = True
        try:
            return function(self.screen)  # type: ignore[operator]
        finally:
            self.cleaned = True


class AssemblyTuiStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.context = load_assembly_context("C55")

    def request(self, **overrides: object) -> AssemblyRequest:
        values = {
            "model": "C55",
            "community_catalog": self.context.community.path,
            "official_catalog": self.context.official.path,
            "baseline": None,
            "region_overrides": (),
            "mob_sw": None,
            "flash_ids": (),
            "bootkey_action": "preserve",
            "bootkey_hash": None,
            "bootkey_source": None,
            "entry_target": None,
            "interactive": True,
        }
        values.update(overrides)
        return AssemblyRequest(**values)  # type: ignore[arg-type]

    def state(self, **request_overrides: object) -> AssemblyTuiState:
        return AssemblyTuiState(
            self.request(**request_overrides),
            output=None,
            force=False,
            context=self.context,
        )

    def test_empty_state_requires_baseline_and_has_every_layout_row(self) -> None:
        state = self.state()
        self.assertFalse(state.build_enabled)
        self.assertIn("select a baseline", state.validation_errors())
        self.assertEqual(tuple(state.regions), _role_names(self.context))
        self.assertTrue(all(value is None for value in state.regions.values()))

    def test_cli_prefill_resolves_to_canonical_request(self) -> None:
        bootkey = bytes.fromhex("00112233445566778899aabbccddeeff")
        digest, source = parse_raw_bootkey(bootkey.hex())
        state = self.state(
            baseline=C55_BASELINE,
            region_overrides=(("T9", "official", C55_T9),),
            mob_sw=C55_MOBSW,
            flash_ids=((0x20, 0x17), (0x89, 0x18)),
            bootkey_action="write-hash",
            bootkey_hash=digest,
            bootkey_source=source,
            entry_target=0x07FFF0,
        )
        request = state.canonical_request()
        self.assertFalse(request.interactive)
        self.assertEqual(request.baseline[:12], C55_BASELINE)
        self.assertEqual(request.region_overrides[0][0:2], ("T9", "official"))
        self.assertEqual(len(request.region_overrides[0][2]), 64)
        self.assertEqual(request.mob_sw[:12], C55_MOBSW)
        self.assertEqual(request.flash_ids, ((0x20, 0x17), (0x89, 0x18)))
        self.assertEqual(request.entry_target, 0x07FFF0)
        self.assertEqual(set(request.expected_catalog_hashes or {}), {"community", "official"})

    def test_cli_regions_seed_remembered_tuple_in_layout_order(self) -> None:
        state = self.state(
            baseline=C55_BASELINE,
            region_overrides=(
                ("LangPack", "official", "249111"),
                ("UNKNOWN_1", "official", "240101"),
            ),
        )
        self.assertEqual(state.remembered_tuple, "249111")
        self.assertEqual(
            [item.role for item in state.selected_regions()],
            ["UNKNOWN_1", "LangPack"],
        )

    def test_baseline_change_preserves_explicit_overrides_and_remembered_tuple(self) -> None:
        state = self.state(baseline=C55_BASELINE)
        candidate = next(
            item for item in region_candidates(self.context, "official", "LangPack")
            if _tuple_key(self.context, item) == "249111"
        )
        state.select_region("LangPack", candidate)
        alternative = next(
            item for item in _baseline_artifacts(self.context)
            if not str(item["sha256"]).startswith(C55_BASELINE)
        )
        state.select_baseline(str(alternative["sha256"]))
        self.assertIs(state.regions["LangPack"], candidate)
        self.assertEqual(state.remembered_tuple, "249111")

    def test_validation_covers_conflicts_and_output_suffix(self) -> None:
        state = self.state(baseline=C55_BASELINE)
        state.select_region("UNKNOWN_1", _erased_candidate(self.context, "UNKNOWN_1"))
        state.mob_sw = _mobsw_selections(self.context)[0]
        state.flash_ids = ((0x20, 0x17),)
        state.entry_target = 0x07FFF0
        state.output = Path("bad.json")
        errors = "\n".join(state.validation_errors())
        self.assertIn("--mobsw conflicts", errors)
        self.assertIn("--flash-id conflicts", errors)
        self.assertIn("--entry-target conflicts", errors)
        self.assertIn(".json suffix", errors)
        self.assertFalse(state.build_enabled)
        state.mob_sw = None
        state.flash_ids = ()
        state.entry_target = None
        state.output = Path("good.bin")
        self.assertTrue(state.build_enabled)

    def test_cli_eeprom_plan_is_visible_and_conflicts_with_region_selection(self) -> None:
        state = self.state(
            baseline=C55_BASELINE,
            eeprom_map=Path("factory.map"),
            eeprom_profile="c55-emulator-minimal-v1",
            eeprom_donor=Path("donor.bin"),
            eeprom_imei="35335000894548",
            eeprom_fsn=0xA35F2F28,
        )
        self.assertIn("c55-emulator-minimal-v1", state.row_value("EEPROM Plan"))
        self.assertIn("A35F2F28", state.row_value("EEPROM Plan"))
        self.assertIn("donor.bin", state.row_value("EEPROM Plan"))
        state.select_region("EEPROM", _erased_candidate(self.context, "EEPROM"))
        self.assertIn(
            "--eeprom-map conflicts",
            "\n".join(state.validation_errors()),
        )

    def test_bcore_erasure_allows_erase_but_rejects_write(self) -> None:
        state = self.state(baseline=C55_BASELINE)
        state.select_region("BCORE", _erased_candidate(self.context, "BCORE"))
        state.bootkey_action = "write-hash"
        state.bootkey_hash = b"\0" * 16
        self.assertIn("BOOTKEY write conflicts", "\n".join(state.validation_errors()))
        state.bootkey_action = "erase"
        state.bootkey_hash = None
        self.assertTrue(state.build_enabled)

    def test_candidate_labels_include_tuple_source_path_and_eeprom_identity(self) -> None:
        state = self.state()
        candidate = next(
            item for item in region_candidates(self.context, "community", "EEPROM")
            if not item.erased
        )
        state.identity_cache[candidate.sha256] = "556677"
        label = state.candidate_label(candidate)
        self.assertIn("community", label)
        self.assertIn(candidate.sha256[:12], label)
        self.assertIn(candidate.payload_path or "", label)
        self.assertIn("IMEI..556677", label)

    def test_every_supported_model_exposes_all_layout_rows(self) -> None:
        for model in _SUPPORTED_MODELS:
            with self.subTest(model=model):
                context = load_assembly_context(model)
                corpus = catalogs(model)
                request = AssemblyRequest(
                    model, corpus / "community", corpus / "official", None,
                    (), None, (), "preserve", None, None, None, True,
                )
                state = AssemblyTuiState(
                    request, output=None, force=False, context=context
                )
                self.assertEqual(tuple(state.regions), _role_names(context))

    def test_pure_parsers_and_scroll_helpers(self) -> None:
        self.assertEqual(parse_flash_ids("baseline"), ())
        self.assertEqual(parse_flash_ids("0x20:0x17, 0x89:0x18"), (
            (0x20, 0x17), (0x89, 0x18),
        ))
        with self.assertRaisesRegex(ValueError, "one or two"):
            parse_flash_ids("1:2 3:4 5:6")
        self.assertIsNone(parse_entry_target("preserve"))
        self.assertEqual(parse_entry_target("0xabcdef"), 0xABCDEF)
        with self.assertRaisesRegex(ValueError, "24 bits"):
            parse_entry_target("0x1000000")
        digest, source = parse_fsn_skey("A35F2F28 12345678")
        self.assertEqual(len(digest), 16)
        self.assertEqual(source["kind"], "fsn-skey")
        raw_digest, raw_source = parse_raw_bootkey("00" * 16)
        self.assertEqual(len(raw_digest), 16)
        self.assertEqual(raw_source["bootkey"], "00" * 16)
        self.assertEqual(output_error(Path("bad.json")), "binary output path must not use the .json suffix")
        self.assertIsNone(output_error(Path("good.bin")))
        self.assertEqual(adjust_scroll(8, 0, 4, 12), 5)
        self.assertEqual(adjust_scroll(1, 5, 4, 12), 1)
        buffer, selected = numeric_activation("", ord("1"), 20)
        self.assertEqual((buffer, selected), ("1", None))
        buffer, selected = numeric_activation(buffer, ord("8"), 20)
        self.assertEqual(numeric_activation(buffer, 10, 20), ("", 17))


class AssemblyTuiInteractionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.context = load_assembly_context("C55")
        corpus = catalogs("C55")
        cls.request = AssemblyRequest(
            "C55", corpus / "community", corpus / "official", None,
            (), None, (), "preserve", None, None, None, True,
        )

    def test_fresh_tui_requires_both_terminal_streams(self) -> None:
        args = build_parser().parse_args(["assemble", "C55", "--community-catalog", str(catalogs("C55") / "community"), "--official-catalog", str(catalogs("C55") / "official")])
        terminal = SimpleNamespace(isatty=lambda: True)
        redirected = SimpleNamespace(isatty=lambda: False)
        with (
            mock.patch.object(assembly.sys, "stdin", terminal),
            mock.patch.object(assembly.sys, "stdout", terminal),
        ):
            self.assertTrue(assembly._request_from_args(args).interactive)
        with (
            mock.patch.object(assembly.sys, "stdin", terminal),
            mock.patch.object(assembly.sys, "stdout", redirected),
        ):
            self.assertFalse(assembly._request_from_args(args).interactive)

    def test_mocked_curses_build_returns_canonical_request_and_cleans_up(self) -> None:
        # Enter opens Baseline, Enter selects its first candidate, row 18 activates Build.
        screen = FakeScreen([10, 10, ord("1"), ord("8"), 10])
        curses = FakeCurses(screen)
        with mock.patch(
            "tools.siemens_tools.fullflash.assembly_tui.importlib.import_module",
            return_value=curses,
        ):
            result = run_assembly_tui(self.request, output=Path("chosen.bin"), force=True)
        self.assertTrue(curses.entered)
        self.assertTrue(curses.cleaned)
        self.assertFalse(result.request.interactive)
        self.assertEqual(result.output, Path("chosen.bin"))
        self.assertTrue(result.force)
        self.assertEqual(len(result.request.baseline or ""), 64)
        self.assertFalse(Path("chosen.bin").exists())

    def test_resize_redraws_and_q_cancel_cleans_terminal(self) -> None:
        screen = FakeScreen([FakeCurses.KEY_RESIZE, ord("q")])
        curses = FakeCurses(screen)
        with (
            mock.patch(
                "tools.siemens_tools.fullflash.assembly_tui.importlib.import_module",
                return_value=curses,
            ),
            self.assertRaisesRegex(FirmwareError, "cancelled"),
        ):
            run_assembly_tui(self.request, output=None, force=False)
        self.assertGreaterEqual(screen.refreshes, 2)
        self.assertTrue(curses.cleaned)

    def test_interruption_and_runtime_error_still_cleanup(self) -> None:
        for failure in (KeyboardInterrupt(), RuntimeError("screen failed")):
            with self.subTest(failure=type(failure).__name__):
                curses = FakeCurses(FakeScreen([failure]))
                with (
                    mock.patch(
                        "tools.siemens_tools.fullflash.assembly_tui.importlib.import_module",
                        return_value=curses,
                    ),
                    self.assertRaises(type(failure)),
                ):
                    run_assembly_tui(self.request, output=None, force=False)
                self.assertTrue(curses.cleaned)

    def test_curses_initialization_error_is_actionable(self) -> None:
        curses = FakeCurses(FakeScreen([]))
        def fail(_function: object) -> object:
            raise curses.error("no terminal")
        curses.wrapper = fail  # type: ignore[method-assign]
        with (
            mock.patch(
                "tools.siemens_tools.fullflash.assembly_tui.importlib.import_module",
                return_value=curses,
            ),
            self.assertRaisesRegex(FirmwareError, "--non-interactive"),
        ):
            run_assembly_tui(self.request, output=None, force=False)

    def test_source_picker_groups_duplicate_tuple_and_selects_sha(self) -> None:
        state = AssemblyTuiState(
            self.request, output=None, force=False, context=self.context
        )
        app = AssemblyTui(FakeScreen([]), FakeCurses(FakeScreen([])), state)
        seen: list[list[object]] = []
        def choose(_title: str, items: list[object]) -> object:
            seen.append(items)
            return "249111" if len(seen) == 1 else "60687cc2f2617d340574875083e439c10e76524ea0db9dbd9c42d2039ff8711d"
        app._picker = choose  # type: ignore[method-assign]
        with mock.patch("tools.siemens_tools.fullflash.assembly_tui.region_candidates",
                        return_value=ambiguous_t9(self.context)):
            selected = app._pick_source_candidate("community", "T9", None)
        self.assertNotEqual(selected, _CANCEL)
        self.assertEqual(selected.sha256[:12], "60687cc2f261")  # type: ignore[union-attr]
        tuple_item = next(
            item for item in seen[0] if item.value == "249111"  # type: ignore[union-attr]
        )
        self.assertIn("2 payloads", tuple_item.label)  # type: ignore[union-attr]
        self.assertGreater(len(seen[1]), 1)

    def test_personalization_editors_update_state(self) -> None:
        state = AssemblyTuiState(
            self.request, output=None, force=False, context=self.context
        )
        app = AssemblyTui(FakeScreen([]), FakeCurses(FakeScreen([])), state)

        app._picker = lambda *_args: "custom"  # type: ignore[method-assign]
        app._text_editor = lambda *_args: "0x20:0x17 0x89:0x18"  # type: ignore[method-assign]
        app._edit_flash_ids()
        self.assertEqual(len(state.flash_ids), 2)

        app._text_editor = lambda *_args: "bad"  # type: ignore[method-assign]
        app._edit_flash_ids()
        self.assertIn("Flash IDs", state.errors)
        app._text_editor = lambda *_args: "0x20:0x17"  # type: ignore[method-assign]
        app._edit_flash_ids()
        self.assertNotIn("Flash IDs", state.errors)

        app._picker = lambda *_args: "raw"  # type: ignore[method-assign]
        app._text_editor = lambda *_args: "11" * 16  # type: ignore[method-assign]
        app._edit_bootkey()
        self.assertEqual(state.bootkey_action, "write-hash")
        self.assertEqual((state.bootkey_source or {})["kind"], "bootkey")

        app._picker = lambda *_args: "fsn"  # type: ignore[method-assign]
        app._text_editor = lambda *_args: "A35F2F28 12345678"  # type: ignore[method-assign]
        app._edit_bootkey()
        self.assertEqual((state.bootkey_source or {})["kind"], "fsn-skey")

        app._picker = lambda *_args: "erase"  # type: ignore[method-assign]
        app._edit_bootkey()
        self.assertEqual(state.bootkey_action, "erase")

        app._picker = lambda *_args: "custom"  # type: ignore[method-assign]
        app._text_editor = lambda *_args: "0x07fff0"  # type: ignore[method-assign]
        app._edit_entry_target()
        self.assertEqual(state.entry_target, 0x07FFF0)

        app._text_editor = lambda *_args: "/tmp/custom.json"  # type: ignore[method-assign]
        app._edit_output()
        self.assertIn("Output", state.errors)
        app._text_editor = lambda *_args: "/tmp/custom.bin"  # type: ignore[method-assign]
        app._edit_output()
        self.assertEqual(state.output, Path("/tmp/custom.bin"))
        self.assertNotIn("Output", state.errors)

        app._picker = lambda *_args: "preserve"  # type: ignore[method-assign]
        app._edit_entry_target()
        self.assertIsNone(state.entry_target)

    def test_command_orders_tui_before_assembly_and_write(self) -> None:
        parser = build_parser()
        args = parser.parse_args(["assemble", "C55", "-o", "/tmp/tui-order.bin", "--community-catalog", str(catalogs("C55") / "community"), "--official-catalog", str(catalogs("C55") / "official")])
        selected_request = AssemblyRequest(
            **{**self.request.__dict__, "baseline": C55_BASELINE, "interactive": False}
        )
        selected = TuiResult(selected_request, Path("/tmp/tui-order.bin"), True)
        events: list[str] = []
        fake_result = SimpleNamespace(image=b"image", warnings=())

        def select(*_args: object, **_kwargs: object) -> TuiResult:
            events.append("tui")
            return selected
        def assemble_request(_request: AssemblyRequest) -> object:
            events.append("assemble")
            return fake_result
        def write(*_args: object, **_kwargs: object) -> tuple[Path, str]:
            events.append("write")
            return Path("/tmp/tui-order.json"), "written"

        with (
            mock.patch.object(assembly, "_request_from_args", return_value=self.request),
            mock.patch(
                "tools.siemens_tools.fullflash.assembly_tui.run_assembly_tui",
                side_effect=select,
            ),
            mock.patch.object(assembly, "_assemble_request", side_effect=assemble_request),
            mock.patch.object(assembly, "_recipe_document", return_value={}),
            mock.patch.object(assembly, "_write_output_pair", side_effect=write),
            mock.patch("builtins.print"),
        ):
            assembly.command_assemble(args)
        self.assertEqual(events, ["tui", "assemble", "write"])


if __name__ == "__main__":
    unittest.main()
