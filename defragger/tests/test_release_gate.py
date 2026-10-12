#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Protect the small, single-path release pipeline and its safety invariants."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = PROJECT_ROOT.parent
WORKFLOWS = REPO_ROOT / ".github" / "workflows"


def require(text: str, *needles: str, label: str) -> None:
    for needle in needles:
        assert needle in text, f"{label} lost required contract: {needle}"


def main() -> None:
    gate = (WORKFLOWS / "quality-gate.yml").read_text(encoding="utf-8")
    release = (WORKFLOWS / "release.yml").read_text(encoding="utf-8")
    preflight = (PROJECT_ROOT / "ci" / "preflight.sh").read_text(encoding="utf-8")
    harness = (PROJECT_ROOT / "tests" / "run_tests.sh").read_text(encoding="utf-8")
    local_run = (PROJECT_ROOT / "packaging" / "build-local-run.sh").read_text(encoding="utf-8")
    cmake = (PROJECT_ROOT / "cmake" / "project.cmake").read_text(encoding="utf-8")
    design = (REPO_ROOT / "docs" / "DESIGN.md").read_text(encoding="utf-8")
    audit = (REPO_ROOT / "docs" / "AUDIT_STATUS.md").read_text(encoding="utf-8")
    version = (PROJECT_ROOT / "VERSION").read_text(encoding="utf-8").strip()
    gitignore = (REPO_ROOT / ".gitignore").read_text(encoding="utf-8")

    # There is exactly one permanent Linux qualification workflow and one release workflow.
    assert not (WORKFLOWS / "local-quality.yml").exists(), "duplicate local quality workflow must stay retired"
    assert not (WORKFLOWS / "apt-refresh.yml").exists(), "fake asynchronous APT refresh workflow must stay retired"

    self_hosted = "runs-on: [self-hosted, Linux, X64, linux-native, bigbedroom]"
    require(
        gate,
        "workflow_call:",
        "push:",
        "- main",
        "./ci/preflight.sh quality",
        "./ci/preflight.sh sanitizers",
        "LD_ENABLE_WERROR=ON",
        "git ls-files",
        "ctest --test-dir build --output-on-failure",
        "tests/test_btrfs_architecture.py",
        "tests/test_release_gate.py",
        "LD_ENABLE_SANITIZERS=ON",
        "Self-hosted ASan / UBSan",
        "Build and verify release artefacts once",
        "packaging/build-deb.sh",
        "packaging/build-local-run.sh",
        "test_local_installer_end_to_end.sh",
        "actions/upload-artifact@v4",
        "defragmenter-release-${{ github.sha }}",
        "publish-release:",
        "needs: [quality-gate, sanitizers]",
        "uses: ./.github/workflows/release.yml",
        "expected_sha: ${{ github.sha }}",
        label="quality gate",
    )
    assert gate.count(self_hosted) == 2, "normal and sanitizer lanes must use the self-hosted Linux fleet"
    assert "runs-on: ubuntu" not in gate
    assert "pull_request:" not in gate
    assert "merge_group:" not in gate
    assert gate.count("-E linux-defragger-tests") == 1, "only sanitizers may exclude the recursive aggregate harness"

    require(
        preflight,
        "quality)",
        "sanitizers)",
        "release)",
        "gtk+-3.0 ext2fs sqlite3 openssl",
        "import gi, cairo",
        label="runner preflight",
    )

    require(
        release,
        "workflow_call:",
        "expected_sha:",
        self_hosted,
        "./ci/preflight.sh release",
        "ref: ${{ inputs.expected_sha }}",
        "Status: **complete**",
        "Audited source commit:",
        "Audited release-governance commit:",
        'git diff --quiet "$AUDITED_COMMIT" HEAD',
        'git diff --quiet "$GOVERNANCE_COMMIT" HEAD',
        "Verify permanent main history protection",
        "required_linear_history",
        "actions/download-artifact@v4",
        "defragmenter-release-${{ inputs.expected_sha }}",
        "sha256sum -c RELEASE_SHA256SUMS.txt",
        "Publish immutable GitHub release",
        "Wait until the exact release is live in APT",
        "catalogue/apps.json",
        "dists/beta/main/binary-amd64/Packages.gz",
        "Central APT repository did not advertise",
        label="release workflow",
    )
    trigger_block = release.split("permissions:", 1)[0]
    assert "workflow_run:" not in trigger_block
    assert "workflow_dispatch:" not in trigger_block
    assert "\n  push:" not in trigger_block
    assert "packaging/build-deb.sh" not in release, "release job must publish the package already tested by qualification"
    assert "packaging/build-local-run.sh" not in release, "release job must not rebuild the local installer"
    assert "actions/upload-artifact" not in release
    assert "runs-on: ubuntu" not in release
    assert "gh release" not in release, "release publication must not depend on the GitHub CLI"
    assert "apt-refresh.yml" not in release

    require(
        cmake,
        "linux-defragger-native-malformed-matrix",
        "tests/test_native_malformed_matrix.py",
        "LINUX_DEFRAGGER_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}",
        label="CMake malformed-media matrix",
    )
    for required in (
        "tests/test_release_artifacts.sh",
        "tests/run_typecheck.sh",
        "tests/test_no_external_fs_tools.py",
        "tests/test_architecture.py",
        "tests/test_gui_models.py",
        "tests/test_gui_services.py",
        "tests/test_safety.py",
        "tests/test_native_top3.py",
        "tests/test_affs_native.py",
        "tests/test_hfsplus_native.py",
        "tests/test_xfs_writer.py",
        "verify_defragged_image.py",
        "verify_growth_defrag.py",
        "verify_growth_fat12_16.py",
    ):
        assert required in harness, f"aggregate harness lost required regression: {required}"

    assert "__pycache__/" in gitignore
    assert "*.py[cod]" in gitignore

    # The pinned Common checkout, committed gitlink, CMake and local installer must agree.
    expected_common = re.search(r'INFILTRATR_COMMON_EXPECTED_COMMIT\s+"([0-9a-f]{40})"', cmake)
    assert expected_common is not None, "CMake lost the exact Common commit"
    workflow_common = re.search(
        r"- name: Check out pinned Infiltratr Common.*?ref:\s*([0-9a-f]{40})",
        gate,
        re.DOTALL,
    )
    assert workflow_common is not None, "quality gate lost the exact Common checkout"
    assert workflow_common.group(1) == expected_common.group(1)

    gitlinks: dict[str, str] = {}
    for entry in subprocess.check_output(
        ["git", "ls-tree", "-r", "HEAD"], cwd=REPO_ROOT, text=True
    ).splitlines():
        metadata, path = entry.split("\t", 1)
        mode, _kind, sha = metadata.split()
        if mode == "160000":
            gitlinks[path] = sha
    assert gitlinks == {"defragger/shared/infiltratr-common": expected_common.group(1)}

    common_contract = (
        ('COMMON_TAG="v1.19.38"', 'INFILTRATR_COMMON_TAG "v1.19.38"'),
        ('COMMON_VERSION="1.19.38"', 'INFILTRATR_COMMON_EXPECTED_VERSION "1.19.38"'),
        (
            'COMMON_COMMIT="7070c5812b50821fd7580101cb2289a3184f6b2c"',
            '7070c5812b50821fd7580101cb2289a3184f6b2c',
        ),
    )
    for installer_required, cmake_required in common_contract:
        assert installer_required in local_run
        assert cmake_required in cmake
    assert "Infiltratr Common 1.19.38" in design
    assert "7070c5812b50821fd7580101cb2289a3184f6b2c" in design

    # The safety audit remains tied to an exact source baseline.
    require(
        audit,
        "Status: **complete**",
        "FAT12/FAT16/FAT32",
        "EXT2/EXT3/EXT4",
        "NTFS",
        "exFAT",
        "XFS",
        "Amiga OFS/FFS",
        "Btrfs",
        "Amiga SFS0",
        "Classic Macintosh HFS",
        "APFS",
        "single active two-object checkpoint",
        "HFS+/HFSX",
        "Project quality gate",
        "protected-main",
        "direct-main",
        "explicit release decision",
        label="completed safety audit",
    )
    assert f"Applies to: release version {version}" in audit

    audited_commit_match = re.search(r"^Audited source commit:\s*([0-9a-f]{40})$", audit, re.MULTILINE)
    assert audited_commit_match is not None
    audited_commit = audited_commit_match.group(1)
    subprocess.run(["git", "cat-file", "-e", f"{audited_commit}^{{commit}}"], cwd=REPO_ROOT, check=True)
    subprocess.run(["git", "merge-base", "--is-ancestor", audited_commit, "HEAD"], cwd=REPO_ROOT, check=True)
    audited_paths = (
        "defragger/CMakeLists.txt",
        "defragger/cmake",
        "defragger/native",
        "defragger/gui",
        "defragger/src",
        "defragger/shared",
        "defragger/packaging",
        "defragger/test_media",
    )
    assert subprocess.run(
        ["git", "diff", "--quiet", audited_commit, "HEAD", "--", *audited_paths],
        cwd=REPO_ROOT,
        check=False,
    ).returncode == 0, "production/build/package source changed after the audited source baseline"

    governance_match = re.search(
        r"^Audited release-governance commit:\s*([0-9a-f]{40})$", audit, re.MULTILINE
    )
    assert governance_match is not None
    governance_commit = governance_match.group(1)
    subprocess.run(["git", "cat-file", "-e", f"{governance_commit}^{{commit}}"], cwd=REPO_ROOT, check=True)
    subprocess.run(["git", "merge-base", "--is-ancestor", governance_commit, "HEAD"], cwd=REPO_ROOT, check=True)

    # Governance can evolve on ordinary main commits. A release commit is the point
    # at which the workflow baseline must already have been audited and frozen.
    subject = subprocess.check_output(["git", "log", "-1", "--format=%s"], cwd=REPO_ROOT, text=True).strip()
    if subject.startswith("Release "):
        assert subprocess.run(
            ["git", "diff", "--quiet", governance_commit, "HEAD", "--", ".github/workflows"],
            cwd=REPO_ROOT,
            check=False,
        ).returncode == 0, "release workflows changed after the recorded governance baseline"


if __name__ == "__main__":
    main()
