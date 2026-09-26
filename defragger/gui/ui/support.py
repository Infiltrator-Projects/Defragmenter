# SPDX-License-Identifier: GPL-3.0-or-later
"""GUI paths and state-directory helpers."""

from __future__ import annotations

import hashlib
import os
import re
from pathlib import Path

from core.paths import resolve_program

PROGRAM_ANCHOR = Path(__file__).resolve().parents[1] / "core"


def safe_journal_name(path: str, identity: str = "") -> str:
    """Return a readable, collision-resistant journal stem."""

    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", path.strip("/"))
    prefix = (cleaned or "volume")[-80:]
    material = f"{path}\0{identity}".encode("utf-8", "surrogatepass")
    digest = hashlib.sha256(material).hexdigest()[:16]
    return f"{prefix}-{digest}"


STATE_ROOT = Path("/var/lib/linux-defragger/state")


def state_dir() -> Path:
    """Return the root-worker-owned persistent journal namespace for this UID."""

    return STATE_ROOT / str(os.getuid())


def find_mapper() -> str:
    return resolve_program("mapper", anchor=PROGRAM_ANCHOR)


def find_operation_engine() -> str:
    return resolve_program("operation-engine", anchor=PROGRAM_ANCHOR)


def find_privileged_helper() -> str:
    return resolve_program("helper", anchor=PROGRAM_ANCHOR)
