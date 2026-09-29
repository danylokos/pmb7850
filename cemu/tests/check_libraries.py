#!/usr/bin/env python3
"""Verify the two self-contained, namespaced CEMU archives."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
LIB = ROOT / "build" / "lib"
EXPECTED = {"libcemu.a", "libcemu_inst.a"}


def output(*arguments: str) -> str:
    return subprocess.run(arguments, check=True, text=True,
                          stdout=subprocess.PIPE).stdout


def normalize_symbol(symbol: str) -> str:
    """Remove the Mach-O object-file prefix used by Darwin nm."""
    if sys.platform == "darwin" and symbol.startswith("_"):
        return symbol[1:]
    return symbol


def main() -> None:
    actual = {path.name for path in LIB.glob("*.a")}
    if actual != EXPECTED:
        raise SystemExit(f"CEMU archive set differs: {sorted(actual)}")
    for name in sorted(EXPECTED):
        path = LIB / name
        defined = output("nm", "-g", "--defined-only", str(path))
        for line in defined.splitlines():
            fields = line.split()
            if len(fields) >= 3 and fields[-2] in {"B", "D", "R", "T", "W"}:
                symbol = normalize_symbol(fields[-1])
                if not symbol.startswith("cemu_"):
                    raise SystemExit(f"{name}: unnamespaced symbol {symbol}")
        undefined = {
            normalize_symbol(fields[-1])
            for fields in (line.split() for line in
                           output("nm", "-u", str(path)).splitlines())
            if fields
        }
        forbidden = sorted(symbol for symbol in undefined
                           if symbol.startswith(("x55_", "emu_")))
        if forbidden:
            raise SystemExit(f"{name}: host symbol dependency {forbidden[0]}")
    print("CEMU libraries: PASS (two archives; cemu_ namespace; no host symbols)")


if __name__ == "__main__":
    main()
