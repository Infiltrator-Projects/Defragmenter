#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression guard for bounded Amiga OFS/FFS transaction batching."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "gui" / "filesystems" / "affs" / "native" / "affs_native.c"
WORKER = ROOT / "gui" / "filesystems" / "affs" / "native" / "affs_worker.c"


def function_body(source: str, name: str, next_name: str) -> str:
    start = source.index(name)
    end = source.index(next_name, start)
    return source[start:end]


def main() -> None:
    native = NATIVE.read_text(encoding="utf-8")
    worker = WORKER.read_text(encoding="utf-8")

    assert "#define AFFS_IO_BATCH_BLOCKS 8192U" in native
    assert "#define AMIGA_IO_BATCH_BLOCKS 8192U" in worker

    copy_allocated = function_body(native, "static int copy_allocated", "static int bitmap_write")
    assert "contiguous_map_run" in copy_allocated
    assert "AFFS_IO_BATCH_BYTES" in copy_allocated
    assert "pread_full_local" in copy_allocated
    assert "pwrite_full_local" in copy_allocated

    relocation = function_body(native, "static int move_file_data_batched", "int affs_build_stage")
    assert "contiguous_file_run" in relocation
    assert "AFFS_IO_BATCH_BYTES" in relocation
    assert "pread_full_local" in relocation
    assert "pwrite_full_local" in relocation
    # OFS still rewrites each block's ownership/sequence/next/checksum in memory
    # before the contiguous run is written, while FFS can copy the run unchanged.
    assert "if (!src->ffs)" in relocation
    assert "fixsum(block, 5)" in relocation

    stage_hash = function_body(worker, "static int stage_sha256", "static int capture_target")
    assert "allocated_run_blocks" in stage_hash
    assert "AMIGA_IO_BATCH_BYTES" in stage_hash
    assert "ld_stop_requested()" in stage_hash
    assert "EVP_DigestUpdate(context, buffer, bytes)" in stage_hash

    commit = function_body(worker, "static int safe_commit_stage", "static bool valid_operation")
    assert "allocated_run_blocks" in commit
    assert "AMIGA_IO_BATCH_BYTES" in commit
    assert "ld_stop_requested()" in commit
    assert "stop_commit(target, error)" in commit
    assert "ld_pread_full(stage.fd, buffer, bytes, offset)" in commit
    assert "ld_pwrite_full(target, buffer, bytes, offset)" in commit
    assert "ld_sync_fd(target)" in commit
    assert "fsync(target)" not in commit

    # These messages are intentionally user-visible: they make long OFS/FFS
    # operations visibly active instead of appearing stalled between milestones.
    assert "Amiga preparation batching:" in native
    assert "Amiga stage integrity: hashing allocated runs" in worker
    assert "Amiga source commit batching:" in worker

    print("Amiga OFS/FFS bounded batched-I/O architecture guard passed")


if __name__ == "__main__":
    main()
