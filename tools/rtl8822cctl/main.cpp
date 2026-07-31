// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2026 RealtekRTL8822C contributors. */

#include <iostream>
#include <iomanip>
#include <string>
#include <algorithm>
#include <termios.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include "RTL8822CClient.h"
#include "RTL8822CUserClientShared.h"

#ifndef RTW_VERSION
#define RTW_VERSION "0.0.0"
#endif

void printCFType(CFTypeRef value) {
    if (!value) {
        std::cout << "null";
        return;
    }
    CFTypeID typeID = CFGetTypeID(value);
    if (typeID == CFStringGetTypeID()) {
        char buf[4096];
        if (CFStringGetCString((CFStringRef)value, buf, sizeof(buf), kCFStringEncodingUTF8)) {
            std::cout << buf;
        } else {
            std::cout << "(unknown string)";
        }
    } else if (typeID == CFNumberGetTypeID()) {
        long long val;
        if (CFNumberGetValue((CFNumberRef)value, kCFNumberLongLongType, &val)) {
            std::cout << val;
        } else {
            std::cout << "(unknown number)";
        }
    } else if (typeID == CFBooleanGetTypeID()) {
        std::cout << (CFBooleanGetValue((CFBooleanRef)value) ? "true" : "false");
    } else if (typeID == CFDataGetTypeID()) {
        CFIndex len = CFDataGetLength((CFDataRef)value);
        const UInt8* bytes = CFDataGetBytePtr((CFDataRef)value);
        std::cout << "Data [len=" << len << "]: ";
        for (CFIndex i = 0; i < len && i < 16; i++) {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i] << " ";
        }
        if (len > 16) std::cout << "...";
        std::cout << std::dec;
    } else {
        std::cout << "(complex object)";
    }
}

static bool readHiddenPassword(std::string& password) {
    if (!isatty(STDIN_FILENO)) return false;
    termios original;
    if (tcgetattr(STDIN_FILENO, &original) != 0) return false;
    termios hidden = original;
    hidden.c_lflag &= ~ECHO;
    std::cerr << "WPA2 password: " << std::flush;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) return false;
    bool read = static_cast<bool>(std::getline(std::cin, password));
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    std::cerr << "\n";
    return read;
}

static bool readStringProperty(io_service_t service, CFStringRef key,
                               std::string& value) {
    CFMutableDictionaryRef properties = nullptr;
    kern_return_t kr = IORegistryEntryCreateCFProperties(
        service, &properties, kCFAllocatorDefault, 0);
    if (kr != kIOReturnSuccess || !properties) return false;
    CFTypeRef object = CFDictionaryGetValue(properties, key);
    bool ok = object && CFGetTypeID(object) == CFStringGetTypeID();
    if (ok) {
        char buffer[8192];
        ok = CFStringGetCString(static_cast<CFStringRef>(object), buffer,
                                sizeof(buffer), kCFStringEncodingUTF8);
        if (ok) value.assign(buffer);
    }
    CFRelease(properties);
    return ok;
}

static void printUsage(std::ostream& out) {
    out << "Usage:\n"
        << "  rtl8822cctl version             Display the command-line tool version\n"
        << "  rtl8822cctl availability        Distinguish driver, compatible PCI hardware, or unsupported hardware\n"
        << "  rtl8822cctl status              Display driver and card status\n"
        << "  rtl8822cctl report              Display the complete diagnostic report\n"
        << "  rtl8822cctl scan                Trigger a wireless scan\n"
        << "  rtl8822cctl bss                 Display the structured BSS snapshot as JSON\n"
        << "  rtl8822cctl connect <SSID> [pw|--ask-password] Connect; prompts when password is omitted\n"
        << "  rtl8822cctl cancel-connect      Cancel a pending connection attempt\n"
        << "  rtl8822cctl disconnect          Disconnect from the current network\n"
        << "  rtl8822cctl on                  Enable the driver Wi-Fi interface\n"
        << "  rtl8822cctl off                 Disable the driver Wi-Fi interface\n";
}

int main(int argc, char* argv[]) {
    std::string cmd = (argc > 1) ? argv[1] : "status";
    if (cmd == "version" || cmd == "--version") {
        std::cout << "rtl8822cctl " << RTW_VERSION << "\n";
        return 0;
    }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        printUsage(std::cout);
        return 0;
    }
    if (cmd == "availability") {
        switch (RTWClientGetAvailability()) {
            case 2: std::cout << "Driver loaded; RTL8822CE is ready.\n"; return 0;
            case 1: std::cout << "Compatible RTL8822CE 10ec:c822 detected; kext is not loaded or failed to start.\n"; return 1;
            default: std::cout << "Supported RTL8822CE 10ec:c822 hardware was not detected.\n"; return 2;
        }
    }
    if (cmd != "status" && cmd != "report" && cmd != "scan" && cmd != "bss" &&
        cmd != "connect" && cmd != "cancel-connect" && cmd != "disconnect" &&
        cmd != "on" && cmd != "off") {
        std::cerr << "Unknown command: " << cmd << "\n";
        printUsage(std::cerr);
        return 64;
    }

    // 1. Locate RealtekRTL8822C service in IORegistry
    CFDictionaryRef matchingDict = IOServiceMatching("RealtekRTL8822C");
    if (!matchingDict) {
        std::cerr << "Failed to create matching dictionary for RealtekRTL8822C.\n";
        return 1;
    }

    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, matchingDict);
    if (!service) {
        std::cerr << "RealtekRTL8822C service not found in IORegistry. Is the kext loaded?\n";
        return 1;
    }

    if (cmd == "bss") {
        char snapshot[64 * 1024];
        kern_return_t kr = RTWClientCopyScanSnapshotJSON(snapshot, sizeof(snapshot));
        if (kr != kIOReturnSuccess) {
            std::cerr << "Failed to read the structured BSS snapshot. Error: 0x"
                      << std::hex << kr << std::dec << "\n";
            IOObjectRelease(service);
            return 2;
        }
        std::cout << snapshot << "\n";
    } else if (cmd == "status" || cmd == "report") {
        // Send UpdateStatus command to driver first to refresh register values
        RTWClientSendCommand(kRTL8822CUserCommandUpdateStatus, "", "", 0);
        CFMutableDictionaryRef properties = nullptr;
        kern_return_t kr = IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0);
        if (kr != kIOReturnSuccess || !properties) {
            std::cerr << "Failed to read properties from RealtekRTL8822C. Error: 0x" << std::hex << kr << std::dec << "\n";
            IOObjectRelease(service);
            return 1;
        }

        std::cout << "=== RealtekRTL8822C WiFi Status ===\n";

        // Print some key properties if they exist
        const char* key_list[] = {
            "DriverVersion",
            "BuildConfiguration",
            "PowerState",
            "InterfaceState",
            "DriverStatus",
            "WiFiStatus",
            "ConnectedSSID",
            "ScanResults",
            "Hardware_CHIP_VERSION",
#if RTW_DEBUG
            "Debug_PM_Registration",
            "Debug_PM_Provider_Managed",
            "Debug_EFUSE_MAC",
            "Debug_EFUSE_MAC_Status",
            "Debug_Last_Checksum",
            "Debug_DDMA_After_Init",
            "Debug_DMA_Cache_Policy",
            "Debug_MSI",
            "Debug_InterruptSource",
            "Debug_Last_ISR",
            "Debug_REG_CR",
            "Debug_REG_RCR",
            "Debug_REG_RXBD_IDX",
            "Debug_REG_BEQ_IDX",
            "Debug_REG_HIMR0",
            "Debug_REG_HISR0",
            "Debug_ISR_Calls",
            "Debug_Last_RF_Reg18",
            "Debug_Rx_Last_Pkt",
            "Debug_Rx_Raw_Desc",
            "Debug_Scan_Count",
            "Debug_Scan_Parse",
            "Debug_Connected_Scan",
            "Debug_Connect_Scan_Source",
            "Debug_HotPath_Diagnostics",
            "Debug_RX_Poll",
            "Debug_RX_Packet_Pool",
            "Debug_RX_PHY",
            "Debug_Lifecycle_Invariant",
            "Debug_Power_Lifecycle",
            "Debug_Interface_Lifecycle",
            "Debug_CAM_Lifecycle",
            "Debug_Interface_Reconcile",
            "Debug_Link_Lifecycle",
            "Debug_Filter_Lifecycle",
            "Debug_TXDMA_Lifecycle_Clear",
            "Debug_H2C_Lifecycle_Reset",
            "Debug_H2C_Mailbox_State",
            "Debug_H2C_Mailbox_Error",
            "Debug_H2C_DMA_Status",
            "Debug_RSSI_Last",
            "Debug_Auth_Rx_Count",
            "Debug_Auth_Rx_Detail",
            "Debug_Tx_Last_Mgmt",
            "Debug_Tx_Last_Data",
            "Debug_DataPlane",
            "Debug_Data_CCX",
            "Debug_Bandwidth_Target",
            "Debug_Bandwidth_Hardware",
            "Debug_Link_Capability",
            "Debug_Assoc_Request",
            "Debug_Assoc_Response",
            "Debug_RA_State",
            "Debug_Media_State",
            "Debug_BA_State",
            "Debug_RX_BA_State",
            "Debug_WMM_EDCA",
            "Debug_Security_State",
            "Debug_Security_Engine",
            "Debug_Peer_Disconnect",
            "Debug_Connection_Lifecycle",
            "Debug_Connection_Watchdog",
            "Debug_MGMT_Recovery",
            "Debug_BEQ_Fault",
            "Debug_BEQ_Recovery",
            "Debug_RX_Data_Last",
            "Debug_RX_Delivered_Last",
            "Debug_BEDOK_Count",
            "Debug_C2H_Count",
            "Debug_C2H_Last_Rpt",
            "Debug_C2H_Kernel_Log",
            "Debug_MGMT_PreDoorbell",
            "Debug_MGMT_PostCCX",
            "Debug_DPK_Init_State",
            "Debug_Phy_Baseline",
            "Debug_CCK_Power_State",
            "Debug_CCK_TxAgc_Write",
            "Debug_Tx_Rf_State",
            "Debug_RFE_Option",
            "Debug_Rf0_PreIQK",
            "Debug_Rf0_PostIQK",
            "Debug_2G_Power_Trim",
            "Debug_Target_Rf0_PreIQK",
            "Debug_Target_Rf0_PostIQK",
            "Debug_Target_IQK_Result",
            "Debug_DACK_Status",
            "Debug_RF_X2_Check",
            "Debug_TXGAPK_Status",
            "Debug_REG_MGMT_IDX",
            "Debug_MGNTDOK_Count",
            "Debug_BCNDMAINT_Count",
            "Debug_MGMT_Rp",
            "Debug_MGMT_Base",
            "Debug_TXDMA_Status",
            "Debug_TXDMA_Status_AfterChannel",
            "Debug_TXDMA_Status_AfterIQK",
            "Debug_TXDMA_Status_PreAuth",
            "Debug_TXDMA_Status_AfterClear",
            "Debug_MGMT_Last_Desc",
            "Debug_BSSID_Readback",
            "Debug_TXERR_Count",
            "Debug_TXFOVW_Count",
            "Debug_Coex_State",
            "Debug_Pinmux_PreCfg",
            "Debug_MGMT_DMA_Addr",
            "Debug_Diagnostics_Revision",
            "Debug_5G_Channel",
            "Debug_5G_Power",
            "Debug_5G_EFUSE_Table",
            "Debug_5G_Power_Trim",
            "Debug_5G_TXAGC",
            "Debug_Snapshot",
            "Debug_EventTrace"
#endif
        };

        for (const char* key : key_list) {
            CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
            CFTypeRef cfVal = CFDictionaryGetValue(properties, cfKey);
            std::cout << std::left << std::setw(28) << (std::string(key) + ":") << " ";
            if (cfVal) {
                printCFType(cfVal);
            } else {
                std::cout << "not set";
            }
            std::cout << "\n";
            CFRelease(cfKey);
        }

        CFRelease(properties);
    } else if (cmd == "scan") {
        kern_return_t kr = RTWClientSendCommand(
            kRTL8822CUserCommandScan, "", "", 0);

        if (kr == kIOReturnSuccess) {
            std::string state;
            bool observedActive = false;
            bool completed = false;
            std::cout << "Scan started; waiting for completion...\n";
            for (int i = 0; i < 150; i++) {
                if (readStringProperty(service, CFSTR("ScanState"), state)) {
                    if (state.find("active=1") != std::string::npos)
                        observedActive = true;
                    if (observedActive &&
                        state.find("active=0") != std::string::npos) {
                        completed = state.find("result=complete") != std::string::npos;
                        break;
                    }
                }
                usleep(100000);
            }
            if (!completed) {
                std::cerr << "Scan did not complete cleanly: " << state << "\n";
                IOObjectRelease(service);
                return 1;
            }
            std::string results;
            std::cout << "Scan complete: " << state << "\n";
            if (readStringProperty(service, CFSTR("ScanResults"), results))
                std::cout << "ScanResults:" << results << "\n";
        } else {
            std::cerr << "Failed to send Scan command. Error: 0x" << std::hex
                      << kr << std::dec << ". The v0.0.2 or newer driver is required.\n";
            IOObjectRelease(service);
            return 2;
        }
    } else if (cmd == "connect") {
        if (argc < 3) {
            std::cerr << "Usage: rtl8822cctl connect <SSID> [password|--ask-password]\n";
            IOObjectRelease(service);
            return 1;
        }
        std::string ssid = argv[2];
        std::string pwd;
        // Omitting the credential is the safe interactive form. Keep the
        // explicit flag for scripts/documentation compatibility, while a
        // positional password remains available for non-interactive callers.
        if (argc == 3 || (argc > 3 && std::string(argv[3]) == "--ask-password")) {
            if (!readHiddenPassword(pwd)) {
                std::cerr << "No password argument was supplied and an interactive "
                          << "terminal is unavailable. Pass the password explicitly "
                          << "for non-interactive use.\n";
                IOObjectRelease(service);
                return 1;
            }
        } else {
            pwd = argv[3];
        }

        kern_return_t kr = RTWClientSendCommand(
            kRTL8822CUserCommandConnect, ssid.c_str(), pwd.c_str(), 0);
        std::fill(pwd.begin(), pwd.end(), '\0');

        if (kr == kIOReturnSuccess) {
            std::cout << "Connect command successfully sent to RealtekRTL8822C for SSID: " << ssid << "\n";
        } else if (kr == kIOReturnUnsupported) {
            std::cerr << "The network '" << ssid << "' does not offer the supported "
                      << "WPA2-PSK/CCMP suite, or requires management-frame protection. "
#if RTW_DEBUG
                      << "Check Debug_Security_State in 'rtl8822cctl report'.\n";
#else
                      << "Check the network security settings.\n";
#endif
            IOObjectRelease(service);
            return 3;
        } else if (kr == kIOReturnBadArgument) {
            std::cerr << "A WPA2 password must contain 8-63 characters, or be a "
                      << "64-digit hexadecimal PMK.\n";
            IOObjectRelease(service);
            return 4;
        } else {
            std::cerr << "Failed to send Connect command. Error: 0x" << std::hex << kr << std::dec
                      << ". Check 'rtl8822cctl report' for WiFiStatus and DriverStatus.\n";
            IOObjectRelease(service);
            return 2;
        }
    } else if (cmd == "cancel-connect") {
        kern_return_t kr = RTWClientSendCommand(
            kRTL8822CUserCommandCancelConnection, "", "", 0);
        if (kr == kIOReturnSuccess) {
            std::cout << "Pending connection cancellation requested.\n";
        } else {
            std::cerr << "Failed to cancel connection. Error: 0x" << std::hex
                      << kr << std::dec << "\n";
            IOObjectRelease(service);
            return 2;
        }
    } else if (cmd == "disconnect") {
        kern_return_t kr = RTWClientSendCommand(
            kRTL8822CUserCommandDisconnect, "", "", 0);

        if (kr == kIOReturnSuccess) {
            std::cout << "Disconnect command successfully sent to RealtekRTL8822C.\n";
        } else {
            std::cerr << "Failed to send Disconnect command. Error: 0x" << std::hex << kr << std::dec << "\n";
            IOObjectRelease(service);
            return 2;
        }
    } else if (cmd == "on" || cmd == "off") {
        const bool enabled = cmd == "on";
        kern_return_t kr = RTWClientSendCommand(
            kRTL8822CUserCommandSetInterfaceEnabled, "", "", enabled ? 1 : 0);
        if (kr == kIOReturnSuccess) {
            std::cout << "Wi-Fi interface " << (enabled ? "enabled" : "disabled")
                      << ".\n";
        } else {
            std::cerr << "Failed to change Wi-Fi interface state. Error: 0x"
                      << std::hex << kr << std::dec << "\n";
            IOObjectRelease(service);
            return 2;
        }
    }

    IOObjectRelease(service);
    return 0;
}
