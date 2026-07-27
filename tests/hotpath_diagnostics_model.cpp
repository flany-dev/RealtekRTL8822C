#include <cassert>
#include <cstdint>

static bool shouldPublish(std::uint32_t counter, std::uint32_t mask,
                          bool protocolImportant) {
    return protocolImportant || (counter & mask) == 1U;
}

int main() {
    const std::uint32_t debugMask = 0x3ffU;
    const std::uint32_t releaseMask = 0x1fffU;

    assert(shouldPublish(1U, debugMask, false));
    assert(!shouldPublish(2U, debugMask, false));
    assert(shouldPublish(1025U, debugMask, false));
    assert(shouldPublish(8193U, releaseMask, false));
    assert(!shouldPublish(1025U, releaseMask, false));
    assert(shouldPublish(2U, releaseMask, true));
    return 0;
}
