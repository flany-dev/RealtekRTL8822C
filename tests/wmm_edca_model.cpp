// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#include <cassert>
#include <cstdint>

static std::uint8_t aifs(std::uint32_t channel, bool capabilityShortSlot,
                         std::uint8_t aifsn) {
    const bool shortSlot = channel > 14U || capabilityShortSlot;
    const std::uint8_t slot = shortSlot ? 9U : 20U;
    const std::uint8_t sifs = channel > 14U ? 16U : 10U;
    return static_cast<std::uint8_t>(sifs + aifsn * slot);
}

static std::uint32_t edca(std::uint16_t txop, std::uint8_t ecwMax,
                          std::uint8_t ecwMin, std::uint8_t aifsValue) {
    return static_cast<std::uint32_t>(txop) << 16 |
           static_cast<std::uint32_t>(ecwMax) << 12 |
           static_cast<std::uint32_t>(ecwMin) << 8 |
           aifsValue;
}

int main() {
    // Webworking's 5-GHz Association Response reports capability 0x016e,
    // without the 2.4-GHz Short Slot bit. 5 GHz still mandates a 9-us slot.
    assert(aifs(40U, false, 3U) == 0x2bU);
    assert(aifs(40U, false, 7U) == 0x4fU);
    assert(aifs(40U, false, 2U) == 0x22U);
    assert(edca(0U, 10U, 4U, aifs(40U, false, 3U)) == 0x0000a42bU);
    assert(edca(0U, 10U, 4U, aifs(40U, false, 7U)) == 0x0000a44fU);
    assert(edca(94U, 4U, 3U, aifs(40U, false, 2U)) == 0x005e4322U);
    assert(edca(47U, 3U, 2U, aifs(40U, false, 2U)) == 0x002f3222U);

    // Preserve long-slot behavior for a legacy 2.4-GHz BSS, and short slot
    // when the ERP capability explicitly enables it.
    assert(aifs(6U, false, 3U) == 70U);
    assert(aifs(6U, true, 3U) == 37U);
    return 0;
}
