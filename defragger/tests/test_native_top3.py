#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end image tests for the current C-first EXT, NTFS and exFAT C engines."""

from __future__ import annotations

import json
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("LINUX_DEFRAGGER_BUILD_DIR", ROOT / "build"))
MAPPER = BUILD / "linux-defragger-mapper"
sys.path.insert(0, str(ROOT / "tests"))
from ntfs_test_fixture import make_image as make_ntfs_image  # noqa: E402


def map_json(filesystem: str, image: Path, cells: int = 128) -> dict:
    completed = subprocess.run(
        [str(MAPPER), str(image), "--fstype", filesystem, "--cells", str(cells)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return json.loads(completed.stdout)


def run_json(worker: Path, image: Path) -> dict:
    completed = subprocess.run(
        [str(worker), "analyse-json", str(image)],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return json.loads(completed.stdout)


def mutate(worker: Path, image: Path, operation: str, journal: Path) -> str:
    arguments = [
        str(worker), operation, str(image), "--write", "--confirm", str(image),
        "--journal", str(journal), "--batch-clusters", "4096",
        "--ram-buffer", "auto", "--workers", "auto", "--live-map-cells", "512",
    ]
    if operation == "growth-defrag":
        arguments.extend(("--growth-percent", "10"))
    completed = subprocess.run(
        arguments,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"{operation} failed with rc={completed.returncode}\noutput:\n{completed.stdout}"
        )
    assert f'@@RESULT {{"operation":"{operation}","status":"completed"' in completed.stdout
    assert "@@LIVE_RESET " in completed.stdout
    assert not journal.exists()
    return completed.stdout


def assert_clean(payload: dict, *, growth: bool) -> None:
    assert payload["fragmented_files"] == 0, payload
    assert payload["fragmented_directories"] == 0, payload
    if growth:
        assert payload["growth_10_satisfied"] is True, payload


def test_exfat(work: Path) -> None:
    worker = BUILD / "linux-defragger-exfat-worker"
    image = work / "exfat.img"
    subprocess.run([sys.executable, str(ROOT / "tests" / "make_exfat_image.py"), str(image)], check=True)
    before = run_json(worker, image)
    assert before["fragmented_files"] > 0 and before["fragmented_directories"] > 0
    mapped_before = map_json("exfat", image)
    assert mapped_before["filesystem"] == "exfat"
    assert mapped_before["total_units"] == before["total_clusters"]
    assert mapped_before["fragmented_files"] == before["fragmented_files"]
    serial = before["serial"]
    output = mutate(worker, image, "defrag", work / "exfat-defrag.journal")
    assert "exFAT unified workspace layout:" in output, output
    assert "internally verified native-C exFAT working image" not in output, output
    packed = run_json(worker, image)
    assert packed["serial"] == serial
    assert_clean(packed, growth=False)
    assert map_json("exfat", image)["fragmented_files"] == 0
    output = mutate(worker, image, "growth-defrag", work / "exfat-growth.journal")
    assert "exFAT unified workspace layout:" in output, output
    assert "internally verified native-C exFAT working image" not in output, output
    grown = run_json(worker, image)
    assert grown["serial"] == serial
    assert_clean(grown, growth=True)


def test_ntfs_combined_analysis(work: Path) -> None:
    worker = BUILD / "linux-defragger-ntfs-worker"
    for dirty in (False, True):
        image = work / f"ntfs-combined-{dirty}.img"
        make_ntfs_image(image, volume_flags=1 if dirty else 0,
                        fragmented_data=True, directory_data=True)
        digest = hashlib.sha256(image.read_bytes()).digest()
        plain = run_json(worker, image)
        completed = subprocess.run(
            [str(worker), "analyse-json", str(image), "--qualify"],
            check=True, text=True, capture_output=True)
        combined = json.loads(completed.stdout)
        for key, value in plain.items():
            assert combined[key] == value, key
        for mode in ("defrag", "growth"):
            standalone = subprocess.run(
                [str(worker), f"preflight-{mode}", str(image)],
                text=True, capture_output=True)
            assert combined[f"{mode}_qualified"] == (standalone.returncode == 0)
            if standalone.returncode and not dirty:
                assert combined[f"{mode}_reason"] in standalone.stderr
        mapped = map_json("ntfs", image)
        assert mapped["defrag_qualified"] == combined["defrag_qualified"]
        assert mapped["growth_qualified"] == combined["growth_qualified"]
        assert "@@ANALYSIS" not in mapped.get("defrag_reason", "")
        assert hashlib.sha256(image.read_bytes()).digest() == digest
        assert completed.stderr.count('"completed":0') == 1
        if dirty:
            assert "dirty flag" in combined["defrag_reason"]


def test_ntfs(work: Path) -> None:
    worker = BUILD / "linux-defragger-ntfs-worker"
    image = work / "ntfs.img"
    make_ntfs_image(image, fragmented_data=True, directory_data=True)
    before = run_json(worker, image)
    assert before["fragmented_files"] > 0
    mapped_before = map_json("ntfs", image)
    assert mapped_before["filesystem"] == "ntfs"
    assert mapped_before["total_units"] == before["total_clusters"]
    assert mapped_before["fragmented_files"] == before["fragmented_files"]
    serial = before["serial"]
    output = mutate(worker, image, "defrag", work / "ntfs-defrag.journal")
    assert "NTFS unified workspace layout:" in output, output
    assert "internally verified raw NTFS working image" not in output, output
    packed = run_json(worker, image)
    assert packed["serial"] == serial
    assert_clean(packed, growth=False)
    assert map_json("ntfs", image)["fragmented_files"] == 0
    output = mutate(worker, image, "growth-defrag", work / "ntfs-growth.journal")
    assert "NTFS direct metadata layout:" in output, output
    assert "internally verified raw NTFS working image" not in output, output
    grown = run_json(worker, image)
    assert grown["serial"] == serial
    assert_clean(grown, growth=True)



def test_ntfs_relocates_named_data_stream(work: Path) -> None:
    worker = BUILD / "linux-defragger-ntfs-worker"
    image = work / "ntfs-ads.img"
    make_ntfs_image(
        image,
        fragmented_data=True,
        directory_data=True,
        named_ads=True,
    )
    from ntfs_test_fixture import ADS_CLUSTERS, CLUSTER_SIZE

    ads_payload = bytes((index * 29 + 13) & 0xFF
                        for index in range(ADS_CLUSTERS * CLUSTER_SIZE))
    assert image.read_bytes().find(ads_payload) == -1
    output = mutate(worker, image, "defrag", work / "ntfs-ads.journal")
    assert "unsupported-but-safe NTFS user stream" not in output
    assert image.read_bytes().find(ads_payload) >= 0

    growth = work / "ntfs-ads-growth.img"
    make_ntfs_image(
        growth,
        fragmented_data=True,
        directory_data=True,
        named_ads=True,
    )
    mutate(worker, growth, "growth-defrag",
           work / "ntfs-ads-growth.journal")
    assert growth.read_bytes().find(ads_payload) >= 0


def test_ntfs_preserves_safe_unsupported_user_stream(work: Path) -> None:
    worker = BUILD / "linux-defragger-ntfs-worker"
    image = work / "ntfs-fixed-user-stream.img"
    make_ntfs_image(
        image,
        fragmented_data=True,
        directory_data=True,
        fixed_attribute_list_stream=True,
    )
    from ntfs_test_fixture import FIXED_USER_LCN, FIXED_USER_CLUSTERS, CLUSTER_SIZE
    start = FIXED_USER_LCN * CLUSTER_SIZE
    length = FIXED_USER_CLUSTERS * CLUSTER_SIZE
    before_fixed = image.read_bytes()[start:start + length]
    completed = subprocess.run(
        [
            str(worker), "defrag", str(image), "--write", "--confirm", str(image),
            "--journal", str(work / "ntfs-fixed.journal"), "--batch-clusters", "4096",
            "--ram-buffer", "auto", "--workers", "auto", "--live-map-cells", "512",
        ],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert "Preserving 1 unsupported-but-safe NTFS user stream" in completed.stdout, completed.stdout
    assert "NTFS unified workspace layout:" in completed.stdout, completed.stdout
    after_fixed = image.read_bytes()[start:start + length]
    assert after_fixed == before_fixed
    assert_clean(run_json(worker, image), growth=False)

    growth = subprocess.run(
        [
            str(worker), "growth-defrag", str(image), "--write", "--confirm", str(image),
            "--journal", str(work / "ntfs-fixed-growth.journal"), "--batch-clusters", "4096",
            "--ram-buffer", "auto", "--workers", "auto", "--live-map-cells", "512",
            "--growth-percent", "10",
        ],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    assert "Preserving 1 unsupported-but-safe NTFS user stream" in growth.stdout, growth.stdout
    assert image.read_bytes()[start:start + length] == before_fixed
    assert_clean(run_json(worker, image), growth=True)

def test_ext(work: Path) -> None:
    worker = BUILD / "linux-defragger-ext-worker"
    fixture = BUILD / "linux-defragger-ext-fixture"
    image = work / "ext4.img"
    subprocess.run([str(fixture), str(image)], check=True)
    before = run_json(worker, image)
    assert before["fragmented_files"] > 0 or before["fragmented_directories"] > 0
    mapped_before = map_json("ext4", image)
    assert mapped_before["filesystem"] in {"ext2", "ext3", "ext4"}
    assert mapped_before["total_units"] == before["total_blocks"]
    assert mapped_before["fragmented_files"] == before["fragmented_files"]
    identity = before["uuid"]
    output = mutate(worker, image, "defrag", work / "ext-defrag.journal")
    assert "EXT unified workspace layout:" in output, output
    assert "EXT workspace staging complete:" in output, output
    assert "internally verified EXT working image" not in output, output
    packed = run_json(worker, image)
    assert packed["uuid"] == identity
    assert_clean(packed, growth=False)
    assert map_json("ext4", image)["fragmented_files"] == 0
    output = mutate(worker, image, "growth-defrag", work / "ext-growth.journal")
    assert "EXT unified workspace layout:" in output, output
    assert "EXT workspace staging complete:" in output, output
    assert "internally verified EXT working image" not in output, output
    grown = run_json(worker, image)
    assert grown["uuid"] == identity
    assert_clean(grown, growth=True)


def main() -> None:
    for name in ("linux-defragger-ext-worker", "linux-defragger-ntfs-worker",
                 "linux-defragger-exfat-worker", "linux-defragger-ext-fixture",
                 "linux-defragger-mapper"):
        assert (BUILD / name).is_file(), f"missing native test executable: {name}"
    with tempfile.TemporaryDirectory(prefix="linux-defragger-native83-") as directory:
        work = Path(directory)
        test_exfat(work)
        test_ntfs_combined_analysis(work)
        test_ntfs(work)
        test_ntfs_relocates_named_data_stream(work)
        test_ntfs_preserves_safe_unsupported_user_stream(work)
        test_ext(work)
    print("native EXT, NTFS and exFAT Defrag/Growth Defrag tests passed")


if __name__ == "__main__":
    main()
