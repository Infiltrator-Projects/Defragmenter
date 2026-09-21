# SPDX-License-Identifier: GPL-3.0-or-later
"""Small synchronous client for installed Defragmenter executables."""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path
from typing import Callable

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
    run: RunCommand = subprocess.run,
) -> str:
    """Probe an image and reject filesystem types absent from the manifest."""

    result = run(
        ["blkid", "-p", "-o", "value", "-s", "TYPE", path],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "LC_ALL": "C"},
    )
    detected = result.stdout.strip().lower() if result.returncode == 0 else ""
    if not catalog.supports(detected):
        detail = result.stderr.strip()
        detected_text = f" Host detection reported {detected!r}." if detected else ""
        raise RuntimeError(
            "The image does not contain a filesystem advertised by this build's "
            f"native backend catalogue.{detected_text}"
            + (f"\n\n{detail}" if detail else "")
        )
    if not Path(path).is_file():
        raise RuntimeError("The selected filesystem image is not a regular file.")
    return detected
