#include <cassert>
#include <cstdint>
#include <cstring>

struct Bss {
    std::uint8_t bssid[6] = {};
    char ssid[33] = {};
    std::uint8_t ssidLength = 0;
    bool hidden = true;
    unsigned channel = 0;
    int rssi = -127;
};

static void merge(Bss& target, const char* ssid, unsigned channel, int rssi) {
    const std::size_t length = ssid ? std::strlen(ssid) : 0;
    assert(length <= 32);
    if (length != 0) {
        std::memset(target.ssid, 0, sizeof(target.ssid));
        std::memcpy(target.ssid, ssid, length);
        target.ssidLength = static_cast<std::uint8_t>(length);
        target.hidden = false;
    }
    if (channel != 0) target.channel = channel;
    if (rssi >= -120 && rssi <= 0) {
        target.rssi = target.rssi >= -120 ?
            (target.rssi * 3 + rssi) / 4 : rssi;
    }
}

static bool sameBssid(const Bss& left, const Bss& right) {
    return std::memcmp(left.bssid, right.bssid, sizeof(left.bssid)) == 0;
}

static bool canTransmit(unsigned channel) {
    if (channel >= 1 && channel <= 11) return true;
    return channel == 36 || channel == 40 || channel == 44 || channel == 48 ||
           channel == 149 || channel == 153 || channel == 157 ||
           channel == 161 || channel == 165;
}

static bool requiresDfs(unsigned channel) {
    return channel >= 52 && channel <= 144;
}

int main() {
    Bss hidden;
    hidden.bssid[5] = 1;
    merge(hidden, "", 36, -62);
    assert(hidden.hidden && hidden.ssidLength == 0 && hidden.channel == 36);

    merge(hidden, "Visible5G", 36, -60);
    assert(!hidden.hidden);
    assert(std::strcmp(hidden.ssid, "Visible5G") == 0);
    assert(hidden.ssidLength == 9);

    merge(hidden, "", 36, -61);
    assert(!hidden.hidden);
    assert(std::strcmp(hidden.ssid, "Visible5G") == 0);

    Bss otherHidden;
    otherHidden.bssid[5] = 2;
    merge(otherHidden, "", 40, -60);
    assert(otherHidden.hidden);
    assert(!sameBssid(hidden, otherHidden));

    Bss sameNameOtherBand;
    sameNameOtherBand.bssid[5] = 3;
    merge(sameNameOtherBand, "Visible5G", 1, -40);
    assert(!sameBssid(hidden, sameNameOtherBand));
    assert(std::strcmp(hidden.ssid, sameNameOtherBand.ssid) == 0);
    assert(hidden.channel != sameNameOtherBand.channel);
    assert(canTransmit(1) && canTransmit(36) && canTransmit(165));
    assert(!canTransmit(12) && !canTransmit(52) && !canTransmit(144));
    assert(requiresDfs(52) && requiresDfs(144));
    assert(!requiresDfs(48) && !requiresDfs(149));
    return 0;
}
