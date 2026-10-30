#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end native HFS+/HFSX mutation and recovery-safety tests."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("DEFRAGMENTER_BUILD_DIR", ROOT / "build"))
WORKER = Path(os.environ.get("DEFRAGMENTER_HFSPLUS_WORKER", BUILD / "defragmenter-hfsplus-worker"))
MAPPER = BUILD / "defragmenter-mapper"
FIXTURE = ROOT / "tests" / "make_hfsplus_fixture.py"
sys.path.insert(0, str(ROOT / "tests"))
import make_hfsplus_fixture as fixture  # noqa: E402


def run(*args: object, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        [str(WORKER), *map(str, args)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )
    if check:
        assert completed.returncode == 0, (completed.stdout, completed.stderr)
    return completed


def analyse(path: Path) -> dict:
    return json.loads(run("analyse-json", path).stdout)


def map_cpp(path: Path) -> dict:
    completed = subprocess.run(
        [str(MAPPER), str(path), "--fstype", "hfsplus", "--cells", "128"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return json.loads(completed.stdout)


def make(path: Path, *options: str) -> None:
    subprocess.run([sys.executable, str(FIXTURE), str(path), *options], check=True)


def verify(path: Path, *, growth: bool = False) -> None:
    args = [sys.executable, str(FIXTURE), str(path), "--verify"]
    if growth:
        args.append("--growth")
    subprocess.run(args, check=True)


def mutate(path: Path, operation: str, *, live: bool = True) -> str:
    journal = Path(str(path) + f".{operation}.journal")
    args: list[object] = [
        operation, path, "--write", "--confirm", path, "--journal", journal,
        "--workers", "auto", "--ram-buffer", "auto", "--batch-clusters", "4096",
        "--live-map-cells", "512",
    ]
    if operation == "growth-defrag":
        args += ["--growth-percent", "10"]
    if live:
        args += ["--live-updates"]
    completed = run(*args)
    assert f'@@RESULT {{"operation":"{operation}","status":"completed"' in completed.stdout
    assert "allocated blocks only" in completed.stdout
    if live:
        assert "@@LIVE_RANGES " in completed.stdout
        assert "@@LIVE_RESET " in completed.stdout
    assert not journal.exists()
    assert not Path(str(journal) + ".hfsplus-stage").exists()
    return completed.stdout


def allocated_ranges(path: Path) -> tuple[int, int, list[tuple[int, int]]]:
    info = analyse(path)
    block_size = int(info["block_size"])
    total_blocks = int(info["total_blocks"])
    free = [(int(start), int(end)) for start, end in info["free_ranges"]]
    used: list[tuple[int, int]] = []
    cursor = 0
    for start, end in free:
        if cursor < start:
            used.append((cursor, start))
        cursor = max(cursor, end)
    if cursor < total_blocks:
        used.append((cursor, total_blocks))
    return block_size, total_blocks, used


def stage_digest(path: Path) -> str:
    block_size, _, used = allocated_ranges(path)
    data = path.read_bytes()
    digest = hashlib.sha256()
    for start, end in used:
        digest.update(data[start * block_size:end * block_size])
    digest.update(data[:1536])
    digest.update(data[-1024:])
    return digest.hexdigest()


def transaction_fields(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    header = data[1024:1536]
    st = path.stat()
    signature, version = struct.unpack_from(">HH", header, 0)
    block_size, total_blocks = struct.unpack_from(">II", header, 40)
    return {
        "device": str(path.resolve()),
        "target_identity": f"file:{st.st_dev}:{st.st_ino}",
        "volume_token": hashlib.sha256(header).hexdigest(),
        "physical_bytes": st.st_size,
        "filesystem_bytes": block_size * total_blocks,
        "block_size": block_size,
        "total_blocks": total_blocks,
        "signature": signature,
        "version": version,
    }


def write_recovery_journal(journal: Path, image: Path, stage: Path,
                           operation: str = "defrag", phase: str = "committing") -> None:
    fields = transaction_fields(image)
    journal.write_text(
        "DEFRAGMENTER-HFSPLUS-JOURNAL-2\n"
        f"device={fields['device']}\n"
        f"target_identity={fields['target_identity']}\n"
        f"stage={stage}\n"
        f"operation={operation}\n"
        f"phase={phase}\n"
        f"volume_token={fields['volume_token']}\n"
        f"stage_sha256={stage_digest(stage)}\n"
        f"physical_bytes={fields['physical_bytes']}\n"
        f"filesystem_bytes={fields['filesystem_bytes']}\n"
        f"block_size={fields['block_size']}\n"
        f"total_blocks={fields['total_blocks']}\n"
        f"signature={fields['signature']}\n"
        f"version={fields['version']}\n",
        encoding="utf-8",
    )


def test_hfsplus_defrag(work: Path) -> None:
    image = work / "hfsplus.img"
    make(image)
    before = analyse(image)
    assert before["variant"] == "HFS+"
    assert before["regular_files"] == 2
    assert before["fragmented_files"] == 2
    mapped_before = map_cpp(image)
    assert mapped_before["filesystem"] == "hfsplus"
    assert mapped_before["total_units"] == before["total_blocks"]
    assert mapped_before["fragmented_files"] == before["fragmented_files"]
    mutate(image, "defrag")
    verify(image)
    assert analyse(image)["fragmented_files"] == 0
    assert map_cpp(image)["fragmented_files"] == 0


def test_hfsx_growth(work: Path) -> None:
    image = work / "hfsx.img"
    make(image, "--hfsx")
    assert analyse(image)["variant"] == "HFSX"
    mutate(image, "growth-defrag")
    verify(image, growth=True)
    assert analyse(image)["fragmented_files"] == 0


def test_extents_overflow_growth(work: Path) -> None:
    image = work / "overflow.img"
    make(image, "--overflow")
    mutate(image, "growth-defrag")
    verify(image, growth=True)


def test_clean_journal_is_preserved(work: Path) -> None:
    image = work / "journaled.img"
    make(image, "--journaled")
    original = image.read_bytes()
    before = analyse(image)
    assert before["journaled"] is True and before["journal_empty"] is True
    mutate(image, "growth-defrag")
    verify(image, growth=True)
    current = image.read_bytes()
    assert current[20 * fixture.BS:25 * fixture.BS] == original[20 * fixture.BS:25 * fixture.BS]


def test_dirty_journal_fails_closed(work: Path) -> None:
    image = work / "dirty-journal.img"
    make(image, "--journaled", "--dirty-journal")
    before = hashlib.sha256(image.read_bytes()).digest()
    journal = work / "dirty.journal"
    completed = run(
        "defrag", image, "--write", "--confirm", image, "--journal", journal,
        check=False,
    )
    assert completed.returncode != 0
    assert "pending transactions" in completed.stderr
    assert hashlib.sha256(image.read_bytes()).digest() == before
    assert not journal.exists()


def test_inconsistent_and_reserved_attributes_fail_closed(work: Path) -> None:
    for option, expected in (("--inconsistent", "marks the filesystem inconsistent"),
                             ("--reserved14", "reserved attribute bit 14")):
        image = work / (option[2:] + ".img")
        make(image, option)
        before = hashlib.sha256(image.read_bytes()).digest()
        journal = work / (option[2:] + ".journal")
        completed = run(
            "defrag", image, "--write", "--confirm", image, "--journal", journal,
            check=False,
        )
        assert completed.returncode != 0
        assert expected in completed.stderr
        assert hashlib.sha256(image.read_bytes()).digest() == before
        assert not journal.exists()


def test_recover_from_verified_bound_stage(work: Path) -> None:
    image = work / "recover.img"
    make(image)
    mutate(image, "defrag", live=False)
    journal = work / "recover.journal"
    stage = Path(str(journal) + ".hfsplus-stage")
    shutil.copyfile(image, stage)
    total, extents = fixture.parse_file_extents(image)[16]
    assert total and len(extents) == 1
    offset = extents[0][0] * fixture.BS
    with image.open("r+b") as stream:
        stream.seek(offset)
        stream.write(b"X" * fixture.BS)
        stream.flush()
        os.fsync(stream.fileno())
    write_recovery_journal(journal, image, stage)
    completed = run("recover", image, "--write", "--confirm", image, "--journal", journal)
    assert '@@RESULT {"operation":"recover","status":"completed"' in completed.stdout
    assert not stage.exists() and not journal.exists()
    verify(image)


def test_corrupt_stage_is_never_committed(work: Path) -> None:
    image = work / "stage-source.img"
    make(image)
    mutate(image, "defrag", live=False)
    journal = work / "corrupt.journal"
    stage = Path(str(journal) + ".hfsplus-stage")
    shutil.copyfile(image, stage)
    write_recovery_journal(journal, image, stage)
    before = hashlib.sha256(image.read_bytes()).digest()
    _, extents = fixture.parse_file_extents(stage)[16]
    with stage.open("r+b") as stream:
        stream.seek(extents[0][0] * fixture.BS)
        stream.write(b"Z" * fixture.BS)
    completed = run(
        "recover", image, "--write", "--confirm", image, "--journal", journal,
        check=False,
    )
    assert completed.returncode != 0
    assert "stage SHA-256" in completed.stderr
    assert hashlib.sha256(image.read_bytes()).digest() == before
    assert journal.exists() and stage.exists()


def test_recovery_refuses_different_target(work: Path) -> None:
    original = work / "identity-original.img"
    other = work / "identity-other.img"
    make(original)
    make(other)
    mutate(original, "defrag", live=False)
    journal = work / "identity.journal"
    stage = Path(str(journal) + ".hfsplus-stage")
    shutil.copyfile(original, stage)
    write_recovery_journal(journal, original, stage)
    before = hashlib.sha256(other.read_bytes()).digest()
    completed = run(
        "recover", other, "--write", "--confirm", other, "--journal", journal,
        check=False,
    )
    assert completed.returncode != 0
    assert "target path, identity or capacity changed" in completed.stderr
    assert hashlib.sha256(other.read_bytes()).digest() == before
    assert journal.exists() and stage.exists()


def test_recovery_refuses_unbound_stage_path(work: Path) -> None:
    image = work / "unbound-source.img"
    make(image)
    mutate(image, "defrag", live=False)
    stage = work / "unbound-external.stage"
    shutil.copyfile(image, stage)
    journal = work / "unbound.journal"
    write_recovery_journal(journal, image, stage)
    before = hashlib.sha256(image.read_bytes()).digest()
    completed = run(
        "recover", image, "--write", "--confirm", image, "--journal", journal,
        check=False,
    )
    assert completed.returncode != 0
    assert "not derived from the selected journal path" in completed.stderr
    assert hashlib.sha256(image.read_bytes()).digest() == before
    assert journal.exists() and stage.exists()


def test_legacy_recovery_journal_fails_closed(work: Path) -> None:
    image = work / "legacy.img"
    make(image)
    stage = work / "legacy.stage"
    shutil.copyfile(image, stage)
    journal = work / "legacy.journal"
    journal.write_text(
        "DEFRAGMENTER-HFSPLUS-1\n"
        f"device={image}\n"
        f"stage={stage}\n"
        "operation=defrag\n",
        encoding="utf-8",
    )
    completed = run(
        "recover", image, "--write", "--confirm", image, "--journal", journal,
        check=False,
    )
    assert completed.returncode != 0
    assert "lacks target identity and stage integrity" in completed.stderr


def main() -> None:
    assert WORKER.is_file(), f"missing native HFS+ worker: {WORKER}"
    assert MAPPER.is_file(), f"missing C++ mapper: {MAPPER}"
    with tempfile.TemporaryDirectory(prefix="defragmenter-hfsplus-") as directory:
        work = Path(directory)
        test_hfsplus_defrag(work)
        test_hfsx_growth(work)
        test_extents_overflow_growth(work)
        test_clean_journal_is_preserved(work)
        test_dirty_journal_fails_closed(work)
        test_inconsistent_and_reserved_attributes_fail_closed(work)
        test_recover_from_verified_bound_stage(work)
        test_corrupt_stage_is_never_committed(work)
        test_recovery_refuses_different_target(work)
        test_recovery_refuses_unbound_stage_path(work)
        test_legacy_recovery_journal_fails_closed(work)
    print("native HFS+/HFSX Defrag/Growth Defrag/identity/integrity recovery tests passed")


if __name__ == "__main__":
    main()
