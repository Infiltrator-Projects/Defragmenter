#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Create the self-contained local hardware-optimised compiler/installer from
# committed source only. Untracked runner state must never enter a release.
set -eu

fail() {
    printf 'local installer: %s\n' "$*" >&2
    exit 1
}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
command -v git >/dev/null 2>&1 || fail 'git is required to construct the source payload'
REPO_ROOT=$(git -C "$ROOT" rev-parse --show-toplevel 2>/dev/null) || \
    fail 'build-local-run.sh must be run from a Git checkout'
[ "$ROOT" = "$REPO_ROOT/defragger" ] || \
    fail "unexpected project root: $ROOT"

# The payload is materialised from HEAD, never from the ambient working tree.
# Refuse tracked edits rather than silently packaging different bytes from the
# ones the caller is looking at; arbitrary untracked files are deliberately
# irrelevant because git archive cannot see them.
git -C "$REPO_ROOT" diff --quiet -- defragger || \
    fail 'tracked Defragmenter files differ from HEAD'
git -C "$REPO_ROOT" diff --cached --quiet -- defragger || \
    fail 'staged Defragmenter files differ from HEAD'

VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
NATIVE_PACKAGE_VERSION="${VERSION}+native1"
OUTPUT=${1:-"$ROOT/Defragmenter-${VERSION}-local-folder.run"}
TEMPLATE="$ROOT/packaging/local-run-header.sh.in"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/defragmenter-run-build.XXXXXX")
COMMON_DIR="$ROOT/shared/infiltratr-common"
COMMON_COMMIT=$(git -C "$REPO_ROOT" rev-parse \
    HEAD:defragger/shared/infiltratr-common 2>/dev/null) || \
    fail 'HEAD does not contain the Libraries gitlink'
[ "${#COMMON_COMMIT}" -eq 40 ] || fail 'Libraries gitlink is not a commit object'
case "$COMMON_COMMIT" in
    *[!0-9a-f]*) fail 'Libraries gitlink is not a commit object' ;;
esac
cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT HUP INT TERM

if [ ! -f "$COMMON_DIR/VERSION" ]; then
    git -C "$REPO_ROOT" submodule update --init --depth 1 -- \
        defragger/shared/infiltratr-common || \
        fail "unable to initialise Libraries at ${COMMON_COMMIT}"
fi
[ -e "$COMMON_DIR/.git" ] || \
    fail 'Libraries must be a checked-out Git submodule'
COMMON_VERSION=$(tr -d '\r\n' <"$COMMON_DIR/VERSION")
[ -n "$COMMON_VERSION" ] || fail 'Libraries VERSION is empty'
ACTUAL_COMMON_COMMIT=$(git -C "$COMMON_DIR" rev-parse HEAD)
[ "$ACTUAL_COMMON_COMMIT" = "$COMMON_COMMIT" ] || \
    fail "Libraries gitlink mismatch: expected ${COMMON_COMMIT}, found ${ACTUAL_COMMON_COMMIT}"
git -C "$COMMON_DIR" diff --quiet || fail 'Libraries has tracked working-tree changes'
git -C "$COMMON_DIR" diff --cached --quiet || fail 'Libraries has staged changes'

for command_name in tar gzip sha256sum sed find; do
    command -v "$command_name" >/dev/null 2>&1 || \
        fail "required command is missing: ${command_name}"
done
[ -f "$TEMPLATE" ] || fail "installer template is missing: ${TEMPLATE}"

PAYLOAD_BASENAME="defragmenter-${VERSION}"
STAGE="$WORK/stage"
TAR_PATH="$WORK/source.tar"
FONT_ARCHIVE="$WORK/mb-corpo-fonts.tar.xz"
mkdir -p "$STAGE"

# Git, not a hand-maintained exclusion list, is the source inventory authority.
# The superproject archive intentionally contains only the Common gitlink; the
# exact pinned Common commit is archived separately into that location. CMake
# remains the executable authority for the supported Common API/version; this
# packager only preserves the dependency identity already recorded by HEAD.
git -C "$REPO_ROOT" archive --format=tar HEAD defragger | tar -xf - -C "$STAGE"
mv "$STAGE/defragger" "$STAGE/$PAYLOAD_BASENAME"
COMMON_STAGE="$STAGE/$PAYLOAD_BASENAME/shared/infiltratr-common"
rm -rf "$COMMON_STAGE"
mkdir -p "$COMMON_STAGE"
git -C "$COMMON_DIR" archive --format=tar "$COMMON_COMMIT" | tar -xf - -C "$COMMON_STAGE"

# Python remains independent qualification/test tooling only. The local source
# installer builds production C/C++ with BUILD_TESTING=OFF, so test drivers are
# not payload inputs and are removed as an explicit product-output policy.
find "$STAGE/$PAYLOAD_BASENAME" -type f \( -name '*.py' -o -name '*.pyc' \) -delete
find "$STAGE/$PAYLOAD_BASENAME" -type d -name '__pycache__' -prune -exec rm -rf '{}' +

"$ROOT/packaging/vendor-mb-fonts.sh" "$FONT_ARCHIVE" >/dev/null
FONT_STAGE="$STAGE/$PAYLOAD_BASENAME/assets/fonts"
mkdir -p "$FONT_STAGE"
rm -f "$FONT_STAGE/mb-corpo-fonts.tar.xz"
cp "$FONT_ARCHIVE" "$FONT_STAGE/mb-corpo-fonts.tar.xz"

# Normalised metadata makes equal committed inputs produce equal source
# payloads; neither local uid/gid nor wall-clock time participates.
tar --sort=name --mtime='@1704067200' --owner=0 --group=0 --numeric-owner \
    -C "$STAGE" -cf "$TAR_PATH" "$PAYLOAD_BASENAME"
gzip -n -9 "$TAR_PATH"
PAYLOAD="$TAR_PATH.gz"

PAYLOAD_SHA256=$(sha256sum "$PAYLOAD" | awk '{print $1}')
umask 022
sed \
    -e "s/@APP_VERSION@/$VERSION/g" \
    -e "s/@NATIVE_PACKAGE_VERSION@/$NATIVE_PACKAGE_VERSION/g" \
    -e "s/@PAYLOAD_SHA256@/$PAYLOAD_SHA256/g" \
    "$TEMPLATE" >"$OUTPUT"

if [ -n "$(tail -c 1 "$OUTPUT" 2>/dev/null || true)" ]; then
    printf '\n' >>"$OUTPUT"
fi
[ "$(tail -n 1 "$OUTPUT")" = '__DEFRAGMENTER_PAYLOAD_BELOW__' ] || \
    fail 'generated installer payload marker is missing or malformed'

dd if="$PAYLOAD" of="$OUTPUT" oflag=append conv=notrunc status=none
chmod 0755 "$OUTPUT"
printf '%s\n' "$OUTPUT"
