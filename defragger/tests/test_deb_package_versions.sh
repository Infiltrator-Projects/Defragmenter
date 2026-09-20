#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

# This script doubles as a minimal CMake shim so the package metadata path can
# be tested without compiling the application a second time.
if [ "${LD_TEST_FAKE_CMAKE:-0}" = 1 ]; then
    case " $* " in
        *" -P "*)
            : "${LD_TEST_REAL_CMAKE:?real cmake is required for script-mode package helpers}"
            exec "$LD_TEST_REAL_CMAKE" "$@"
            ;;
    esac
    if [ "${1:-}" = --install ]; then
        : "${DESTDIR:?DESTDIR is required for the fake install}"
        : "${LD_TEST_SOURCE_ROOT:?LD_TEST_SOURCE_ROOT is required for the fake install}"
        mkdir -p \
            "$DESTDIR/usr/lib/linux-defragger/filesystems/fat" \
            "$DESTDIR/usr/share/icons/hicolor/96x96/apps" \
            "$DESTDIR/usr/share/app-install/icons"
        : >"$DESTDIR/usr/lib/linux-defragger/filesystems/fat/linux-defragger-fat-worker"
        chmod 0755 "$DESTDIR/usr/lib/linux-defragger/filesystems/fat/linux-defragger-fat-worker"
        cp "$LD_TEST_SOURCE_ROOT/packaging/io.github.linuxdefragger.png" \
            "$DESTDIR/usr/lib/linux-defragger/defragmenter-icon.png"
        cp "$LD_TEST_SOURCE_ROOT/packaging/io.github.linuxdefragger.png" \
            "$DESTDIR/usr/share/icons/hicolor/96x96/apps/io.github.linuxdefragger.png"
        cp "$LD_TEST_SOURCE_ROOT/packaging/io.github.linuxdefragger.png" \
            "$DESTDIR/usr/share/app-install/icons/infiltrator-defragmenter.png"
    fi
    exit 0
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
NATIVE_VERSION="${VERSION}+native1"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-version-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

mkdir -p "$WORK/bin"
ln -s "$ROOT/tests/test_deb_package_versions.sh" "$WORK/bin/cmake"

PACKAGE="$WORK/infiltrator-defragmenter_${NATIVE_VERSION}_amd64.deb"
LD_TEST_REAL_CMAKE=$(command -v cmake)
PATH="$WORK/bin:$PATH" \
LD_TEST_FAKE_CMAKE=1 \
LD_TEST_REAL_CMAKE="$LD_TEST_REAL_CMAKE" \
LD_TEST_SOURCE_ROOT="$ROOT" \
LD_BUILD_FLAVOR=native \
LD_BUILD_TESTING=OFF \
DEB_PACKAGE_VERSION="$NATIVE_VERSION" \
BUILD_DIR="$WORK/build" \
OUTPUT_PATH="$PACKAGE" \
    "$ROOT/packaging/build-deb.sh" >/dev/null

[ "$(dpkg-deb -f "$PACKAGE" Package)" = infiltrator-defragmenter ]
[ "$(dpkg-deb -f "$PACKAGE" Version)" = "$NATIVE_VERSION" ]
[ "$(dpkg-deb -f "$PACKAGE" Architecture)" = amd64 ]
[ "$(dpkg-deb -f "$PACKAGE" X-Linux-Defragger-Build)" = native ]

printf '%s\n' 'Debian package-version metadata test passed.'
