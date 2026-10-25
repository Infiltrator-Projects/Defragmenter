#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

if [ "${LD_INSTALLER_TEST_COMMAND:-0}" = 1 ]; then
    command_name=$(basename -- "$0")
    case "$command_name" in
        dpkg-query)
            case "$*" in
                *'${Version}'*defragmenter*)
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
        cp "$LD_INSTALLER_TEST_SOURCE_ROOT/README.md" "$DESTDIR/System/Resources/Documentation/Defragmenter/README.md"
        cp "$LD_INSTALLER_TEST_SOURCE_ROOT/LICENSE" "$DESTDIR/System/Resources/Documentation/Defragmenter/COPYING.GPL-3.0"
        cp "$LD_INSTALLER_TEST_SOURCE_ROOT/gui/ui/art/hero-landscape.jpg" "$DESTDIR/System/Resources/Themes/Defragmenter/art/hero-landscape.jpg"
        cp "$LD_INSTALLER_TEST_SOURCE_ROOT/packaging/io.github.defragmenter.png" \
                    "$DESTDIR/System/Resources/Icons/defragmenter.png"
                cp "$LD_INSTALLER_TEST_SOURCE_ROOT/packaging/io.github.defragmenter.png" \
                    "$DESTDIR/usr/share/icons/hicolor/96x96/apps/io.github.defragmenter.png"
                cp "$LD_INSTALLER_TEST_SOURCE_ROOT/packaging/io.github.defragmenter.png" \
                    "$DESTDIR/usr/share/app-install/icons/defragmenter.png"
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
WORK=$(mktemp -d "${TMPDIR:-/tmp}/defragmenter-installer-test.XXXXXX")
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
