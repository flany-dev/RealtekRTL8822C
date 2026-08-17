#!/bin/sh
set -eu

debug_root="build/Debug"
app_root="build/app/RealtekRTL8822CMenu.app"
app_binary="$app_root/Contents/MacOS/RealtekRTL8822CMenu"
tool_binary="$debug_root/rtl8822cctl"

test -x "$app_binary"
test -x "$tool_binary"

app_minos=$(otool -l "$app_binary" | awk '/minos/{print $2; exit}')
tool_minos=$(otool -l "$tool_binary" | awk '/minos/{print $2; exit}')
test "$app_minos" = "12.0"
test "$tool_minos" = "12.0"

plist_minos=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' \
    "$app_root/Contents/Info.plist")
test "$plist_minos" = "12.0"

# SMAppService is macOS 13+, so the framework must remain weak-linked while
# source calls stay guarded for the declared macOS 12 userspace target.
otool -L "$app_binary" | grep -q 'ServiceManagement.*weak'
grep -q 'if #available(macOS 13.0, \*)' app/RealtekRTL8822CMenu.swift
grep -q 'guard #available(macOS 13.0, \*)' app/RealtekRTL8822CMenu.swift

# Match the RTL8822CE PCI identity only. Subsystem vendor/device and revision
# are diagnostic evidence, not hardware eligibility gates.
match=$(/usr/libexec/PlistBuddy -c \
    'Print :IOKitPersonalities:RealtekRTL8822C:IOPCIMatch' Info.plist)
case " $match " in
    *" 0xc82210ec "*) ;;
    *) echo "missing exact RTL8822CE PCI match" >&2; exit 1 ;;
esac
score=$(/usr/libexec/PlistBuddy -c \
    'Print :IOKitPersonalities:RealtekRTL8822C:IOProbeScore' Info.plist)
test "$score" = "5000"
if grep -q 'IONameMatch' Info.plist; then
    echo "PCI identity must be selected by exact vendor/device match" >&2
    exit 1
fi
if grep -Eqi 'subsystem-(vendor|id)|revision-id' Info.plist; then
    echo "hardware match must not depend on subsystem or revision" >&2
    exit 1
fi

# Only Debug lowers the experimental kext deployment target. Release remains
# evidence-gated until the older-kernel hardware matrix passes.
grep -q '^DEBUG_DRIVER_MACOS_MIN ?= 12\.0$' Makefile
grep -q '^RELEASE_DRIVER_MACOS_MIN ?= 15\.5$' Makefile
grep -q '^USERSPACE_MACOS_MIN ?= 12\.0$' Makefile
grep -q -- '-L$(MAC_KERNEL_SDK)/Library/universal' Makefile
strings build/Debug/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C |
    grep -qx 'macOS-12.0'
strings build/Release/RealtekRTL8822C.kext/Contents/MacOS/RealtekRTL8822C |
    grep -qx 'macOS-15.5'

# A failed controller start must remain distinguishable from an unloaded kext.
# The bounded report lives on the PCI provider and does not require a surviving
# RealtekRTL8822C service or unavailable kernel logs.
grep -q 'RTL8822CStartResult' src/RealtekRTL8822C.cpp
grep -q 'configurePciPhyCompatibility' src/RealtekRTL8822C.cpp
grep -q 'RTL8822CPCIPhyConfig' src/RealtekRTL8822C.cpp
grep -q 'strcmp(key, "RTL8822CPCIPhyConfig")' app/bridge/RTL8822CClient.c
grep -q 'strcmp(key, "RTL8822CPCIeLinkConfig")' app/bridge/RTL8822CClient.c
grep -q 'strcmp(key, "RTL8822CRXPacketPoolStatus")' \
    app/bridge/RTL8822CClient.c
grep -q 'RTL8822CDebugRXPacketPoolSetup' src/RealtekRTL8822C.cpp
grep -q 'RTW_DEBUG_PROPERTY("Debug_Diagnostics_Revision", RTW_VERSION)' \
    src/RealtekRTL8822C.cpp
if grep -Eq 'diag=0\.0\.[0-9]|0\.0\.3-full-band-passive-scan' \
    src/RealtekRTL8822C.cpp; then
    echo "Debug diagnostic revision must follow RTW_VERSION" >&2
    exit 1
fi
if grep -q 'refillRxPacketPool(kRtwRxPacketPoolCapacity)' \
    src/RealtekRTL8822C.cpp; then
    echo "initial RX reserve must use bounded refill batches" >&2
    exit 1
fi
# The current datapath owns only BEQ and MGMTQ. Do not reintroduce physically
# contiguous queues that have no producer and only increase startup pressure.
if grep -Eq 'start:(bkq|viq|voq)-|[bBvV][kio][qQ](Desc|PayloadDmaCmd)' \
    src/RealtekRTL8822C.cpp; then
    echo "unused BKQ/VIQ/VOQ DMA pools must remain removed" >&2
    exit 1
fi
grep -q 'qsel != 18' src/RealtekRTL8822C.cpp
grep -q '#if RTW_DEBUG' src/RealtekRTL8822C.cpp
grep -q 'if (includeDebug && controllerService)' app/bridge/RTL8822CClient.c
grep -q 'RTWMergeMissingProperty, properties' app/bridge/RTL8822CClient.c
grep -q 'driverAvailability == .ready && debugDriver' \
    app/RealtekRTL8822CMenu.swift
grep -q 'includeDebug: true' app/RealtekRTL8822CMenu.swift
grep -q 'RTL8822CDebugEFUSESummary' src/RealtekRTL8822C.cpp
grep -q 'RTWCopyCompatiblePCIDevice' app/bridge/RTL8822CClient.c
grep -q 'case initializationFailed = 3' app/RealtekRTL8822CMenu.swift
grep -q 'client.property("BuildConfiguration")' app/RealtekRTL8822CMenu.swift
grep -q 'client.property("RTL8822CBuildConfiguration")' \
    app/RealtekRTL8822CMenu.swift
grep -q 'title: "Fail Info…"' app/RealtekRTL8822CMenu.swift
grep -q 'case 3:' tools/rtl8822cctl/main.cpp

echo "Compatibility surface check passed"
