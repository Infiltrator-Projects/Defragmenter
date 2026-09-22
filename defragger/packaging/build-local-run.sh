#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Create the self-contained local hardware-optimised compiler/installer.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
NATIVE_PACKAGE_VERSION="${VERSION}+native1"
OUTPUT=${1:-"$ROOT/Defragmenter-${VERSION}-local-folder.run"}
TEMPLATE="$ROOT/packaging/local-run-header.sh.in"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-run-build.XXXXXX")
COMMON_DIR="$ROOT/shared/infiltratr-common"
COMMON_URL="https://github.com/Infiltrator-Projects/Infiltrator-Libraries.git"
COMMON_TAG="v1.19.22"
COMMON_VERSION="1.19.22"
COMMON_COMMIT="302c44eb7436803dee020667453a9a0681da8bbf"
COMMON_TEMP=0
cleanup() {
    rm -rf "$WORK"
    if [ "$COMMON_TEMP" -eq 1 ]; then
        rm -rf "$COMMON_DIR"
        rmdir "$ROOT/shared" 2>/dev/null || true
    fi
}
trap cleanup EXIT HUP INT TERM

if [ ! -f "$COMMON_DIR/VERSION" ]; then
    command -v git >/dev/null 2>&1 || {
        printf 'git is required to retrieve Infiltratr Common %s\n' "$COMMON_VERSION" >&2
        exit 1
    }
    if [ -e "$ROOT/.git" ]; then
        git -C "$ROOT" submodule update --init --depth 1 -- shared/infiltratr-common
    else
        mkdir -p "$ROOT/shared"
        git clone --quiet --depth 1 --branch "$COMMON_TAG" "$COMMON_URL" "$COMMON_DIR"
        COMMON_TEMP=1
    fi
fi
ACTUAL_COMMON_VERSION=$(tr -d '\r\n' <"$COMMON_DIR/VERSION")
[ "$ACTUAL_COMMON_VERSION" = "$COMMON_VERSION" ] || {
    printf 'Infiltratr Common %s is required; found %s\n' "$COMMON_VERSION" "$ACTUAL_COMMON_VERSION" >&2
    exit 1
}
if [ -e "$COMMON_DIR/.git" ]; then
    ACTUAL_COMMON_COMMIT=$(git -C "$COMMON_DIR" rev-parse HEAD)
    [ "$ACTUAL_COMMON_COMMIT" = "$COMMON_COMMIT" ] || {
        printf 'Infiltratr Common commit mismatch: %s\n' "$ACTUAL_COMMON_COMMIT" >&2
        exit 1
    }
fi

for command_name in tar gzip sha256sum sed python3; do
    command -v "$command_name" >/dev/null 2>&1 || {
        printf 'Required command is missing: %s\n' "$command_name" >&2
        exit 1
    }
done

[ -f "$TEMPLATE" ] || {
    printf 'Installer template is missing: %s\n' "$TEMPLATE" >&2
    exit 1
}

PARENT=$(dirname -- "$ROOT")
BASENAME=$(basename -- "$ROOT")
PAYLOAD_BASENAME="linux-defragger-${VERSION}"
TAR_PATH="$WORK/source.tar"
FONT_ARCHIVE="$WORK/mb-corpo-fonts.tar.xz"
"$ROOT/packaging/vendor-mb-fonts.sh" "$FONT_ARCHIVE" >/dev/null

tar --sort=name --mtime='@1704067200' --owner=0 --group=0 --numeric-owner \
    --exclude-vcs \
    --exclude='__pycache__' \
    --exclude='*.pyc' \
    --exclude="$BASENAME/build*" \
    --exclude="$BASENAME/native-verify" \
    --exclude="$BASENAME/release" \
    --exclude='*.deb' \
    --exclude='*.run' \
    --exclude='*.zip' \
    --exclude="$BASENAME/assets/fonts/mb-corpo-fonts.tar.xz" \
    --transform="s,^${BASENAME},${PAYLOAD_BASENAME}," \
    -C "$PARENT" -cf "$TAR_PATH" "$BASENAME"
FONT_STAGE="$WORK/font-stage/$PAYLOAD_BASENAME/assets/fonts"
mkdir -p "$FONT_STAGE"
cp "$FONT_ARCHIVE" "$FONT_STAGE/mb-corpo-fonts.tar.xz"
tar --mtime='@1704067200' --owner=0 --group=0 --numeric-owner \
    -C "$WORK/font-stage" -rf "$TAR_PATH" "$PAYLOAD_BASENAME/assets/fonts/mb-corpo-fonts.tar.xz"
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
[ "$(tail -n 1 "$OUTPUT")" = '__LINUX_DEFRAGGER_PAYLOAD_BELOW__' ] || {
    printf '%s\n' 'Generated installer payload marker is missing or malformed.' >&2
    exit 1
}

dd if="$PAYLOAD" of="$OUTPUT" oflag=append conv=notrunc status=none
chmod 0755 "$OUTPUT"
printf '%s\n' "$OUTPUT"
