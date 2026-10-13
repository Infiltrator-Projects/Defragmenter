#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Release implementation for Defragmenter. GitHub Actions only orchestrates
# these commands; release policy and publication mechanics live here.
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
REPO_ROOT=$(cd -- "$PROJECT_ROOT/.." && pwd)
AUDIT_FILE="$REPO_ROOT/docs/AUDIT_STATUS.md"
VERSION_FILE="$PROJECT_ROOT/VERSION"

fail() {
    printf 'release: %s\n' "$*" >&2
    exit 1
}

need_command() {
    command -v "$1" >/dev/null 2>&1 || fail "required command is missing: $1"
}

release_version() {
    local value
    value=$(tr -d '\r\n' <"$VERSION_FILE")
    [[ "$value" =~ ^[0-9]+\.[0-9]+\.[0-9]+-[0-9]+$ ]] ||
        fail "VERSION is not a release version: $value"
    printf '%s\n' "$value"
}

require_sha() {
    [[ "$1" =~ ^[0-9a-f]{40}$ ]] || fail "invalid commit SHA: $1"
}

audit_commit() {
    local label=$1
    local value
    value=$(sed -n "s/^${label}: \([0-9a-f]\{40\}\)$/\\1/p" "$AUDIT_FILE")
    require_sha "$value"
    printf '%s\n' "$value"
}

require_github_context() {
    : "${GH_TOKEN:?GH_TOKEN is required}"
    : "${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
}

github_api() {
    local method=$1
    local url=$2
    shift 2
    curl --fail-with-body --silent --show-error --request "$method" \
        --header "Authorization: Bearer ${GH_TOKEN}" \
        --header 'Accept: application/vnd.github+json' \
        --header 'X-GitHub-Api-Version: 2022-11-28' \
        "$@" "$url"
}

fetch_release() {
    local tag=$1
    github_api GET "https://api.github.com/repos/${GITHUB_REPOSITORY}/releases/tags/${tag}"
}

verify_source() {
    local expected_sha=${1:?expected SHA is required}
    require_sha "$expected_sha"
    need_command git

    cd "$REPO_ROOT"
    git fetch origin main --tags
    [[ "$(git rev-parse HEAD)" == "$expected_sha" ]] ||
        fail "checked-out commit is not the qualified release commit"
    git merge-base --is-ancestor "$expected_sha" origin/main ||
        fail "qualified release commit is no longer part of main"

    local version tag audited_commit governance_commit
    version=$(release_version)
    tag="v${version}"

    grep -Fq 'Status: **complete**' "$AUDIT_FILE" ||
        fail "safety audit is not complete"
    audited_commit=$(audit_commit 'Audited source commit')
    governance_commit=$(audit_commit 'Audited release-governance commit')

    git cat-file -e "${audited_commit}^{commit}"
    git cat-file -e "${governance_commit}^{commit}"
    git merge-base --is-ancestor "$audited_commit" "$expected_sha" ||
        fail "audited source baseline is not an ancestor of the release"
    git merge-base --is-ancestor "$governance_commit" "$expected_sha" ||
        fail "release-governance baseline is not an ancestor of the release"

    local source_paths=(
        defragger/CMakeLists.txt
        defragger/cmake
        defragger/native
        defragger/gui
        defragger/src
        defragger/shared
        defragger/packaging
        defragger/test_media
    )
    git diff --quiet "$audited_commit" "$expected_sha" -- "${source_paths[@]}" ||
        fail "production/build/package source changed after the audited source baseline"

    local governance_paths=(
        .github/workflows
        defragger/ci/preflight.sh
        defragger/ci/release.sh
        defragger/tests/test_release_gate.py
    )
    git diff --quiet "$governance_commit" "$expected_sha" -- "${governance_paths[@]}" ||
        fail "release governance changed after the audited governance baseline"

    if git rev-parse -q --verify "refs/tags/${tag}" >/dev/null; then
        [[ "$(git rev-list -n 1 "$tag")" == "$expected_sha" ]] ||
            fail "release tag ${tag} does not point at the qualified commit"
    fi

    printf 'release %s is qualified at %s\n' "$version" "$expected_sha"
}

verify_main_protection() {
    require_github_context
    need_command curl
    need_command jq

    local rules required
    rules=$(github_api GET \
        "https://api.github.com/repos/${GITHUB_REPOSITORY}/rules/branches/main")
    for required in deletion non_fast_forward required_linear_history; do
        jq -e --arg required "$required" \
            'any(.[]; .type == $required)' <<<"$rules" >/dev/null ||
            fail "main is missing permanent ${required} protection"
    done
}

load_artifacts() {
    local directory=${1:?artifact directory is required}
    need_command sha256sum
    need_command dpkg-deb

    RELEASE_VERSION=$(release_version)
    RELEASE_DIRECTORY=$(cd -- "$directory" && pwd)
    RELEASE_DEB="Defragmenter-${RELEASE_VERSION}-amd64.deb"
    RELEASE_RUN="Defragmenter-${RELEASE_VERSION}-local-folder.run"
    RELEASE_SUMS='RELEASE_SHA256SUMS.txt'

    cd "$RELEASE_DIRECTORY"
    [[ -f "$RELEASE_DEB" ]] || fail "missing release asset: $RELEASE_DEB"
    [[ -f "$RELEASE_RUN" ]] || fail "missing release asset: $RELEASE_RUN"
    [[ -f "$RELEASE_SUMS" ]] || fail "missing release asset: $RELEASE_SUMS"
    sha256sum -c "$RELEASE_SUMS"
    [[ "$(dpkg-deb -f "$RELEASE_DEB" Version)" == "$RELEASE_VERSION" ]] ||
        fail "Debian package version does not match VERSION"
    RELEASE_PACKAGE=$(dpkg-deb -f "$RELEASE_DEB" Package)
}

publish_release() {
    local expected_sha=${1:?expected SHA is required}
    local directory=${2:?artifact directory is required}
    require_sha "$expected_sha"
    require_github_context
    need_command curl
    need_command jq

    [[ "$(git -C "$REPO_ROOT" rev-parse HEAD)" == "$expected_sha" ]] ||
        fail "publish attempted from a different commit than the qualified release"
    load_artifacts "$directory"

    local tag="v${RELEASE_VERSION}"
    local release_file status release_json upload_url
    release_file=$(mktemp)
    trap "rm -f '$release_file'" EXIT

    status=$(curl --silent --show-error --output "$release_file" --write-out '%{http_code}' \
        --header "Authorization: Bearer ${GH_TOKEN}" \
        --header 'Accept: application/vnd.github+json' \
        --header 'X-GitHub-Api-Version: 2022-11-28' \
        "https://api.github.com/repos/${GITHUB_REPOSITORY}/releases/tags/${tag}")

    if [[ "$status" == 200 ]]; then
        jq -e --arg sha "$expected_sha" \
            'select(.draft == false and .prerelease == false and .target_commitish == $sha)' \
            "$release_file" >/dev/null ||
            fail "existing release ${tag} does not match the qualified commit"
    elif [[ "$status" == 404 ]]; then
        local body payload
        body=$(printf '%s\n\n%s\n%s\n%s\n%s\n' \
            "Defragmenter ${RELEASE_VERSION}." \
            'These are the exact artefacts built and tested by the project quality gate:' \
            '- Generic AMD64 Debian package' \
            '- Hardware-optimised local compile-and-install program' \
            '- SHA-256 manifest for both artefacts')
        payload=$(jq -n \
            --arg tag "$tag" \
            --arg target "$expected_sha" \
            --arg title "Defragmenter ${RELEASE_VERSION}" \
            --arg body "$body" \
            '{tag_name:$tag,target_commitish:$target,name:$title,body:$body,draft:false,prerelease:false}')
        github_api POST \
            "https://api.github.com/repos/${GITHUB_REPOSITORY}/releases" \
            --header 'Content-Type: application/json' \
            --data "$payload" >/dev/null
    else
        cat "$release_file" >&2
        fail "unable to inspect release ${tag} (HTTP ${status})"
    fi

    release_json=$(fetch_release "$tag")
    jq -e --arg sha "$expected_sha" \
        'select(.draft == false and .prerelease == false and .target_commitish == $sha)' \
        <<<"$release_json" >/dev/null ||
        fail "published release ${tag} does not match the qualified commit"
    upload_url=$(jq -er '.upload_url | split("{")[0]' <<<"$release_json")

    local unexpected_assets asset asset_count
    unexpected_assets=$(jq -r \
        --arg deb "$RELEASE_DEB" --arg run "$RELEASE_RUN" --arg sums "$RELEASE_SUMS" \
        '.assets[].name | select(. != $deb and . != $run and . != $sums)' \
        <<<"$release_json")
    [[ -z "$unexpected_assets" ]] ||
        fail "release ${tag} contains unexpected assets: ${unexpected_assets}"

    cd "$RELEASE_DIRECTORY"
    for asset in "$RELEASE_DEB" "$RELEASE_RUN" "$RELEASE_SUMS"; do
        release_json=$(fetch_release "$tag")
        asset_count=$(jq --arg name "$asset" \
            '[.assets[] | select(.name == $name)] | length' <<<"$release_json")
        if [[ "$asset_count" -eq 0 ]]; then
            github_api POST "${upload_url}?name=${asset}" \
                --header 'Content-Type: application/octet-stream' \
                --data-binary "@${asset}" >/dev/null
        elif [[ "$asset_count" -ne 1 ]]; then
            fail "release ${tag} contains duplicate asset ${asset}"
        fi
    done

    release_json=$(fetch_release "$tag")
    local published_count expected_digest published_digest
    published_count=$(jq '.assets | length' <<<"$release_json")
    [[ "$published_count" -eq 3 ]] ||
        fail "release ${tag} has an unexpected asset count"

    for asset in "$RELEASE_DEB" "$RELEASE_RUN" "$RELEASE_SUMS"; do
        asset_count=$(jq --arg name "$asset" \
            '[.assets[] | select(.name == $name)] | length' <<<"$release_json")
        [[ "$asset_count" -eq 1 ]] ||
            fail "release ${tag} does not contain exactly one ${asset}"
        expected_digest="sha256:$(sha256sum "$asset" | awk '{print $1}')"
        published_digest=$(jq -er --arg name "$asset" \
            '.assets[] | select(.name == $name) | .digest' <<<"$release_json")
        [[ "$published_digest" == "$expected_digest" ]] ||
            fail "published asset ${asset} does not match the qualified artefact"
    done

    printf 'release %s published and verified\n' "$tag"
}

wait_for_apt() {
    local directory=${1:?artifact directory is required}
    load_artifacts "$directory"
    need_command curl
    need_command gzip
    need_command awk
    need_command jq

    local catalogue='https://infiltrator-projects.github.io/Infiltrator-Repository/catalogue/apps.json'
    local packages='https://infiltrator-projects.github.io/Infiltrator-Repository/dists/beta/main/binary-amd64/Packages.gz'
    local attempt cache_key catalogue_version apt_match

    for attempt in $(seq 1 20); do
        cache_key="${GITHUB_RUN_ID:-manual}-${attempt}"
        catalogue_version=$(curl -fsSL --retry 2 --retry-delay 1 \
            -H 'Cache-Control: no-cache' "${catalogue}?release=${cache_key}" 2>/dev/null \
            | jq -r '.[] | select(.id == "defragger") | (.version // empty)' \
            | head -n1 || true)

        apt_match=false
        if curl -fsSL --retry 2 --retry-delay 1 \
            -H 'Cache-Control: no-cache' "${packages}?release=${cache_key}" 2>/dev/null \
            | gzip -dc 2>/dev/null \
            | awk -v package="$RELEASE_PACKAGE" -v version="$RELEASE_VERSION" '
                BEGIN { RS=""; FS="\n"; found=0 }
                {
                    p=""; v=""
                    for (i=1; i<=NF; ++i) {
                        if ($i ~ /^Package: /) { p=substr($i,10) }
                        if ($i ~ /^Version: /) { v=substr($i,10) }
                    }
                    if (p == package && v == version) { found=1 }
                }
                END { exit(found ? 0 : 1) }
            '; then
            apt_match=true
        fi

        if [[ "$catalogue_version" == "$RELEASE_VERSION" && "$apt_match" == true ]]; then
            printf 'Defragmenter %s is live in the public APT repository.\n' "$RELEASE_VERSION"
            return 0
        fi

        printf 'APT propagation pending (%d/20): catalogue=%s package=%s\n' \
            "$attempt" "${catalogue_version:-missing}" "$apt_match"
        sleep 15
    done

    fail "APT propagation has not completed yet; GitHub release remains valid"
}

usage() {
    cat <<'USAGE'
Usage: ci/release.sh COMMAND [ARGS]

Commands:
  verify-source EXPECTED_SHA       Verify audited source/governance and release identity.
  verify-main-protection           Verify permanent main history protections.
  publish EXPECTED_SHA DIRECTORY   Publish/resume the exact qualified GitHub release.
  wait-apt DIRECTORY               Observe public APT propagation; safe to retry.
USAGE
}

case "${1:-}" in
    verify-source)
        shift
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        verify_source "$1"
        ;;
    verify-main-protection)
        shift
        [[ $# -eq 0 ]] || { usage >&2; exit 2; }
        verify_main_protection
        ;;
    publish)
        shift
        [[ $# -eq 2 ]] || { usage >&2; exit 2; }
        publish_release "$1" "$2"
        ;;
    wait-apt)
        shift
        [[ $# -eq 1 ]] || { usage >&2; exit 2; }
        wait_for_apt "$1"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
