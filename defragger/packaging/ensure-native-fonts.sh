#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Reviewed copy of Libraries 1.19.58 InfiltratrEnsureNativeFonts.sh.
# Establish verified shared Mercedes-Benz font resources without installing
# duplicate application-owned font payloads. This cache survives uninstalls.
set -eu
umask 022

SOURCE_COMMIT="66c17fe9ea3ea9678b4028ffea1862a37d488f79"
SOURCE_URL="https://raw.githubusercontent.com/Infiltrator-Projects/InfiltratorOS/$SOURCE_COMMIT/assets/fonts"
SOURCE_DIR="${INFILTRATR_FONT_SOURCE_DIR:-/usr/lib/infiltratoros/font-seed}"
ROOT="${INFILTRATR_NATIVE_ROOT:-}"
case "$ROOT" in ""|/*) ;; *) echo "INFILTRATR_NATIVE_ROOT must be absolute" >&2; exit 2 ;; esac
case "$SOURCE_DIR" in /*) ;; *) echo "INFILTRATR_FONT_SOURCE_DIR must be absolute" >&2; exit 2 ;; esac
native_path() { printf '%s%s\n' "$ROOT" "$1"; }
FONT_DIR="$(native_path /System/Resources/Fonts)"
FONTCONFIG_DIR="$(native_path /etc/fonts/conf.d)"
FONTCONFIG_FILE="$FONTCONFIG_DIR/90-infiltrator-native-fonts.conf"
for path in "$(native_path /System)" "$(native_path /System/Resources)" "$FONT_DIR"; do
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

pending=""
trap 'if [ -n "$pending" ]; then rm -f -- "$pending"; fi' 0

ensure_font() {
    expected=$1
    filename=$2
    destination="$FONT_DIR/$filename"
    font_matches "$expected" "$destination" && return 0

    pending="$FONT_DIR/.$filename.new.$$"
    rm -f -- "$pending"
    local_seed="$SOURCE_DIR/$filename"
    if font_matches "$expected" "$local_seed"; then
        ln -- "$local_seed" "$pending" 2>/dev/null ||
            install -m 0644 -- "$local_seed" "$pending"
    elif command -v curl >/dev/null 2>&1; then
        curl --fail --location --silent --show-error --retry 3 \
            "$SOURCE_URL/$filename" --output "$pending"
        chmod 0644 "$pending"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$pending" "$SOURCE_URL/$filename"
        chmod 0644 "$pending"
    else
        echo "Missing verified font and no download tool available: $filename" >&2
        exit 4
    fi
    if ! font_matches "$expected" "$pending"; then
        echo "Incorrect SHA-256 for shared MB Corpo font: $filename" >&2
        exit 4
    fi
    mv -f -- "$pending" "$destination"
    pending=""
    font_matches "$expected" "$destination"
}

ensure_font "db395c845eeec3a2d24dc7879e0c69029368fb4c7390a531369e0c8157ce3e85" "mb_corpo_a_title_cond_regular.ttf"
ensure_font "b046af5f32cd166643630f4a70876ec2a13403c01f7cd64958d1d43616dc7e9d" "mb_corpo_s_title_bold.ttf"
ensure_font "d452c95fd1017bfee6ad5c7f9d5378a4b1b77a3e03911f85ed3ce0893f69f9c1" "mb_corpo_s_title_regular.ttf"

config_tmp="$FONTCONFIG_FILE.new.$$"
pending="$config_tmp"
cat > "$config_tmp" <<'EOF'
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>/System/Resources/Fonts</dir>
</fontconfig>
EOF
chmod 0644 "$config_tmp"
if [ -f "$FONTCONFIG_FILE" ] && cmp -s "$config_tmp" "$FONTCONFIG_FILE"; then
    rm -f -- "$config_tmp"
else
    mv -f -- "$config_tmp" "$FONTCONFIG_FILE"
fi
pending=""
if [ -z "$ROOT" ] && command -v fc-cache >/dev/null 2>&1; then
    fc-cache -f "$FONT_DIR" >/dev/null 2>&1 || true
fi
