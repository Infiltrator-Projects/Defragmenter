# SPDX-License-Identifier: GPL-3.0-or-later
# Defragmenter
# Author: Shannon Smith
# Purpose: Thin GUI adapter for native C UFS1/UFS2 read-only analysis.

"""Read-only UFS backend delegated entirely to the native C worker."""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

from backends.base import (
    BackendError,
    BackendInfo,
    CAP_ANALYSE,
    CAP_MAP,
    FilesystemBackend,
)
from core.paths import resolve_program

INFO = BackendInfo(
    "ufs",
    "Solaris/BSD UFS",
    ("ufs", "ufs1", "ufs2", "4.2bsd"),
    CAP_ANALYSE | CAP_MAP,
    "variant-dependent",
)


class UfsBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(
        path: str,
        mode: str,
        *options: str,
    ) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("ufs-native", anchor=anchor)
        return subprocess.run(
            [worker, mode, path, *options],
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
            raise BackendError("native UFS worker returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native UFS worker returned a non-object result")
        if payload.get("filesystem") != "ufs":
            raise BackendError("native UFS worker returned the wrong filesystem identity")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return completed.returncode == 0 and self._json_result(completed)["filesystem"] == "ufs"
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "map", "--cells", str(max(1, cells)))
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native UFS mapper failed")
        payload = self._json_result(completed)
        if payload.get("schema") != 1 or payload.get("map_accuracy") not in {
            "summary",
            "exact-allocation",
            "exact",
        }:
            raise BackendError("native UFS mapper returned an invalid map contract")
        if not isinstance(payload.get("cells"), list) or not isinstance(payload.get("details"), dict):
            raise BackendError("native UFS mapper returned an incomplete map")
        return payload


BACKEND = UfsBackend()
