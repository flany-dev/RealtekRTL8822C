#!/bin/sh
set -eu

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <version> <kext> <rtl8822cctl> <output>" >&2
    exit 64
fi

version=$1
kext=$2
rtl8822cctl=$3
output=$4
kext_binary="$kext/Contents/MacOS/RealtekRTL8822C"
dsym_binary="$(dirname "$kext")/RealtekRTL8822C.dSYM/Contents/Resources/DWARF/RealtekRTL8822C"

{
    echo "RealtekRTL8822C build manifest"
    echo "version=$version"
    echo "configuration=Release"
    echo "architecture=x86_64"
    echo "macos_min=15.5"
    echo "firmware_sha256=3deecb31210986d98cdbfb000391e08d602a6eee4ffc883969faa2b907ab03ba"
    echo "firmware_licence_sha256=a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e"
    echo "firmware_source=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/rtw88/rtw8822c_fw.bin"
    echo "linux_rtw88_reference=a56bcd26e770257612a0803249cbd4095fc6feca"
    echo "mackernelsdk_reference=05094e5e88cec7caedbfb35e8449ed0db94bf95b"
    echo "kext_binary_sha256=$(shasum -a 256 "$kext_binary" | awk '{print $1}')"
    if [ -f "$dsym_binary" ]; then
        echo "dsym_binary_sha256=$(shasum -a 256 "$dsym_binary" | awk '{print $1}')"
    fi
    echo "rtl8822cctl_sha256=$(shasum -a 256 "$rtl8822cctl" | awk '{print $1}')"
    echo "compiler=$(clang --version | sed -n '1p')"
    echo "sdk=$(xcrun --sdk macosx --show-sdk-version)"
} > "$output"
