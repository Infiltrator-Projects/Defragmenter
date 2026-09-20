#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ensure release publication cannot bypass main-only CI or the completed audit."""

from __future__ import annotations

import ast
import re
import subprocess
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = PROJECT_ROOT.parent


def main() -> None:
    gate = (REPO_ROOT / ".github" / "workflows" / "quality-gate.yml").read_text(encoding="utf-8")
    local_quality = (REPO_ROOT / ".github" / "workflows" / "local-quality.yml").read_text(encoding="utf-8")
    release = (REPO_ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8")
    apt_refresh = (REPO_ROOT / ".github" / "workflows" / "apt-refresh.yml").read_text(encoding="utf-8")
    harness = (PROJECT_ROOT / "tests" / "run_tests.sh").read_text(encoding="utf-8")
    local_run = (PROJECT_ROOT / "packaging" / "build-local-run.sh").read_text(encoding="utf-8")
    release_artifacts = (PROJECT_ROOT / "tests" / "test_release_artifacts.sh").read_text(encoding="utf-8")
    cmake = (PROJECT_ROOT / "cmake" / "project.cmake").read_text(encoding="utf-8")
    design = (REPO_ROOT / "docs" / "DESIGN.md").read_text(encoding="utf-8")
    audit = (REPO_ROOT / "docs" / "AUDIT_STATUS.md").read_text(encoding="utf-8")
    architecture = (PROJECT_ROOT / "tests" / "test_architecture.py").read_text(encoding="utf-8")
    version = (PROJECT_ROOT / "VERSION").read_text(encoding="utf-8").strip()
    gitignore = (REPO_ROOT / ".gitignore").read_text(encoding="utf-8")

    for required in (
        "workflow_call:",
        "push:",
        "- main",
        "LD_ENABLE_WERROR=ON",
        "git ls-files",
        "ctest --test-dir build --output-on-failure",
        "tests/test_btrfs_architecture.py",
        "tests/test_release_gate.py",
    ):
        assert required in gate, f"quality gate lost required check: {required}"
    assert "pull_request:" not in gate, "quality gate must not require PR branches"
    assert "merge_group:" not in gate, "quality gate must not require merge branches"
    assert '"c-first-*"' not in gate, "quality gate must run from main only"
    assert "local-quality" not in gate, "home-runner qualification must not gate hosted release publication"
    for required in (
        "publish-release:",
        "needs: [quality-gate, sanitizers]",
        "github.event_name == 'push'",
        "github.ref == 'refs/heads/main'",
        "startsWith(github.event.head_commit.message, 'Release ')",
        "uses: ./.github/workflows/release.yml",
        "expected_sha: ${{ github.sha }}",
        "secrets: inherit",
    ):
        assert required in gate, f"qualified release handoff lost required contract: {required}"
    for required in (
        "runs-on: [self-hosted, Linux, X64, linux-native]",
        "Verify local qualification dependencies",
        "Self-hosted runner is missing commands",
        "Self-hosted runner is missing pkg-config packages",
        "workflow_dispatch:",
    ):
        assert required in local_quality, f"local qualification lost required check: {required}"
    assert "run: ctest --test-dir build --output-on-failure" in gate, (
        "primary quality gate must run the complete aggregate project suite"
    )
    sanitizer_exclusion = "-E linux-defragger-tests"
    assert gate.count(sanitizer_exclusion) == 1, (
        "only the sanitizer lane may exclude the recursive aggregate packaging harness"
    )

    for required in (
        "tests/test_release_artifacts.sh",
        "tests/run_typecheck.sh",
        "tests/test_no_external_fs_tools.py",
        "tests/test_native_malformed_matrix.py",
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

    trigger_block = release.split("permissions:", 1)[0]
    assert "workflow_call:" in trigger_block
    assert "expected_sha:" in trigger_block
    assert "workflow_run:" not in trigger_block, (
        "release publication must be a direct dependency of the quality gate so retries cannot lose the handoff"
    )
    assert "workflow_dispatch:" not in trigger_block, (
        "release publication must not require manual approval"
    )
    assert "\n  push:" not in trigger_block, (
        "release publication must be invoked only by the completed main quality gate"
    )
    assert ".release-request" not in release, (
        "legacy root release marker must not be part of the release contract"
    )
    for required in (
        "EXPECTED_SHA: ${{ inputs.expected_sha }}",
        "ref: ${{ inputs.expected_sha }}",
        "version: ${{ steps.release_identity.outputs.version }}",
        "release_sha: ${{ steps.release_identity.outputs.release_sha }}",
        "Verify permanent main history protection",
        '"repos/${GITHUB_REPOSITORY}/rules/branches/main"',
        "deletion",
        "non_fast_forward",
        "required_linear_history",
        "EXPECTED_SHA",
        "origin/main",
        "RELEASE_SHA256SUMS.txt",
        "Status: **complete**",
        'Applies to: release version ${VERSION}',
        "Audited source commit:",
        'git diff --quiet "$AUDITED_COMMIT" HEAD',
        "CMakeLists.txt cmake native gui src shared packaging",
        "Audited release-governance commit:",
        "GOVERNANCE_COMMIT",
        ":(top).github/workflows",
        "points at ${tag_commit}, not the tested commit",
        "tag_exists=false",
        "if [ \"$tag_exists\" = false ]",
        "gh release create",
        "uses: ./.github/workflows/apt-refresh.yml",
        "needs: release",
        "version: ${{ needs.release.outputs.version }}",
        "release_sha: ${{ needs.release.outputs.release_sha }}",
    ):
        assert required in release, f"release workflow lost required contract: {required}"
    assert "quarantine-notice:" not in release
    assert "if: ${{ false }}" not in release
    assert "--clobber" not in release
    assert "gh release edit" not in release
    assert "packaging/build-source-zip.sh" not in release
    assert 'Defragmenter-${VERSION}.zip' not in release
    assert not (PROJECT_ROOT / "packaging" / "build-source-zip.sh").exists()
    assert "Refresh and verify Infiltrator APT repository" not in release
    assert "workflow_run:" not in apt_refresh, (
        "APT publication must be directly called from the release workflow so release retries cannot lose the handoff"
    )
    for required in (
        "workflow_call:",
        "workflow_dispatch:",
        "version:",
        "release_sha:",
        "REQUESTED_VERSION",
        "REQUESTED_SHA",
        "releases/tags/${release_tag}",
        "Published release ${release_tag} points at",
        "APT_REPOSITORY_DISPATCH_TOKEN",
        "application-release",
        "catalogue/apps.json",
        "Central APT repository did not advertise",
    ):
        assert required in apt_refresh, f"APT refresh workflow lost required contract: {required}"

    assert "LD_ENABLE_SANITIZERS=ON" in gate
    assert "Hosted ASan / UBSan" in gate
    assert "__pycache__/" in gitignore
    assert "*.py[cod]" in gitignore

    common_contract = (
        ('COMMON_TAG="v1.19.10"', 'INFILTRATR_COMMON_TAG "v1.19.10"'),
        ('COMMON_VERSION="1.19.10"', 'INFILTRATR_COMMON_EXPECTED_VERSION "1.19.10"'),
        (
            'COMMON_COMMIT="33e69c0a462b56d388881d89c4eb49f72fa0b0fe"',
            '33e69c0a462b56d388881d89c4eb49f72fa0b0fe',
        ),
    )
    for local_required, cmake_required in common_contract:
        assert local_required in local_run, (
            f"local installer lost Common release contract: {local_required}"
        )
        assert cmake_required in cmake, (
            f"CMake lost matching Common release contract: {cmake_required}"
        )
    for stale_version in ("1.5.0", "1.6.0"):
        assert f"v{stale_version}" not in local_run
        assert f'COMMON_VERSION="{stale_version}"' not in local_run
        assert f"Infiltratr Common {stale_version}" not in design
    assert "Infiltratr Common 1.19.10" in design
    assert "33e69c0a462b56d388881d89c4eb49f72fa0b0fe" in design

    for required in (
        "Status: **complete**",
        "FAT12/FAT16/FAT32",
        "EXT2/EXT3/EXT4",
        "NTFS",
        "exFAT",
        "XFS",
        "Amiga OFS/FFS",
        "Amiga SFS0",
        "HFS+/HFSX",
        "Project quality gate",
        "protected-main",
        "direct-main",
        "explicit release decision",
    ):
        assert required in audit, f"completed safety audit lost evidence: {required}"

    assert f"Applies to: release version {version}" in audit, (
        "completed safety audit does not apply to the current VERSION"
    )

    audited_commit_match = re.search(
        r"^Audited source commit:\s*([0-9a-f]{40})$", audit, re.MULTILINE
    )
    assert audited_commit_match is not None, (
        "completed safety audit is not bound to an exact source commit"
    )
    audited_commit = audited_commit_match.group(1)
    subprocess.run(
        ["git", "cat-file", "-e", f"{audited_commit}^{{commit}}"],
        cwd=REPO_ROOT,
        check=True,
    )
    subprocess.run(
        ["git", "merge-base", "--is-ancestor", audited_commit, "HEAD"],
        cwd=REPO_ROOT,
        check=True,
    )
    audited_paths = (
        "defragger/CMakeLists.txt",
        "defragger/cmake",
        "defragger/native",
        "defragger/gui",
        "defragger/src",
        "defragger/shared",
        "defragger/packaging",
    )
    drift = subprocess.run(
        ["git", "diff", "--quiet", audited_commit, "HEAD", "--", *audited_paths],
        cwd=REPO_ROOT,
        check=False,
    )
    assert drift.returncode == 0, (
        "audited production/build/package source changed after the recorded audit "
        f"baseline {audited_commit}"
    )

    governance_match = re.search(
        r"^Audited release-governance commit:\s*([0-9a-f]{40})$",
        audit,
        re.MULTILINE,
    )
    assert governance_match is not None, (
        "completed safety audit is not bound to an exact release-governance commit"
    )
    governance_commit = governance_match.group(1)
    subprocess.run(
        ["git", "cat-file", "-e", f"{governance_commit}^{{commit}}"],
        cwd=REPO_ROOT,
        check=True,
    )
    subprocess.run(
        ["git", "merge-base", "--is-ancestor", governance_commit, "HEAD"],
        cwd=REPO_ROOT,
        check=True,
    )
    governance_drift = subprocess.run(
        [
            "git",
            "diff",
            "--quiet",
            governance_commit,
            "HEAD",
            "--",
            ".github/workflows",
        ],
        cwd=REPO_ROOT,
        check=False,
    )
    assert governance_drift.returncode == 0, (
        "release-governance workflows changed after the recorded audit "
        f"baseline {governance_commit}"
    )

    writer_assignment = None
    for node in ast.parse(architecture).body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id == "NATIVE_WRITERS"
            for target in node.targets
        ):
            writer_assignment = ast.literal_eval(node.value)
            break
    assert isinstance(writer_assignment, dict), (
        "cannot determine authoritative native writer set from architecture test"
    )
    match = re.search(r"^Audited writer IDs:\s*(.+)$", audit, re.MULTILINE)
    assert match is not None, "completed safety audit lost Audited writer IDs"
    audited_writer_ids = {
        item.strip() for item in match.group(1).split(",") if item.strip()
    }
    expected_writer_ids = set(writer_assignment)
    assert audited_writer_ids == expected_writer_ids, (
        "safety audit writer scope does not match enabled native writers: "
        f"audited={sorted(audited_writer_ids)} enabled={sorted(expected_writer_ids)}"
    )

    print("release quality-gate contract passed")


if __name__ == "__main__":
    main()
