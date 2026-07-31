// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#ifndef RTL8822C_USER_CLIENT_SHARED_H
#define RTL8822C_USER_CLIENT_SHARED_H

#include <stdint.h>

#define RTL8822C_USER_CLIENT_TYPE 0U
#define RTL8822C_USER_CLIENT_PROTOCOL_VERSION 2U
#define RTL8822C_SCAN_SNAPSHOT_MAX_ENTRIES 32U

enum RTL8822CScanEntryFlags {
    kRTL8822CScanEntryHidden = 1U << 0,
    kRTL8822CScanEntrySecure = 1U << 1,
    kRTL8822CScanEntryCck = 1U << 2,
    kRTL8822CScanEntryHt = 1U << 3,
    kRTL8822CScanEntryVht = 1U << 4,
    kRTL8822CScanEntryWmm = 1U << 5,
    kRTL8822CScanEntryConnectable = 1U << 6,
    kRTL8822CScanEntryDfsRequired = 1U << 7
};

enum RTL8822CUserCommand {
    kRTL8822CUserCommandUpdateStatus = 1,
    kRTL8822CUserCommandScan = 2,
    kRTL8822CUserCommandConnect = 3,
    kRTL8822CUserCommandDisconnect = 4,
    kRTL8822CUserCommandSetInterfaceEnabled = 5,
    kRTL8822CUserCommandDirectedScan = 6,
    kRTL8822CUserCommandCancelConnection = 7
};

enum RTL8822CUserClientSelector {
    kRTL8822CUserClientSelectorCommand = 0,
    kRTL8822CUserClientSelectorScanSnapshot = 1,
    kRTL8822CUserClientSelectorCount = 2
};

struct RTL8822CUserClientCommand {
    uint32_t version;
    uint32_t command;
    uint32_t enabled;
    uint32_t reserved;
    char ssid[33];
    char password[65];
};

struct RTL8822CUserClientScanEntry {
    uint8_t bssid[6];
    uint8_t ssidLength;
    uint8_t securityMode;
    int32_t rssi;
    uint16_t primaryChannel;
    uint16_t centerChannel;
    uint8_t bandwidth;
    uint8_t flags;
    uint16_t reserved;
    uint32_t lastSeenAgeMs;
    char ssid[33];
    uint8_t padding[7];
};

struct RTL8822CUserClientScanSnapshot {
    uint32_t version;
    uint32_t generation;
    uint32_t count;
    uint32_t entrySize;
    struct RTL8822CUserClientScanEntry entries[RTL8822C_SCAN_SNAPSHOT_MAX_ENTRIES];
};

#endif
