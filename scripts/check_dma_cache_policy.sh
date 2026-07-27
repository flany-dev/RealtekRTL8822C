#!/bin/sh
set -eu

source_file="src/RealtekRTL8822C.cpp"

grep -q 'kRtwDmaRingMemoryOptions' "$source_file"
grep -q 'kIODirectionInOut | kIOMapInhibitCache' "$source_file"
grep -q 'kRtwDmaPayloadMemoryOptions' "$source_file"
grep -q 'kIODirectionInOut | kIOMapCopybackCache' "$source_file"

payload_uses=$(grep -c 'kRtwDmaPayloadMemoryOptions' "$source_file")
test "$payload_uses" -eq 8

if grep 'inTaskWithPhysicalMask' "$source_file" | grep -q 'kIOMapInhibitCache'; then
    echo "DMA cache policy check failed: literal uncached allocation remains" >&2
    exit 1
fi

grep -q 'rxBufferDmaCmd->synchronize(kIODirectionIn)' "$source_file"
grep -q 'beqPayloadDmaCmd->synchronize(kIODirectionOut)' "$source_file"

echo "DMA cache policy check passed"
