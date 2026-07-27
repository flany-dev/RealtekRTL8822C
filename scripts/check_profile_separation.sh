#!/bin/sh
set -eu

debug_kext=build/Debug/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C
release_kext=build/Release/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C
debug_tool=build/Debug/rtl8822cctl
release_tool=build/Release/rtl8822cctl

for path in "$debug_kext" "$release_kext" "$debug_tool" "$release_tool"; do
    test -f "$path" || { echo "profile check missing: $path" >&2; exit 1; }
done

if strings "$release_kext" | grep -q 'Debug_'; then
    echo "Release kext contains a Debug_ registry key" >&2
    exit 1
fi
if strings "$release_tool" | grep -q 'Debug_'; then
    echo "Release rtl8822cctl contains a Debug_ report key" >&2
    exit 1
fi
if nm -u "$release_kext" | grep -q '_IOLog'; then
    echo "Release kext still links kernel logging" >&2
    exit 1
fi

strings "$debug_kext" | grep -q 'Debug_Diagnostics_Revision'
strings "$debug_tool" | grep -q 'Debug_Diagnostics_Revision'
nm -u "$debug_kext" | grep -q '_IOLog'

if grep -nE '(setProperty|removeProperty)\("Debug_' src/RealtekRTL8822C.cpp; then
    echo "Debug registry publication bypasses the profile macro" >&2
    exit 1
fi

echo "Debug/Release profile separation check passed"
