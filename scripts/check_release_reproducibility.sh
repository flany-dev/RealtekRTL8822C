#!/bin/sh
set -eu

first="$(mktemp -t rtl8822c-release-first.XXXXXX)"
second="$(mktemp -t rtl8822c-release-second.XXXXXX)"
trap 'rm -f "$first" "$second"' EXIT HUP INT TERM

hash_release() {
    shasum -a 256 \
        build/Release/RealtekRTL8822C.kext/Contents/Info.plist \
        build/Release/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C \
        build/Release/rtl8822cctl | awk '{print $1}'
}

make clean >/dev/null
make release >/dev/null
hash_release > "$first"

make clean >/dev/null
make release >/dev/null
hash_release > "$second"

if ! cmp -s "$first" "$second"; then
    echo "Release build is not reproducible:" >&2
    diff -u "$first" "$second" >&2 || true
    exit 1
fi

echo "Release reproducibility check passed"
