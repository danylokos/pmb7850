#!/usr/bin/env python3
"""Prove the C port (cemu) matches the Python emulator by state, not by trace.

Runs the Python emulator (pemu/) from reset to a given icount, then compares its
CPU registers + SFR words + RAM bytes against a snapshot the C driver wrote at the
SAME icount (`cemu --snapshot-out FILE --snapshot-at N`). Reports the FIRST
divergence (first differing SFR/RAM address) in the project's "compare by side
effects / surface the first divergence" style, or confirms byte-for-byte parity.

Two comparison sources:
  * --python-run  : boot the Python emulator from reset to the icount (slow; the
                    definitive from-reset check).
  * --python-snapshot DIR : diff against an existing Python EmuSnapshot directory
                    (fast; no re-run — reuses a snapshot the emulator already wrote).

Usage:
    # C side: dump state at icount N (N = the Python snapshot's icount). The
    # snapshot lands in shots/<auto-or-label>/snapshot/ (a directory).
    cemu <flash> --snapshot-at N --snapshot --limit <>=N>

    # (a) fast: reuse an existing Python snapshot dir
    python cmp_snapshot.py --c-snapshot shots/<run>/snapshot --python-snapshot pemu/shots/snapshots/pre-e002
    # (b) definitive: re-run Python from reset
    python cmp_snapshot.py <flash> --icount N --c-snapshot shots/<run>/snapshot --python-run [--device NAME]

Exit code 0 on full parity, 1 on any divergence.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Put pemu/src on the path (this file lives at cemu/tools/).
_EMU_SRC = Path(__file__).resolve().parents[2] / "pemu" / "src"
sys.path.insert(0, str(_EMU_SRC))

def run_python(flash_path: str, icount: int, device: str | None) -> dict:
    """Boot the Python emulator from reset to `icount` (faithful CLI defaults)."""
    from cpu.c166 import CPU, UnimplementedOpcode
    from soc.pmb7850 import FlashImage, MemoryAccessError, PMB7850
    from soc.devices import resolve_device

    data = Path(flash_path).read_bytes()
    cfg = resolve_device(device, data)
    soc = PMB7850(
        FlashImage(bytes(data), bases=cfg.flash_bases),
        trace=False,
        low_ram_overlay=True,               # boot.py CLI default
        boot_straps=cfg.boot_straps,
    )
    cpu = CPU(soc, logger=soc.cpu_log, trace=False)
    soc.attach_cpu(cpu)
    cpu.reset()
    try:
        while cpu.icount < icount:
            cpu.step()
            if cpu.halted:
                break
            if cpu.idle and not soc.idle_wake_possible():
                break
    except (UnimplementedOpcode, MemoryAccessError) as exc:
        print(f"python stopped early at icount {cpu.icount}: {exc}", file=sys.stderr)
    return {
        "icount": cpu.icount, "pc": cpu.pc(),
        "csp": cpu.csp, "ip": cpu.ip,
        "sfr": {a: v & 0xFFFF for a, v in soc.sfr.items()},
        "ram": dict(soc.ram),
    }


def load_python_snapshot(dir_path: str) -> dict:
    """Read an existing Python EmuSnapshot directory (no re-run).

    RAM comes from the `ram_*.bin` region blobs; SFR/cpu from snapshot.json.
    """
    d = Path(dir_path)
    doc = json.loads((d / "snapshot.json").read_text())
    cpu = doc["cpu"]
    csp = int(cpu["csp"], 16) & 0xFF
    ip = int(cpu["ip"], 16) & 0xFFFF
    sfr = {int(k, 16): int(v, 16) & 0xFFFF for k, v in doc["sfr"].items()}
    ram: dict[int, int] = {}
    for r in doc["regions"]:
        start = int(r["start"], 16)
        blob = (d / r["file"]).read_bytes()
        for i, byte in enumerate(blob):
            ram[start + i] = byte
    return {
        "icount": cpu["icount"], "pc": (csp << 16) | ip,
        "csp": csp, "ip": ip, "sfr": sfr, "ram": ram,
    }


def load_c_snapshot(path: str) -> dict:
    """Read a C snapshot DIRECTORY (or its snapshot.json).

    Same directory layout as a Python EmuSnapshot: `snapshot.json` (scalars + SFR
    + region index) plus one `ram_<start>.bin` per RAM region. cpu scalars live
    under the "cpu" key (hex strings), mirroring _jsonable_cpu.
    """
    p = Path(path)
    directory = p.parent if p.name == "snapshot.json" else p
    doc = json.loads((directory / "snapshot.json").read_text())
    cpu = doc["cpu"]
    sfr = {int(k, 16): int(v, 16) for k, v in doc.get("sfr", {}).items()}
    ram: dict[int, int] = {}
    for r in doc.get("regions", []):
        start = int(r["start"], 16)
        blob = (directory / r["file"]).read_bytes()
        for i, byte in enumerate(blob):
            ram[start + i] = byte
    return {
        "icount": cpu["icount"],
        "pc": int(cpu["pc"], 16),
        "csp": int(cpu["csp"], 16),
        "ip": int(cpu["ip"], 16),
        "sfr": sfr,
        "ram": ram,
    }


def compare(py: dict, c: dict) -> int:
    """Diff two normalized state dicts; print first divergence or confirm parity."""
    fails = []
    if py["icount"] != c["icount"]:
        print(f"MISMATCH: python icount {py['icount']} != C icount {c['icount']}")
        return 1

    if py["pc"] != c["pc"]:
        fails.append(f"PC: python {py['pc']:#08x} != C {c['pc']:#08x}")

    py_sfr_nz = {a: v for a, v in py["sfr"].items() if v != 0}
    all_sfr = sorted(set(py_sfr_nz) | set(c["sfr"]))
    sfr_diffs = 0
    first_sfr = None
    for a in all_sfr:
        pv = py["sfr"].get(a, 0) & 0xFFFF
        cv = c["sfr"].get(a, 0) & 0xFFFF
        if pv != cv:
            sfr_diffs += 1
            if first_sfr is None:
                first_sfr = (a, pv, cv)
    if sfr_diffs:
        a, pv, cv = first_sfr
        fails.append(f"SFR: {sfr_diffs} word(s) differ; first {a:#06x}: "
                     f"python {pv:#06x} != C {cv:#06x}")

    # RAM: only nonzero bytes are meaningful (a Python snapshot region may carry
    # explicit zeros; the C side records any written byte). Compare on the union.
    all_ram = set(py["ram"]) | set(c["ram"])
    ram_diffs = 0
    first_ram = None
    for a in sorted(all_ram):
        pv = py["ram"].get(a, 0) & 0xFF
        cv = c["ram"].get(a, 0) & 0xFF
        if pv != cv:
            ram_diffs += 1
            if first_ram is None:
                first_ram = (a, pv, cv)
    if ram_diffs:
        a, pv, cv = first_ram
        fails.append(f"RAM: {ram_diffs} byte(s) differ; first {a:#08x}: "
                     f"python {pv:#04x} != C {cv:#04x}")

    print(f"checkpoint icount={c['icount']}  pc={c['pc']:#08x}")
    print(f"  python: {len(py_sfr_nz)} nonzero SFR, {len(py['ram'])} RAM bytes")
    print(f"  C:      {len(c['sfr'])} nonzero SFR, {len(c['ram'])} RAM bytes")
    if fails:
        print("DIVERGENCE:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("PARITY: CPU + SFR + RAM identical")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("flash", nargs="?", help="fullflash (only needed for --python-run)")
    ap.add_argument("--c-snapshot", required=True)
    ap.add_argument("--python-snapshot", help="existing Python EmuSnapshot dir (no re-run)")
    ap.add_argument("--python-run", action="store_true", help="re-run Python from reset")
    ap.add_argument("--icount", type=int, help="checkpoint icount (required for --python-run)")
    ap.add_argument("--device", default=None)
    args = ap.parse_args(argv)

    c = load_c_snapshot(args.c_snapshot)

    if args.python_snapshot:
        py = load_python_snapshot(args.python_snapshot)
    elif args.python_run:
        if not args.flash or args.icount is None:
            print("--python-run needs <flash> and --icount", file=sys.stderr)
            return 2
        py = run_python(args.flash, args.icount, args.device)
    else:
        print("choose --python-snapshot DIR or --python-run", file=sys.stderr)
        return 2

    return compare(py, c)


if __name__ == "__main__":
    raise SystemExit(main())
