#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end SFS0/SFS2 transaction and recovery qualification."""

from __future__ import annotations

import json
import os
import stat
import subprocess
import tempfile
from pathlib import Path


BUILD = Path(os.environ["LINUX_DEFRAGGER_BUILD_DIR"])
FIXTURE = BUILD / "linux-defragger-sfs-native-test"
WORKER = BUILD / "linux-defragger-sfs-worker"


def run(*args: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(WORKER), *map(str, args)],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "LC_ALL": "C", "LANG": "C"},
    )


def make_fixture(path: Path, sfs2: bool) -> None:
    layout = "sfs2-fragmented" if sfs2 else "fragmented"
    subprocess.run(
        [str(FIXTURE), "--write-fixture", str(path), layout],
        check=True,
    )


def analysis(path: Path) -> dict:
    completed = run("analyse-json", path)
    assert completed.returncode == 0, completed.stderr
    return json.loads(completed.stdout)


def test_defrag(root: Path, sfs2: bool) -> None:
    suffix = "sfs2" if sfs2 else "sfs0"
    image = root / f"defrag-{suffix}.sfs"
    journal = root / f"defrag-{suffix}.journal"
    make_fixture(image, sfs2)
    completed = run(
        "defrag", image, "--write", "--confirm", image,
        "--journal", journal,
    )
    assert completed.returncode == 0, completed.stderr
    payload = analysis(image)
    assert payload["format"] == ("SFS2" if sfs2 else "SFS0")
    assert payload["fragmented_files"] == 0
    assert not journal.exists()
    assert not Path(str(journal) + ".sfs-stage").exists()


def test_growth(root: Path, sfs2: bool) -> None:
    suffix = "sfs2" if sfs2 else "sfs0"
    image = root / f"growth-{suffix}.sfs"
    journal = root / f"growth-{suffix}.journal"
    make_fixture(image, sfs2)
    completed = run(
        "growth-defrag", image, "--write", "--confirm", image,
        "--journal", journal, "--growth-percent", "10",
    )
    assert completed.returncode == 0, completed.stderr
    payload = analysis(image)
    assert payload["format"] == ("SFS2" if sfs2 else "SFS0")
    assert payload["fragmented_files"] == 0
    assert payload["growth_10_satisfied"] is True


def test_recovery(root: Path, sfs2: bool) -> None:
    suffix = "sfs2" if sfs2 else "sfs0"
    image = root / f"recover-{suffix}.sfs"
    journal = root / f"recover-{suffix}.journal"
    make_fixture(image, sfs2)
    image.chmod(stat.S_IRUSR)
    failed = run(
        "defrag", image, "--write", "--confirm", image,
        "--journal", journal,
    )
    image.chmod(stat.S_IRUSR | stat.S_IWUSR)
    assert failed.returncode != 0
    stage = Path(str(journal) + ".sfs-stage")
    assert journal.exists()
    assert stage.exists()

    recovered = run(
        "recover", image, "--write", "--confirm", image,
        "--journal", journal,
    )
    assert recovered.returncode == 0, recovered.stderr
    payload = analysis(image)
    assert payload["format"] == ("SFS2" if sfs2 else "SFS0")
    assert payload["fragmented_files"] == 0
    assert not journal.exists()
    assert not stage.exists()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="linux-defragger-sfs-transaction-") as temp:
        root = Path(temp)
        for sfs2 in (False, True):
            test_defrag(root, sfs2)
            test_growth(root, sfs2)
            test_recovery(root, sfs2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
