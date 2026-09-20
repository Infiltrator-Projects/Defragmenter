# SPDX-License-Identifier: GPL-3.0-or-later
# Defragmenter
# Author: Shannon Smith
# Purpose: Thin GUI adapter for the authoritative native C Classic HFS analyser.

"""Read-only Classic Macintosh HFS backend delegated to native C."""

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
    "hfs",
    "Apple HFS",
    ("hfs",),
    CAP_ANALYSE | CAP_MAP | CAP_DEFRAG | CAP_GROWTH_DEFRAG | CAP_RECOVER | CAP_LIVE_MAP,
    "exact",
    operations=(
        operation(
            "defrag", "hfs-native",
            warning="Classic HFS writing uses Defragmenter's offline first-party native C engine and fails closed when a regular-file fork requires overflow extent records.",
        ),
        operation(
            "growth-defrag", "hfs-native",
            warning="Classic HFS Growth Defrag leaves a 10% free allocation-block reserve after each supported non-empty file fork.",
        ),
        operation("recover", "hfs-native"),
    ),
)


class HFSBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(
        path: str,
        mode: str,
        *options: str,
    ) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("hfs-native", anchor=anchor)
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
            raise BackendError("native HFS analyser returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native HFS analyser returned a non-object result")
        if payload.get("filesystem") != "hfs":
            raise BackendError("native HFS analyser returned the wrong filesystem identity")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return completed.returncode == 0 and self._json_result(completed)["filesystem"] == "hfs"
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "map", "--cells", str(max(1, cells)))
        if completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip()
            raise BackendError(detail or "native HFS mapper failed")
        payload = self._json_result(completed)
        if payload.get("schema") != 1 or payload.get("map_accuracy") != "exact":
            raise BackendError("native HFS mapper returned an invalid map contract")
        return payload


BACKEND = HFSBackend()
