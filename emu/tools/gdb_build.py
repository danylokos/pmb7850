#!/usr/bin/env python3
"""Build the remote C166 GDB, or smoke-test an existing binary (no install)."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


SOURCE = Path(__file__).resolve().parents[2] / "gdb"
DEFAULT_BUILD = SOURCE / "build" / "c166"
OPTIONS = [
    "--target=c166-unknown-none",
    "--with-expat", "--without-python", "--without-guile",
    "--disable-sim", "--disable-gdbserver", "--disable-gas",
    "--disable-ld", "--disable-gold", "--disable-werror",
    "CFLAGS=-O2 -g", "CXXFLAGS=-O2 -g",
]
# Environment inputs which can change configure's result. Flags above are fixed.
CONFIG_ENV = (
    "CC", "CXX", "CPP", "CXXCPP", "CPPFLAGS", "LDFLAGS", "LIBS",
    "AR", "AS", "LD", "NM", "RANLIB", "STRIP", "PATH",
    "PKG_CONFIG", "PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR", "CONFIG_SITE",
)
STAMP = "emu-gdb-config.json"


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(command: list[str], *, cwd: Path | None = None,
        capture: bool = False,
        extra_env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    print(f"$ {shlex.join(command)}", flush=True)
    env = dict(os.environ, LC_ALL="C")
    if extra_env:
        env.update(extra_env)
    # The helper owns parallelism; do not inherit a parent make's jobserver,
    # command-line variables, or dry-run flags into the upstream build.
    for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES"):
        env.pop(name, None)
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.STDOUT if capture else None,
                            timeout=30 if capture else None)
    if capture:
        print(result.stdout, end="", flush=True)
    return result


def dependency_environment() -> dict[str, str]:
    """Find Homebrew dependency prefixes without changing Linux builds."""
    if sys.platform != "darwin":
        return {}
    pkg_config = os.environ.get("PKG_CONFIG") or shutil.which("pkg-config")
    if not pkg_config:
        return {}
    packages = []
    for package in ("mpfr", "gmp", "expat", "ncurses"):
        result = subprocess.run(
            [pkg_config, "--exists", package],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False,
        )
        if result.returncode == 0:
            packages.append(package)
    if not packages:
        return {}

    discovered = {}
    for name, option in (("CPPFLAGS", "--cflags"),
                         ("LDFLAGS", "--libs-only-L"),
                         ("LIBS", "--libs-only-l")):
        result = subprocess.run(
            [pkg_config, option, *packages],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, check=False,
        )
        if result.returncode == 0 and result.stdout.strip():
            existing = os.environ.get(name, "").strip()
            discovered[name] = " ".join(
                part for part in (existing, result.stdout.strip()) if part
            )
    return discovered


def build(source: Path, build_dir: Path, jobs: int) -> None:
    source, build_dir = source.resolve(), build_dir.resolve()
    configure = source / "configure"
    if not configure.is_file():
        raise ValueError("GDB source missing; run git submodule update --init gdb")
    if build_dir == source or build_dir in source.parents:
        raise ValueError("GDB_BUILD_DIR must be a separate out-of-tree directory")
    build_environment = dependency_environment()
    expected = {
        "source": str(source), "options": OPTIONS,
        "configure_sha256": digest(configure),
        "environment": {key: build_environment.get(key, os.environ.get(key))
                        for key in CONFIG_ENV},
    }
    stamp = build_dir / STAMP
    status = build_dir / "config.status"
    incompatible = (
        f"Incompatible or incomplete GDB configuration in {build_dir}. "
        "Choose a fresh GDB_BUILD_DIR or manually move the existing build; "
        "nothing was deleted."
    )
    if build_dir.exists() and any(build_dir.iterdir()):
        try:
            cached = json.loads(stamp.read_text())
            matches = (cached["request"] == expected
                       and cached["config_status_sha256"] == digest(status))
        except (OSError, ValueError, KeyError, TypeError):
            matches = False
        if not matches:
            raise ValueError(incompatible)
        print(f"Reusing matching GDB configuration: {build_dir}", flush=True)
    else:
        build_dir.mkdir(parents=True, exist_ok=True)
        # Keep generated products ignored even in a newly initialized submodule,
        # without modifying upstream tracked files or the evaluation build.
        if (source / "build") in build_dir.parents:
            ignore = source / "build" / ".gitignore"
            if not ignore.exists():
                ignore.write_text("*\n")
        run([str(configure), *OPTIONS], cwd=build_dir,
            extra_env=build_environment).check_returncode()
        stamp.write_text(json.dumps({"request": expected,
                                    "config_status_sha256": digest(status)},
                                   indent=2) + "\n")
    run(["make", f"-j{jobs}", "all-gdb"], cwd=build_dir,
        extra_env=build_environment).check_returncode()
    binary = build_dir / "gdb" / "gdb"
    print(f"SHA-256 {digest(binary)}  {binary}")


def smoke(binary: Path) -> None:
    binary = binary.resolve()
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError(f"GDB binary missing or not executable: {binary}; "
                         "build it with make -C emu gdb or set GDB_BINARY")
    base = [str(binary), "-nx", "-nh", "-batch",
            "-iex", "set auto-load off"]
    version = run([*base, "--version"], capture=True)
    version.check_returncode()
    if not re.search(r"^GNU gdb .*\b17\.2\s*$", version.stdout, re.MULTILINE):
        raise ValueError("Expected GNU GDB 17.2")
    config = run([*base, "-ex", "show configuration"], capture=True)
    config.check_returncode()
    for option in ("--target=c166-unknown-none", "--with-expat",
                   "--without-python", "--without-guile"):
        if option not in config.stdout.split():
            raise ValueError(f"GDB configuration lacks {option}")
    with tempfile.TemporaryDirectory(prefix="emu-gdb-smoke-") as tmp:
        script = Path(tmp) / "smoke.gdb"
        script.write_text("set architecture c166\n"
                          "show architecture\n"
                          "set $phase2 = 6 * 7\n"
                          "if $phase2 != 42\n"
                          "  echo GDB_ASSERTION_FAILED\\n\n"
                          "  quit 1\nend\n"
                          "printf \"GDB_COMMAND_FILE_OK %d\\n\", $phase2\n"
                          "quit 0\n")
        print(script.read_text(), end="", flush=True)
        commands = run([*base, "-x", str(script)], capture=True)
        commands.check_returncode()
        if "GDB_COMMAND_FILE_OK 42" not in commands.stdout:
            raise ValueError("GDB command-file assertion did not complete")
    python = run([*base, "-ex", "python print(42)"], capture=True)
    if python.returncode == 0 or "Python scripting is not supported" not in python.stdout:
        raise ValueError("Expected embedded Python to be unavailable")
    print(f"PASS: GDB 17.2, XML, no embedded Python, command file; "
          f"SHA-256 {digest(binary)}  {binary}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build")
    builder.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    builder.add_argument("--jobs", type=int, default=4)
    tester = commands.add_parser("smoke")
    tester.add_argument("--binary", type=Path, default=DEFAULT_BUILD / "gdb/gdb")
    args = parser.parse_args()
    try:
        if args.command == "build":
            if args.jobs < 1:
                raise ValueError("GDB_JOBS must be a positive integer")
            build(SOURCE, args.build_dir, args.jobs)
        else:
            smoke(args.binary)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
