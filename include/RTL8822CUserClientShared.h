// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#ifndef RTL8822C_USER_CLIENT_SHARED_H
#define RTL8822C_USER_CLIENT_SHARED_H

#include <stdint.h>

#define RTL8822C_USER_CLIENT_TYPE 0U
#define RTL8822C_USER_CLIENT_PROTOCOL_VERSION 1U

enum RTL8822CUserCommand {
    kRTL8822CUserCommandUpdateStatus = 1,
    kRTL8822CUserCommandScan = 2,
    kRTL8822CUserCommandConnect = 3,
    kRTL8822CUserCommandDisconnect = 4,
    kRTL8822CUserCommandSetInterfaceEnabled = 5
};

enum RTL8822CUserClientSelector {
    kRTL8822CUserClientSelectorCommand = 0,
    kRTL8822CUserClientSelectorCount = 1
};

struct RTL8822CUserClientCommand {
    uint32_t version;
    uint32_t command;
    uint32_t enabled;
    uint32_t reserved;
    char ssid[33];
    char password[65];
};

#endif
