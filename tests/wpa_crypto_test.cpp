// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#include "RtwWpaCrypto.hpp"

#include <iostream>

int main() {
    if (!RtwWpaCrypto::selfTest()) {
        std::cerr << "WPA crypto vectors failed\n";
        return 1;
    }
    std::cout << "WPA crypto vectors passed\n";
    return 0;
}
