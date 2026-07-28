#!/bin/sh
set -eu

debug_kext=build/Debug/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C
release_kext=build/Release/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C
debug_tool=build/Debug/rtl8822cctl
release_tool=build/Release/rtl8822cctl
debug_app=build/Debug/RealtekRTL8822CMenu.app/Contents/MacOS/RealtekRTL8822CMenu
release_app=build/Release/RealtekRTL8822CMenu.app/Contents/MacOS/RealtekRTL8822CMenu

for path in "$debug_kext" "$release_kext" "$debug_tool" "$release_tool" \
    "$debug_app" "$release_app"; do
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
if strings "$release_app" | grep -Eq 'Debug_|Debug Info'; then
    echo "Release menu app contains a Debug surface" >&2
    exit 1
fi
if nm -u "$release_kext" | grep -q '_IOLog'; then
    echo "Release kext still links kernel logging" >&2
    exit 1
fi

strings "$debug_kext" | grep -q 'Debug_Diagnostics_Revision'
strings "$debug_tool" | grep -q 'Debug_Diagnostics_Revision'
strings "$debug_app" | grep -q 'Debug Info'
nm -u "$debug_kext" | grep -q '_IOLog'

if grep -nE '(setProperty|removeProperty)\("Debug_' src/RealtekRTL8822C.cpp; then
    echo "Debug registry publication bypasses the profile macro" >&2
    exit 1
fi

echo "Debug/Release profile separation check passed"
