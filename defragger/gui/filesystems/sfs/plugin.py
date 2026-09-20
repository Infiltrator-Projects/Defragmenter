#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Defragmenter
# Author: Shannon Smith
# Purpose: Thin GUI adapter for the native C Amiga Smart File System analyser.

"""Native SFS0/SFS2 allocation and offline relocation backend.

The native worker validates redundant root blocks, BTMP allocation bitmaps,
object containers and extent B-trees directly from raw storage.  SFS2 uses
its native structure-version 4 object and 32-bit extent records while sharing
the same fail-closed staging, verification and recovery contract.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

from backends.base import (
    BackendError, BackendInfo, CAP_ANALYSE, CAP_DEFRAG, CAP_GROWTH_DEFRAG,
    CAP_LIVE_MAP, CAP_MAP, CAP_RECOVER, FilesystemBackend, operation,
)
from core.paths import resolve_program

INFO = BackendInfo(
    "sfs", "Amiga SFS", ("sfs", "sfs0", "sfs2", "smartfilesystem"),
    CAP_ANALYSE | CAP_MAP | CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP,
    "exact-allocation",
    operations=(
        operation("defrag", "sfs-native", warning="Amiga SFS0/SFS2 writing uses Defragmenter's offline first-party native C raw engine."),
        operation("growth-defrag", "sfs-native", warning="SFS0/SFS2 Growth Defrag leaves an exact 10% free-block reserve after every regular file."),
        operation("recover", "sfs-native"),
    ),
)


class SfsBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(path: str, mode: str, *args: object) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("sfs-native", anchor=anchor)
        return subprocess.run(
            [worker, mode, path, *map(str, args)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C", "LANG": "C"},
        )

    @staticmethod
    def _json_result(completed: subprocess.CompletedProcess[str]) -> dict[str, Any]:
        try:
            payload = json.loads(completed.stdout)
        except json.JSONDecodeError as exc:
            detail = completed.stderr.strip()
            if detail:
                raise BackendError(detail) from exc
            raise BackendError("native SFS worker returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native SFS worker returned a non-object result")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return (
                completed.returncode == 0
                and self._json_result(completed).get("filesystem") == "sfs"
            )
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "map", "--cells", max(1, int(cells)))
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native SFS analyser failed")
        payload = self._json_result(completed)
        if payload.get("filesystem") != "sfs":
            raise BackendError("native SFS analyser returned the wrong filesystem identity")
        if payload.get("schema") != 1 or payload.get("backend") != "read-only-domain":
            raise BackendError("native SFS analyser returned an incompatible map schema")
        if payload.get("map_accuracy") != "exact-allocation":
            raise BackendError("native SFS analyser returned an unexpected accuracy contract")
        if not isinstance(payload.get("cells"), list) or not isinstance(payload.get("details"), dict):
            raise BackendError("native SFS analyser returned an incomplete map")
        return payload


BACKEND = SfsBackend()
