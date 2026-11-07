#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only regression for runtime-discovered Defrag V2 filesystem modules."""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def run(binary: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [binary, *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=20,
        check=False,
    )


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_generic_module_dispatch.py ENGINE")
    engine = sys.argv[1]

    discovered = run(engine, "--list-installed-modules")
    assert discovered.returncode == 0, discovered.stderr
    inventory = json.loads(discovered.stdout)
    assert inventory["schema"] == 1
    assert isinstance(inventory["modules"], list)
    for module in inventory["modules"]:
        assert re.fullmatch(r"[a-z0-9_]{1,15}", module["format_id"])
        assert re.fullmatch(r"[A-Za-z0-9_.-]{1,59}\.so", module["filename"])
        assert isinstance(module["capabilities"], int)
        assert 0 < module["capabilities"]

    # A filesystem never has to be compiled into the legacy registry.
    # Without the trusted module its mutation must fail closed, and even a
    # supplied regular-file target must remain unchanged.
    with tempfile.TemporaryDirectory(prefix="defragger-v2-") as temporary:
        target = Path(temporary) / "untouched.img"
        target.write_bytes(b"UNMODIFIED")
        future = run(engine, "defrag", str(target), "--filesystem", "zzdefragtest")
        assert future.returncode != 0
        assert "no installed trusted Defrag V2 SO advertises zzdefragtest" in future.stderr, future.stderr
        assert target.read_bytes() == b"UNMODIFIED"

    invalid = run(engine, "defrag", "/dev/null", "--filesystem", "bad/path")
    assert invalid.returncode == 2
    assert "invalid generic filesystem identifier" in invalid.stderr
    print("generic V2 discovery, unknown-module refusal and identity checks passed")


if __name__ == "__main__":
    main()
