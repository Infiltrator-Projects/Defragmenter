#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Protect release behaviour without coupling tests to workflow prose."""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = PROJECT_ROOT.parent
WORKFLOWS = REPO_ROOT / ".github" / "workflows"
RELEASE_SCRIPT = PROJECT_ROOT / "ci" / "release.sh"
AUDIT_FILE = REPO_ROOT / "docs" / "AUDIT_STATUS.md"

SETUP_NODE_ACTION = "actions/setup-node@49933ea5288caeca8642d1e84afbd3f7d6820020"
UPLOAD_ARTIFACT_ACTION = "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02"
DOWNLOAD_ARTIFACT_ACTION = "actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _step_containing(workflow: str, needle: str) -> str:
    for block in workflow.split("\n      - "):
        if needle in block:
            return block
    raise AssertionError(f"workflow has no step containing {needle!r}")


def _audit_sha(audit: str, label: str) -> str:
    match = re.search(rf"^{re.escape(label)}:\s*([0-9a-f]{{40}})$", audit, re.MULTILINE)
    assert match is not None, f"audit lost {label}"
    return match.group(1)


def _assert_ancestor(commit: str) -> None:
    subprocess.run(
        ["git", "cat-file", "-e", f"{commit}^{{commit}}"],
        cwd=REPO_ROOT,
        check=True,
    )
    subprocess.run(
        ["git", "merge-base", "--is-ancestor", commit, "HEAD"],
        cwd=REPO_ROOT,
        check=True,
    )


def main() -> None:
    gate = _read(WORKFLOWS / "quality-gate.yml")
    release = _read(WORKFLOWS / "release.yml")
    release_script = _read(RELEASE_SCRIPT)
    audit = _read(AUDIT_FILE)

    # Keep one Linux qualification workflow, one publication workflow and the
    # independent Windows runner smoke check. Historical helper workflows stay gone.
    assert {path.name for path in WORKFLOWS.glob("*.yml")} == {
        "quality-gate.yml",
        "release.yml",
        "windows-runner-smoke.yml",
    }

    # Qualification runs on main, not PR-only branches, and has two permanent
    # self-hosted lanes. A VERSION change is the release signal; English commit
    # wording is not part of the control plane.
    assert "workflow_call:" in gate
    assert "push:" in gate and "- main" in gate
    assert "pull_request:" not in gate
    assert "merge_group:" not in gate
    runner = "runs-on: [self-hosted, Linux, X64, linux-native, bigbedroom]"
    assert gate.count(runner) == 2
    assert "release_intent:" in gate
    intent = _step_containing(gate, "id: release_intent")
    assert "defragger/VERSION" in intent
    assert "github.event.before" in intent
    assert "GITHUB_OUTPUT" in intent
    assert "head_commit.message" not in gate
    assert "startsWith(" not in gate

    # Third-party workflow code is itself part of the qualified build graph.
    # Pin mutable major tags to exact commits so rerunning an old source revision
    # cannot silently execute different setup or artefact-transfer code.
    assert SETUP_NODE_ACTION in gate
    preserve_release = _step_containing(gate, UPLOAD_ARTIFACT_ACTION)
    assert "actions/setup-node@v4" not in gate
    assert "actions/upload-artifact@v4" not in gate

    build_release = _step_containing(gate, "packaging/build-deb.sh")
    assert "if: steps.release_intent.outputs.release == 'true'" in build_release
    assert "if: steps.release_intent.outputs.release == 'true'" in preserve_release
    assert "needs: [quality-gate, sanitizers]" in gate
    assert "if: needs.quality-gate.outputs.release_intent == 'true'" in gate
    assert "uses: ./.github/workflows/release.yml" in gate
    assert "expected_sha: ${{ github.sha }}" in gate

    # Publication YAML is deliberately thin. Procedure belongs in release.sh,
    # while the workflow owns only checkout, permissions, artefact handoff and order.
    assert "workflow_call:" in release
    assert "expected_sha:" in release
    assert release.count(runner) == 1
    assert "./ci/preflight.sh release" in release
    for command in (
        "./ci/release.sh verify-source",
        "./ci/release.sh verify-main-protection",
        "./ci/release.sh publish",
        "./ci/release.sh wait-apt",
    ):
        assert command in release
    assert DOWNLOAD_ARTIFACT_ACTION in release
    assert "actions/download-artifact@v4" not in release
    assert "packaging/build-deb.sh" not in release
    assert "packaging/build-local-run.sh" not in release
    assert "curl " not in release
    assert "jq " not in release
    assert "api.github.com" not in release
    assert "catalogue/apps.json" not in release
    assert "Packages.gz" not in release
    apt_step = _step_containing(release, "./ci/release.sh wait-apt")
    assert "continue-on-error: true" in apt_step

    # The release implementation itself is executable shell with a small stable
    # command interface. Audit every script that can change the environment
    # contract seen by qualification/publication, not only the publisher itself.
    assert RELEASE_SCRIPT.stat().st_mode & 0o111
    subprocess.run(["bash", "-n", str(RELEASE_SCRIPT)], check=True)
    help_text = subprocess.check_output(
        [str(RELEASE_SCRIPT), "--help"], text=True, cwd=PROJECT_ROOT
    )
    for command in ("verify-source", "verify-main-protection", "publish", "wait-apt"):
        assert command in help_text
    for governance_path in (
        ".github/workflows",
        "defragger/ci/preflight.sh",
        "defragger/ci/release.sh",
        "defragger/tests/test_release_gate.py",
    ):
        assert governance_path in release_script
    assert "git diff --quiet" in release_script
    assert "sha256sum -c" in release_script
    assert ".digest" in release_script
    assert "required_linear_history" in release_script

    # The source audit remains exact. Governance may evolve on ordinary main
    # commits; release.sh freezes it at publication time against this baseline.
    assert "Status: **complete**" in audit
    audited_source = _audit_sha(audit, "Audited source commit")
    audited_governance = _audit_sha(audit, "Audited release-governance commit")
    _assert_ancestor(audited_source)
    _assert_ancestor(audited_governance)

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
    result = subprocess.run(
        ["git", "diff", "--quiet", audited_source, "HEAD", "--", *audited_paths],
        cwd=REPO_ROOT,
        check=False,
    )
    assert result.returncode == 0, (
        "production/build/package source changed after the audited source baseline"
    )

    print("release contracts: semantic gate passed")


if __name__ == "__main__":
    main()
