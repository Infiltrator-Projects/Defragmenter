#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The application hosts a Filesystems module rather than a second FAT engine."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
assert not (root / "gui/filesystems/fat/native").exists()
loader = (root / "native/fat_module_loader.c").read_text()
assert 'infiltrator_filesystem_defrag_v1' in loader
assert 'FAT12.so' in loader and 'FAT16.so' in loader and 'FAT32.so' in loader
assert 'O_NOFOLLOW' in loader and 'trusted_module' in loader
runtime = (root / "native/runtime.cpp").read_text()
assert 'for (const int bits : {12, 16, 32})' in runtime
assert 'write, "exact", "fat-native", MapAdapter::Fat' in runtime
assert 'standard_write_ops("fat-native")' in runtime
print("FAT operations use a versioned Filesystems module")
