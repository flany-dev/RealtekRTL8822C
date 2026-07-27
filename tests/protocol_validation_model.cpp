// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

static uint16_t readBe16(const uint8_t* value) {
    return static_cast<uint16_t>((static_cast<uint16_t>(value[0]) << 8) |
                                 value[1]);
}

static bool isSuite(const uint8_t* suite, uint8_t type) {
    return suite[0] == 0x00 && suite[1] == 0x0f &&
           suite[2] == 0xac && suite[3] == type;
}

static bool acceptsWpa2PskCcmp(const std::vector<uint8_t>& ie) {
    if (ie.size() < 20 || ie.size() > 64 || ie[0] != 48 ||
        static_cast<size_t>(ie[1]) + 2 != ie.size()) return false;
    const uint8_t* body = ie.data() + 2;
    size_t left = ie.size() - 2;
    if (left < 2 || body[0] != 1 || body[1] != 0) return false;
    body += 2; left -= 2;
    if (left < 4 || !isSuite(body, 4)) return false;
    body += 4; left -= 4;
    if (left < 2) return false;
    uint16_t pairwiseCount = static_cast<uint16_t>(body[0] | (body[1] << 8));
    body += 2; left -= 2;
    if (pairwiseCount == 0 || static_cast<size_t>(pairwiseCount) > left / 4)
        return false;
    bool pairwiseCcmp = false;
    for (uint16_t i = 0; i < pairwiseCount; i++)
        pairwiseCcmp = pairwiseCcmp || isSuite(body + i * 4, 4);
    body += static_cast<size_t>(pairwiseCount) * 4;
    left -= static_cast<size_t>(pairwiseCount) * 4;
    if (left < 2) return false;
    uint16_t akmCount = static_cast<uint16_t>(body[0] | (body[1] << 8));
    body += 2; left -= 2;
    if (akmCount == 0 || static_cast<size_t>(akmCount) > left / 4) return false;
    bool psk = false;
    for (uint16_t i = 0; i < akmCount; i++)
        psk = psk || isSuite(body + i * 4, 2);
    body += static_cast<size_t>(akmCount) * 4;
    left -= static_cast<size_t>(akmCount) * 4;
    uint16_t capabilities = left >= 2 ?
        static_cast<uint16_t>(body[0] | (body[1] << 8)) : 0;
    return pairwiseCcmp && psk && !(capabilities & (1U << 6));
}

enum class EapolKind { Invalid, Message1, Message3, GroupMessage1 };

static EapolKind classifyEapol(const std::vector<uint8_t>& eapol) {
    if (eapol.size() < 99 || eapol[1] != 3 || eapol[4] != 2)
        return EapolKind::Invalid;
    size_t total = 4U + readBe16(eapol.data() + 2);
    if (total < 99 || total > eapol.size() ||
        99U + readBe16(eapol.data() + 97) > total)
        return EapolKind::Invalid;
    uint16_t keyInfo = readBe16(eapol.data() + 5);
    if ((keyInfo & 7) != 2 ||
        (keyInfo & ((1U << 10) | (1U << 11) | (1U << 13))))
        return EapolKind::Invalid;
    bool pairwise = (keyInfo & (1U << 3)) != 0;
    bool mic = (keyInfo & (1U << 8)) != 0;
    bool ack = (keyInfo & (1U << 7)) != 0;
    if (pairwise && ack && !mic) return EapolKind::Message1;
    if (pairwise && ack && mic) return EapolKind::Message3;
    if (!pairwise && ack && mic) return EapolKind::GroupMessage1;
    return EapolKind::Invalid;
}

static bool acceptPn(uint64_t& lastPn, uint64_t pn) {
    if (pn == 0 || pn <= lastPn) return false;
    lastPn = pn;
    return true;
}

static std::vector<uint8_t> validRsn() {
    return {48, 20, 1, 0,
            0x00,0x0f,0xac,0x04,
            1,0, 0x00,0x0f,0xac,0x04,
            1,0, 0x00,0x0f,0xac,0x02,
            0,0};
}

static std::vector<uint8_t> eapolWith(uint16_t keyInfo) {
    std::vector<uint8_t> frame(99, 0);
    frame[0] = 2;
    frame[1] = 3;
    frame[2] = 0;
    frame[3] = 95;
    frame[4] = 2;
    frame[5] = static_cast<uint8_t>(keyInfo >> 8);
    frame[6] = static_cast<uint8_t>(keyInfo);
    return frame;
}

int main() {
    std::vector<uint8_t> rsn = validRsn();
    assert(acceptsWpa2PskCcmp(rsn));
    rsn.pop_back();
    assert(!acceptsWpa2PskCcmp(rsn));
    rsn = validRsn();
    rsn[8] = 2; // Claims two pairwise suites but carries only one.
    assert(!acceptsWpa2PskCcmp(rsn));
    rsn = validRsn();
    rsn[19] = 8; // SAE instead of PSK.
    assert(!acceptsWpa2PskCcmp(rsn));
    rsn = validRsn();
    rsn[20] = 0x40; // Management-frame protection required.
    assert(!acceptsWpa2PskCcmp(rsn));

    const uint16_t descriptorV2 = 2;
    assert(classifyEapol(eapolWith(descriptorV2 | (1U << 3) | (1U << 7))) ==
           EapolKind::Message1);
    assert(classifyEapol(eapolWith(descriptorV2 | (1U << 3) | (1U << 7) |
                                          (1U << 8))) == EapolKind::Message3);
    assert(classifyEapol(eapolWith(descriptorV2 | (1U << 7) | (1U << 8))) ==
           EapolKind::GroupMessage1);
    std::vector<uint8_t> truncated(98, 0);
    assert(classifyEapol(truncated) == EapolKind::Invalid);
    std::vector<uint8_t> oversized = eapolWith(descriptorV2 | (1U << 3) |
                                               (1U << 7));
    oversized[97] = 0;
    oversized[98] = 1;
    assert(classifyEapol(oversized) == EapolKind::Invalid);
    assert(classifyEapol(eapolWith(descriptorV2 | (1U << 3) | (1U << 7) |
                                          (1U << 10))) == EapolKind::Invalid);

    uint64_t lastPn = 0;
    assert(!acceptPn(lastPn, 0));
    assert(acceptPn(lastPn, 1));
    assert(!acceptPn(lastPn, 1));
    assert(!acceptPn(lastPn, 0));
    assert(acceptPn(lastPn, 2));
    return 0;
}
