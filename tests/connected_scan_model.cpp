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
    static const std::uint8_t channels[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        36, 40, 44, 48, 149, 153, 157, 161, 165
    };

    std::vector<Step> plan;
    for (std::uint8_t channel : channels) {
        plan.push_back({Phase::Drain, homeChannel, 0});
        plan.push_back({Phase::Dwell, channel, 55});
        plan.push_back({Phase::Home, homeChannel, 65});
    }
    return plan;
}

static bool shouldRefreshBeforeConnect(bool targetPresent, std::uint64_t ageMs) {
    return !targetPresent || ageMs > 10000U;
}

static bool shouldAutoScanOnMenuOpen(bool hasPreviousAttempt,
                                     std::uint64_t ageMs) {
    return !hasPreviousAttempt || ageMs >= 15000U;
}

int main() {
    const std::uint8_t homeChannel = 149;
    const std::vector<Step> plan = buildConnectedScanPlan(homeChannel);

    assert(plan.size() == 20U * 3U);
    unsigned offChannelBudgetMs = 0;
    bool sawChannel165 = false;
    for (std::size_t index = 0; index < plan.size(); index += 3U) {
        assert(plan[index].phase == Phase::Drain);
        assert(plan[index + 1U].phase == Phase::Dwell);
        assert(plan[index + 1U].durationMs <= 55U);
        assert(plan[index + 2U].phase == Phase::Home);
        assert(plan[index + 2U].channel == homeChannel);
        assert(plan[index + 2U].durationMs >= 65U);
        offChannelBudgetMs += plan[index + 1U].durationMs;
        sawChannel165 = sawChannel165 || plan[index + 1U].channel == 165U;
    }

    assert(offChannelBudgetMs == 1100U);
    assert(sawChannel165);
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
