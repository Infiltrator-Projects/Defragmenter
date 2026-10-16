#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native EXT parser must fail closed on non-EXT input."""
from __future__ import annotations
import os
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("DEFRAGMENTER_BUILD_DIR", ROOT / "build"))
worker = BUILD / "defragmenter-ext-worker"
with tempfile.TemporaryDirectory() as directory:
    image = Path(directory) / "garbage.img"
    image.write_bytes(os.urandom(1024 * 1024))
    completed = subprocess.run([str(worker), "analyse-json", str(image)], check=False,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert completed.returncode != 0
print("native EXT malformed-input fail-closed test passed")
