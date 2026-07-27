// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

struct PlaintextView {
    const std::uint8_t* payload;
    std::uint32_t length;
};

struct PacketReserve {
    std::uint32_t count;
    std::uint32_t minimum;
    bool refillPending;
};

static bool takeReservedPacket(PacketReserve& reserve,
                               std::uint32_t lowWatermark) {
    if (reserve.count == 0U) return false;
    reserve.count--;
    if (reserve.count < reserve.minimum) reserve.minimum = reserve.count;
    if (reserve.count < lowWatermark) reserve.refillPending = true;
    return true;
}

static void refillReserve(PacketReserve& reserve, std::uint32_t capacity,
                          std::uint32_t lowWatermark,
                          std::uint32_t budget) {
    const std::uint32_t missing = capacity - reserve.count;
    reserve.count += missing < budget ? missing : budget;
    reserve.refillPending = reserve.count < lowWatermark;
}

static PlaintextView ccmpPlaintextView(const std::uint8_t* frame,
                                       std::uint32_t len,
                                       std::uint32_t headerLen) {
    assert(len >= headerLen + 16U);
    return {frame + headerLen + 8U, len - headerLen - 16U};
}

int main() {
    constexpr std::uint32_t headerLen = 26U;
    constexpr std::uint32_t payloadLen = 120U;
    std::array<std::uint8_t, headerLen + 8U + payloadLen + 8U> frame{};
    for (std::uint32_t i = 0; i < headerLen; i++) frame[i] = static_cast<std::uint8_t>(i);
    for (std::uint32_t i = 0; i < 8U; i++) frame[headerLen + i] = 0xc0U + i;
    for (std::uint32_t i = 0; i < payloadLen; i++) frame[headerLen + 8U + i] = static_cast<std::uint8_t>(0x40U + i);
    for (std::uint32_t i = 0; i < 8U; i++) frame[headerLen + 8U + payloadLen + i] = 0xe0U + i;

    const auto original = frame;
    const PlaintextView view = ccmpPlaintextView(
        frame.data(), frame.size(), headerLen);
    assert(view.length == payloadLen);
    assert(view.payload == frame.data() + headerLen + 8U);
    for (std::uint32_t i = 0; i < payloadLen; i++)
        assert(view.payload[i] == static_cast<std::uint8_t>(0x40U + i));
    assert(frame == original); // DMA storage remains immutable.

    // RX packet allocation is moved off the hardware poll. The poll consumes
    // only prepared packets; an independent worker replenishes in bounded
    // batches and preserves a reserve across bursts.
    PacketReserve reserve{512U, 512U, false};
    for (std::uint32_t i = 0; i < 160U; i++)
        assert(takeReservedPacket(reserve, 384U));
    assert(reserve.count == 352U);
    assert(reserve.minimum == 352U);
    assert(reserve.refillPending);
    // The worker asks the KPI allocator for one packet list rather than taking
    // the global mbuf allocator lock once per replacement.
    refillReserve(reserve, 512U, 384U, 128U);
    assert(reserve.count == 480U);
    assert(!reserve.refillPending);

    return 0;
}
