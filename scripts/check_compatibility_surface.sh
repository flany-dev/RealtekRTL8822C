#!/bin/sh
set -eu

debug_root="build/Debug"
app_binary="$debug_root/RealtekRTL8822CMenu.app/Contents/MacOS/RealtekRTL8822CMenu"
tool_binary="$debug_root/rtl8822cctl"

test -x "$app_binary"
test -x "$tool_binary"

app_minos=$(otool -l "$app_binary" | awk '/minos/{print $2; exit}')
tool_minos=$(otool -l "$tool_binary" | awk '/minos/{print $2; exit}')
test "$app_minos" = "12.0"
test "$tool_minos" = "12.0"

plist_minos=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' \
    "$debug_root/RealtekRTL8822CMenu.app/Contents/Info.plist")
test "$plist_minos" = "12.0"

# SMAppService is macOS 13+, so the framework must remain weak-linked while
# source calls stay guarded for the declared macOS 12 userspace target.
otool -L "$app_binary" | grep -q 'ServiceManagement.*weak'
grep -q 'if #available(macOS 13.0, \*)' app/RealtekRTL8822CMenu.swift
grep -q 'guard #available(macOS 13.0, \*)' app/RealtekRTL8822CMenu.swift

# Match the RTL8822CE PCI identity only. Subsystem vendor/device and revision
# must not become false-negative gates for another board using the same chip.
match=$(/usr/libexec/PlistBuddy -c \
    'Print :IOKitPersonalities:RealtekRTL8822C:IOPCIMatch' Info.plist)
case " $match " in
    *" 0xc82210ec "*) ;;
    *) echo "missing exact RTL8822CE PCI match" >&2; exit 1 ;;
esac
if grep -Eqi 'subsystem-(vendor|id)|revision-id' Info.plist; then
    echo "hardware match must not depend on subsystem or revision" >&2
    exit 1
fi

# Only Debug lowers the experimental kext deployment target. Release remains
# evidence-gated until the older-kernel hardware matrix passes.
grep -q '^DEBUG_DRIVER_MACOS_MIN ?= 12\.0$' Makefile
grep -q '^RELEASE_DRIVER_MACOS_MIN ?= 15\.5$' Makefile
grep -q '^USERSPACE_MACOS_MIN ?= 12\.0$' Makefile
strings build/Debug/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C |
    grep -qx 'macOS-12.0'
strings build/Release/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C |
    grep -qx 'macOS-15.5'

echo "Compatibility surface check passed"
