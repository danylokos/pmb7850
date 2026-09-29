#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""C55 entry point for the bundled-image matrix.

Original-dump register/frame expectations are not applicable to synthesized
images. Qualification policy comes from firmware/manifest.json.
"""
from __future__ import annotations

import sys

from .run_x55_boot import matrix_main


def main() -> int:
    return matrix_main(["--device", "c55", *sys.argv[1:]])


if __name__ == "__main__":
    raise SystemExit(main())
