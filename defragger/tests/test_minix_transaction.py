#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end Minix mutation, Growth Defrag and recovery qualification."""

from __future__ import annotations

import json
import os
import stat
import subprocess
import tempfile
from pathlib import Path


BUILD = Path(os.environ["LINUX_DEFRAGGER_BUILD_DIR"])
FIXTURE = BUILD / "linux-defragger-minix-native-test"
WORKER = BUILD / "linux-defragger-minix-worker"


def run(*args: object) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(WORKER), *map(str, args)],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={**os.environ, "LC_ALL": "C", "LANG": "C"},
    )


def make_fixture(path: Path) -> None:
    subprocess.run(
        [str(FIXTURE), "--write-fixture", str(path), "fragmented"],
        check=True,
    )


def analysis(path: Path) -> dict:
    completed = run("analyse-json", path)
    assert completed.returncode == 0, completed.stderr
    return json.loads(completed.stdout)


def mutate(operation: str, image: Path, journal: Path) -> subprocess.CompletedProcess[str]:
    args: list[object] = [
        operation,
        image,
        "--write",
        "--confirm",
        image,
        "--journal",
        journal,
    ]
    if operation == "growth-defrag":
        args.extend(["--growth-percent", "10"])
    return run(*args)


def test_defrag(root: Path) -> None:
    image = root / "defrag.minix"
    journal = root / "defrag.journal"
    make_fixture(image)

    before = analysis(image)
    assert before["fragmented_files"] == 1
    assert before["fragmented_directories"] == 1

    completed = mutate("defrag", image, journal)
    assert completed.returncode == 0, completed.stderr
    assert '"operation":"defrag","status":"completed"' in completed.stdout

    after = analysis(image)
    assert after["fragmented_files"] == 0
    assert after["fragmented_directories"] == 0
    assert not journal.exists()
    assert not Path(str(journal) + ".minix-stage").exists()

    second = mutate("defrag", image, root / "defrag-second.journal")
    assert second.returncode == 0, second.stderr
    assert '"operation":"defrag","status":"not-needed"' in second.stdout


def test_growth(root: Path) -> None:
    image = root / "growth.minix"
    journal = root / "growth.journal"
    make_fixture(image)

    completed = mutate("growth-defrag", image, journal)
    assert completed.returncode == 0, completed.stderr
    assert '"operation":"growth-defrag","status":"completed"' in completed.stdout
    after = analysis(image)
    assert after["fragmented_files"] == 0
    assert after["fragmented_directories"] == 0

    second = mutate(
        "growth-defrag", image, root / "growth-second.journal"
    )
    assert second.returncode == 0, second.stderr
    assert '"operation":"growth-defrag","status":"not-needed"' in second.stdout

    bad = run(
        "growth-defrag",
        image,
        "--write",
        "--confirm",
        image,
        "--journal",
        root / "bad-growth.journal",
        "--growth-percent",
        "5",
    )
    assert bad.returncode != 0


def interrupted_transaction(root: Path, name: str) -> tuple[Path, Path, Path]:
    image = root / f"{name}.minix"
    journal = root / f"{name}.journal"
    make_fixture(image)

    image.chmod(stat.S_IRUSR)
    failed = mutate("defrag", image, journal)
    image.chmod(stat.S_IRUSR | stat.S_IWUSR)
    assert failed.returncode != 0

    stage = Path(str(journal) + ".minix-stage")
    assert journal.exists()
    assert stage.exists()
    return image, journal, stage


def test_recovery(root: Path) -> None:
    image, journal, stage = interrupted_transaction(root, "recover")

    recovered = run(
        "recover",
        image,
        "--write",
        "--confirm",
        image,
        "--journal",
        journal,
    )
    assert recovered.returncode == 0, recovered.stderr
    assert '"operation":"recover","status":"completed"' in recovered.stdout
    assert analysis(image)["fragmented_files"] == 0
    assert analysis(image)["fragmented_directories"] == 0
    assert not journal.exists()
    assert not stage.exists()


def test_corrupt_stage_is_rejected(root: Path) -> None:
    image, journal, stage = interrupted_transaction(root, "corrupt-stage")
    original = image.read_bytes()

    with stage.open("r+b") as stream:
        stream.seek(8192)
        byte = stream.read(1)
        assert byte
        stream.seek(8192)
        stream.write(bytes([byte[0] ^ 0x5A]))

    rejected = run(
        "recover",
        image,
        "--write",
        "--confirm",
        image,
        "--journal",
        journal,
    )
    assert rejected.returncode != 0
    assert "SHA-256" in rejected.stderr
    assert image.read_bytes() == original
    assert journal.exists()
    assert stage.exists()


def main() -> int:
    with tempfile.TemporaryDirectory(
        prefix="linux-defragger-minix-transaction-"
    ) as temp:
        root = Path(temp)
        test_defrag(root)
        test_growth(root)
        test_recovery(root)
        test_corrupt_stage_is_rejected(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
