#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class Phase {
    Drain,
    Dwell,
    Home
};

struct Step {
    Phase phase;
    std::uint8_t channel;
    unsigned durationMs;
    bool sendsProbe = false;
};

struct ScanChannel {
    std::uint8_t channel;
    bool active;
};

static const ScanChannel kScanChannels[] = {
    { 1, true }, { 2, true }, { 3, true }, { 4, true }, { 5, true },
    { 6, true }, { 7, true }, { 8, true }, { 9, true }, { 10, true },
    { 11, true }, { 12, false }, { 13, false },
    { 36, true }, { 40, true }, { 44, true }, { 48, true },
    { 52, false }, { 56, false }, { 60, false }, { 64, false },
    { 100, false }, { 104, false }, { 108, false }, { 112, false },
    { 116, false }, { 120, false }, { 124, false }, { 128, false },
    { 132, false }, { 136, false }, { 140, false }, { 144, false },
    { 149, true }, { 153, true }, { 157, true }, { 161, true },
    { 165, true }
};

struct CachedResult {
    std::uint64_t ageNs;
    bool current;
};

static std::size_t retainedResultCount(const std::vector<CachedResult>& results,
                                       std::uint64_t maxAgeNs) {
    std::size_t retained = 0;
    for (const CachedResult& result : results) {
        if (result.current || result.ageNs <= maxAgeNs) retained++;
    }
    return retained;
}

static std::vector<Step> buildConnectedScanPlan(std::uint8_t homeChannel) {
    std::vector<Step> plan;
    for (const ScanChannel& channel : kScanChannels) {
        plan.push_back({Phase::Drain, homeChannel, 0, false});
        plan.push_back({Phase::Dwell, channel.channel,
                        channel.active ? 55U : 110U, channel.active});
        plan.push_back({Phase::Home, homeChannel, 65, false});
    }
    return plan;
}

static std::vector<Step> buildDisconnectedScanPlan() {
    std::vector<Step> plan;
    for (const ScanChannel& channel : kScanChannels)
        plan.push_back({Phase::Dwell, channel.channel,
                        channel.active ? 55U : 110U, channel.active});
    return plan;
}

static bool shouldRefreshBeforeConnect(bool targetPresent, std::uint64_t ageMs) {
    return !targetPresent || ageMs > 10000U;
}

static bool shouldAutoScanOnMenuOpen(bool hasPreviousAttempt,
                                     std::uint64_t ageMs) {
    return !hasPreviousAttempt || ageMs >= 15000U;
}

static std::size_t directedChannelVisitCount(std::uint8_t knownChannel) {
    if (knownChannel == 0) return sizeof(kScanChannels) / sizeof(kScanChannels[0]);
    for (const ScanChannel& channel : kScanChannels) {
        if (channel.channel == knownChannel) return 1U;
    }
    return sizeof(kScanChannels) / sizeof(kScanChannels[0]);
}

int main() {
    const std::uint8_t homeChannel = 149;
    const std::vector<Step> plan = buildConnectedScanPlan(homeChannel);

    assert(plan.size() == 38U * 3U);
    unsigned offChannelBudgetMs = 0;
    bool sawChannel165 = false;
    bool sawPassiveDfs = false;
    unsigned activeProbeCount = 0;
    for (std::size_t index = 0; index < plan.size(); index += 3U) {
        assert(plan[index].phase == Phase::Drain);
        assert(plan[index + 1U].phase == Phase::Dwell);
        assert(plan[index + 1U].durationMs <= 110U);
        assert(plan[index + 2U].phase == Phase::Home);
        assert(plan[index + 2U].channel == homeChannel);
        assert(plan[index + 2U].durationMs >= 65U);
        offChannelBudgetMs += plan[index + 1U].durationMs;
        sawChannel165 = sawChannel165 || plan[index + 1U].channel == 165U;
        sawPassiveDfs = sawPassiveDfs ||
            (plan[index + 1U].channel == 100U && !plan[index + 1U].sendsProbe);
        activeProbeCount += plan[index + 1U].sendsProbe ? 1U : 0U;
    }

    assert(offChannelBudgetMs == 3080U);
    assert(sawChannel165);
    assert(sawPassiveDfs);
    assert(activeProbeCount == 20U);
    const std::vector<Step> disconnected = buildDisconnectedScanPlan();
    assert(disconnected.size() == 38U);
    unsigned disconnectedProbeCount = 0;
    for (const Step& step : disconnected) {
        assert(step.phase == Phase::Dwell);
        assert(step.durationMs == (step.sendsProbe ? 55U : 110U));
        disconnectedProbeCount += step.sendsProbe ? 1U : 0U;
    }
    assert(disconnectedProbeCount == 20U);
    assert(directedChannelVisitCount(36U) == 1U);
    assert(directedChannelVisitCount(52U) == 1U);
    assert(directedChannelVisitCount(0U) == 38U);
    assert(!shouldRefreshBeforeConnect(true, 0U));
    assert(!shouldRefreshBeforeConnect(true, 10000U));
    assert(shouldRefreshBeforeConnect(true, 10001U));
    assert(shouldRefreshBeforeConnect(false, 1U));
    assert(shouldAutoScanOnMenuOpen(false, 0U));
    assert(!shouldAutoScanOnMenuOpen(true, 14999U));
    assert(shouldAutoScanOnMenuOpen(true, 15000U));
    assert(retainedResultCount({{0U, false}, {299999999999ULL, false},
                                {300000000001ULL, false},
                                {600000000000ULL, true}},
                               300000000000ULL) == 3U);
    return 0;
}
