#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

if [ "${LD_INSTALLER_TEST_COMMAND:-0}" = 1 ]; then
    command_name=$(basename -- "$0")
    case "$command_name" in
        dpkg-query)
            case "$*" in
                *'${Version}'*infiltrator-defragmenter*)
                    [ -f "${LD_INSTALLER_TEST_STATE:?}" ] || exit 1
                    cat "$LD_INSTALLER_TEST_STATE"
                    ;;
                *'${Status}'*)
                    printf '%s\n' 'install ok installed'
                    ;;
                *) exit 1 ;;
            esac
            ;;
        dpkg)
            case "${1:-}" in
                --print-architecture) printf '%s\n' amd64 ;;
                --compare-versions) exec /usr/bin/dpkg "$@" ;;
                --install)
                    /usr/bin/dpkg-deb -f "${2:?package path is required}" Version \
                        >"$LD_INSTALLER_TEST_STATE"
                    ;;
                *) exit 1 ;;
            esac
            ;;
        cmake)
            case " $* " in
                *" -P "*)
                    : "${LD_INSTALLER_TEST_REAL_CMAKE:?real cmake is required for script-mode package helpers}"
                    exec "$LD_INSTALLER_TEST_REAL_CMAKE" "$@"
                    ;;
            esac
            if [ "${1:-}" = --install ]; then
                : "${DESTDIR:?DESTDIR is required for the fake install}"
                : "${LD_INSTALLER_TEST_SOURCE_ROOT:?source root is required for the fake install}"
                mkdir -p \
                    "$DESTDIR/usr/lib/linux-defragger/filesystems/fat" \
                    "$DESTDIR/usr/share/icons/hicolor/96x96/apps" \
                    "$DESTDIR/usr/share/app-install/icons"
                : >"$DESTDIR/usr/lib/linux-defragger/filesystems/fat/linux-defragger-fat-worker"
                chmod 0755 "$DESTDIR/usr/lib/linux-defragger/filesystems/fat/linux-defragger-fat-worker"
                cp "$LD_INSTALLER_TEST_SOURCE_ROOT/packaging/io.github.linuxdefragger.png" \
                    "$DESTDIR/usr/lib/linux-defragger/defragmenter-icon.png"
                cp "$LD_INSTALLER_TEST_SOURCE_ROOT/packaging/io.github.linuxdefragger.png" \
                    "$DESTDIR/usr/share/icons/hicolor/96x96/apps/io.github.linuxdefragger.png"
                cp "$LD_INSTALLER_TEST_SOURCE_ROOT/packaging/io.github.linuxdefragger.png" \
                    "$DESTDIR/usr/share/app-install/icons/infiltrator-defragmenter.png"
            fi
            ;;
        sudo) shift 0; exec "$@" ;;
        gcc|make|makefs) exit 0 ;;
        apt-get)
            printf '%s\n' 'The isolated installer test unexpectedly invoked APT.' >&2
            exit 1
            ;;
        *) exit 1 ;;
    esac
    exit 0
fi

[ "$#" -eq 1 ] || {
    printf 'Usage: %s LOCAL-INSTALLER.run\n' "$0" >&2
    exit 2
}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
EXPECTED_VERSION="${VERSION}+native1"
RUN=$1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-installer-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

for command_name in dpkg-query dpkg cmake sudo gcc make makefs apt-get; do
    ln -s "$ROOT/tests/test_local_installer_end_to_end.sh" \
        "$WORK/$command_name"
done

REAL_CMAKE=$(command -v cmake)
LD_INSTALLER_TEST_COMMAND=1 \
LD_INSTALLER_TEST_STATE="$WORK/installed-version" \
LD_INSTALLER_TEST_REAL_CMAKE="$REAL_CMAKE" \
LD_INSTALLER_TEST_SOURCE_ROOT="$ROOT" \
PATH="$WORK:$PATH" \
    "$RUN" >"$WORK/output.log"

[ "$(cat "$WORK/installed-version")" = "$EXPECTED_VERSION" ]
grep -Fq "Defragmenter $EXPECTED_VERSION is installed." "$WORK/output.log"
grep -Fq 'The native package replaced the generic package' "$WORK/output.log"

printf '%s\n' 'Local compiler end-to-end package-version test passed.'
