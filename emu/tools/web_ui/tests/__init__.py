"""Shared paths for the Web UI test package."""

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]
CEMU_ROOT = REPO_ROOT / "cemu"
