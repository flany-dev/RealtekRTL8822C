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
unzip -tq "$release_kext_archive" >/dev/null
unzip -Z1 "$release_kext_archive" | grep -Fxq 'RealtekRTL8822C.kext/' || {
    echo "Release kext archive has no top-level RealtekRTL8822C.kext" >&2
    exit 1
}
if unzip -Z1 "$release_kext_archive" | grep -Evq '^RealtekRTL8822C\.kext(/|$)'; then
    echo "Release kext archive contains an unexpected top-level entry" >&2
    exit 1
fi
debug_root="RealtekRTL8822C-$(cat VERSION)-Debug"
unzip -tq "$debug_kext_archive" >/dev/null
unzip -Z1 "$debug_kext_archive" |
    grep -Fxq "$debug_root/RealtekRTL8822C.kext/" || {
    echo "Debug archive has no diagnostic kext" >&2
    exit 1
}
unzip -Z1 "$debug_kext_archive" | grep -Fxq "$debug_root/rtl8822cctl" || {
    echo "Debug archive has no diagnostic rtl8822cctl" >&2
    exit 1
}
if unzip -Z1 "$debug_kext_archive" |
    grep -Evq "^$debug_root(/?$|/RealtekRTL8822C\.kext(/|$)|/rtl8822cctl$)"; then
    echo "Debug archive contains an unexpected entry" >&2
    exit 1
fi
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
