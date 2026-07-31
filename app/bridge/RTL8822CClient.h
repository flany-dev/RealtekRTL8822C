// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#ifndef RTL8822C_CLIENT_H
#define RTL8822C_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t RTWClientSendCommand(uint32_t command, const char* ssid,
                             const char* password, uint32_t enabled);
int32_t RTWClientCopyProperty(const char* key, char* output,
                              size_t outputCapacity);
int32_t RTWClientCopyReport(char* output, size_t outputCapacity,
                            uint32_t includeDebug);
int32_t RTWClientCopyScanSnapshotJSON(char* output, size_t outputCapacity);
uint32_t RTWClientGetAvailability(void);

#ifdef __cplusplus
}
#endif

#endif
