# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-tree build metadata fallback.

Installed builds receive a generated version.py from CMake. This file exists
only so an in-tree GUI launch has the same single VERSION authority instead of
carrying a second hard-coded release number.
"""

from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
VERSION = (_ROOT / "VERSION").read_text(encoding="utf-8").strip()
BUILD_LABEL = "Source / development build"
