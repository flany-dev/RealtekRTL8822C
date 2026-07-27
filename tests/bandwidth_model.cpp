#include <cassert>
#include <cstdint>

static std::uint32_t vhtMask(std::uint8_t streams, std::uint8_t bandwidth)
{
    const std::uint32_t rates = bandwidth == 0 ? 0x1ffU : 0x3ffU;
    std::uint32_t mask = 0x10U | (rates << 12);
    if (streams == 2) mask |= rates << 22;
    return mask;
}

static std::uint8_t dataSc(std::uint8_t bandwidth, std::uint8_t primaryIndex)
{
    std::uint8_t txsc40 = 0;
    if (bandwidth == 2)
        txsc40 = (primaryIndex == 1 || primaryIndex == 3) ? 9 : 10;
    return static_cast<std::uint8_t>(primaryIndex | (txsc40 << 4));
}

static std::uint32_t rf18(std::uint8_t center, std::uint8_t bandwidth)
{
    const std::uint32_t bw = bandwidth == 2 ? 0x1000U :
                             (bandwidth == 1 ? 0x2000U : 0x3000U);
    const std::uint32_t rfsi = center > 140 ? 0x40000U :
                               (center >= 80 ? 0x20000U : 0U);
    return 0x10100U | rfsi | bw | center;
}

static std::uint8_t unii1PrimaryIndex(std::uint8_t primary)
{
    switch (primary) {
    case 36: return 4;
    case 40: return 2;
    case 44: return 1;
    case 48: return 3;
    default: return 0;
    }
}

static std::uint8_t primaryIndex(std::uint8_t primary, std::uint8_t center)
{
    const int offset = static_cast<int>(primary) - static_cast<int>(center);
    if (offset == -6) return 4;
    if (offset == -2) return 2;
    if (offset == 2) return 1;
    if (offset == 6) return 3;
    return 0;
}

static std::uint8_t powerGroup(std::uint8_t center)
{
    if (center <= 42) return 0;
    if (center <= 50) return 1;
    if (center >= 149 && center <= 155) return 10;
    if (center >= 157 && center <= 161) return 11;
    if (center == 165) return 12;
    return 0xff;
}

static std::uint8_t rtl8822cPhyBandwidth(std::uint8_t rxsc,
                                        std::uint8_t currentBandwidth)
{
    if (rxsc == 0) return currentBandwidth;
    if (rxsc <= 8) return 0;
    if (rxsc <= 12) return 1;
    return 2;
}

struct Limits {
    int ofdm;
    int ht1;
    int ht2;
    int vht1;
    int vht2;
};

static Limits fccLimits(std::uint8_t primary, std::uint8_t center,
                        std::uint8_t bandwidth)
{
    if (primary >= 149) {
        if (bandwidth == 2) return {8, 8, 8, 8, -2};
        if (bandwidth == 1) return {8, 8, 8, 8, 8};
        return {8, 12, 12, 12, 12};
    }
    if (bandwidth == 2) return {primary == 36 ? 6 : 8, 2, -4, 0, -10};
    if (center == 38) return {primary == 36 ? 6 : 8, 2, -4, 2, -4};
    if (center == 46) return {8, 8, 4, 8, 4};
    return {primary == 36 ? 6 : 8, primary == 36 ? 8 : 12, 4,
            primary == 36 ? 8 : 12, 4};
}

int main()
{
    // VHT20 excludes MCS9; VHT40/80 include MCS0-9.
    assert(vhtMask(1, 0) == 0x001ff010U);
    assert(vhtMask(2, 0) == 0x7fdff010U);
    assert(vhtMask(1, 2) == 0x003ff010U);
    assert(vhtMask(2, 2) == 0xfffff010U);
    assert(vhtMask(1, 1) == 0x003ff010U);
    assert(vhtMask(2, 1) == 0xfffff010U);

    // Every primary position in the UNII-1 80 MHz block maps to Linux's
    // RTW_SC_20_* values and DATA_SC encoding around center channel 42.
    assert(unii1PrimaryIndex(36) == 4U);
    assert(unii1PrimaryIndex(40) == 2U);
    assert(unii1PrimaryIndex(44) == 1U);
    assert(unii1PrimaryIndex(48) == 3U);
    assert(dataSc(2, unii1PrimaryIndex(36)) == 0xa4U);
    assert(dataSc(2, unii1PrimaryIndex(40)) == 0xa2U);
    assert(dataSc(2, unii1PrimaryIndex(44)) == 0x91U);
    assert(dataSc(2, unii1PrimaryIndex(48)) == 0x93U);
    assert(rf18(42, 2) == 0x1112aU);

    // FCC non-DFS UNII-3 80 MHz block mirrors the primary-index geometry
    // around center 155 and uses adjacent EFUSE groups 10/11.
    assert(primaryIndex(149, 155) == 4U);
    assert(primaryIndex(153, 155) == 2U);
    assert(primaryIndex(157, 155) == 1U);
    assert(primaryIndex(161, 155) == 3U);
    assert(dataSc(2, primaryIndex(149, 155)) == 0xa4U);
    assert(dataSc(2, primaryIndex(157, 155)) == 0x91U);
    assert(rf18(155, 2) == 0x5119bU);
    assert(rf18(151, 1) == 0x52197U);
    assert(rf18(159, 1) == 0x5219fU);
    assert(rf18(165, 0) == 0x531a5U);
    assert(powerGroup(151) == 10U);
    assert(powerGroup(155) == 10U);
    assert(powerGroup(159) == 11U);
    assert(powerGroup(165) == 12U);
    const Limits unii1 = fccLimits(36, 42, 2);
    assert(unii1.ofdm == 6 && unii1.ht1 == 2 && unii1.ht2 == -4);
    assert(unii1.vht1 == 0 && unii1.vht2 == -10);
    const Limits unii3 = fccLimits(149, 155, 2);
    assert(unii3.ofdm == 8 && unii3.ht1 == 8 && unii3.ht2 == 8);
    assert(unii3.vht1 == 8 && unii3.vht2 == -2);

    // 40 MHz examples: primary 36/center 38 and primary 44/center 46.
    assert(dataSc(1, 2) == 0x02U);
    assert(dataSc(1, 1) == 0x01U);
    assert(rf18(38, 1) == 0x12126U);
    assert(rf18(46, 1) == 0x1212eU);

    // RTL8822C Linux query_phy_status_page1() treats descriptor W4.BW only
    // as a preliminary value. PHY RXSC is authoritative for the received PPDU.
    assert(rtl8822cPhyBandwidth(0, 2) == 2U);
    assert(rtl8822cPhyBandwidth(1, 2) == 0U);
    assert(rtl8822cPhyBandwidth(8, 2) == 0U);
    assert(rtl8822cPhyBandwidth(9, 2) == 1U);
    assert(rtl8822cPhyBandwidth(12, 2) == 1U);
    assert(rtl8822cPhyBandwidth(13, 2) == 2U);
    assert(rtl8822cPhyBandwidth(15, 2) == 2U);
    return 0;
}
