#include <algorithm>
#include <cassert>
#include <cstdint>

struct PollResult {
    std::uint32_t polls;
    std::uint32_t packets;
    std::uint32_t maxBatch;
    std::uint32_t budgetHits;
};

static PollResult drain(std::uint32_t pending, std::uint32_t budget) {
    PollResult result{};
    bool rxMasked = pending != 0U;
    while (pending != 0U) {
        assert(rxMasked);
        const std::uint32_t batch = std::min(pending, budget);
        pending -= batch;
        result.polls++;
        result.packets += batch;
        result.maxBatch = std::max(result.maxBatch, batch);
        if (batch == budget && pending != 0U) result.budgetHits++;
        rxMasked = pending != 0U;
    }
    assert(!rxMasked);
    return result;
}

int main() {
    constexpr std::uint32_t linuxRtl8822cImr0 = 0x000044fdU;
    constexpr std::uint32_t driverImr0WithoutBeacon = 0x000004fdU;
    constexpr std::uint32_t rdu = 1U << 1;
    constexpr std::uint32_t rok = 1U << 0;
    static_assert((linuxRtl8822cImr0 & rdu) == 0U, "Linux RTL8822C mask excludes RDU");
    static_assert((driverImr0WithoutBeacon & rdu) == 0U, "driver must exclude RDU");
    static_assert((driverImr0WithoutBeacon & rok) != 0U, "driver must retain ROK");

    PollResult shortBurst = drain(16U, 8U);
    assert(shortBurst.polls == 2U);
    assert(shortBurst.maxBatch == 8U);

    PollResult fullRing = drain(511U, 8U);
    assert(fullRing.polls == 64U);
    assert(fullRing.packets == 511U);
    assert(fullRing.maxBatch == 8U);
    assert(fullRing.budgetHits == 63U);

    // A stopped output queue must be serviceable by RX polling even when the
    // serialized work loop cannot dispatch the latched BEDOK callback.
    // Linux submits at most 254 entries: after that submission avail_desc()
    // is one and the software queue is stopped before entry 255 is consumed.
    constexpr std::uint32_t maxSubmitted = 254U;
    static_assert(255U - maxSubmitted < 2U,
                  "Linux stop threshold must preserve the reserved slot");
    std::uint32_t outstanding = maxSubmitted;
    bool stalled = true;
    const std::uint32_t hardwareConsumed = 16U;
    outstanding -= std::min(outstanding, hardwareConsumed);
    // Linux wakes only when avail_desc() > 4. For a 256-entry ring with one
    // reserved slot this is equivalent to outstanding <= 250.
    if (stalled && outstanding < 251U) stalled = false;
    assert(outstanding == 238U);
    assert(!stalled);

    // The hardware-completion callback must only schedule the independent
    // basic output queue. Synchronously draining retained packets here would
    // put TX work back onto the latency-sensitive RX work loop.
    bool asynchronousServiceRequested = !stalled;
    bool synchronousDrainOnRxLoop = false;
    assert(asynchronousServiceRequested);
    assert(!synchronousDrainOnRxLoop);
    return 0;
}
