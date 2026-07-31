#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

static std::vector<std::uint8_t> buildProbe(const std::uint8_t mac[6],
                                            unsigned channel,
                                            const char* ssid) {
    const std::size_t ssidLength = ssid ? std::strlen(ssid) : 0;
    assert(ssidLength <= 32);
    std::vector<std::uint8_t> frame(24, 0);
    frame[0] = 0x40;
    std::memset(frame.data() + 4, 0xff, 6);
    std::memcpy(frame.data() + 10, mac, 6);
    std::memset(frame.data() + 16, 0xff, 6);
    frame.push_back(0);
    frame.push_back(static_cast<std::uint8_t>(ssidLength));
    frame.insert(frame.end(), ssid, ssid + ssidLength);
    frame.push_back(1);
    if (channel > 14) {
        const std::uint8_t rates[] = {
            0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c
        };
        frame.push_back(sizeof(rates));
        frame.insert(frame.end(), rates, rates + sizeof(rates));
    } else {
        const std::uint8_t rates[] = {
            0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24
        };
        const std::uint8_t extended[] = { 0x30, 0x48, 0x60, 0x6c };
        frame.push_back(sizeof(rates));
        frame.insert(frame.end(), rates, rates + sizeof(rates));
        frame.push_back(50);
        frame.push_back(sizeof(extended));
        frame.insert(frame.end(), extended, extended + sizeof(extended));
    }
    return frame;
}

int main() {
    const std::uint8_t mac[6] = { 0, 1, 2, 3, 4, 5 };
    const std::vector<std::uint8_t> broadcast = buildProbe(mac, 36, "");
    assert(broadcast[0] == 0x40);
    assert(broadcast[24] == 0 && broadcast[25] == 0);
    assert(broadcast[26] == 1 && broadcast[27] == 8);
    assert(std::memcmp(broadcast.data() + 4, "\xff\xff\xff\xff\xff\xff", 6) == 0);

    const std::vector<std::uint8_t> directed = buildProbe(mac, 1, "HiddenAP");
    assert(directed[24] == 0 && directed[25] == 8);
    assert(std::memcmp(directed.data() + 26, "HiddenAP", 8) == 0);
    assert(directed[34] == 1 && directed[35] == 8);
    assert(directed[44] == 50 && directed[45] == 4);
    assert(buildProbe(mac, 36, "01234567890123456789012345678901").size() < 128);
    return 0;
}
