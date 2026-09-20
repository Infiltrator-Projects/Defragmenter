#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Defragmenter
# Author: Shannon Smith
# Purpose: Thin GUI adapter for the native APFS checkpoint/spaceman analyser.

"""Exact bounded APFS allocation and file-fragmentation backend.

The native C worker owns checkpoint selection, Fletcher validation, spaceman
allocation decoding, object-map resolution and catalog extent analysis.
Python is presentation/registry glue only.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Any

from backends.base import BackendError, BackendInfo, CAP_ANALYSE, CAP_MAP, FilesystemBackend
from core.paths import resolve_program

INFO = BackendInfo("apfs", "Apple APFS", ("apfs",), CAP_ANALYSE | CAP_MAP, "exact")


class APFSBackend(FilesystemBackend):
    info = INFO

    @staticmethod
    def _run_native(path: str, mode: str, *options: str) -> subprocess.CompletedProcess[str]:
        anchor = Path(__file__).resolve().parents[2] / "core"
        worker = resolve_program("apfs-native", anchor=anchor)
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
            raise BackendError(detail or "native APFS worker returned invalid JSON") from exc
        if not isinstance(payload, dict):
            raise BackendError("native APFS worker returned a non-object result")
        return payload

    def probe(self, path: str) -> bool:
        try:
            completed = self._run_native(path, "identify")
            return completed.returncode == 0 and self._json_result(completed).get("filesystem") == "apfs"
        except (BackendError, FileNotFoundError, OSError):
            return False

    def map(self, path: str, cells: int) -> dict:
        completed = self._run_native(path, "map", "--cells", str(max(1, cells)))
        if completed.returncode != 0:
            raise BackendError(completed.stderr.strip() or "native APFS analyser failed")
        payload = self._json_result(completed)
        if payload.get("filesystem") != "apfs":
            raise BackendError("native APFS analyser returned the wrong filesystem identity")
        return payload


BACKEND = APFSBackend()
