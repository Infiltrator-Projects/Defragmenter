#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build a Debian-managed generic or locally optimised package.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(tr -d '\n\r' <"$ROOT/VERSION")
PACKAGE_VERSION=${DEB_PACKAGE_VERSION:-$VERSION}
ARCH=${DEB_HOST_ARCH:-$(dpkg --print-architecture)}
BUILD_FLAVOR=${LD_BUILD_FLAVOR:-generic}
BUILD_TESTING=${LD_BUILD_TESTING:-ON}
BUILD=${BUILD_DIR:-"$ROOT/build-deb-$BUILD_FLAVOR"}
OUTPUT=${OUTPUT_PATH:-"$ROOT/Defragmenter-${PACKAGE_VERSION}-${ARCH}.deb"}
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/linux-defragger-deb.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT HUP INT TERM

if [ "$ARCH" != amd64 ]; then
    printf '%s\n' "Defragmenter $VERSION release packages support amd64 only." >&2
    exit 1
fi

case "$BUILD_FLAVOR" in
    generic) FLAVOR_DESCRIPTION='Generic x86-64 build for broad amd64 compatibility.' ;;
    native) FLAVOR_DESCRIPTION='Locally compiled build optimised for this machine CPU.' ;;
    *) printf 'Unknown LD_BUILD_FLAVOR: %s\n' "$BUILD_FLAVOR" >&2; exit 1 ;;
esac
case "$BUILD_TESTING" in ON|OFF) ;; *) printf 'LD_BUILD_TESTING must be ON or OFF, not %s\n' "$BUILD_TESTING" >&2; exit 1 ;; esac

set -- -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DLD_ENABLE_WERROR=ON \
    -DBUILD_TESTING="$BUILD_TESTING" -DCMAKE_INSTALL_PREFIX=/usr
if [ "$BUILD_FLAVOR" = generic ]; then
    set -- "$@" -DLD_GENERIC_AMD64=ON -DLD_NATIVE_OPTIMIZATION=OFF
else
    set -- "$@" -DLD_GENERIC_AMD64=OFF -DLD_NATIVE_OPTIMIZATION=ON
fi
cmake "$@"
cmake --build "$BUILD" -j"${BUILD_JOBS:-2}"
DESTDIR="$STAGE/root" cmake --install "$BUILD"

SOURCE_ICON="$ROOT/packaging/io.github.linuxdefragger.png"
for INSTALLED_ICON in \
    "$STAGE/root/usr/lib/linux-defragger/defragmenter-icon.png" \
    "$STAGE/root/usr/share/icons/hicolor/96x96/apps/io.github.linuxdefragger.png" \
    "$STAGE/root/usr/share/app-install/icons/infiltrator-defragmenter.png"
do
    [ -f "$INSTALLED_ICON" ] || {
        printf 'Required Defragmenter icon missing from package stage: %s\n' "$INSTALLED_ICON" >&2
        exit 1
    }
    cmp -s "$SOURCE_ICON" "$INSTALLED_ICON" || {
        printf 'Packaged Defragmenter icon differs from approved artwork: %s\n' "$INSTALLED_ICON" >&2
        exit 1
    }
done

FONT_ARCHIVE="$STAGE/mb-corpo-fonts.tar.xz"
if [ -f "$ROOT/assets/fonts/mb-corpo-fonts.tar.xz" ]; then
    cp "$ROOT/assets/fonts/mb-corpo-fonts.tar.xz" "$FONT_ARCHIVE"
fi
"$ROOT/packaging/vendor-mb-fonts.sh" "$FONT_ARCHIVE" >/dev/null
FONT_WORK="$STAGE/fonts"
mkdir -p "$FONT_WORK" "$STAGE/root/usr/share/fonts/truetype/linux-defragger"
tar -xJf "$FONT_ARCHIVE" -C "$FONT_WORK"
install -m 0644 "$FONT_WORK/mb_corpo_a_cond_regular.ttf" "$STAGE/root/usr/share/fonts/truetype/linux-defragger/"
install -m 0644 "$FONT_WORK/mb_corpo_s_bold.ttf" "$STAGE/root/usr/share/fonts/truetype/linux-defragger/"
install -m 0644 "$FONT_WORK/mb_corpo_s_regular.ttf" "$STAGE/root/usr/share/fonts/truetype/linux-defragger/"

mkdir -p "$STAGE/root/DEBIAN"
INSTALLED_SIZE=$(du -sk "$STAGE/root/usr" | awk '{print $1}')
{
    printf 'Package: infiltrator-defragmenter\n'
    printf 'Version: %s\n' "$PACKAGE_VERSION"
    printf 'Section: utils\n'
    printf 'Priority: optional\n'
    printf 'Architecture: %s\n' "$ARCH"
    printf 'Provides: linux-defragger\n'
    printf 'Breaks: linux-defragger (<< 1.8.0-172)\n'
    printf 'Replaces: linux-defragger (<< 1.8.0-172)\n'
    printf 'Maintainer: Shannon Smith\n'
    printf 'X-Linux-Defragger-Build: %s\n' "$BUILD_FLAVOR"
    printf 'Depends: python3, python3-gi, python3-cairo, gir1.2-gtk-3.0, libgtk-3-0t64, fontconfig, policykit-1, ca-certificates, desktop-file-utils, udisks2, util-linux, makefs, libext2fs2, libsqlite3-0, libssl3t64, libstdc++6\n'
    printf 'Installed-Size: %s\n' "$INSTALLED_SIZE"
    printf 'Description: Safe direct filesystem analysis and canonical layout rewriting\n'
    printf ' Defragmenter analyses filesystem allocation and safely rewrites\n'
    printf ' supported unmounted FAT, exFAT, NTFS, EXT2/3/4, XFS, Amiga OFS/FFS/SFS/PFS3,\n'
    printf ' classic HFS, HFS+/HFSX, Btrfs, APFS and Minix filesystems. UFS and ZFS\n'
    printf ' remain analysis-only. The package also includes the separate all-C GTK\n'
    printf ' Defragmenter Test Media program for building sacrificial field-test disks.\n'
    printf ' The supplied MB Corpo typography is installed for the Defragmenter interfaces.\n'
    printf ' %s\n' "$FLAVOR_DESCRIPTION"
} >"$STAGE/root/DEBIAN/control"
cat >"$STAGE/root/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
command -v fc-cache >/dev/null 2>&1 && fc-cache -f >/dev/null 2>&1 || true
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true
exit 0
EOF
cat >"$STAGE/root/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
command -v fc-cache >/dev/null 2>&1 && fc-cache -f >/dev/null 2>&1 || true
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true
exit 0
EOF
chmod 0755 "$STAGE/root/DEBIAN/postinst" "$STAGE/root/DEBIAN/postrm"
(
    cd "$STAGE/root"
    find usr -type f -print0 | sort -z | xargs -0 md5sum >DEBIAN/md5sums
)
chmod 0644 "$STAGE/root/DEBIAN/control" "$STAGE/root/DEBIAN/md5sums"
dpkg-deb --root-owner-group --build "$STAGE/root" "$OUTPUT"
printf '%s\n' "$OUTPUT"
