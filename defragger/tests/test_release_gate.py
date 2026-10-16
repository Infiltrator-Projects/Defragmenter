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



def _assert_release_intent_behaviour(intent: str) -> None:
    """Execute the real workflow decision against disposable Git histories."""
    import os
    import tempfile

    script = intent.split("run: |\n", 1)[1]
    script = "\n".join(line[10:] if line.startswith("          ") else line
                       for line in script.splitlines())
    with tempfile.TemporaryDirectory(prefix="defrag-release-intent-") as temporary:
        root = Path(temporary)
        project = root / "defragger"
        project.mkdir()
        version_file = project / "VERSION"
        version_file.write_text("1.8.0-267\n", encoding="utf-8")

        def git(*arguments: str) -> str:
            return subprocess.check_output(
                ["git", "-C", str(root), *arguments], text=True).strip()

        git("init", "-q", "-b", "main")
        git("config", "user.name", "Release contract fixture")
        git("config", "user.email", "fixture@example.invalid")
        git("config", "commit.gpgsign", "false")
        git("config", "tag.gpgsign", "false")
        git("add", ".")
        git("commit", "-qm", "Initial candidate")

        def decision(expected: str) -> None:
            output = root / "outputs"
            output.write_text("", encoding="utf-8")
            environment = dict(os.environ, GITHUB_WORKSPACE=str(root),
                               GITHUB_SHA=git("rev-parse", "HEAD"),
                               GITHUB_OUTPUT=str(output))
            result = subprocess.run(["bash", "-eu", "-o", "pipefail", "-c", script],
                                    cwd=project, env=environment,
                                    capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            assert output.read_text(encoding="utf-8").strip() == f"release={expected}"

        decision("true")
        (root / "correction").write_text("Reviewed correction\n", encoding="utf-8")
        git("add", ".")
        git("commit", "-qm", "Correct candidate without changing VERSION")
        decision("true")
        git("tag", "v1.8.0-267")
        decision("true")
        (root / "ordinary").write_text("Documentation maintenance\n", encoding="utf-8")
        git("add", ".")
        git("commit", "-qm", "Ordinary maintenance after publication")
        decision("false")
        version_file.write_text("1.8.0-268\n", encoding="utf-8")
        git("add", ".")
        git("commit", "-qm", "Advance VERSION")
        decision("true")


def main() -> None:
    gate = _read(WORKFLOWS / "quality-gate.yml")
    release = _read(WORKFLOWS / "release.yml")
    release_script = _read(RELEASE_SCRIPT)
    audit = _read(AUDIT_FILE)

    # The maintained workflow surface is deliberately complete and minimal:
    # one Linux qualification workflow and one publication workflow. Ad-hoc
    # runner probes, experiments and historical helper workflows must stay out
    # of the product repository because they create an unaudited CI control plane.
    assert {path.name for path in WORKFLOWS.glob("*.yml")} == {
        "quality-gate.yml",
        "release.yml",
    }

    # Qualification runs on main, not PR-only branches, and has two permanent
    # self-hosted lanes. A VERSION change is the release signal; English commit
    # wording is not part of the control plane. An unpublished VERSION persists
    # across corrective commits. Main candidates retain serialized qualification
    # and the exact-commit retry contract.
    assert "workflow_call:" in gate
    assert "push:" in gate and "- main" in gate
    assert "pull_request:" not in gate
    assert "merge_group:" not in gate
    assert "cancel-in-progress: false" in gate
    assert "cancel-in-progress: true" not in gate
    runner = "runs-on: [self-hosted, Linux, X64, linux-native, bigbedroom]"
    assert gate.count(runner) == 2
    assert "release_intent:" in gate
    intent = _step_containing(gate, "id: release_intent")
    assert "defragger/VERSION" in intent
    assert "refs/tags/v" in intent
    _assert_release_intent_behaviour(intent)
    assert "GITHUB_OUTPUT" in intent
    assert "head_commit.message" not in gate
    assert "startsWith(" not in gate

    # Third-party workflow code is itself part of the qualified build graph.
    # Only actions that are actually required remain, and mutable major tags are
    # forbidden so rerunning an old source revision cannot execute newer code.
    preserve_release = _step_containing(gate, UPLOAD_ARTIFACT_ACTION)
    assert "actions/setup-node@" not in gate
    assert "actions/upload-artifact@v4" not in gate
    # The SHA-bound artefact is the only permitted publication input. Retain it
    # long enough for a multi-day GitHub/APT outage to be resumed without ever
    # rebuilding a release that has already been qualified.
    assert "retention-days: 7" in preserve_release

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
    for qualified_input in (
        "defragger/CMakeLists.txt",
        "defragger/cmake",
        "defragger/native",
        "defragger/gui",
        "defragger/src",
        "defragger/shared",
        "defragger/packaging",
        "defragger/test_media",
        "defragger/tests",
        "defragger/install.sh",
    ):
        assert qualified_input in release_script
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
        "defragger/tests",
        "defragger/install.sh",
    )
    result = subprocess.run(
        ["git", "diff", "--quiet", audited_source, "HEAD", "--", *audited_paths],
        cwd=REPO_ROOT,
        check=False,
    )
    assert result.returncode == 0, (
        "qualified source/test/install input changed after the audited source baseline"
    )

    print("release contracts: semantic gate passed")


if __name__ == "__main__":
    main()
