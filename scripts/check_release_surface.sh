#!/bin/sh
set -eu

project=RealtekRTL8822C
bundle_id=org.realtekrtl8822c.driver.RealtekRTL8822C

for path in \
    src/RealtekRTL8822C.cpp \
    src/RealtekRTL8822C_info.c \
    tools/rtl8822cctl/main.cpp \
    docs/RTL8822CCTL.md; do
    test -f "$path" || { echo "release surface missing: $path" >&2; exit 1; }
done

test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' Info.plist)" = "$project"
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' Info.plist)" = "$bundle_id"
test "$(/usr/libexec/PlistBuddy -c 'Print :IOKitPersonalities:RealtekRTL8822C:IOClass' Info.plist)" = "$project"

if grep -Eq '^(stage|install|install-kext|install-tools):' Makefile; then
    echo "release surface must not contain deployment targets" >&2
    exit 1
fi

list_file="$(mktemp -t rtl8822c-release.XXXXXX)"
trap 'rm -f "$list_file"' EXIT HUP INT TERM
find . \( -path './.git' -o -path './build' \) -prune -o \
    -path './scripts/check_release_surface.sh' -prune -o \
    -type f -print0 > "$list_file"

if xargs -0 grep -nE 'RealtekRtw88|org\.realtekrtw88|(^|[^[:alnum:]_])rtwctl([^[:alnum:]_]|$)' < "$list_file"; then
    echo "release surface contains a legacy project or CLI name" >&2
    exit 1
fi

echo "Release surface check passed"
