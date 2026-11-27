#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
test_program=$1
first=$2
second=$3
manual=$4
temp=$(mktemp -d)
trap 'rm -rf -- "$temp"' EXIT
chmod 700 "$temp"
export IFS_TEST_MODULE_PARENT="$temp"
"$test_program" 0
mkdir -m 700 "$temp/Filesystems"
"$test_program" 0
install -m 0600 "$first" "$temp/Filesystems/UNRELATED_FILENAME.so"
one=$("$test_program" 1)
install -m 0600 "$second" "$temp/Filesystems/ANOTHER.so"
two=$("$test_program" 2)
test "$one" != "$two"
mv "$temp/Filesystems/UNRELATED_FILENAME.so" "$temp/Filesystems/Z.so"
test "$two" = "$("$test_program" 2)"
install -m 0600 "$first" "$temp/Filesystems/DUPLICATE.so"
"$test_program" 0
rm "$temp/Filesystems/DUPLICATE.so" "$temp/Filesystems/ANOTHER.so"
install -m 0600 "$manual" "$temp/Filesystems/READ_ONLY.so"
"$test_program" 2 manual
rm "$temp/Filesystems/READ_ONLY.so" "$temp/Filesystems/Z.so"
"$test_program" 0
echo 'TEST-MEDIA-DYNAMIC-COMMON-V2-CATALOGUE-PASS'
