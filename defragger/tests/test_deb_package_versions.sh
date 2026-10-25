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
            "$DESTDIR/System/Commands/Defragmenter/fat" \
            "$DESTDIR/System/Commands/Defragmenter/ext4" \
            "$DESTDIR/System/Resources/Icons" \
            "$DESTDIR/System/Resources/Documentation/Defragmenter" \
            "$DESTDIR/System/Resources/Themes/Defragmenter/art" \
            "$DESTDIR/Programs" \
            "$DESTDIR/usr/share/icons/hicolor/96x96/apps" \
            "$DESTDIR/usr/share/app-install/icons"
                for program in defragmenter defragmenter-test-media; do
            printf '#!/bin/sh\nexit 0\n' >"$DESTDIR/Programs/$program"
            chmod 0755 "$DESTDIR/Programs/$program"
        done
        : >"$DESTDIR/System/Commands/Defragmenter/fat/defragmenter-fat-worker"
        chmod 0755 "$DESTDIR/System/Commands/Defragmenter/fat/defragmenter-fat-worker"
        for program in defragmenter-privileged-helper defragmenter-operation-engine defragmenter-mapper; do
            printf '#!/bin/sh\nexit 0\n' >"$DESTDIR/System/Commands/Defragmenter/$program"
            chmod 0755 "$DESTDIR/System/Commands/Defragmenter/$program"
        done
        printf '#!/bin/sh\nexit 0\n' >"$DESTDIR/System/Commands/Defragmenter/ext4/ext-metadata-worker"
        chmod 0755 "$DESTDIR/System/Commands/Defragmenter/ext4/ext-metadata-worker"
        cp "$LD_TEST_SOURCE_ROOT/README.md" "$DESTDIR/System/Resources/Documentation/Defragmenter/README.md"
        cp "$LD_TEST_SOURCE_ROOT/LICENSE" "$DESTDIR/System/Resources/Documentation/Defragmenter/COPYING.GPL-3.0"
        cp "$LD_TEST_SOURCE_ROOT/gui/ui/art/hero-landscape.jpg" "$DESTDIR/System/Resources/Themes/Defragmenter/art/hero-landscape.jpg"
        cp "$LD_TEST_SOURCE_ROOT/packaging/io.github.defragmenter.png" \
            "$DESTDIR/System/Resources/Icons/defragmenter.png"
        cp "$LD_TEST_SOURCE_ROOT/packaging/io.github.defragmenter.png" \
            "$DESTDIR/usr/share/icons/hicolor/96x96/apps/io.github.defragmenter.png"
        cp "$LD_TEST_SOURCE_ROOT/packaging/io.github.defragmenter.png" \
            "$DESTDIR/usr/share/app-install/icons/defragmenter.png"
    fi
    exit 0
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
NATIVE_VERSION="${VERSION}+native1"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/defragmenter-version-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

mkdir -p "$WORK/bin"
ln -s "$ROOT/tests/test_deb_package_versions.sh" "$WORK/bin/cmake"

PACKAGE="$WORK/defragmenter_${NATIVE_VERSION}_amd64.deb"
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

[ "$(dpkg-deb -f "$PACKAGE" Package)" = defragmenter ]
[ "$(dpkg-deb -f "$PACKAGE" Version)" = "$NATIVE_VERSION" ]
[ "$(dpkg-deb -f "$PACKAGE" Architecture)" = amd64 ]
[ "$(dpkg-deb -f "$PACKAGE" X-Defragmenter-Build)" = native ]

dpkg-deb -x "$PACKAGE" "$WORK/extracted"
for program in defragmenter defragmenter-test-media; do
    [ -x "$WORK/extracted/Programs/$program" ]
    [ ! -e "$WORK/extracted/usr/bin/$program" ]
done
for resource in \
    System/Commands/Defragmenter/fat/defragmenter-fat-worker \
    System/Commands/Defragmenter/ext4/ext-metadata-worker \
    System/Resources/Icons/defragmenter.png \
    System/Resources/Documentation/Defragmenter/README.md
do
    [ -f "$WORK/extracted/$resource" ]
done
[ ! -e "$WORK/extracted/usr/lib/defragmenter" ]
[ ! -e "$WORK/extracted/usr/share/doc/defragmenter" ]
dpkg-deb -e "$PACKAGE" "$WORK/metadata"
for program in defragmenter defragmenter-desktop defragmenter-test-media; do
    grep -Fq "Programs/$program" "$WORK/metadata/md5sums"
done

printf '%s\n' 'Debian package-version metadata test passed.'
