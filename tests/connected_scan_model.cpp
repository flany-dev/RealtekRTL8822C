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

static std::vector<Step> buildConnectedScanPlan(std::uint8_t homeChannel) {
    static const std::uint8_t channels[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        36, 40, 44, 48, 149, 153, 157, 161, 165
    };

    std::vector<Step> plan;
    for (std::uint8_t channel : channels) {
        plan.push_back({Phase::Drain, homeChannel, 0});
        plan.push_back({Phase::Dwell, channel, 35});
        plan.push_back({Phase::Home, homeChannel, 65});
    }
    return plan;
}

static bool shouldRefreshBeforeConnect(bool targetPresent, std::uint64_t ageMs) {
    return !targetPresent || ageMs > 10000U;
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
        assert(plan[index + 1U].durationMs <= 35U);
        assert(plan[index + 2U].phase == Phase::Home);
        assert(plan[index + 2U].channel == homeChannel);
        assert(plan[index + 2U].durationMs >= 65U);
        offChannelBudgetMs += plan[index + 1U].durationMs;
        sawChannel165 = sawChannel165 || plan[index + 1U].channel == 165U;
    }

    assert(offChannelBudgetMs == 700U);
    assert(sawChannel165);
    assert(!shouldRefreshBeforeConnect(true, 0U));
    assert(!shouldRefreshBeforeConnect(true, 10000U));
    assert(shouldRefreshBeforeConnect(true, 10001U));
    assert(shouldRefreshBeforeConnect(false, 1U));
    return 0;
}
