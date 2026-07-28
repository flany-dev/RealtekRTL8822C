// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#include "RTL8822CClient.h"
#include "RTL8822CUserClientShared.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RTW_APP_DEBUG
#define RTW_APP_DEBUG 0
#endif

static io_service_t RTWCopyService(void) {
    CFMutableDictionaryRef matching = IOServiceMatching("RealtekRTL8822C");
    if (!matching) return IO_OBJECT_NULL;
    return IOServiceGetMatchingService(kIOMainPortDefault, matching);
}

static void RTWSecureZero(void* pointer, size_t size) {
    volatile unsigned char* bytes = (volatile unsigned char*)pointer;
    while (size--) *bytes++ = 0;
}

int32_t RTWClientSendCommand(uint32_t command, const char* ssid,
                             const char* password, uint32_t enabled) {
    io_service_t service = RTWCopyService();
    if (!service) return kIOReturnNotFound;

    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t result = IOServiceOpen(
        service, mach_task_self(), RTL8822C_USER_CLIENT_TYPE, &connection);
    IOObjectRelease(service);
    if (result != kIOReturnSuccess) return result;

    struct RTL8822CUserClientCommand request;
    memset(&request, 0, sizeof(request));
    request.version = RTL8822C_USER_CLIENT_PROTOCOL_VERSION;
    request.command = command;
    request.enabled = enabled ? 1U : 0U;
    if (ssid) strlcpy(request.ssid, ssid, sizeof(request.ssid));
    if (password) strlcpy(request.password, password, sizeof(request.password));

    result = IOConnectCallStructMethod(
        connection, kRTL8822CUserClientSelectorCommand,
        &request, sizeof(request), NULL, NULL);
    RTWSecureZero(&request, sizeof(request));
    IOServiceClose(connection);
    return result;
}

static bool RTWCopyCFValueString(CFTypeRef value, char* output,
                                 size_t outputCapacity) {
    if (!value || !output || outputCapacity == 0) return false;
    output[0] = '\0';

    if (CFGetTypeID(value) == CFStringGetTypeID()) {
        return CFStringGetCString((CFStringRef)value, output,
                                  (CFIndex)outputCapacity,
                                  kCFStringEncodingUTF8);
    }
    if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
        strlcpy(output, CFBooleanGetValue((CFBooleanRef)value) ? "true" : "false",
                outputCapacity);
        return true;
    }
    if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        long long number = 0;
        if (!CFNumberGetValue((CFNumberRef)value, kCFNumberLongLongType,
                              &number)) return false;
        snprintf(output, outputCapacity, "%lld", number);
        return true;
    }
    return false;
}

int32_t RTWClientCopyProperty(const char* key, char* output,
                              size_t outputCapacity) {
    if (!key || !output || outputCapacity == 0) return kIOReturnBadArgument;
    output[0] = '\0';

    io_service_t service = RTWCopyService();
    if (!service) return kIOReturnNotFound;
    CFStringRef propertyKey = CFStringCreateWithCString(
        kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!propertyKey) {
        IOObjectRelease(service);
        return kIOReturnNoMemory;
    }
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, propertyKey, kCFAllocatorDefault, 0);
    CFRelease(propertyKey);
    IOObjectRelease(service);
    if (!value) return kIOReturnNotFound;

    bool copied = RTWCopyCFValueString(value, output, outputCapacity);
    CFRelease(value);
    return copied ? kIOReturnSuccess : kIOReturnUnsupported;
}

static bool RTWAppend(char* output, size_t outputCapacity, size_t* used,
                      const char* text) {
    size_t length = strlen(text);
    if (*used + length + 1 > outputCapacity) return false;
    memcpy(output + *used, text, length);
    *used += length;
    output[*used] = '\0';
    return true;
}

int32_t RTWClientCopyReport(char* output, size_t outputCapacity,
                            uint32_t includeDebug) {
    if (!output || outputCapacity == 0) return kIOReturnBadArgument;
    output[0] = '\0';

    io_service_t service = RTWCopyService();
    if (!service) return kIOReturnNotFound;
    CFMutableDictionaryRef properties = NULL;
    kern_return_t result = IORegistryEntryCreateCFProperties(
        service, &properties, kCFAllocatorDefault, 0);
    IOObjectRelease(service);
    if (result != kIOReturnSuccess || !properties) return result;

    CFIndex count = CFDictionaryGetCount(properties);
    const void** keys = (const void**)calloc((size_t)count, sizeof(void*));
    const void** values = (const void**)calloc((size_t)count, sizeof(void*));
    if (!keys || !values) {
        free(keys);
        free(values);
        CFRelease(properties);
        return kIOReturnNoMemory;
    }
    CFDictionaryGetKeysAndValues(properties, keys, values);

    size_t used = 0;
    for (CFIndex index = 0; index < count; index++) {
        if (CFGetTypeID(keys[index]) != CFStringGetTypeID()) continue;
        char key[256];
        char value[8192];
        if (!CFStringGetCString((CFStringRef)keys[index], key, sizeof(key),
                                kCFStringEncodingUTF8)) continue;
#if RTW_APP_DEBUG
        bool isDebug = strncmp(key, "Debug_", 6) == 0;
#else
        bool isDebug = false;
        (void)includeDebug;
#endif
        bool isPublic = strcmp(key, "DriverVersion") == 0 ||
                        strcmp(key, "BuildConfiguration") == 0 ||
                        strcmp(key, "PowerState") == 0 ||
                        strcmp(key, "InterfaceState") == 0 ||
                        strcmp(key, "InterfaceUserEnabled") == 0 ||
                        strcmp(key, "DriverStatus") == 0 ||
                        strcmp(key, "WiFiStatus") == 0 ||
                        strcmp(key, "ConnectedSSID") == 0 ||
                        strcmp(key, "SignalStrength") == 0 ||
                        strcmp(key, "ConnectedScanState") == 0 ||
                        strcmp(key, "ScanResults") == 0;
        if ((!includeDebug || !isDebug) && !isPublic) continue;
        if (!RTWCopyCFValueString(values[index], value, sizeof(value))) continue;

        char line[8704];
        snprintf(line, sizeof(line), "%s: %s\n", key, value);
        if (!RTWAppend(output, outputCapacity, &used, line)) {
            result = kIOReturnNoSpace;
            break;
        }
    }

    free(keys);
    free(values);
    CFRelease(properties);
    return result;
}
