# SPDX-License-Identifier: GPL-3.0-or-later
"""Small synchronous client for installed Defragmenter executables."""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path
from typing import Callable

from core.paths import resolve_program
from version import VERSION

from .backend_catalog import BackendCatalog


RunCommand = Callable[..., subprocess.CompletedProcess[str]]


def query_engine_version(
    operation_engine: str,
    *,
    run: RunCommand = subprocess.run,
) -> str:
    """Return the native engine release, falling back to the GUI release."""

    try:
        result = run(
            [operation_engine, "--version"],
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C"},
        )
        match = re.search(r"(\d+\.\d+\.\d+-\d+)", result.stdout)
        return match.group(1) if match else VERSION
    except (OSError, subprocess.SubprocessError):
        return VERSION


def load_backend_catalog(
    mapper: str,
    *,
    run: RunCommand = subprocess.run,
) -> BackendCatalog:
    """Load and validate one immutable backend catalogue."""

    result = run(
        [mapper, "--list-backends"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "LC_ALL": "C"},
    )
    return BackendCatalog.from_manifest(json.loads(result.stdout))


def detect_image_fstype(
    path: str,
    catalog: BackendCatalog,
    *,
    mapper: str | None = None,
    run: RunCommand = subprocess.run,
) -> str:
    """Identify an image through Defragmenter's authoritative native registry."""

    if not Path(path).is_file():
        raise RuntimeError("The selected filesystem image is not a regular file.")

    if mapper is None:
        anchor = Path(__file__).resolve().parents[1] / "core"
        mapper = resolve_program("mapper", anchor=anchor)

    try:
        result = run(
            [mapper, path, "--probe"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C", "LANG": "C"},
        )
    except OSError as exc:
        raise RuntimeError(
            f"The native filesystem identifier could not be started: {exc}"
        ) from exc

    if result.returncode != 0:
        detail = result.stderr.strip()
        raise RuntimeError(
            "No first-party Defragmenter backend recognised this filesystem image."
            + (f"\n\n{detail}" if detail else "")
        )

    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            "The native filesystem identifier returned malformed protocol data."
        ) from exc
    if not isinstance(payload, dict):
        raise RuntimeError(
            "The native filesystem identifier returned an invalid result."
        )

    detected = str(payload.get("filesystem") or "").strip().lower()
    if not catalog.supports(detected):
        raise RuntimeError(
            "The native filesystem identifier reported a filesystem absent from "
            "this build's backend catalogue."
        )
    return detected
