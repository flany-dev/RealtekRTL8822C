#include <cassert>
#include <cstdint>

enum class Availability : std::uint32_t {
    unsupported = 0,
    kextNotLoaded = 1,
    ready = 2,
    initializationFailed = 3
};

enum class FailureAction {
    none,
    copyCompatibilityReport,
    showFailInfo
};

enum class StartFailureClass {
    nonFatalCompatibility,
    fatalResource,
    fatalHardware
};

static StartFailureClass classifyStartFailure(bool optionalCompatibility,
                                              bool resourceFailure) {
    if (optionalCompatibility) return StartFailureClass::nonFatalCompatibility;
    return resourceFailure ? StartFailureClass::fatalResource :
                             StartFailureClass::fatalHardware;
}

static Availability availability(bool pciPresent, bool controllerPresent,
                                 bool failedPostmortem) {
    if (controllerPresent) return Availability::ready;
    if (!pciPresent) return Availability::unsupported;
    return failedPostmortem ? Availability::initializationFailed :
                             Availability::kextNotLoaded;
}

static FailureAction failureAction(Availability state, bool debugBuild) {
    if (state != Availability::initializationFailed)
        return FailureAction::none;
    return debugBuild ? FailureAction::showFailInfo :
                        FailureAction::copyCompatibilityReport;
}

static bool detectDebugBuild(Availability state, bool serviceSaysDebug,
                             bool providerSaysDebug) {
    if (state == Availability::ready)
        return serviceSaysDebug || providerSaysDebug;
    if (state == Availability::initializationFailed)
        return providerSaysDebug || serviceSaysDebug;
    return false;
}

struct ChipProfile {
    std::uint8_t cut;
    std::uint8_t rfPathCount;
    std::uint8_t rfPathMask;
    bool fastEmacClock;
};

struct ProviderPostmortem {
    bool chip = true;
    bool rfe = true;
    bool efuse = true;

    void beginAttempt() {
        chip = false;
        rfe = false;
        efuse = false;
    }
};

static ChipProfile decodeChip(std::uint32_t sysCfg1) {
    ChipProfile profile{};
    profile.cut = static_cast<std::uint8_t>((sysCfg1 >> 12) & 0x0fU);
    profile.rfPathCount = (sysCfg1 & (1U << 27)) ? 2 : 1;
    profile.rfPathMask = profile.rfPathCount == 2 ? 0x03 : 0x01;
    profile.fastEmacClock = profile.cut >= 3;
    return profile;
}

static bool rfeSupported(std::uint8_t rfe) {
    return rfe <= 6;
}

static bool permitTwoStreams(const ChipProfile& profile,
                             bool boardPowerValid) {
    return profile.rfPathCount == 2 && boardPowerValid;
}

int main() {
    assert(classifyStartFailure(true, false) ==
           StartFailureClass::nonFatalCompatibility);
    assert(classifyStartFailure(false, true) == StartFailureClass::fatalResource);
    assert(classifyStartFailure(false, false) == StartFailureClass::fatalHardware);
    assert(availability(false, false, false) == Availability::unsupported);
    assert(availability(true, false, false) == Availability::kextNotLoaded);
    assert(availability(true, false, true) ==
           Availability::initializationFailed);
    assert(availability(true, true, true) == Availability::ready);
    assert(failureAction(Availability::initializationFailed, true) ==
           FailureAction::showFailInfo);
    assert(failureAction(Availability::initializationFailed, false) ==
           FailureAction::copyCompatibilityReport);
    assert(failureAction(Availability::kextNotLoaded, true) ==
           FailureAction::none);
    assert(failureAction(Availability::unsupported, true) ==
           FailureAction::none);
    assert(failureAction(Availability::ready, true) == FailureAction::none);
    assert(detectDebugBuild(Availability::ready, true, false));
    assert(detectDebugBuild(Availability::ready, false, true));
    assert(detectDebugBuild(Availability::initializationFailed, false, true));
    assert(!detectDebugBuild(Availability::initializationFailed, false, false));
    assert(!detectDebugBuild(Availability::kextNotLoaded, false, true));

    ProviderPostmortem repeatedStart;
    repeatedStart.beginAttempt();
    assert(!repeatedStart.chip && !repeatedStart.rfe && !repeatedStart.efuse);

    const ChipProfile onePathCutC = decodeChip(2U << 12);
    assert(onePathCutC.cut == 2);
    assert(onePathCutC.rfPathCount == 1);
    assert(onePathCutC.rfPathMask == 0x01);
    assert(!onePathCutC.fastEmacClock);
    assert(!permitTwoStreams(onePathCutC, true));

    const ChipProfile twoPathCutD = decodeChip((3U << 12) | (1U << 27));
    assert(twoPathCutD.cut == 3);
    assert(twoPathCutD.rfPathCount == 2);
    assert(twoPathCutD.rfPathMask == 0x03);
    assert(twoPathCutD.fastEmacClock);
    assert(permitTwoStreams(twoPathCutD, true));
    assert(!permitTwoStreams(twoPathCutD, false));

    for (std::uint8_t rfe = 0; rfe <= 6; rfe++) assert(rfeSupported(rfe));
    assert(!rfeSupported(7));
    assert(!rfeSupported(0xff));
    return 0;
}
