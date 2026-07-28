#!/bin/sh
set -eu

archive="build/package/RealtekRTL8822C-$(cat VERSION).zip"
root="RealtekRTL8822C-$(cat VERSION)"
checksum="build/package/RealtekRTL8822C-$(cat VERSION).sha256"
release_kext_archive="build/package/RealtekRTL8822C-$(cat VERSION)-Release.zip"
debug_kext_archive="build/package/RealtekRTL8822C-$(cat VERSION)-Debug.zip"
menu_app_archive="build/package/RealtekRTL8822CMenu-$(cat VERSION).zip"
test -f "$archive"
test -f "$checksum"
test -f "$release_kext_archive"
test -f "$debug_kext_archive"
test -f "$menu_app_archive"
expected_checksum_line="$(shasum -a 256 "$archive" | awk -v file="$(basename "$archive")" '{print $1 "  " file}')"
test "$(cat "$checksum")" = "$expected_checksum_line" || {
    echo "Release checksum must use the portable archive basename" >&2
    exit 1
}
first="$(shasum -a 256 "$archive" | awk '{print $1}')"
first_release_kext="$(shasum -a 256 "$release_kext_archive" | awk '{print $1}')"
first_debug_kext="$(shasum -a 256 "$debug_kext_archive" | awk '{print $1}')"
first_menu_app="$(shasum -a 256 "$menu_app_archive" | awk '{print $1}')"
make package >/dev/null
second="$(shasum -a 256 "$archive" | awk '{print $1}')"
second_release_kext="$(shasum -a 256 "$release_kext_archive" | awk '{print $1}')"
second_debug_kext="$(shasum -a 256 "$debug_kext_archive" | awk '{print $1}')"
second_menu_app="$(shasum -a 256 "$menu_app_archive" | awk '{print $1}')"
test "$first" = "$second" || {
    echo "Release package is not reproducible: $first != $second" >&2
    exit 1
}
test "$first_release_kext" = "$second_release_kext" || {
    echo "Release kext archive is not reproducible" >&2
    exit 1
}
test "$first_debug_kext" = "$second_debug_kext" || {
    echo "Debug kext archive is not reproducible" >&2
    exit 1
}
test "$first_menu_app" = "$second_menu_app" || {
    echo "Menu app archive is not reproducible" >&2
    exit 1
}
if unzip -l "$archive" | grep -q '__MACOSX'; then
    echo "Release package contains AppleDouble metadata" >&2
    exit 1
fi
for entry in \
    "$root/README.md" \
    "$root/RELEASE_NOTES.md" \
    "$root/LICENSE" \
    "$root/CONTRIBUTING.md" \
    "$root/ROADMAP.md" \
    "$root/docs/RTL8822CCTL.md" \
    "$root/docs/MENU_APP.md" \
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
for kext_archive in "$release_kext_archive" "$debug_kext_archive"; do
    unzip -tq "$kext_archive" >/dev/null
    unzip -Z1 "$kext_archive" | grep -Fxq 'RealtekRTL8822C.kext/' || {
        echo "Kext archive has no top-level RealtekRTL8822C.kext: $kext_archive" >&2
        exit 1
    }
    if unzip -Z1 "$kext_archive" | grep -Evq '^RealtekRTL8822C\.kext(/|$)'; then
        echo "Kext archive contains an unexpected top-level entry: $kext_archive" >&2
        exit 1
    fi
done
unzip -tq "$menu_app_archive" >/dev/null
unzip -Z1 "$menu_app_archive" | grep -Fxq 'RealtekRTL8822CMenu.app/' || {
    echo "Menu app archive has no top-level RealtekRTL8822CMenu.app" >&2
    exit 1
}
if unzip -Z1 "$menu_app_archive" | grep -Evq '^RealtekRTL8822CMenu\.app(/|$)'; then
    echo "Menu app archive contains an unexpected top-level entry" >&2
    exit 1
fi
unzip -tq "$archive" >/dev/null
echo "Release package reproducibility check passed"
