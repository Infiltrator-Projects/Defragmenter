#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\r\n' <"$ROOT/VERSION")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-artifact-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
RUN="$WORK/Defragmenter-${VERSION}-local-folder.run"
if [ -e "$ROOT/packaging/build-source-zip.sh" ]; then
    printf '%s\n' 'Custom source ZIP builder must remain removed.' >&2
    exit 1
fi

sh -n "$ROOT/packaging/build-deb.sh"
sh -n "$ROOT/packaging/build-local-run.sh"
grep -Fq 'OUTPUT=${OUTPUT_PATH:-"$ROOT/Defragmenter-${PACKAGE_VERSION}-${ARCH}.deb"}' "$ROOT/packaging/build-deb.sh"
grep -Fq 'libstdc++6' "$ROOT/packaging/build-deb.sh"
grep -Fq 'StartupWMClass=io.github.linuxdefragger' \
    "$ROOT/packaging/io.github.linuxdefragger.desktop"
grep -Fq 'OUTPUT=${1:-"$ROOT/Defragmenter-${VERSION}-local-folder.run"}' "$ROOT/packaging/build-local-run.sh"
"$ROOT/packaging/build-local-run.sh" "$RUN" >/dev/null

MARKER_LINE=$(grep -an '^__LINUX_DEFRAGGER_PAYLOAD_BELOW__$' "$RUN" | \
    head -1 | cut -d: -f1)
[ -n "$MARKER_LINE" ]
PAYLOAD_LINE=$((MARKER_LINE + 1))
HEADER="$WORK/header.sh"
head -n "$MARKER_LINE" "$RUN" >"$HEADER"
sh -n "$HEADER"

EXPECTED=$(sed -n "s/^PAYLOAD_SHA256='\([^']*\)'$/\1/p" "$HEADER")
ACTUAL=$(tail -n +"$PAYLOAD_LINE" "$RUN" | sha256sum | awk '{print $1}')
[ "$ACTUAL" = "$EXPECTED" ]

tail -n +"$PAYLOAD_LINE" "$RUN" | tar -tzf - >"$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/CMakeLists.txt" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/packaging/build-deb.sh" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/packaging/build-local-run.sh" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/LICENSE" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/shared/infiltratr-common/VERSION" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/shared/infiltratr-common/src/core.c" "$WORK/files.txt"
grep -qx "linux-defragger-${VERSION}/shared/infiltratr-common/src/posix.c" "$WORK/files.txt"
if grep -Eq '/(build[^/]*)/|__pycache__|\.pyc$|\.deb$|\.run$|\.zip$' "$WORK/files.txt"; then
    printf '%s\n' 'Generated local installer contains a forbidden generated file.' >&2
    exit 1
fi

dpkg --compare-versions "${VERSION}+native1" gt "$VERSION"
NEXT_REVISION=${VERSION%-*}-$(( ${VERSION##*-} + 1 ))
dpkg --compare-versions "$NEXT_REVISION" gt "${VERSION}+native1"

CMAKE_SOURCE="$WORK/cmake-source.txt"
cat "$ROOT/CMakeLists.txt" "$ROOT"/cmake/*.cmake >"$CMAKE_SOURCE"
grep -q -- '-march=x86-64' "$CMAKE_SOURCE"
grep -q -- '-mtune=generic' "$CMAKE_SOURCE"
grep -q -- '-march=native' "$CMAKE_SOURCE"
grep -q -- '-mtune=native' "$CMAKE_SOURCE"
grep -Fq "printf 'Version: %s\\n' \"\$PACKAGE_VERSION\"" \
    "$ROOT/packaging/build-deb.sh"
if grep -Fq "printf 'Version: %s\\n' \"\$VERSION\"" \
    "$ROOT/packaging/build-deb.sh"; then
    printf '%s\n' 'Debian control metadata ignores DEB_PACKAGE_VERSION.' >&2
    exit 1
fi
"$ROOT/tests/test_deb_package_versions.sh"

if command -v makefs >/dev/null 2>&1; then
    UFS_BUILD="$WORK/ufs-build"
    cmake -S "$ROOT" -B "$UFS_BUILD" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLD_ENABLE_WERROR=ON \
        -DBUILD_TESTING=OFF >/dev/null
    cmake --build "$UFS_BUILD" --target linux-defragger-ufs-worker -j2 >/dev/null
    /bin/sh "$ROOT/tests/test_ufs_makefs.sh" "$UFS_BUILD/linux-defragger-ufs-worker"
else
    printf '%s\n' 'makefs unavailable; UFS2 release integration test skipped.'
fi

"$ROOT/tests/test_local_installer_end_to_end.sh" "$RUN"

printf '%s\n' 'Installable release packaging tests passed.'
