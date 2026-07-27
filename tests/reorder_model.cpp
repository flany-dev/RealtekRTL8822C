// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

class ReorderModel {
public:
    explicit ReorderModel(uint16_t start, uint16_t window)
        : head(start), window(window) { slots.fill(-1); }

    void receive(uint16_t seq) {
        uint16_t delta = (seq - head) & 0x0fff;
        if (delta >= 2048) return;
        if (delta >= window) advance((seq - window + 1) & 0x0fff);
        if (seq == head) {
            output.push_back(seq);
            head = (head + 1) & 0x0fff;
            release();
        } else {
            slots[seq % slots.size()] = seq;
        }
    }

    void advance(uint16_t newHead) {
        uint16_t count = (newHead - head) & 0x0fff;
        if (count >= 2048) return;
        while (count--) {
            int& slot = slots[head % slots.size()];
            if (slot == head) { output.push_back(head); slot = -1; }
            head = (head + 1) & 0x0fff;
        }
        release();
    }

    uint16_t head;
    std::vector<uint16_t> output;

private:
    void release() {
        for (;;) {
            int& slot = slots[head % slots.size()];
            if (slot != head) return;
            output.push_back(head);
            slot = -1;
            head = (head + 1) & 0x0fff;
        }
    }

    uint16_t window;
    std::array<int, 64> slots;
};

struct BaAgreementModel {
    bool active = false;
    uint8_t token = 0;
    uint16_t window = 0;
    uint16_t timeout = 0;
    uint32_t starts = 0;
    uint32_t retries = 0;
    uint32_t stops = 0;

    void request(uint8_t newToken, uint16_t newWindow, uint16_t newTimeout) {
        if (active && token == newToken && window == newWindow &&
            timeout == newTimeout) {
            retries++;
            return;
        }
        if (active) stops++;
        active = true;
        token = newToken;
        window = newWindow;
        timeout = newTimeout;
        starts++;
    }
};

int main() {
    ReorderModel ordinary(100, 64);
    ordinary.receive(100);
    ordinary.receive(102);
    ordinary.receive(101);
    assert((ordinary.output == std::vector<uint16_t>{100, 101, 102}));

    ReorderModel wrapped(4094, 64);
    wrapped.receive(0);
    wrapped.receive(4094);
    wrapped.receive(4095);
    assert((wrapped.output == std::vector<uint16_t>{4094, 4095, 0}));

    ReorderModel timeout(20, 64);
    timeout.receive(22);
    timeout.advance(22);
    assert((timeout.output == std::vector<uint16_t>{22}));

    ReorderModel shifted(10, 4);
    shifted.receive(15);
    assert(shifted.head == 12);
    shifted.receive(12);
    shifted.receive(13);
    shifted.receive(14);
    assert((shifted.output == std::vector<uint16_t>{12, 13, 14, 15}));

    BaAgreementModel agreement;
    agreement.request(7, 64, 0);
    agreement.request(7, 64, 0); // management retry, not a new session
    assert(agreement.starts == 1);
    assert(agreement.retries == 1);
    assert(agreement.stops == 0);
    agreement.request(8, 64, 0);
    assert(agreement.starts == 2);
    assert(agreement.stops == 1);
}
