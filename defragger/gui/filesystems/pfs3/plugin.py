#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native bounded PFS3 allocation and offline relocation backend."""

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
    "pfs3", "Amiga PFS3", ("pfs3", "pfs", "professionalfilesystem"),
    CAP_ANALYSE | CAP_MAP | CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP,
    "exact-allocation",
    operations=(
        operation("defrag", "pfs3-native", warning="Amiga PFS3 writing uses Defragmenter's offline first-party native C raw engine and fails closed outside its qualified subset."),
        operation("growth-defrag", "pfs3-native", warning="PFS3 Growth Defrag leaves an exact 10% free-block reserve after every supported regular file."),
        operation("recover", "pfs3-native"),
    ),
)

class Pfs3Backend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(path: str, mode: str, *args: object) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("pfs3-native", anchor=anchor)
        return subprocess.run(
            [worker, mode, path, *map(str, args)], check=False, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
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
            raise BackendError("native PFS3 worker returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native PFS3 worker returned a non-object result")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return completed.returncode == 0 and self._json_result(completed).get("filesystem") == "pfs3"
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "map", "--cells", max(1, int(cells)))
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native PFS3 analyser failed")
        payload = self._json_result(completed)
        if payload.get("filesystem") != "pfs3":
            raise BackendError("native PFS3 analyser returned the wrong filesystem identity")
        if payload.get("schema") != 1 or payload.get("backend") != "read-only-domain":
            raise BackendError("native PFS3 analyser returned an incompatible map schema")
        if payload.get("map_accuracy") != "exact-allocation":
            raise BackendError("native PFS3 analyser returned an unexpected accuracy contract")
        if not isinstance(payload.get("cells"), list) or not isinstance(payload.get("details"), dict):
            raise BackendError("native PFS3 analyser returned an incomplete map")
        return payload

BACKEND = Pfs3Backend()
