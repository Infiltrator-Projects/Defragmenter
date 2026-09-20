# SPDX-License-Identifier: GPL-3.0-or-later
# Defragmenter
# Author: Shannon Smith
# Purpose: Thin GUI adapter for native C Minix analysis and offline relayout.

"""Native Minix exact analysis and recoverable offline relayout backend."""

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
    CAP_DEFRAG,
    CAP_GROWTH_DEFRAG,
    CAP_LIVE_MAP,
    CAP_MAP,
    CAP_RECOVER,
    FilesystemBackend,
    operation,
)
from core.paths import resolve_program

INFO = BackendInfo(
    "minix",
    "Minix Filesystem",
    ("minix", "minix2", "minix3"),
    CAP_ANALYSE | CAP_MAP | CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP,
    "exact",
    operations=(
        operation(
            "defrag",
            "minix-native",
            warning="Minix writing uses Defragmenter's offline first-party native C raw engine.",
        ),
        operation(
            "growth-defrag",
            "minix-native",
            warning="Minix Growth Defrag leaves an exact 10% free-zone reserve after every regular file.",
        ),
        operation("recover", "minix-native"),
    ),
)


class MinixBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(
        path: str,
        mode: str,
        *options: str,
    ) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("minix-native", anchor=anchor)
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
            raise BackendError("native Minix worker returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native Minix worker returned a non-object result")
        if payload.get("filesystem") != "minix":
            raise BackendError("native Minix worker returned the wrong filesystem identity")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return completed.returncode == 0 and self._json_result(completed)["filesystem"] == "minix"
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "map", "--cells", str(max(1, cells)))
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native Minix mapper failed")
        payload = self._json_result(completed)
        if payload.get("schema") != 1 or payload.get("map_accuracy") != "exact":
            raise BackendError("native Minix mapper returned an invalid exact-map contract")
        if not isinstance(payload.get("cells"), list) or not isinstance(payload.get("details"), dict):
            raise BackendError("native Minix mapper returned an incomplete exact map")
        return payload


BACKEND = MinixBackend()
