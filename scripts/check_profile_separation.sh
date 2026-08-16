#!/bin/sh
set -eu

debug_kext=build/Debug/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C
release_kext=build/Release/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C
debug_tool=build/Debug/rtl8822cctl
release_tool=build/Release/rtl8822cctl
debug_app=build/app/RealtekRTL8822CMenu.app/Contents/MacOS/RealtekRTL8822CMenu
release_app=$debug_app

for path in "$debug_kext" "$release_kext" "$debug_tool" "$release_tool" \
    "$debug_app" "$release_app"; do
    test -f "$path" || { echo "profile check missing: $path" >&2; exit 1; }
done

if strings "$release_kext" | grep -q 'Debug_'; then
    echo "Release kext contains a Debug_ registry key" >&2
    exit 1
fi
if strings "$release_kext" | grep -q 'RTL8822CDebug'; then
    echo "Release kext contains deep startup diagnostics" >&2
    exit 1
fi
if strings "$release_tool" | grep -q 'Debug_Diagnostics_Revision'; then
    echo "Release rtl8822cctl contains an individual deep report key" >&2
    exit 1
fi
for binary in "$debug_kext" "$release_kext"; do
    if nm -u "$binary" | grep -q '_IOLog'; then
        echo "kext still relies on unavailable kernel logging: $binary" >&2
        exit 1
    fi
done

strings "$debug_kext" | grep -q 'Debug_Diagnostics_Revision'
strings "$debug_kext" | grep -q 'RTL8822CDebugStartTrace'
strings "$debug_tool" | grep -q 'Debug_Diagnostics_Revision'
strings "$debug_app" | grep -q 'Debug Info'
strings "$release_app" | grep -q 'Debug Info'
strings "$release_app" | grep -q 'Fail Info'
strings "$release_app" | grep -q 'RTL8822CDebug'
if strings "$release_app" | grep -q 'Debug_Diagnostics_Revision'; then
    echo "Release menu app embeds an individual Debug property" >&2
    exit 1
fi

if grep -nE '(setProperty|removeProperty)\("Debug_' src/RealtekRTL8822C.cpp; then
    echo "Debug registry publication bypasses the profile macro" >&2
    exit 1
fi

echo "Debug/Release profile separation check passed"
