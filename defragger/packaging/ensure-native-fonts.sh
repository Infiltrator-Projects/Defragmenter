#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Canonical copy of Libraries 1.19.51 InfiltratrEnsureNativeFonts.sh.
# Establish the canonical Linux Infiltrator font resource cache.
set -eu

umask 022
SOURCE_COMMIT="aa161e7342112beab8feb7669f072c870f742765"
ARCHIVE_URL="https://raw.githubusercontent.com/Infiltrator-Projects/MBLink/${SOURCE_COMMIT}/assets/fonts/mb-corpo-fonts.tar.xz"
ARCHIVE_SHA256="bdb6063f838a7fab22b4d6b412170640c69511df53aa3dfa9a4ea8431c9d8274"
BRAND_FILE="mb_corpo_a_cond_regular.ttf"
UI_BOLD_FILE="mb_corpo_s_bold.ttf"
UI_REGULAR_FILE="mb_corpo_s_regular.ttf"
BRAND_SHA256="c8bcd7e1a7d71169b38491d9b7c1ffe7ba7b46e888f0c1219931343a47bc0e05"
UI_BOLD_SHA256="d37ea986e2344d83390f94f170e6272b56efd00bfec808afe8314c4ca45d43b4"
UI_REGULAR_SHA256="94ede6629443c03d4362dcef425fb3ff520be5d654370021a34e81286804465c"

ROOT="${INFILTRATR_NATIVE_ROOT:-}"
case "$ROOT" in ""|/*) ;; *) echo "INFILTRATR_NATIVE_ROOT must be empty or absolute" >&2; exit 2 ;; esac
native_path() { printf '%s%s\n' "$ROOT" "$1"; }
SYSTEM_DIR="$(native_path /System)"
RESOURCE_DIR="$(native_path /System/Resources)"
FONT_DIR="$(native_path /System/Resources/Fonts)"
FONTCONFIG_DIR="$(native_path /etc/fonts/conf.d)"
FONTCONFIG_FILE="$FONTCONFIG_DIR/90-infiltrator-native-fonts.conf"

for path in "$SYSTEM_DIR" "$RESOURCE_DIR" "$FONT_DIR"; do
    if [ -L "$path" ]; then
        echo "Refusing native font resource path through symlink: $path" >&2
        exit 3
    fi
done
mkdir -p "$FONT_DIR" "$FONTCONFIG_DIR"

font_matches() {
    expected=$1
    path=$2
    [ -f "$path" ] && [ ! -L "$path" ] &&
        printf '%s  %s\n' "$expected" "$path" | sha256sum -c - >/dev/null 2>&1
}

need_fonts=false
font_matches "$BRAND_SHA256" "$FONT_DIR/$BRAND_FILE" || need_fonts=true
font_matches "$UI_BOLD_SHA256" "$FONT_DIR/$UI_BOLD_FILE" || need_fonts=true
font_matches "$UI_REGULAR_SHA256" "$FONT_DIR/$UI_REGULAR_FILE" || need_fonts=true

if [ "$need_fonts" = true ]; then
    work=$(mktemp -d "${TMPDIR:-/tmp}/infiltratr-fonts.XXXXXX")
    trap 'rm -rf "$work"' EXIT HUP INT TERM
    archive="$work/mb-corpo-fonts.tar.xz"
    if [ -n "${INFILTRATR_MB_CORPO_ARCHIVE_FILE:-}" ]; then
        cp -- "$INFILTRATR_MB_CORPO_ARCHIVE_FILE" "$archive"
    elif command -v curl >/dev/null 2>&1; then
        curl --fail --location --silent --show-error "$ARCHIVE_URL" --output "$archive"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$archive" "$ARCHIVE_URL"
    else
        echo "MB Corpo resources are missing and neither curl nor wget is available" >&2
        exit 4
    fi
    printf '%s  %s\n' "$ARCHIVE_SHA256" "$archive" | sha256sum -c - >/dev/null
    extract="$work/extract"
    mkdir -p "$extract"
    tar -xJf "$archive" -C "$extract"
    printf '%s  %s\n' "$BRAND_SHA256" "$extract/$BRAND_FILE" | sha256sum -c - >/dev/null
    printf '%s  %s\n' "$UI_BOLD_SHA256" "$extract/$UI_BOLD_FILE" | sha256sum -c - >/dev/null
    printf '%s  %s\n' "$UI_REGULAR_SHA256" "$extract/$UI_REGULAR_FILE" | sha256sum -c - >/dev/null
    for file in "$BRAND_FILE" "$UI_BOLD_FILE" "$UI_REGULAR_FILE"; do
        staged="$FONT_DIR/.${file}.new.$$"
        install -m 0644 "$extract/$file" "$staged"
        mv -f -- "$staged" "$FONT_DIR/$file"
    done
    rm -rf "$work"
    trap - EXIT HUP INT TERM
fi

conf_tmp="$FONTCONFIG_FILE.new.$$"
cat > "$conf_tmp" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>/System/Resources/Fonts</dir>
</fontconfig>
EOF
chmod 0644 "$conf_tmp"
if [ -f "$FONTCONFIG_FILE" ] && cmp -s "$conf_tmp" "$FONTCONFIG_FILE"; then
    rm -f "$conf_tmp"
else
    mv -f -- "$conf_tmp" "$FONTCONFIG_FILE"
fi
font_matches "$BRAND_SHA256" "$FONT_DIR/$BRAND_FILE"
font_matches "$UI_BOLD_SHA256" "$FONT_DIR/$UI_BOLD_FILE"
font_matches "$UI_REGULAR_SHA256" "$FONT_DIR/$UI_REGULAR_FILE"
if [ -z "$ROOT" ] && command -v fc-cache >/dev/null 2>&1; then
    fc-cache -f "$FONT_DIR" >/dev/null 2>&1 || true
fi
