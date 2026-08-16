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

static io_service_t RTWCopyService(void) {
    CFMutableDictionaryRef matching = IOServiceMatching("RealtekRTL8822C");
    if (!matching) return IO_OBJECT_NULL;
    return IOServiceGetMatchingService(kIOMainPortDefault, matching);
}

static bool RTWCopyPCIIdentifier(io_registry_entry_t entry, CFStringRef key,
                                 uint32_t* identifier) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        entry, key, kCFAllocatorDefault, 0);
    if (!value) return false;
    bool copied = false;
    if (CFGetTypeID(value) == CFDataGetTypeID() &&
        CFDataGetLength((CFDataRef)value) >= (CFIndex)sizeof(uint32_t)) {
        memcpy(identifier, CFDataGetBytePtr((CFDataRef)value), sizeof(uint32_t));
        copied = true;
    } else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
        copied = CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type,
                                  identifier);
    }
    CFRelease(value);
    return copied;
}

static bool RTWCopyStringPropertyEquals(io_registry_entry_t entry,
                                        CFStringRef key,
                                        CFStringRef expected) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        entry, key, kCFAllocatorDefault, 0);
    bool equal = value && CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringCompare((CFStringRef)value, expected, 0) == kCFCompareEqualTo;
    if (value) CFRelease(value);
    return equal;
}

static io_service_t RTWCopyCompatiblePCIDevice(void) {
    CFMutableDictionaryRef matching = IOServiceMatching("IOPCIDevice");
    if (!matching) return IO_OBJECT_NULL;
    io_iterator_t iterator = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) !=
        kIOReturnSuccess) return IO_OBJECT_NULL;

    io_service_t compatible = IO_OBJECT_NULL;
    bool compatibleHasDiagnostics = false;
    io_registry_entry_t entry;
    while ((entry = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        uint32_t vendor = 0;
        uint32_t device = 0;
        if (RTWCopyPCIIdentifier(entry, CFSTR("vendor-id"), &vendor) &&
            RTWCopyPCIIdentifier(entry, CFSTR("device-id"), &device) &&
            (vendor & 0xffffU) == 0x10ecU && (device & 0xffffU) == 0xc822U) {
            bool failed = RTWCopyStringPropertyEquals(
                entry, CFSTR("RTL8822CStartResult"), CFSTR("failed"));
            CFTypeRef startResult = IORegistryEntryCreateCFProperty(
                entry, CFSTR("RTL8822CStartResult"), kCFAllocatorDefault, 0);
            bool hasDiagnostics = startResult != NULL;
            if (startResult) CFRelease(startResult);
            if (failed || !compatible ||
                (hasDiagnostics && !compatibleHasDiagnostics)) {
                if (compatible) IOObjectRelease(compatible);
                compatible = entry;
                compatibleHasDiagnostics = hasDiagnostics;
                if (failed) break;
                continue;
            }
        }
        IOObjectRelease(entry);
    }
    IOObjectRelease(iterator);
    return compatible;
}

uint32_t RTWClientGetAvailability(void) {
    io_service_t driver = RTWCopyService();
    if (driver) {
        IOObjectRelease(driver);
        return 2U;
    }

    io_service_t compatible = RTWCopyCompatiblePCIDevice();
    if (!compatible) return 0U;
    CFTypeRef result = IORegistryEntryCreateCFProperty(
        compatible, CFSTR("RTL8822CStartResult"), kCFAllocatorDefault, 0);
    IOObjectRelease(compatible);
    bool failed = result && CFGetTypeID(result) == CFStringGetTypeID() &&
        CFStringCompare((CFStringRef)result, CFSTR("failed"), 0) ==
            kCFCompareEqualTo;
    if (result) CFRelease(result);
    return failed ? 3U : 1U;
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

static bool RTWAppendJSON(char* output, size_t capacity, size_t* used,
                          const char* value) {
    size_t length = strlen(value);
    if (*used + length + 1 > capacity) return false;
    memcpy(output + *used, value, length);
    *used += length;
    output[*used] = '\0';
    return true;
}

int32_t RTWClientCopyScanSnapshotJSON(char* output, size_t outputCapacity) {
    if (!output || outputCapacity == 0) return kIOReturnBadArgument;
    output[0] = '\0';

    io_service_t service = RTWCopyService();
    if (!service) return kIOReturnNotFound;
    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t result = IOServiceOpen(
        service, mach_task_self(), RTL8822C_USER_CLIENT_TYPE, &connection);
    IOObjectRelease(service);
    if (result != kIOReturnSuccess) return result;

    struct RTL8822CUserClientScanSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    size_t snapshotSize = sizeof(snapshot);
    result = IOConnectCallStructMethod(
        connection, kRTL8822CUserClientSelectorScanSnapshot,
        NULL, 0, &snapshot, &snapshotSize);
    IOServiceClose(connection);
    if (result != kIOReturnSuccess) return result;
    if (snapshotSize != sizeof(snapshot) ||
        snapshot.version != RTL8822C_USER_CLIENT_PROTOCOL_VERSION ||
        snapshot.entrySize != sizeof(struct RTL8822CUserClientScanEntry) ||
        snapshot.count > RTL8822C_SCAN_SNAPSHOT_MAX_ENTRIES) {
        return kIOReturnBadMessageID;
    }

    size_t used = 0;
    char header[96];
    snprintf(header, sizeof(header), "{\"version\":%u,\"generation\":%u,\"entries\":[",
             snapshot.version, snapshot.generation);
    if (!RTWAppendJSON(output, outputCapacity, &used, header))
        return kIOReturnNoSpace;

    for (uint32_t index = 0; index < snapshot.count; index++) {
        const struct RTL8822CUserClientScanEntry* entry = &snapshot.entries[index];
        if (entry->ssidLength > 32 || entry->reserved != 0) return kIOReturnBadMessageID;
        char prefix[320];
        snprintf(prefix, sizeof(prefix),
                 "%s{\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ssidHex\":\"",
                 index == 0 ? "" : ",",
                 entry->bssid[0], entry->bssid[1], entry->bssid[2],
                 entry->bssid[3], entry->bssid[4], entry->bssid[5]);
        if (!RTWAppendJSON(output, outputCapacity, &used, prefix))
            return kIOReturnNoSpace;
        for (uint8_t byteIndex = 0; byteIndex < entry->ssidLength; byteIndex++) {
            char byteText[3];
            snprintf(byteText, sizeof(byteText), "%02x",
                     (unsigned char)entry->ssid[byteIndex]);
            if (!RTWAppendJSON(output, outputCapacity, &used, byteText))
                return kIOReturnNoSpace;
        }
        char suffix[384];
        snprintf(suffix, sizeof(suffix),
                 "\",\"hidden\":%s,\"security\":%u,\"channel\":%u,\"centerChannel\":%u,\"bandwidth\":%u,\"rssi\":%d,\"flags\":%u,\"ageMs\":%u}",
                 (entry->flags & kRTL8822CScanEntryHidden) ? "true" : "false",
                 entry->securityMode, entry->primaryChannel,
                 entry->centerChannel, entry->bandwidth, entry->rssi,
                 entry->flags, entry->lastSeenAgeMs);
        if (!RTWAppendJSON(output, outputCapacity, &used, suffix))
            return kIOReturnNoSpace;
    }
    if (!RTWAppendJSON(output, outputCapacity, &used, "]}"))
        return kIOReturnNoSpace;
    return kIOReturnSuccess;
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

    bool providerProperty = strncmp(key, "RTL8822C", 8) == 0;
    io_service_t service = RTWCopyService();
    if (!service && providerProperty) service = RTWCopyCompatiblePCIDevice();
    if (!service) return kIOReturnNotFound;
    CFStringRef propertyKey = CFStringCreateWithCString(
        kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!propertyKey) {
        IOObjectRelease(service);
        return kIOReturnNoMemory;
    }
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        service, propertyKey, kCFAllocatorDefault, 0);
    IOObjectRelease(service);
    if (!value && providerProperty) {
        service = RTWCopyCompatiblePCIDevice();
        if (service) {
            value = IORegistryEntryCreateCFProperty(
                service, propertyKey, kCFAllocatorDefault, 0);
            IOObjectRelease(service);
        }
    }
    CFRelease(propertyKey);
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

static void RTWMergeMissingProperty(const void* key, const void* value,
                                    void* context) {
    CFMutableDictionaryRef target = (CFMutableDictionaryRef)context;
    if (!CFDictionaryContainsKey(target, key))
        CFDictionarySetValue(target, key, value);
}

int32_t RTWClientCopyReport(char* output, size_t outputCapacity,
                            uint32_t includeDebug) {
    if (!output || outputCapacity == 0) return kIOReturnBadArgument;
    output[0] = '\0';

    io_service_t service = RTWCopyService();
    bool controllerService = service != IO_OBJECT_NULL;
    if (!service) service = RTWCopyCompatiblePCIDevice();
    if (!service) return kIOReturnNotFound;
    CFMutableDictionaryRef properties = NULL;
    kern_return_t result = IORegistryEntryCreateCFProperties(
        service, &properties, kCFAllocatorDefault, 0);
    IOObjectRelease(service);
    if (result != kIOReturnSuccess || !properties) return result;

    if (includeDebug && controllerService) {
        io_service_t provider = RTWCopyCompatiblePCIDevice();
        if (provider) {
            CFMutableDictionaryRef providerProperties = NULL;
            kern_return_t providerResult = IORegistryEntryCreateCFProperties(
                provider, &providerProperties, kCFAllocatorDefault, 0);
            IOObjectRelease(provider);
            if (providerResult == kIOReturnSuccess && providerProperties) {
                CFDictionaryApplyFunction(providerProperties,
                                          RTWMergeMissingProperty, properties);
                CFRelease(providerProperties);
            }
        }
    }

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
        bool isDebug = strncmp(key, "RTL8822CDebug",
                               sizeof("RTL8822CDebug") - 1) == 0;
        isDebug = isDebug || strncmp(key, "Debug_", 6) == 0;
        bool isPublic = strcmp(key, "DriverVersion") == 0 ||
                        strcmp(key, "BuildConfiguration") == 0 ||
                        strcmp(key, "DriverBuildTargetMacOS") == 0 ||
                        strcmp(key, "PowerState") == 0 ||
                        strcmp(key, "InterfaceState") == 0 ||
                        strcmp(key, "InterfaceUserEnabled") == 0 ||
                        strcmp(key, "DriverStatus") == 0 ||
                        strcmp(key, "WiFiStatus") == 0 ||
                        strcmp(key, "ConnectedSSID") == 0 ||
                        strcmp(key, "SignalStrength") == 0 ||
                        strcmp(key, "ScanGeneration") == 0 ||
                        strcmp(key, "ScanState") == 0 ||
                        strcmp(key, "ConnectionQueueState") == 0 ||
                        strcmp(key, "ConnectionAttemptID") == 0 ||
                        strcmp(key, "ConnectionTargetSSID") == 0 ||
                        strcmp(key, "ConnectionPhase") == 0 ||
                        strcmp(key, "ConnectionResult") == 0 ||
                        strcmp(key, "ConnectionFailureCode") == 0 ||
                        strcmp(key, "LinkEventGeneration") == 0 ||
                        strcmp(key, "LinkEventType") == 0 ||
                        strcmp(key, "LinkEventSSID") == 0 ||
                        strcmp(key, "LinkEventReason") == 0 ||
                        strcmp(key, "ConnectedScanState") == 0 ||
                        strcmp(key, "ScanResults") == 0 ||
                        strcmp(key, "RTL8822CStartResult") == 0 ||
                        strcmp(key, "RTL8822CStartStage") == 0 ||
                        strcmp(key, "RTL8822CStartStageCode") == 0 ||
                        strcmp(key, "RTL8822CStartFailure") == 0 ||
                        strcmp(key, "RTL8822CDriverVersion") == 0 ||
                        strcmp(key, "RTL8822CBuildConfiguration") == 0 ||
                        strcmp(key, "RTL8822CBuildTargetMacOS") == 0 ||
                        strcmp(key, "RTL8822CPCIVendor") == 0 ||
                        strcmp(key, "RTL8822CPCIDevice") == 0 ||
                        strcmp(key, "RTL8822CPCIRevision") == 0 ||
                        strcmp(key, "RTL8822CPCISubsystemVendor") == 0 ||
                        strcmp(key, "RTL8822CPCISubsystemDevice") == 0 ||
                        strcmp(key, "RTL8822CChipVersion") == 0 ||
                        strcmp(key, "RTL8822CChipCut") == 0 ||
                        strcmp(key, "RTL8822CRFPathCount") == 0 ||
                        strcmp(key, "RTL8822CRFEOption") == 0 ||
                        strcmp(key, "RTL8822CPCIELinkSpeed") == 0 ||
                        strcmp(key, "RTL8822CPCIELinkWidth") == 0 ||
                        strcmp(key, "RTL8822CPCIPhyConfig") == 0 ||
                        strcmp(key, "RTL8822CPCIeLinkConfig") == 0;
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
