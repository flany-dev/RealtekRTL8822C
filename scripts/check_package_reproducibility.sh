#!/bin/sh
set -eu

archive="build/package/RealtekRTL8822C-$(cat VERSION).zip"
root="RealtekRTL8822C-$(cat VERSION)"
checksum="build/package/RealtekRTL8822C-$(cat VERSION).sha256"
test -f "$archive"
test -f "$checksum"
expected_checksum_line="$(shasum -a 256 "$archive" | awk -v file="$(basename "$archive")" '{print $1 "  " file}')"
test "$(cat "$checksum")" = "$expected_checksum_line" || {
    echo "Release checksum must use the portable archive basename" >&2
    exit 1
}
first="$(shasum -a 256 "$archive" | awk '{print $1}')"
make package >/dev/null
second="$(shasum -a 256 "$archive" | awk '{print $1}')"
test "$first" = "$second" || {
    echo "Release package is not reproducible: $first != $second" >&2
    exit 1
}
if unzip -l "$archive" | grep -q '__MACOSX'; then
    echo "Release package contains AppleDouble metadata" >&2
    exit 1
fi
for entry in \
    "$root/README.md" \
    "$root/LICENSE" \
    "$root/CONTRIBUTING.md" \
    "$root/ROADMAP.md" \
    "$root/docs/RTL8822CCTL.md" \
    "$root/docs/HARDWARE_ACCEPTANCE.md" \
    "$root/firmware/README.md" \
    "$root/firmware/LICENCE.rtlwifi_firmware.txt"; do
    unzip -Z1 "$archive" | grep -Fxq "$entry" || {
        echo "Release package is missing public document: $entry" >&2
        exit 1
    }
done
if unzip -Z1 "$archive" | grep -Eq '(^|/)AGENTS\.md$|/docs/internal/'; then
    echo "Release package contains private project memory" >&2
    exit 1
fi
unzip -tq "$archive" >/dev/null
echo "Release package reproducibility check passed"
