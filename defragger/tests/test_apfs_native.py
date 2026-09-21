#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end bounded APFS checkpoint/spaceman/catalog qualification."""

from __future__ import annotations

import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
WORKER = BUILD / "linux-defragger-apfs-worker"
MAKER = ROOT / "tests" / "make_apfs_fixture.py"


def make(path: Path, *options: str) -> None:
    subprocess.run([sys.executable, str(MAKER), str(path), *options], check=True)


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        [str(WORKER), *(str(arg) for arg in args)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    if check:
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
    return completed



def mutate(path: Path, operation: str) -> subprocess.CompletedProcess[str]:
    journal = Path(str(path) + f".{operation}.journal")
    args: list[object] = [
        operation, path, "--write", "--confirm", path,
        "--journal", journal, "--live-updates",
    ]
    if operation == "growth-defrag":
        args += ["--growth-percent", "10"]
    completed = run(*args)
    assert f'@@RESULT {{"operation":"{operation}","status":"completed"' in completed.stdout
    assert not journal.exists()
    assert not Path(str(journal) + ".apfs-stage").exists()
    return completed


def test_writer(work: Path) -> None:
    image = work / "defrag.img"
    make(image)
    mutate(image, "defrag")
    analysis = json.loads(run("analyse-json", image).stdout)
    assert analysis["fragmented_files"] == 0
    raw = image.read_bytes()
    assert raw[10 * 4096:11 * 4096] == b"A" * 4096
    assert raw[11 * 4096:12 * 4096] == b"B" * 4096

    growth = work / "growth.img"
    make(growth)
    mutate(growth, "growth-defrag")
    analysis = json.loads(run("analyse-json", growth).stdout)
    assert analysis["fragmented_files"] == 0
    raw = growth.read_bytes()
    bitmap = raw[33 * 4096:34 * 4096]
    assert bitmap[12 >> 3] & (1 << (12 & 7)) == 0

    for option, expected in (
        ("--stale-checkpoint", "older checkpoint"),
        ("--shared", "unshared"),
    ):
        rejected = work / (option[2:] + ".img")
        make(rejected, option)
        before = hashlib.sha256(rejected.read_bytes()).digest()
        failed = run(
            "defrag", rejected, "--write", "--confirm", rejected,
            "--journal", work / (option[2:] + ".journal"),
            check=False,
        )
        assert failed.returncode != 0
        assert expected in failed.stderr.lower()
        assert hashlib.sha256(rejected.read_bytes()).digest() == before


def test_recovery(work: Path) -> None:
    image = work / "recover.img"
    make(image)
    journal = work / "recover.journal"
    image.chmod(stat.S_IRUSR)
    failed = run(
        "defrag", image, "--write", "--confirm", image,
        "--journal", journal, check=False,
    )
    image.chmod(stat.S_IRUSR | stat.S_IWUSR)
    assert failed.returncode != 0
    stage = Path(str(journal) + ".apfs-stage")
    assert journal.exists() and stage.exists()
    recovered = run(
        "recover", image, "--write", "--confirm", image,
        "--journal", journal,
    )
    assert '@@RESULT {"operation":"recover","status":"completed"' in recovered.stdout
    assert not journal.exists() and not stage.exists()
    assert json.loads(run("analyse-json", image).stdout)["fragmented_files"] == 0


def main() -> None:
    assert WORKER.is_file(), WORKER
    with tempfile.TemporaryDirectory(prefix="defragger-apfs-") as directory:
        work = Path(directory)
        image = work / "apfs.img"
        make(image)
        analysis = json.loads(run("analyse-json", image).stdout)
        assert analysis["filesystem"] == "apfs"
        assert analysis["block_size"] == 4096
        assert analysis["block_count"] == 16384
        assert analysis["xid"] == 7
        assert analysis["active_nx_block"] == 2
        assert analysis["spaceman_block"] == 9
        assert analysis["regular_files"] == 1
        assert analysis["directories"] == 1
        assert analysis["fragmented_files"] == 1
        assert analysis["free_blocks"] + analysis["used_blocks"] == analysis["block_count"]

        mapped = json.loads(run("map", image, "--cells", 128).stdout)
        assert mapped["schema"] == 1
        assert mapped["map_accuracy"] == "exact-bounded-spaceman"
        assert mapped["unknown_bytes"] == 0
        assert mapped["free_bytes"] + mapped["used_bytes"] == 16384 * 4096
        assert mapped["fragmented_files"] == 1
        assert any(cell["fragmented"] for cell in mapped["cells"])

        corrupt = work / "corrupt.img"
        make(corrupt, "--corrupt-bitmap")
        failed = run("analyse-json", corrupt, check=False)
        assert failed.returncode != 0
        assert "spaceman" in failed.stderr.lower() or "free" in failed.stderr.lower()

        multi = work / "multi.img"
        make(multi, "--second-volume")
        failed = run("analyse-json", multi, check=False)
        assert failed.returncode != 0
        assert "one volume" in failed.stderr.lower()

        sparse = work / "sparse.img"
        make(sparse, "--sparse")
        failed = run("analyse-json", sparse, check=False)
        assert failed.returncode != 0
        assert "non-sparse" in failed.stderr.lower()

        test_writer(work)
        test_recovery(work)

    print("bounded APFS checkpoint/spaceman/catalog/writer/recovery tests passed")


if __name__ == "__main__":
    main()
