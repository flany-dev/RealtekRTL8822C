// SPDX-License-Identifier: BSD-3-Clause
/*
 * Portions derived from Linux rtw88.
 * Copyright (c) 2018-2019 Realtek Corporation.
 * Copyright (c) 2026 RealtekRTL8822C contributors.
 */

#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/IOPM.h>

// IO80211Family - must be defined BEFORE including IO80211Controller.h
#ifndef __MAC_12_0
#define __MAC_12_0 120000
#endif
#ifndef __MAC_13_0
#define __MAC_13_0 130000
#endif
#ifndef __MAC_14_0
#define __MAC_14_0 140000
#endif
#ifndef __MAC_14_4
#define __MAC_14_4 140400
#endif

#ifndef __IO80211_TARGET
#define __IO80211_TARGET __MAC_13_0
#endif

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOBasicOutputQueue.h>
#include <IOKit/network/IONetworkMedium.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <sys/kpi_mbuf.h>
#include <libkern/crypto/rand.h>

#include "RtwWpaCrypto.hpp"
#include "RTL8822CUserClientShared.h"

#ifndef RTW_DEBUG
#define RTW_DEBUG 0
#endif
#ifndef RTW_VERSION
#define RTW_VERSION "0.0.0"
#endif

#if RTW_DEBUG
#define RTW_DEBUG_LOG(...) IOLog(__VA_ARGS__)
#define RTW_ERROR_LOG(...) IOLog(__VA_ARGS__)
#define RTW_DEBUG_PROPERTY(...) setProperty(__VA_ARGS__)
#define RTW_DEBUG_REMOVE_PROPERTY(...) removeProperty(__VA_ARGS__)
#else
#define RTW_DEBUG_LOG(...) do { } while (0)
#define RTW_ERROR_LOG(...) do { } while (0)
#define RTW_DEBUG_PROPERTY(...) do { } while (0)
#define RTW_DEBUG_REMOVE_PROPERTY(...) do { } while (0)
#endif

static UInt16 rtwReadBe16(const UInt8* p) {
    return (UInt16)(((UInt16)p[0] << 8) | p[1]);
}

static UInt64 rtwReadBe64(const UInt8* p) {
    UInt64 value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | p[i];
    return value;
}

static UInt64 rtwReadLe48(const UInt8* p) {
    UInt64 value = 0;
    for (int i = 5; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}

static void rtwWriteBe16(UInt8* p, UInt16 value) {
    p[0] = (UInt8)(value >> 8);
    p[1] = (UInt8)value;
}

static void rtwWriteBe64(UInt8* p, UInt64 value) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (UInt8)value;
        value >>= 8;
    }
}

static int rtwHexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

#define APPLE80211_MAX_SSID_LEN 32
#define APPLE80211_ADDR_LEN 6
#define APPLE80211_MAX_CC_LEN 3
#define APPLE80211_VERSION 1
#define APPLE80211_POWER_ON 1
#define APPLE80211_POWER_OFF 0
#define APPLE80211_S_INIT 0
#define APPLE80211_S_RUN 1
#define APPLE80211_AUTHTYPE_OPEN 1
#define APPLE80211_AUTHTYPE_NONE 0
#define APPLE80211_C_FLAG_2GHZ 0x00000002
#define APPLE80211_C_FLAG_5GHZ 0x00000004
#define APPLE80211_C_FLAG_20MHZ 0x00000010
#define APPLE80211_C_FLAG_ACTIVE 0x00000100

// Linux rtw88 pci_gen_new enables ROK for RTL8822C but deliberately omits
// RDU. Keeping RDU enabled on a level-triggered legacy pin can repeatedly
// assert the line while the bounded poll owns or replenishes the RX ring.
static const UInt32 kRtwHimr0 = 0x000004fd;
static const UInt32 kRtwHimr1 = 0x00000a00;
static const UInt32 kRtwHimr3 = 0x00010000;
static const UInt32 kRtwHotDiagnosticMask = RTW_DEBUG ? 0x000003ffU : 0x00001fffU;
// Diagnostic baseline: restore the last profile that sustained normal
// downlink throughput before the v54/v55 RX experiments. Stage timing below
// will determine which work must move out of the interrupt work loop.
static const UInt32 kRtwRxPollBudget = 8;
static const UInt32 kRtwRxPollDelayUs = 500;
// IONetworkController::allocatePacket() costs roughly 130 us on the target
// machine. Never perform that allocation in the serialized hardware RX poll.
// Keep a bounded reserve and refill it on an independent work loop instead.
static const UInt32 kRtwRxPacketPoolCapacity = 512;
static const UInt32 kRtwRxPacketPoolLowWatermark = 384;
static const UInt32 kRtwRxPacketPoolRefillBudget = 128;
static const UInt32 kRtwRxPacketBufferSize = 1514 + ETHER_ALIGN;
// RTL8822C is PCIe DMA-coherent on the x86 target and every mapping uses
// IODMACommand::kBypassed. Keep small producer/consumer rings uncached for
// conservative device-index visibility, but never force bulk packet storage
// through an uncached CPU mapping. v63 measured a 131-us average just to copy
// one <=1514-byte RX frame from the former kIOMapInhibitCache arena.
static const IOOptionBits kRtwDmaRingMemoryOptions =
    kIODirectionInOut | kIOMapInhibitCache |
    kIOMemoryPhysicallyContiguous;
static const IOOptionBits kRtwDmaPayloadMemoryOptions =
    kIODirectionInOut | kIOMapCopybackCache |
    kIOMemoryPhysicallyContiguous;
static const UInt64 kRtwRecentScanMaxAgeNs = 10000000000ULL;
struct apple80211_channel {
    UInt32 version;
    UInt32 channel;
    UInt32 flags;
};

struct apple80211_scan_result {
    UInt8 dummy[512];
};

// Firmware Array
#include "rtw8822c_fw.h"
#include "power_seq.h"

class RealtekRTL8822C;

class RealtekRTL8822CUserClient final : public IOUserClient {
    OSDeclareDefaultStructors(RealtekRTL8822CUserClient)

private:
    RealtekRTL8822C* owner;

    static IOReturn commandAction(OSObject* target, void* reference,
                                  IOExternalMethodArguments* arguments);

public:
    bool start(IOService* provider) override;
    void stop(IOService* provider) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments* arguments,
                            IOExternalMethodDispatch* dispatch = nullptr,
                            OSObject* target = nullptr,
                            void* reference = nullptr) override;
};

class RealtekRTL8822C : public IOEthernetController {
    OSDeclareDefaultStructors(RealtekRTL8822C)

    friend class RealtekRTL8822CUserClient;
    typedef IOEthernetController super;

private:
    IOPCIDevice* pciDevice;
    IOMemoryMap* bar2Map;
    IOMemoryDescriptor* bar2Desc;
    volatile UInt8* ioBase;

    IOEthernetInterface* netif;

    // DMA Buffers
    IOBufferMemoryDescriptor* bcnqDesc;
    IODMACommand* bcnqDmaCmd;

    // H2CQ DMA Buffers
    IOBufferMemoryDescriptor* h2cqDesc;
    IODMACommand* h2cqDmaCmd;
    addr64_t h2cqPhysAddr;

    IOBufferMemoryDescriptor* h2cPayloadDesc;
    IODMACommand* h2cPayloadDmaCmd;
    addr64_t h2cPayloadPhysAddr;

    UInt32 h2cqWp;

    // RX DMA Buffers
    IOBufferMemoryDescriptor* rxRingDesc;
    IODMACommand* rxRingDmaCmd;
    IOBufferMemoryDescriptor* rxBufferDesc;
    addr64_t rxRingPhysAddr;
    addr64_t rxBufferPhysAddr;
    // BEQ DMA
    IOBufferMemoryDescriptor* beqDesc;
    IODMACommand* beqDmaCmd;
    addr64_t beqPhysAddr;
    IOBufferMemoryDescriptor* beqPayloadDesc;
    IODMACommand* beqPayloadDmaCmd;
    addr64_t beqPayloadPhysAddr;
    UInt32 beqWp;
    UInt32 beqRp;
    UInt32 beqLastHwRp;
    UInt32 beqOutstanding;
    bool beqQueueStalled;
    // outputPacket() and the interrupt event source can run independently.
    // This lock protects the BEQ software indices and the descriptor-to-doorbell
    // transaction; it must never cover setProperty(), queue control, or sleeps.
    IOSimpleLock* beqLock;
    UInt32 debugBeqLastSlot;
    UInt32 debugBeqLastWifiLen;
    UInt32 debugBeqLastPsbLen;
    UInt32 debugBeqFaults;
    UInt32 debugBeqRecoveries;

    // BKQ DMA
    IOBufferMemoryDescriptor* bkqDesc;
    IODMACommand* bkqDmaCmd;
    addr64_t bkqPhysAddr;
    IOBufferMemoryDescriptor* bkqPayloadDesc;
    IODMACommand* bkqPayloadDmaCmd;
    addr64_t bkqPayloadPhysAddr;
    UInt32 bkqWp;

    // VIQ DMA
    IOBufferMemoryDescriptor* viqDesc;
    IODMACommand* viqDmaCmd;
    addr64_t viqPhysAddr;
    IOBufferMemoryDescriptor* viqPayloadDesc;
    IODMACommand* viqPayloadDmaCmd;
    addr64_t viqPayloadPhysAddr;
    UInt32 viqWp;

    // VOQ DMA
    IOBufferMemoryDescriptor* voqDesc;
    IODMACommand* voqDmaCmd;
    addr64_t voqPhysAddr;
    IOBufferMemoryDescriptor* voqPayloadDesc;
    IODMACommand* voqPayloadDmaCmd;
    addr64_t voqPayloadPhysAddr;
    UInt32 voqWp;

    // MGMTQ DMA
    IOBufferMemoryDescriptor* mgmtDesc;
    IODMACommand* mgmtDmaCmd;
    addr64_t mgmtPhysAddr;
    IOBufferMemoryDescriptor* mgmtPayloadDesc;
    IODMACommand* mgmtPayloadDmaCmd;
    addr64_t mgmtPayloadPhysAddr;
    UInt32 mgmtWp;
    IODMACommand* rxBufferDmaCmd;
    UInt8* rxBufferVirtAddr;
    UInt32 rxRp;
    UInt8 powerState;
    bool interfaceEnabled;
    bool interfaceUserDisabled;
    bool hardwareSuspended;
    bool hardwareReady;
    bool interfaceEnabledBeforeSleep;
    UInt32 lifecycleSuspendCount;
    UInt32 lifecycleResumeCount;
    UInt32 lifecycleResumeFailures;
    UInt32 lifecycleCamClearCount;

    // Interrupts
    IOWorkLoop* workLoop;
    IOInterruptEventSource* interruptSource;
    void handleInterrupt(OSObject* owner, IOInterruptEventSource* sender, int count);
    void rxPollTimerFired(OSObject* owner, IOTimerEventSource* sender);
    void restoreHardwareInterruptMasks();

    // Firmware Loader Helpers
    bool downloadFirmware();
    bool sendFirmwarePacket(UInt16 page, const UInt8* data, UInt32 size);
    bool macPreSystemCfg();
    bool macInitSystemCfg();
    bool parsePowerSeq(const struct rtw_pwr_seq_cmd *cmd_seq);
    bool reprogramDmaRings();
    bool resumeHardware();
    bool suspendHardware();
    bool rollbackFailedResume(const char* stage);
    bool failStart(IOService* provider, const char* status, const char* event);
    bool prepareDmaMapping(IOBufferMemoryDescriptor* descriptor,
                           IODMACommand* command, addr64_t* address);
    void maskAndAckInterrupts();
    void enableHardwareInterrupts();
    bool clearTxdmaLifecycleStatus(const char* reason);
    void resetFirmwareH2cState(const char* reason);
    void publishLifecycleInvariant(const char* event, bool powerSequenceOk);
    bool testBcnqRoundtrip();
    IOReturn setPropertiesGated(OSObject* properties);
    static IOReturn setPropertiesAction(void* target, void* properties,
                                        void*, void*, void*);
    static IOReturn powerStateAction(void* target, void* ordinal,
                                     void*, void*, void*);
    IOReturn setInterfaceEnabledGated(bool enabled);
    IOReturn setUserInterfaceEnabledGated(bool enabled);
    IOReturn executeUserClientCommand(const RTL8822CUserClientCommand& command);
    void reconcileInterfaceStateFromBsd();
    void activateNetworkDataPath(const char* reason);

    struct RTL8822CScanResult {
        char ssid[33];
        UInt8 bssid[6];
        int channel;
        int rssi;
        bool is_secure;
        bool supports_cck;
        bool supports_ht;
        bool supports_vht;
        bool supports_wmm;
        UInt8 security_mode;
        UInt8 rsn_ie_len;
        UInt8 rsn_ie[64];
        UInt16 ht_cap_info;
        UInt8 ht_ampdu_params;
        UInt8 ht_mcs0;
        UInt8 ht_mcs1;
        UInt8 ht_primary_channel;
        UInt8 ht_operation_info;
        UInt32 vht_cap_info;
        UInt16 vht_rx_mcs_map;
        UInt16 vht_rx_highest;
        UInt16 vht_tx_mcs_map;
        UInt16 vht_tx_highest;
        UInt8 vht_channel_width;
        UInt8 vht_center_segment0;
        UInt8 vht_center_segment1;
        uint64_t last_seen_time;
    };

    RTL8822CScanResult customScanResults[32];
    int customScanResultsCount;
    uint64_t lastScanCompletedTime;
    int isrCallsCount;
    int debugAuthRxCount;
    int debugBedokCount;
    int debugTxerrCount;
    int debugTxfovwCount;
    int debugC2hRxCount;
    int txSeqNum;
    UInt16 dataSeqNum;
    int debugMgntdokCount;
    int debugBcndmaintCount;
    int debugOutputCalls;
    int debugOutputSuccess;
    int debugOutputDropped;
    int debugOutputStalled;
    int debugDataRxFrames;
    int debugDataRxDelivered;
    int debugDataRxRejected;
    char debugOutputLastDrop[64];
    UInt32 mgmtRp;
    int connStateCounter;
    uint64_t connStateStartTime;
    bool targetSupportsCck;
    bool targetSupportsHt;
    bool targetSupportsVht;
    bool targetSupportsWmm;
    UInt16 targetHtCapInfo;
    UInt8 targetHtAmpduParams;
    UInt8 targetHtMcs0;
    UInt8 targetHtMcs1;
    UInt8 targetHtPrimaryChannel;
    UInt8 targetHtOperationInfo;
    UInt32 targetVhtCapInfo;
    UInt16 targetVhtRxMcsMap;
    UInt16 targetVhtRxHighest;
    UInt16 targetVhtTxMcsMap;
    UInt16 targetVhtTxHighest;
    UInt8 targetVhtChannelWidth;
    UInt8 targetVhtCenterSegment0;
    UInt8 targetVhtCenterSegment1;
    bool htNegotiated;
    bool vhtNegotiated;
    bool fiveGTwoStreamPowerValid;
    UInt32 negotiatedRaMask;
    UInt8 negotiatedRateId;
    bool baEstablished;
    UInt8 baDialogToken;
    UInt8 baTid;
    UInt16 baBufferSize;
    UInt16 baTimeout;
    UInt32 baRequests;
    UInt32 baResponses;
    UInt32 authAttempts;
    UInt32 assocAttempts;
    UInt32 baAttempts;
    uint64_t baStateStartTime;
    static const UInt8 RX_BA_TID_COUNT = 16;
    static const UInt8 RX_REORDER_MAX_WINDOW = 64;
    struct RxReorderEntry {
        UInt8* frame;
        UInt32 len;
        UInt16 seq;
        UInt8 encType;
        bool decrypted;
        uint64_t queuedAt;
    };
    struct RxBaSession {
        bool active;
        UInt8 dialogToken;
        UInt16 headSeq;
        UInt16 windowSize;
        UInt16 timeoutTu;
        UInt32 buffered;
        RxReorderEntry entry[RX_REORDER_MAX_WINDOW];
    };
    RxBaSession rxBa[RX_BA_TID_COUNT];
    UInt32 rxBaAccepted;
    UInt32 rxBaStopped;
    UInt32 rxBaRetries;
    UInt32 rxReorderBuffered;
    UInt32 rxReorderReleased;
    UInt32 rxReorderDuplicates;
    UInt32 rxReorderHoles;
    UInt32 rxAmsduDelivered;
    uint64_t lastPeerRxTime;
    int connectedSignalDbm;
    int publishedSignalDbm;
    uint64_t lastSignalPublishTime;

    enum RTL8822CSecurityMode {
        SECURITY_OPEN = 0,
        SECURITY_WPA = 1,
        SECURITY_WPA2 = 2
    };
    RTL8822CSecurityMode targetSecurityMode;
    bool portAuthorized;
    bool useShortSlot;
    UInt8 targetRsnIe[64];
    UInt8 targetRsnIeLen;
    UInt8 assocRsnIe[64];
    UInt8 assocRsnIeLen;
    enum RtwWpaState {
        WPA_STATE_DISABLED,
        WPA_STATE_WAIT_M1,
        WPA_STATE_WAIT_M3,
        WPA_STATE_COMPLETED
    };
    RtwWpaState wpaState;
    UInt8 wpaPmk[32];
    UInt8 wpaPtk[64];
    UInt8 wpaInstalledPtk[64];
    UInt8 wpaAnonce[32];
    UInt8 wpaSnonce[32];
    UInt8 wpaGtk[16];
    UInt8 wpaGtkKeyId;
    UInt8 wpaEapolVersion;
    bool wpaPmkValid;
    bool wpaCryptoReady;
    bool wpaPtkInstalled;
    bool wpaNewPtkPending;
    bool wpaGtkInstalled;
    UInt64 wpaM1Replay;
    UInt64 wpaLastReplay;
    bool wpaReplayValid;
    UInt64 wpaTxPn;
    UInt64 wpaPairwiseRxPn[16];
    UInt64 wpaGroupRxPn[4][16];
    UInt8 wpaLastTx[384];
    UInt32 wpaLastTxLen;
    bool wpaLastTxProtected;
    UInt32 wpaRetries;
    UInt32 wpaRxM1;
    UInt32 wpaRxM3;
    UInt32 wpaRxGroupM1;
    UInt32 wpaMicFailures;
    UInt32 wpaReplayDrops;
    UInt32 wpaDecryptFailures;
    uint64_t wpaStateStartTime;
    bool channelCalibrationValid;
    int calibratedChannel;
    UInt8 calibratedBandwidth;

    enum RTL8822CConnState {
        CONN_STATE_DISCONNECTED,
        CONN_STATE_SCANNING,
        CONN_STATE_CONNECTING_AUTH,
        CONN_STATE_CONNECTING_ASSOC,
        CONN_STATE_CONNECTED
    };

    RTL8822CConnState connState;
    char targetSsid[33];
    UInt8 targetBssid[6];
    int targetChannel;
    int targetCenterChannel;
    UInt8 targetBandwidth;
    UInt8 targetPrimaryChannelIndex;

    // A compact boot-to-association trace for post-reboot debugging via rtl8822cctl.
    static const int DEBUG_TRACE_CAPACITY = 24;
    static const int DEBUG_TRACE_ENTRY_SIZE = 96;
    char debugTrace[DEBUG_TRACE_CAPACITY][DEBUG_TRACE_ENTRY_SIZE];
    UInt32 debugTraceCount;
    UInt32 debugTraceNext;
    UInt32 debugTraceSequence;
    UInt32 debugMgmtOfdmTxBaseline;
    UInt32 debugMgmtSlot;
    UInt8 debugMgmtReportSn;
    UInt8 debugMgmtShadow[78];
    bool debugMgmtShadowValid;
    UInt8 debugDataReportSn;
    UInt16 debugDataReportEtherType;
    UInt16 debugDataReportSeq;
    bool debugDataReportPending;
    static const bool kSkipTargetIqkDiagnostic = false;
    void traceEvent(const char* event);
    void publishDebugSnapshot();
    void publishConnectedScanState(const char* state);
    const char* connectionStateName() const;
    void connectionWatchdog(OSObject* owner, IOTimerEventSource* sender);
    void runConnectionWatchdog();
    void publishSignalStrength();
    bool setupRxPacketPool();
    void teardownRxPacketPool();
    void refillRxPacketPool(UInt32 budget);
    mbuf_t takeRxPacket();
    void rxPacketPoolTimerFired(OSObject* owner, IOTimerEventSource* sender);

    bool transmitWifiFrame(const UInt8* frame, UInt32 len, UInt8 qsel);
    bool performWifiScan();
    bool startConnectedScan();
    void connectedScanTimerFired(OSObject* owner, IOTimerEventSource* sender);
    void advanceConnectedScan(IOTimerEventSource* sender);
    void finishConnectedScan(bool cancelled, const char* reason, bool restoreHome);
    bool restoreConnectedScanHome();
    void publishScanResults();
    void pruneScanResults(uint64_t maxAgeNs);
    void processRxPacketsForScan();
    void parseBeaconOrProbeResponse(const UInt8* frame, UInt32 len, int signalDbm);
    int extractRxSignalDbm(const UInt8* phyStatus, UInt32 phyStatusLen);
    void handleMgmtFrame(const UInt8* frame, UInt32 len);
    void handleControlFrame(const UInt8* frame, UInt32 len);
    void handleRxDataFrame(const UInt8* frame, UInt32 len, UInt8 encType, bool decrypted);
    bool deliverWifiDataFrame(const UInt8* frame, UInt32 len, UInt8 encType, bool decrypted);
    bool deliverPlainWifiDataFrame(const UInt8* frame, UInt32 len,
                                   UInt32 payloadSkip = 0, UInt32 tailTrim = 0);
    bool deliverEthernetFrame(const UInt8* frame, UInt32 len);
    bool deliverEthernetPayload(const UInt8* dst, const UInt8* src,
                                UInt16 ethType, const UInt8* payload,
                                UInt32 payloadLen);
    bool startRxBaSession(UInt8 tid, UInt8 dialogToken, UInt16 startSeq,
                          UInt16 windowSize, UInt16 timeoutTu);
    void stopRxBaSession(UInt8 tid, bool flush);
    void resetRxBaSessions(bool flush);
    void processRxReorderTimeouts();
    void publishRxBaState();
    void advanceRxReorder(UInt8 tid, UInt16 newHead);
    void releaseRxReorder(UInt8 tid);
    void configureWmmEdca(const UInt8* ies, UInt32 len);
    void initSecurityEngine();
    void clearSecurityCam(UInt8 slot);
    void clearAllSecurityCam(const char* reason);
    bool writeSecurityCam(UInt8 slot, UInt8 keyIndex, UInt8 keyType,
                          bool group, const UInt8* address, const UInt8* key);
    bool parseAndSelectRsn();
    bool prepareWpaPmk(const char* password);
    void resetWpaState(bool clearPmk);
    void publishWpaState(const char* event);
    bool handleEapolKey(const UInt8* eapol, UInt32 len);
    bool handleWpaMessage1(const UInt8* eapol, UInt32 len, UInt16 keyInfo, UInt64 replay);
    bool handleWpaMessage3(const UInt8* eapol, UInt32 len, UInt16 keyInfo, UInt64 replay);
    bool handleWpaGroupMessage1(const UInt8* eapol, UInt32 len, UInt16 keyInfo, UInt64 replay);
    bool sendWpaMessage2(UInt64 replay);
    bool sendWpaMessage4(UInt64 replay);
    bool sendWpaGroupMessage2(UInt64 replay);
    bool sendEapolKeyFrame(const UInt8* eapol, UInt32 len, bool protect, bool remember);
    enum DataEnqueueResult {
        DATA_ENQUEUE_SUCCESS = 0,
        DATA_ENQUEUE_RING_FULL,
        DATA_ENQUEUE_INVALID_FRAME,
        DATA_ENQUEUE_NOT_READY
    };
    DataEnqueueResult enqueueEthernetData(const UInt8* ethFrame, UInt32 ethLen,
                                          bool protect, bool allowAggregation,
                                          bool requestReport);
    bool installPairwiseKey();
    bool installGroupKey(UInt8 keyId, const UInt8* key, UInt64 rsc);
    bool validateCcmpView(const UInt8* frame, UInt32 len, UInt8 encType,
                          bool decrypted, UInt32* payloadSkip, UInt32* tailTrim);
    bool sendAuthRequest();
    bool sendAssocRequest();
    bool sendAddBaRequest();
    bool sendAddBaResponse(UInt8 token, UInt8 tid, UInt16 status,
                           UInt16 bufferSize, UInt16 timeoutTu, bool retry);
    bool sendDelBa();
    bool waitForMgmtQueueEmpty(UInt32 timeoutUs);
    void resetMgmtQueue(const char* reason);
    void resetBaState();
    void disconnectFromNetwork(const char* reason, bool sendDeauth);
    void updateBeqCompletion();
    void updateBeqCompletionLocked();
    void serviceBeqCompletion(bool fromRxPoll);
    void captureBeqPayloadOverflow(const char* reason);
    bool recoverBeqPayloadOverflow();
    bool sendMediaStatus(UInt8 mac_id, bool connect);
    bool sendRaInfo(UInt8 mac_id, UInt32 rate_mask, UInt8 rate_id);

    // EFUSE and MAC Helpers
    bool translate8023to80211(const UInt8* eth_frame, UInt32 eth_len, UInt8* wifi_frame, UInt32* wifi_len);
    UInt32 dropOutputPacket(mbuf_t m, const char* reason);
    UInt8 macAddress[6];
    UInt8 physEfuseMap[512];
    UInt8 logicalEfuseMap[768];
    UInt8 rfeOption;
    UInt32 cutVersion;
    UInt16 h2cSeq;
    UInt8 lastBoxNum;
    bool readEfuse();
    bool initMac();
    bool initPhy();
    void runDackCalibration();
    void dackBackupResults();
    void dackSample(UInt32* iv, UInt32* qv, bool* complete);
    void dackSearch(UInt32* iv, UInt32* qv, UInt32* iValue, UInt32* qValue);
    void dackRfMode(UInt32* iValue, UInt32* qValue, bool* complete);
    bool dackAdc(UInt8 path, UInt32* adcI, UInt32* adcQ, UInt32* attempts);
    bool dackStep1(UInt8 path);
    void dackStep2(UInt8 path, UInt32* iOut, UInt32* qOut, bool* samplesComplete);
    bool dackStep3(UInt8 path, UInt32 adcI, UInt32 adcQ, UInt32* iIn, UInt32* qIn,
                   UInt32* iOut, UInt32* qOut, bool* samplesComplete);
    void dackStep4(UInt8 path);
    bool dackWait(UInt32 offset, UInt32 mask, UInt32 target);
    void runRfX2Check();
    void runTxGapkCalibration();
    bool txGapkRfkHandshake(bool start, bool* btIdle, UInt32* polls);

    bool sendH2CPacket(const UInt8* packet, UInt32 size);
    bool sendH2CCommand(const UInt8* h2c);
    bool runIqk();
    bool checkLteCoexReady();
    UInt32 readLteCoexReg(UInt16 offset);
    bool writeLteCoexReg(UInt16 offset, UInt32 mask, UInt32 value);
    void initCoexWifiOnly();
    bool sendGeneralInfo();
    bool sendPhyDmInfo();
    void loadTable(const UInt32* data, UInt32 size, UInt8 type, UInt8 rf_path);
    void loadTable3(const UInt32* data, UInt32 size); // 3-element format: addr, mask, val
    bool checkPositive(UInt32 cond_w0, UInt32 cond2_w1);

    // RF Helpers
    void writeRfMask(UInt8 path, UInt32 addr, UInt32 mask, UInt32 data);
    UInt32 readRfMask(UInt8 path, UInt32 addr, UInt32 mask);

    void headerFileInit(bool pre);
    void configTrxMode(UInt8 tx_path, UInt8 rx_path, bool is_tx2_path);

    // HAL Wrappers
    inline UInt8 read8(UInt32 offset) { return *(volatile UInt8*)(ioBase + offset); }
    inline UInt16 read16(UInt32 offset) { return OSReadLittleInt16(ioBase, offset); }
    inline UInt32 read32(UInt32 offset) { return OSReadLittleInt32(ioBase, offset); }

    inline void write8(UInt32 offset, UInt8 val) { *(volatile UInt8*)(ioBase + offset) = val; }
    inline void write16(UInt32 offset, UInt16 val) { OSWriteLittleInt16(ioBase, offset, val); }
    inline void write32(UInt32 offset, UInt32 val) { OSWriteLittleInt32(ioBase, offset, val); }

    inline UInt32 read32Mask(UInt32 offset, UInt32 mask) {
        UInt32 val = read32(offset);
        int shift = 0;
        if (mask == 0) return 0;
        while (shift < 32 && !((mask >> shift) & 1)) {
            shift++;
        }
        return (val & mask) >> shift;
    }

    inline void write32Mask(UInt32 offset, UInt32 mask, UInt32 val) {
        UInt32 old = read32(offset);
        int shift = 0;
        if (mask == 0) return;
        while (shift < 32 && !((mask >> shift) & 1)) {
            shift++;
        }
        write32(offset, (old & ~mask) | ((val << shift) & mask));
    }

    inline void write32_set(UInt32 offset, UInt32 set) { write32(offset, read32(offset) | set); }
    inline void write32_clr(UInt32 offset, UInt32 clr) { write32(offset, read32(offset) & ~clr); }

    UInt32 dackAdck[2];
    UInt16 dackMsbk[2][2][15];
    UInt8 dackDck[2][2][2];

    // ===================== WiFi State Machine =====================
    // Current WiFi state (apple80211_state)
    UInt32 wifiState;  // APPLE80211_S_INIT, SCAN, AUTH, ASSOC, RUN

    // Current channel
    apple80211_channel currentChannel;

    // Current SSID (for association)
    UInt8 currentSsid[APPLE80211_MAX_SSID_LEN];
    UInt32 currentSsidLen;

    // Current BSSID
    UInt8 currentBssid[APPLE80211_ADDR_LEN];

    // Auth type
    UInt32 authTypeLower;
    UInt32 authTypeUpper;

    // TX power (dBm)
    int32_t txPowerDbm;

    // Country code
    UInt8 countryCode[APPLE80211_MAX_CC_LEN];

    // Scan results ring buffer
    static const int MAX_SCAN_RESULTS = 32;
    apple80211_scan_result scanResults[MAX_SCAN_RESULTS];
    int scanResultCount;
    bool scanInProgress;



    UInt32 scanChannel;
    UInt32 scanChannelFlags;
    IOTimerEventSource* scanTimer;
    IOTimerEventSource* offchannelScanTimer;
    IOTimerEventSource* rxPollTimer;
    IOWorkLoop* rxPacketPoolWorkLoop;
    IOTimerEventSource* rxPacketPoolTimer;
    IOSimpleLock* rxPacketPoolLock;
    mbuf_t rxPacketPool[kRtwRxPacketPoolCapacity];
    UInt32 rxPacketPoolHead;
    UInt32 rxPacketPoolTail;
    UInt32 rxPacketPoolCount;
    UInt32 rxPacketPoolMinimum;
    bool rxPacketPoolStopping;
    bool rxPacketPoolRefillPending;
    UInt32 debugRxPacketPoolHits;
    UInt32 debugRxPacketPoolMisses;
    UInt32 debugRxPacketPoolRefilled;
    UInt32 debugRxPacketPoolAllocFailures;
    UInt32 debugRxPacketPoolBatchCalls;
    bool rxPollActive;
    UInt32 debugRxPollCalls;
    UInt32 debugRxPollPackets;
    UInt32 debugRxPollBudgetHits;
    UInt32 debugRxPollMaxBatch;
    UInt32 debugRxRokInterrupts;
    UInt32 debugRxRduInterrupts;
    UInt32 debugRxTimerPolls;
    UInt32 debugRxTxServiceCalls;
    UInt32 debugRxTxQueueRestarts;
    UInt64 debugRxTxServiceTotalNs;
    UInt64 debugRxTxServiceMaxNs;
    UInt64 debugRxPollTotalNs;
    UInt64 debugRxPollMaxNs;
    UInt64 debugRxSyncTotalNs;
    UInt64 debugRxSyncMaxNs;
    UInt32 debugRxSyncCalls;
    UInt64 debugRxInputTotalNs;
    UInt64 debugRxInputMaxNs;
    UInt32 debugRxInputCalls;
    UInt64 debugRxCcmpTotalNs;
    UInt64 debugRxCcmpMaxNs;
    UInt32 debugRxCcmpCalls;
    UInt64 debugRxBuildTotalNs;
    UInt64 debugRxBuildMaxNs;
    UInt32 debugRxBuildCalls;
    UInt64 debugRxPoolTakeTotalNs;
    UInt64 debugRxPoolTakeMaxNs;
    UInt64 debugRxCopyTotalNs;
    UInt64 debugRxCopyMaxNs;
    UInt64 debugRxFinalizeTotalNs;
    UInt64 debugRxFinalizeMaxNs;
    UInt32 debugRxRateCounts[84];
    UInt32 debugRxBwCounts[3];
    UInt32 debugRxDescBwCounts[3];
    UInt32 debugRxScCounts[16];
    UInt8 debugRxPpduBw[4];
    bool debugRxPpduBwValid[4];
    UInt8 debugRxLastRate;
    UInt8 debugRxLastBw;
    UInt32 connectedScanIndex;
    UInt32 connectedScanOriginalRcr;
    UInt32 connectedScanHomePrimary;
    UInt32 connectedScanHomeCenter;
    UInt8 connectedScanHomeBandwidth;
    UInt8 connectedScanHomePrimaryIndex;
    UInt8 connectedScanPhase;
    UInt8 connectedScanDrainRetries;

    // Channel switching
    bool setChannelHw(UInt32 channel, UInt32 flags, UInt8 bandwidth = 0,
                      UInt8 primaryChannelIndex = 0,
                      bool scanSwitch = false);
    UInt32 programChannelRf(UInt32 rfReg18A, UInt32 rfReg18B, UInt32 rfRxbb);
    bool configure5GTxPower(UInt32 primaryChannel, UInt32 centerChannel,
                            UInt8 bandwidth, bool publishDiagnostics = true);

public:
    virtual bool init(OSDictionary* dictionary = 0) override;
    virtual void free(void) override;
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService* provider) override;
    virtual IOReturn setProperties(OSObject* properties) override;
    virtual IOReturn newUserClient(task_t owningTask, void* securityID,
                                   UInt32 type, OSDictionary* properties,
                                   IOUserClient** handler) override;
    virtual void stop(IOService* provider) override;



    // IO80211Controller pure virtuals







    // IOEthernetController virtuals
    virtual IOReturn enable(IONetworkInterface* netif) override;
    virtual IOReturn disable(IONetworkInterface* netif) override;

    // Power Management
    virtual IOReturn registerWithPolicyMaker(IOService* policyMaker) override;
    virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService* whatDevice) override;
    virtual IOReturn getHardwareAddress(IOEthernetAddress* addr) override;
    virtual IOReturn setHardwareAddress(const IOEthernetAddress* addr) override;
    virtual UInt32 outputPacket(mbuf_t m, void* param) override;
    virtual IOOutputQueue* createOutputQueue() override;
    virtual bool configureInterface(IONetworkInterface* iface) override;
    virtual IOReturn selectMedium(const IONetworkMedium* medium) override;
    virtual IOReturn getPacketFilters(UInt32* filters) const override;
    virtual IOReturn setMulticastMode(bool active) override;
    virtual IOReturn setMulticastList(IOEthernetAddress* addrs,
                                      UInt32 count) override;

    // Fix IOServiceOpen


    // Additional IO80211Controller/IONetworkController overrides
    virtual const OSString * newVendorString() const override;
    virtual const OSString * newModelString() const override;



};

OSDefineMetaClassAndStructors(RealtekRTL8822C, IOEthernetController)
OSDefineMetaClassAndStructors(RealtekRTL8822CUserClient, IOUserClient)

bool RealtekRTL8822CUserClient::start(IOService* provider) {
    owner = OSDynamicCast(RealtekRTL8822C, provider);
    if (!owner || !IOUserClient::start(provider)) {
        owner = nullptr;
        return false;
    }
    return true;
}

void RealtekRTL8822CUserClient::stop(IOService* provider) {
    owner = nullptr;
    IOUserClient::stop(provider);
}

IOReturn RealtekRTL8822CUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn RealtekRTL8822CUserClient::commandAction(
    OSObject* target, void*, IOExternalMethodArguments* arguments) {
    RealtekRTL8822CUserClient* client =
        OSDynamicCast(RealtekRTL8822CUserClient, target);
    if (!client || !client->owner || !arguments ||
        !arguments->structureInput ||
        arguments->structureInputSize != sizeof(RTL8822CUserClientCommand)) {
        return kIOReturnBadArgument;
    }

    const RTL8822CUserClientCommand* command =
        static_cast<const RTL8822CUserClientCommand*>(arguments->structureInput);
    return client->owner->executeUserClientCommand(*command);
}

IOReturn RealtekRTL8822CUserClient::externalMethod(
    uint32_t selector, IOExternalMethodArguments* arguments,
    IOExternalMethodDispatch* dispatch, OSObject* target, void* reference) {
    (void)dispatch;
    static const IOExternalMethodDispatch methods[kRTL8822CUserClientSelectorCount] = {
        { &RealtekRTL8822CUserClient::commandAction, 0,
          sizeof(RTL8822CUserClientCommand), 0, 0 }
    };

    if (selector >= kRTL8822CUserClientSelectorCount) {
        return kIOReturnUnsupported;
    }
    return IOUserClient::externalMethod(
        selector, arguments,
        const_cast<IOExternalMethodDispatch*>(&methods[selector]),
        target ? target : this, reference);
}

IOReturn RealtekRTL8822C::newUserClient(task_t owningTask, void* securityID,
                                       UInt32 type, OSDictionary* properties,
                                       IOUserClient** handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    if (type != RTL8822C_USER_CLIENT_TYPE) return kIOReturnUnsupported;
    if (IOUserClient::clientHasPrivilege(
            securityID, kIOClientPrivilegeLocalUser) != kIOReturnSuccess) {
        return kIOReturnNotPrivileged;
    }

    RealtekRTL8822CUserClient* client =
        OSTypeAlloc(RealtekRTL8822CUserClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type, properties) ||
        !client->attach(this) || !client->start(this)) {
        if (client->getProvider()) client->detach(this);
        client->release();
        return kIOReturnError;
    }

    *handler = client;
    return kIOReturnSuccess;
}

IOReturn RealtekRTL8822C::executeUserClientCommand(
    const RTL8822CUserClientCommand& command) {
    if (command.version != RTL8822C_USER_CLIENT_PROTOCOL_VERSION ||
        command.reserved != 0 || command.enabled > 1 ||
        !memchr(command.ssid, '\0', sizeof(command.ssid)) ||
        !memchr(command.password, '\0', sizeof(command.password))) {
        return kIOReturnBadArgument;
    }

    bool emptySsid = command.ssid[0] == '\0';
    bool emptyPassword = command.password[0] == '\0';
    switch (command.command) {
        case kRTL8822CUserCommandConnect:
            if (emptySsid || command.enabled != 0) return kIOReturnBadArgument;
            break;
        case kRTL8822CUserCommandSetInterfaceEnabled:
            if (!emptySsid || !emptyPassword) return kIOReturnBadArgument;
            break;
        case kRTL8822CUserCommandUpdateStatus:
        case kRTL8822CUserCommandScan:
        case kRTL8822CUserCommandDisconnect:
            if (!emptySsid || !emptyPassword || command.enabled != 0)
                return kIOReturnBadArgument;
            break;
        default:
            return kIOReturnUnsupported;
    }

    const char* name = nullptr;
    switch (command.command) {
        case kRTL8822CUserCommandUpdateStatus: name = "UpdateStatus"; break;
        case kRTL8822CUserCommandScan: name = "Scan"; break;
        case kRTL8822CUserCommandConnect: name = "Connect"; break;
        case kRTL8822CUserCommandDisconnect: name = "Disconnect"; break;
        case kRTL8822CUserCommandSetInterfaceEnabled:
            name = "SetInterfaceEnabled";
            break;
        default:
            return kIOReturnUnsupported;
    }

    OSDictionary* dictionary = OSDictionary::withCapacity(4);
    OSString* commandName = OSString::withCString(name);
    if (!dictionary || !commandName) {
        OSSafeReleaseNULL(dictionary);
        OSSafeReleaseNULL(commandName);
        return kIOReturnNoMemory;
    }
    dictionary->setObject("Command", commandName);
    commandName->release();

    IOReturn result = kIOReturnNoMemory;
    if (command.command == kRTL8822CUserCommandConnect) {
        OSString* ssid = OSString::withCString(command.ssid);
        OSString* password = OSString::withCString(command.password);
        if (ssid && password) {
            dictionary->setObject("SSID", ssid);
            dictionary->setObject("Password", password);
            result = setProperties(dictionary);
        }
        OSSafeReleaseNULL(ssid);
        OSSafeReleaseNULL(password);
    } else {
        if (command.command == kRTL8822CUserCommandSetInterfaceEnabled) {
            dictionary->setObject("Enabled", command.enabled ? kOSBooleanTrue :
                                                             kOSBooleanFalse);
        }
        result = setProperties(dictionary);
    }

    dictionary->release();
    return result;
}

bool RealtekRTL8822C::init(OSDictionary* dictionary) {
    if (!super::init(dictionary)) { return false; }
    bcnqDesc = nullptr;
    bcnqDmaCmd = nullptr;
    h2cqDesc = nullptr;
    h2cqDmaCmd = nullptr;
    h2cPayloadDesc = nullptr;
    h2cPayloadDmaCmd = nullptr;
    h2cqWp = 0;

    rxRingDesc = nullptr;
    rxRingDmaCmd = nullptr;
    rxBufferDesc = nullptr;
    rxRingPhysAddr = 0;
    rxBufferPhysAddr = 0;
    interfaceEnabled = true;
    interfaceUserDisabled = false;
    hardwareSuspended = false;
    hardwareReady = false;
    interfaceEnabledBeforeSleep = true;
    lifecycleSuspendCount = 0;
    lifecycleResumeCount = 0;
    lifecycleResumeFailures = 0;
    lifecycleCamClearCount = 0;
    beqDesc = nullptr; beqDmaCmd = nullptr; beqPayloadDesc = nullptr; beqPayloadDmaCmd = nullptr;
    beqWp = 0; beqRp = 0; beqLastHwRp = 0; beqOutstanding = 0; beqQueueStalled = false;
    beqLock = IOSimpleLockAlloc();
    if (!beqLock) return false;
    rxPacketPoolLock = IOSimpleLockAlloc();
    if (!rxPacketPoolLock) {
        IOSimpleLockFree(beqLock);
        beqLock = nullptr;
        return false;
    }
    memset(rxPacketPool, 0, sizeof(rxPacketPool));
    rxPacketPoolWorkLoop = nullptr;
    rxPacketPoolTimer = nullptr;
    rxPacketPoolHead = 0;
    rxPacketPoolTail = 0;
    rxPacketPoolCount = 0;
    rxPacketPoolMinimum = kRtwRxPacketPoolCapacity;
    rxPacketPoolStopping = true;
    rxPacketPoolRefillPending = false;
    debugRxPacketPoolHits = 0;
    debugRxPacketPoolMisses = 0;
    debugRxPacketPoolRefilled = 0;
    debugRxPacketPoolAllocFailures = 0;
    debugRxPacketPoolBatchCalls = 0;
    debugBeqLastSlot = 0; debugBeqLastWifiLen = 0; debugBeqLastPsbLen = 0;
    debugBeqFaults = 0; debugBeqRecoveries = 0;
    customScanResultsCount = 0;
    lastScanCompletedTime = 0;
    connState = CONN_STATE_DISCONNECTED;
    memset(targetSsid, 0, sizeof(targetSsid));
    memset(targetBssid, 0, sizeof(targetBssid));
    targetChannel = 1;
    targetCenterChannel = 1;
    targetBandwidth = 0;
    targetPrimaryChannelIndex = 0;
    memset(debugTrace, 0, sizeof(debugTrace));
    debugTraceCount = 0;
    debugTraceNext = 0;
    debugTraceSequence = 0;
    debugMgmtOfdmTxBaseline = 0;
    debugMgmtSlot = 0;
    debugMgmtReportSn = 0;
    memset(debugMgmtShadow, 0, sizeof(debugMgmtShadow));
    debugMgmtShadowValid = false;
    debugDataReportSn = 0;
    debugDataReportEtherType = 0;
    debugDataReportSeq = 0;
    debugDataReportPending = false;
    isrCallsCount = 0;
    debugAuthRxCount = 0;
    debugBedokCount = 0;
    debugTxerrCount = 0;
    debugTxfovwCount = 0;
    debugC2hRxCount = 0;
    txSeqNum = 0;
    dataSeqNum = 0;
    debugMgntdokCount = 0;
    debugBcndmaintCount = 0;
    debugOutputCalls = 0;
    debugOutputSuccess = 0;
    debugOutputDropped = 0;
    debugOutputStalled = 0;
    debugDataRxFrames = 0;
    debugDataRxDelivered = 0;
    debugDataRxRejected = 0;
    strlcpy(debugOutputLastDrop, "none", sizeof(debugOutputLastDrop));
    mgmtRp = 0;
    connStateCounter = 0;
    connStateStartTime = 0;
    targetSupportsCck = true;
    targetSupportsHt = false;
    targetSupportsVht = false;
    targetSupportsWmm = false;
    targetHtCapInfo = 0;
    targetHtAmpduParams = 0;
    targetHtMcs0 = targetHtMcs1 = 0;
    targetHtPrimaryChannel = targetHtOperationInfo = 0;
    targetVhtCapInfo = 0;
    targetVhtRxMcsMap = targetVhtTxMcsMap = 0xffff;
    targetVhtRxHighest = targetVhtTxHighest = 0;
    targetVhtChannelWidth = 0;
    targetVhtCenterSegment0 = targetVhtCenterSegment1 = 0;
    htNegotiated = false;
    vhtNegotiated = false;
    fiveGTwoStreamPowerValid = false;
    negotiatedRaMask = 0x00000FFF;
    negotiatedRateId = 6;
    baEstablished = false;
    baDialogToken = 0;
    baTid = 0;
    baBufferSize = 0;
    baTimeout = 0;
    baRequests = 0;
    baResponses = 0;
    authAttempts = 0;
    assocAttempts = 0;
    baAttempts = 0;
    baStateStartTime = 0;
    memset(rxBa, 0, sizeof(rxBa));
    rxBaAccepted = 0;
    rxBaStopped = 0;
    rxBaRetries = 0;
    rxReorderBuffered = 0;
    rxReorderReleased = 0;
    rxReorderDuplicates = 0;
    rxReorderHoles = 0;
    rxAmsduDelivered = 0;
    lastPeerRxTime = 0;
    connectedSignalDbm = -127;
    publishedSignalDbm = -127;
    lastSignalPublishTime = 0;
    targetSecurityMode = SECURITY_OPEN;
    portAuthorized = false;
    useShortSlot = true;
    memset(targetRsnIe, 0, sizeof(targetRsnIe));
    targetRsnIeLen = 0;
    memset(assocRsnIe, 0, sizeof(assocRsnIe));
    assocRsnIeLen = 0;
    wpaState = WPA_STATE_DISABLED;
    memset(wpaPmk, 0, sizeof(wpaPmk));
    memset(wpaPtk, 0, sizeof(wpaPtk));
    memset(wpaInstalledPtk, 0, sizeof(wpaInstalledPtk));
    memset(wpaAnonce, 0, sizeof(wpaAnonce));
    memset(wpaSnonce, 0, sizeof(wpaSnonce));
    memset(wpaGtk, 0, sizeof(wpaGtk));
    wpaGtkKeyId = 0;
    wpaEapolVersion = 2;
    wpaPmkValid = false;
    wpaCryptoReady = false;
    wpaPtkInstalled = false;
    wpaNewPtkPending = false;
    wpaGtkInstalled = false;
    wpaM1Replay = 0;
    wpaLastReplay = 0;
    wpaReplayValid = false;
    wpaTxPn = 0;
    memset(wpaPairwiseRxPn, 0, sizeof(wpaPairwiseRxPn));
    memset(wpaGroupRxPn, 0, sizeof(wpaGroupRxPn));
    memset(wpaLastTx, 0, sizeof(wpaLastTx));
    wpaLastTxLen = 0;
    wpaLastTxProtected = false;
    wpaRetries = 0;
    wpaRxM1 = wpaRxM3 = wpaRxGroupM1 = 0;
    wpaMicFailures = wpaReplayDrops = wpaDecryptFailures = 0;
    wpaStateStartTime = 0;
    channelCalibrationValid = false;
    calibratedChannel = 0;
    calibratedBandwidth = 0;
    mgmtDesc = nullptr; mgmtDmaCmd = nullptr; mgmtPayloadDesc = nullptr; mgmtPayloadDmaCmd = nullptr; mgmtWp = 0;
    bkqDesc = nullptr; bkqDmaCmd = nullptr; bkqPayloadDesc = nullptr; bkqPayloadDmaCmd = nullptr; bkqWp = 0;
    viqDesc = nullptr; viqDmaCmd = nullptr; viqPayloadDesc = nullptr; viqPayloadDmaCmd = nullptr; viqWp = 0;
    voqDesc = nullptr; voqDmaCmd = nullptr; voqPayloadDesc = nullptr; voqPayloadDmaCmd = nullptr; voqWp = 0;
    rxBufferDmaCmd = nullptr;
    rxBufferVirtAddr = nullptr;
    rxRp = 0;
    powerState = APPLE80211_POWER_ON;
    h2cqPhysAddr = 0;
    h2cPayloadPhysAddr = 0;
    netif = nullptr;
    workLoop = nullptr;
    interruptSource = nullptr;

    macAddress[0] = 0x00;
    macAddress[1] = 0x11;
    macAddress[2] = 0x22;
    macAddress[3] = 0x33;
    macAddress[4] = 0x44;
    macAddress[5] = 0x55;

    h2cSeq = 0;
    lastBoxNum = 0;
    rfeOption = 0;
    memset(physEfuseMap, 0xff, 512);
    memset(logicalEfuseMap, 0xff, 768);

    // WiFi state machine init
    wifiState = APPLE80211_S_INIT;
    memset(&currentChannel, 0, sizeof(currentChannel));
    currentChannel.version = APPLE80211_VERSION;
    currentChannel.channel = 6;
    currentChannel.flags = APPLE80211_C_FLAG_2GHZ | APPLE80211_C_FLAG_20MHZ;
    memset(currentSsid, 0, sizeof(currentSsid));
    currentSsidLen = 0;
    memset(currentBssid, 0, sizeof(currentBssid));
    authTypeLower = APPLE80211_AUTHTYPE_OPEN;
    authTypeUpper = APPLE80211_AUTHTYPE_NONE;
    txPowerDbm = 14; // 14 dBm default
    countryCode[0] = 'U'; countryCode[1] = 'S'; countryCode[2] = 0;
    memset(scanResults, 0, sizeof(scanResults));
    scanResultCount = 0;
    scanInProgress = false;
    scanTimer = nullptr;
    offchannelScanTimer = nullptr;
    rxPollTimer = nullptr;
    rxPollActive = false;
    debugRxPollCalls = 0;
    debugRxPollPackets = 0;
    debugRxPollBudgetHits = 0;
    debugRxPollMaxBatch = 0;
    debugRxRokInterrupts = 0;
    debugRxRduInterrupts = 0;
    debugRxTimerPolls = 0;
    debugRxTxServiceCalls = 0;
    debugRxTxQueueRestarts = 0;
    debugRxTxServiceTotalNs = 0;
    debugRxTxServiceMaxNs = 0;
    debugRxPollTotalNs = 0;
    debugRxPollMaxNs = 0;
    debugRxSyncTotalNs = 0;
    debugRxSyncMaxNs = 0;
    debugRxSyncCalls = 0;
    debugRxInputTotalNs = 0;
    debugRxInputMaxNs = 0;
    debugRxInputCalls = 0;
    debugRxCcmpTotalNs = 0;
    debugRxCcmpMaxNs = 0;
    debugRxCcmpCalls = 0;
    debugRxBuildTotalNs = 0;
    debugRxBuildMaxNs = 0;
    debugRxBuildCalls = 0;
    debugRxPoolTakeTotalNs = 0;
    debugRxPoolTakeMaxNs = 0;
    debugRxCopyTotalNs = 0;
    debugRxCopyMaxNs = 0;
    debugRxFinalizeTotalNs = 0;
    debugRxFinalizeMaxNs = 0;
    memset(debugRxRateCounts, 0, sizeof(debugRxRateCounts));
    memset(debugRxBwCounts, 0, sizeof(debugRxBwCounts));
    memset(debugRxDescBwCounts, 0, sizeof(debugRxDescBwCounts));
    memset(debugRxScCounts, 0, sizeof(debugRxScCounts));
    memset(debugRxPpduBw, 0, sizeof(debugRxPpduBw));
    memset(debugRxPpduBwValid, 0, sizeof(debugRxPpduBwValid));
    debugRxLastRate = 0;
    debugRxLastBw = 0;
    connectedScanIndex = 0;
    connectedScanOriginalRcr = 0;
    connectedScanHomePrimary = 1;
    connectedScanHomeCenter = 1;
    connectedScanHomeBandwidth = 0;
    connectedScanHomePrimaryIndex = 0;
    connectedScanPhase = 0;
    connectedScanDrainRetries = 0;
    scanChannel = 6;
    scanChannelFlags = APPLE80211_C_FLAG_2GHZ | APPLE80211_C_FLAG_20MHZ | APPLE80211_C_FLAG_ACTIVE;

    return true;
}
IOReturn RealtekRTL8822C::setPropertiesAction(void* target, void* properties,
                                           void*, void*, void*) {
    RealtekRTL8822C* controller = static_cast<RealtekRTL8822C*>(target);
    if (!controller) return kIOReturnBadArgument;
    return controller->setPropertiesGated(static_cast<OSObject*>(properties));
}

IOReturn RealtekRTL8822C::setProperties(OSObject* properties) {
    if (!properties) return kIOReturnBadArgument;
    return executeCommand(this, &RealtekRTL8822C::setPropertiesAction,
                          this, properties);
}

IOReturn RealtekRTL8822C::powerStateAction(void* target, void* ordinal,
                                        void*, void*, void*) {
    RealtekRTL8822C* controller = static_cast<RealtekRTL8822C*>(target);
    if (!controller) return kIOReturnBadArgument;
    unsigned long requested = static_cast<unsigned long>(
        reinterpret_cast<uintptr_t>(ordinal));
    bool ok = requested == 0 ? controller->suspendHardware() :
                               controller->resumeHardware();
    return ok ? kIOReturnSuccess : kIOReturnError;
}

IOReturn RealtekRTL8822C::setPropertiesGated(OSObject *properties) {
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict) return kIOReturnBadArgument;

    OSString *cmd = OSDynamicCast(OSString, dict->getObject("Command"));
    if (!cmd) return kIOReturnBadArgument;

    // IO80211's BSD interface can retain IFF_UP across a controller disable
    // and therefore omit a matching enable() callback. Reconcile only the
    // upward transition; never modify BSD flags from the controller.
    reconcileInterfaceStateFromBsd();

    if (hardwareSuspended) {
        setProperty("PowerState", "Suspended");
        if (cmd->isEqualTo("UpdateStatus") || cmd->isEqualTo("Disconnect"))
            return kIOReturnSuccess;
        return kIOReturnOffline;
    }

    if (cmd->isEqualTo("SetInterfaceEnabled")) {
        OSBoolean* enabled = OSDynamicCast(OSBoolean, dict->getObject("Enabled"));
        if (!enabled) return kIOReturnBadArgument;
        return setUserInterfaceEnabledGated(enabled->isTrue());
    }

    if (!interfaceEnabled &&
        (cmd->isEqualTo("Scan") || cmd->isEqualTo("Connect"))) {
        setProperty("DriverStatus", "Network interface is disabled");
        return kIOReturnNotReady;
    }

    if (cmd->isEqualTo("UpdateStatus")) {
#if RTW_DEBUG
        RTW_DEBUG_PROPERTY("Debug_REG_CR", (uint64_t)read8(0x0100), 32);
        RTW_DEBUG_PROPERTY("Debug_REG_RCR", (uint64_t)read32(0x0608), 32);
        RTW_DEBUG_PROPERTY("Debug_REG_RXBD_IDX", (uint64_t)read32(0x03B4), 32);
        RTW_DEBUG_PROPERTY("Debug_REG_BEQ_IDX", (uint64_t)read32(0x03A8), 32);
        RTW_DEBUG_PROPERTY("Debug_REG_HIMR0", (uint64_t)read32(0x00B0), 32);
        RTW_DEBUG_PROPERTY("Debug_REG_HISR0", (uint64_t)read32(0x00B4), 32);
        RTW_DEBUG_PROPERTY("Debug_ISR_Calls", (uint64_t)isrCallsCount, 32);
        RTW_DEBUG_PROPERTY("Debug_Scan_Count", (uint64_t)customScanResultsCount, 32);
        RTW_DEBUG_PROPERTY("Debug_Auth_Rx_Count", (uint64_t)debugAuthRxCount, 32);
        RTW_DEBUG_PROPERTY("Debug_BEDOK_Count", (uint64_t)debugBedokCount, 32);
        RTW_DEBUG_PROPERTY("Debug_TXERR_Count", (uint64_t)debugTxerrCount, 32);
        RTW_DEBUG_PROPERTY("Debug_TXFOVW_Count", (uint64_t)debugTxfovwCount, 32);
        RTW_DEBUG_PROPERTY("Debug_C2H_Count", (uint64_t)debugC2hRxCount, 32);
        char rxPollDb[1200];
        snprintf(rxPollDb, sizeof(rxPollDb),
                 "available=%d active=%d budget=%u delay_us=%u polls=%u timer_polls=%u packets=%u budget_hits=%u max_batch=%u rok_irq=%u rdu_irq=%u tx_service=%u tx_restarts=%u tx_service_avg_us=%llu tx_service_max_us=%llu restart=async input=immediate poll_avg_us=%llu poll_max_us=%llu sync_avg_us=%llu sync_max_us=%llu build_avg_us=%llu build_max_us=%llu take_avg_us=%llu take_max_us=%llu copy_avg_us=%llu copy_max_us=%llu finalize_avg_us=%llu finalize_max_us=%llu input_avg_us=%llu input_max_us=%llu ccmp_avg_us=%llu ccmp_max_us=%llu",
                 rxPollTimer ? 1 : 0, rxPollActive ? 1 : 0,
                 rxPollTimer ? kRtwRxPollBudget : 512U,
                 rxPollTimer ? kRtwRxPollDelayUs : 0U,
                 (unsigned int)debugRxPollCalls,
                 (unsigned int)debugRxTimerPolls,
                 (unsigned int)debugRxPollPackets,
                 (unsigned int)debugRxPollBudgetHits,
                 (unsigned int)debugRxPollMaxBatch,
                 (unsigned int)debugRxRokInterrupts,
                 (unsigned int)debugRxRduInterrupts,
                 (unsigned int)debugRxTxServiceCalls,
                 (unsigned int)debugRxTxQueueRestarts,
                 (unsigned long long)(debugRxTxServiceCalls ?
                     debugRxTxServiceTotalNs / debugRxTxServiceCalls / 1000ULL : 0ULL),
                 (unsigned long long)(debugRxTxServiceMaxNs / 1000ULL),
                 (unsigned long long)(debugRxPollCalls ?
                     debugRxPollTotalNs / debugRxPollCalls / 1000ULL : 0ULL),
                 (unsigned long long)(debugRxPollMaxNs / 1000ULL),
                 (unsigned long long)(debugRxSyncCalls ?
                     debugRxSyncTotalNs / debugRxSyncCalls / 1000ULL : 0ULL),
                 (unsigned long long)(debugRxSyncMaxNs / 1000ULL),
                 (unsigned long long)(debugRxBuildCalls ?
                     debugRxBuildTotalNs / debugRxBuildCalls / 1000ULL : 0ULL),
                 (unsigned long long)(debugRxBuildMaxNs / 1000ULL),
                 (unsigned long long)(debugRxBuildCalls ?
                     debugRxPoolTakeTotalNs / debugRxBuildCalls / 1000ULL : 0ULL),
                 (unsigned long long)(debugRxPoolTakeMaxNs / 1000ULL),
                 (unsigned long long)(debugRxBuildCalls ?
                     debugRxCopyTotalNs / debugRxBuildCalls / 1000ULL : 0ULL),
                 (unsigned long long)(debugRxCopyMaxNs / 1000ULL),
                 (unsigned long long)(debugRxBuildCalls ?
                     debugRxFinalizeTotalNs / debugRxBuildCalls / 1000ULL : 0ULL),
                 (unsigned long long)(debugRxFinalizeMaxNs / 1000ULL),
                 (unsigned long long)(debugRxInputCalls ?
                     debugRxInputTotalNs / debugRxInputCalls / 1000ULL : 0ULL),
                 (unsigned long long)(debugRxInputMaxNs / 1000ULL),
                 (unsigned long long)(debugRxCcmpCalls ?
                     debugRxCcmpTotalNs / debugRxCcmpCalls / 1000ULL : 0ULL),
                 (unsigned long long)(debugRxCcmpMaxNs / 1000ULL));
        RTW_DEBUG_PROPERTY("Debug_RX_Poll", rxPollDb);
        UInt32 poolCount = 0;
        UInt32 poolMinimum = 0;
        bool poolPending = false;
        if (rxPacketPoolLock) {
            IOInterruptState interruptState =
                IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
            poolCount = rxPacketPoolCount;
            poolMinimum = rxPacketPoolMinimum;
            poolPending = rxPacketPoolRefillPending;
            IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);
        }
        char rxPoolDb[384];
        snprintf(rxPoolDb, sizeof(rxPoolDb),
                 "available=%d worker=independent allocator=batch-list capacity=%u count=%u minimum=%u low=%u pending=%d hits=%u misses=%u refilled=%u batch_calls=%u alloc_fail=%u fallback=%u",
                 rxPacketPoolTimer ? 1 : 0,
                 (unsigned int)kRtwRxPacketPoolCapacity,
                 (unsigned int)poolCount, (unsigned int)poolMinimum,
                 (unsigned int)kRtwRxPacketPoolLowWatermark,
                 poolPending ? 1 : 0,
                 (unsigned int)debugRxPacketPoolHits,
                 (unsigned int)debugRxPacketPoolMisses,
                 (unsigned int)debugRxPacketPoolRefilled,
                 (unsigned int)debugRxPacketPoolBatchCalls,
                 (unsigned int)debugRxPacketPoolAllocFailures,
                 (unsigned int)debugRxPacketPoolMisses);
        RTW_DEBUG_PROPERTY("Debug_RX_Packet_Pool", rxPoolDb);
        UInt32 legacy = 0, ht = 0, vht1 = 0, vht2 = 0;
        UInt32 topCount = 0;
        UInt8 topRate = 0;
        for (UInt32 rate = 0; rate < 84; rate++) {
            UInt32 rateCount = debugRxRateCounts[rate];
            if (rate <= 0x0b) legacy += rateCount;
            else if (rate <= 0x2b) ht += rateCount;
            else if (rate <= 0x35) vht1 += rateCount;
            else if (rate <= 0x3f) vht2 += rateCount;
            if (rateCount > topCount) {
                topCount = rateCount;
                topRate = (UInt8)rate;
            }
        }
        char rxRateDb[512];
        snprintf(rxRateDb, sizeof(rxRateDb),
                 "last=0x%02x bw=%u top=0x%02x/%u legacy=%u ht=%u vht1=%u vht2=%u phy_bw20=%u phy_bw40=%u phy_bw80=%u desc_bw20=%u desc_bw40=%u desc_bw80=%u rxsc=0:%u 1-8:%u 9-12:%u 13-15:%u",
                 debugRxLastRate, debugRxLastBw == 2 ? 80U :
                     (debugRxLastBw == 1 ? 40U : 20U),
                 topRate, (unsigned int)topCount,
                 (unsigned int)legacy, (unsigned int)ht,
                 (unsigned int)vht1, (unsigned int)vht2,
                 (unsigned int)debugRxBwCounts[0],
                 (unsigned int)debugRxBwCounts[1],
                 (unsigned int)debugRxBwCounts[2],
                 (unsigned int)debugRxDescBwCounts[0],
                 (unsigned int)debugRxDescBwCounts[1],
                 (unsigned int)debugRxDescBwCounts[2],
                 (unsigned int)debugRxScCounts[0],
                 (unsigned int)(debugRxScCounts[1] + debugRxScCounts[2] +
                     debugRxScCounts[3] + debugRxScCounts[4] +
                     debugRxScCounts[5] + debugRxScCounts[6] +
                     debugRxScCounts[7] + debugRxScCounts[8]),
                 (unsigned int)(debugRxScCounts[9] + debugRxScCounts[10] +
                     debugRxScCounts[11] + debugRxScCounts[12]),
                 (unsigned int)(debugRxScCounts[13] + debugRxScCounts[14] +
                     debugRxScCounts[15]));
        RTW_DEBUG_PROPERTY("Debug_RX_PHY", rxRateDb);
        char baDb[192];
        snprintf(baDb, sizeof(baDb), "established=%d requests=%u responses=%u attempts=%u token=%u tid=%u buf=%u timeout=%u agg=%d",
                 baEstablished ? 1 : 0, (unsigned int)baRequests, (unsigned int)baResponses,
                 (unsigned int)baAttempts, baDialogToken, baTid, baBufferSize, baTimeout,
                 baEstablished ? 1 : 0);
        RTW_DEBUG_PROPERTY("Debug_BA_State", baDb);
        publishRxBaState();

        UInt32 dataBeqWp = 0, dataBeqRp = 0, dataBeqOutstanding = 0, dataBeqFaults = 0, dataBeqRecoveries = 0;
        if (beqLock) {
            IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
            dataBeqWp = beqWp;
            dataBeqRp = beqRp;
            dataBeqOutstanding = beqOutstanding;
            dataBeqFaults = debugBeqFaults;
            dataBeqRecoveries = debugBeqRecoveries;
            IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
        }
        // A completed output necessarily follows its call counter increment.
        // Snapshot success first and calls second so a concurrent packet cannot
        // produce the misleading transient `ok=out+1` seen in the v41 report.
        int dataOutputSuccess = debugOutputSuccess;
        OSSynchronizeIO();
        int dataOutputCalls = debugOutputCalls;
        char dataPlaneDb[384];
        snprintf(dataPlaneDb, sizeof(dataPlaneDb),
                 "out=%d ok=%d drop=%d stall=%d fault=%u recover=%u last=%s rx=%d delivered=%d rejected=%d beq_wp=%u beq_rp=%u outstanding=%u idx=%08x num=%04x txdma=%08x q0=%08x pages2=%04x acqstop=%08x pause=%02x",
                 dataOutputCalls, dataOutputSuccess, debugOutputDropped, debugOutputStalled,
                 (unsigned int)dataBeqFaults, (unsigned int)dataBeqRecoveries, debugOutputLastDrop,
                 debugDataRxFrames, debugDataRxDelivered, debugDataRxRejected,
                 (unsigned int)dataBeqWp, (unsigned int)dataBeqRp, (unsigned int)dataBeqOutstanding,
                 (unsigned int)read32(0x03a8), (unsigned int)read16(0x0388),
                 (unsigned int)read32(0x0210),
                 (unsigned int)read32(0x0400), (unsigned int)read16(0x0236),
                 (unsigned int)read32(0x045c), read8(0x0522));
        RTW_DEBUG_PROPERTY("Debug_DataPlane", dataPlaneDb);

        char cck_db[256];
        snprintf(cck_db, sizeof(cck_db), "EfA=%02x 18a0=%08x 18e8=%08x 41a0=%08x 41e8=%08x 3a00=%08x 3a04=%08x 3a08=%08x CCKSEL=%08x",
                 logicalEfuseMap[0x10],
                 (unsigned int)read32(0x18a0), (unsigned int)read32(0x18e8),
                 (unsigned int)read32(0x41a0), (unsigned int)read32(0x41e8),
                 (unsigned int)read32(0x3a00), (unsigned int)read32(0x3a04),
                 (unsigned int)read32(0x3a08), (unsigned int)read32(0x1a04));
        RTW_DEBUG_PROPERTY("Debug_CCK_Power_State", cck_db);

        char txRfDb[384];
        snprintf(txRfDb, sizeof(txRfDb),
                 "rfe=%u anmap=%08x txlg=%08x ccksel=%08x rfeA=%08x rfeB=%08x modeA=%08x modeB=%08x wire=%08x/%08x cca=%08x txfifo=%08x pause=%02x rf0=%05x/%05x rf18=%05x/%05x",
                 (unsigned int)rfeOption, (unsigned int)read32(0x0820),
                 (unsigned int)read32(0x1e2c), (unsigned int)read32(0x1a04),
                 (unsigned int)read32(0x1840), (unsigned int)read32(0x4144),
                 (unsigned int)read32(0x1800), (unsigned int)read32(0x4100),
                 (unsigned int)read32(0x180c), (unsigned int)read32(0x410c),
                 (unsigned int)read32(0x1d58), (unsigned int)read32(0x1e70), read8(0x0522),
                 (unsigned int)readRfMask(0, 0x00, 0xfffff),
                 (unsigned int)readRfMask(1, 0x00, 0xfffff),
                 (unsigned int)readRfMask(0, 0x18, 0xfffff),
                 (unsigned int)readRfMask(1, 0x18, 0xfffff));
        RTW_DEBUG_PROPERTY("Debug_Tx_Rf_State", txRfDb);

        RTW_DEBUG_PROPERTY("Debug_REG_MGMT_IDX", (uint64_t)read32(0x03B0), 32);
        RTW_DEBUG_PROPERTY("Debug_MGNTDOK_Count", (uint64_t)debugMgntdokCount, 32);
        RTW_DEBUG_PROPERTY("Debug_BCNDMAINT_Count", (uint64_t)debugBcndmaintCount, 32);
        RTW_DEBUG_PROPERTY("Debug_MGMT_Base", (uint64_t)read32(0x0310), 32);
        RTW_DEBUG_PROPERTY("Debug_TXDMA_Status", (uint64_t)read32(0x0210), 32);

        char bssid_db[128];
        snprintf(bssid_db, sizeof(bssid_db), "%02x:%02x:%02x:%02x:%02x:%02x",
                 read8(0x0618), read8(0x0619), read8(0x061a),
                 read8(0x061b), read8(0x061c), read8(0x061d));
        RTW_DEBUG_PROPERTY("Debug_BSSID_Readback", bssid_db);
        RTW_DEBUG_PROPERTY("Debug_MGMT_Rp", (uint64_t)mgmtRp, 32);

        char coexDb[128];
        snprintf(coexDb, sizeof(coexDb), "73=%02x 40=%08x 4c6=%04x 762=%04x Ind38=%08x Ind54=%08x",
                 read8(0x0073), (unsigned int)read32(0x0040), (unsigned int)read16(0x04C6),
                 (unsigned int)read16(0x0762), (unsigned int)readLteCoexReg(0x38), (unsigned int)readLteCoexReg(0x54));
        RTW_DEBUG_PROPERTY("Debug_Coex_State", coexDb);
#endif

        runConnectionWatchdog();

#if RTW_DEBUG
        publishDebugSnapshot();
#endif

        return kIOReturnSuccess;
    } else if (cmd->isEqualTo("Scan")) {
        if (scanInProgress) {
            setProperty("DriverStatus", "Scan already in progress");
            traceEvent("scan:already-active");
            return kIOReturnBusy;
        }
        if (connState == CONN_STATE_CONNECTED) {
            if (!startConnectedScan()) {
                setProperty("DriverStatus",
                            "Connected scan could not start in the current link state");
                return kIOReturnNotReady;
            }
            return kIOReturnSuccess;
        }
        if (connState != CONN_STATE_DISCONNECTED) {
            setProperty("DriverStatus", "Scan rejected while connection setup is active");
            traceEvent("scan:rejected-connecting");
            return kIOReturnBusy;
        }
        RTW_DEBUG_LOG("RealtekRTL8822C: Scanning Wi-Fi channels...\n");
        traceEvent("scan:requested");
        setProperty("WiFiStatus", "Scanning");
        bool scanOk = performWifiScan();
        setProperty("WiFiStatus", "Idle");
        setProperty("DriverStatus", scanOk ?
                    (customScanResultsCount > 0 ? "Scan complete" :
                                                  "Scan complete: no networks found") :
                    "Scan failed while programming a channel");
        traceEvent(scanOk ? "scan:completed" : "scan:channel-program-failed");
        return scanOk ? kIOReturnSuccess : kIOReturnIOError;
    } else if (cmd->isEqualTo("Disconnect")) {
        disconnectFromNetwork("user", true);
        return kIOReturnSuccess;
    } else if (cmd->isEqualTo("Connect")) {
        if (scanInProgress) {
            setProperty("DriverStatus", "Connect rejected while off-channel scan is active");
            traceEvent("connect:rejected-scan-active");
            return kIOReturnBusy;
        }
        OSString *ssid = OSDynamicCast(OSString, dict->getObject("SSID"));
        if (!ssid) return kIOReturnBadArgument;

        const char* requestedSsid = ssid->getCStringNoCopy();
        bool sameTarget = requestedSsid && strcmp(targetSsid, requestedSsid) == 0;
        if (connState == CONN_STATE_CONNECTED && sameTarget && portAuthorized) {
            traceEvent("connect:already-connected");
            setProperty("WiFiStatus", "Connected");
            return kIOReturnSuccess;
        }
        if (connState == CONN_STATE_CONNECTED && sameTarget && !portAuthorized) {
            // A repeated explicit Connect may contain corrected credentials.
            // Never keep using the PMK from an earlier attempt merely because
            // the four-way handshake is still inside its timeout window.
            traceEvent("connect:restart-wpa-credentials");
        }
        if ((connState == CONN_STATE_CONNECTING_AUTH || connState == CONN_STATE_CONNECTING_ASSOC) && sameTarget) {
            // The AP may also have restarted or changed its security/BSSID.
            // Treat a new command as an intentional replacement attempt.
            traceEvent("connect:restart-pending-attempt");
        }
        if (connState != CONN_STATE_DISCONNECTED) {
            disconnectFromNetwork("reconnect", connState == CONN_STATE_CONNECTED);
        }

        RTW_DEBUG_LOG("RealtekRTL8822C: Request to connect to SSID: %s\n", ssid->getCStringNoCopy());
        traceEvent("connect:requested");
        setProperty("WiFiStatus", "Connecting");
        int found_idx = -1;
        for (int i = 0; i < customScanResultsCount; i++) {
            if (strcmp(customScanResults[i].ssid, ssid->getCStringNoCopy()) == 0) {
                found_idx = i;
                break;
            }
        }

        UInt64 scanAgeNs = kRtwRecentScanMaxAgeNs + 1;
        UInt64 resultAgeNs = kRtwRecentScanMaxAgeNs + 1;
        uint64_t scanNow = 0;
        clock_get_uptime(&scanNow);
        if (lastScanCompletedTime != 0) {
            absolutetime_to_nanoseconds(scanNow - lastScanCompletedTime,
                                        &scanAgeNs);
        }
        if (found_idx >= 0 && customScanResults[found_idx].last_seen_time != 0 &&
            scanNow >= customScanResults[found_idx].last_seen_time) {
            absolutetime_to_nanoseconds(
                scanNow - customScanResults[found_idx].last_seen_time,
                &resultAgeNs);
        }
        bool useRecentScan = found_idx >= 0 &&
                             scanAgeNs <= kRtwRecentScanMaxAgeNs &&
                             resultAgeNs <= kRtwRecentScanMaxAgeNs;
        if (useRecentScan) {
            traceEvent("connect:recent-scan-cache");
            setProperty("DriverStatus", "Using recent scan result for connection");
        } else {
            // Refresh stale or missing entries so a hotspot security/channel
            // change cannot reuse an old Open/RSN description. A recent
            // explicit scan is authoritative and must not be immediately
            // replaced by a second five-second sweep that can miss a weak AP.
            traceEvent("connect:refresh-scan");
            setProperty("DriverStatus", "Refreshing stale scan before connection");
            if (!performWifiScan()) {
                traceEvent("connect:refresh-scan-failed");
                setProperty("WiFiStatus", "Failed");
                setProperty("DriverStatus",
                            "Connection scan failed while programming a channel");
                return kIOReturnIOError;
            }
            found_idx = -1;
            for (int i = 0; i < customScanResultsCount; i++) {
                if (strcmp(customScanResults[i].ssid, ssid->getCStringNoCopy()) == 0) {
                    found_idx = i;
                    break;
                }
            }
            scanAgeNs = 0;
            resultAgeNs = 0;
        }
        char scanSource[192];
        snprintf(scanSource, sizeof(scanSource),
                 "source=%s scan_age_ms=%llu result_age_ms=%llu found=%d count=%d",
                 useRecentScan ? "recent-cache" : "fresh-sweep",
                 (unsigned long long)(scanAgeNs / 1000000ULL),
                 (unsigned long long)(resultAgeNs / 1000000ULL),
                 found_idx >= 0 ? 1 : 0, customScanResultsCount);
        RTW_DEBUG_PROPERTY("Debug_Connect_Scan_Source", scanSource);

        if (found_idx == -1) {
            RTW_ERROR_LOG("RealtekRTL8822C: Requested SSID was not found\n");
            traceEvent("connect:target-not-found");
            setProperty("WiFiStatus", "Failed");
            setProperty("DriverStatus", "Requested SSID not found in fresh scan");
            return kIOReturnNotFound;
        }

        // Save target parameters
        strlcpy(targetSsid, customScanResults[found_idx].ssid,
                sizeof(targetSsid));
        memcpy(targetBssid, customScanResults[found_idx].bssid, 6);
        targetChannel = customScanResults[found_idx].channel;
        targetSupportsCck = customScanResults[found_idx].supports_cck;
        targetSupportsHt = customScanResults[found_idx].supports_ht;
        targetSupportsVht = customScanResults[found_idx].supports_vht;
        targetSupportsWmm = customScanResults[found_idx].supports_wmm;
        targetSecurityMode = (RTL8822CSecurityMode)customScanResults[found_idx].security_mode;
        targetRsnIeLen = customScanResults[found_idx].rsn_ie_len;
        memcpy(targetRsnIe, customScanResults[found_idx].rsn_ie, sizeof(targetRsnIe));
        targetHtCapInfo = customScanResults[found_idx].ht_cap_info;
        targetHtAmpduParams = customScanResults[found_idx].ht_ampdu_params;
        targetHtMcs0 = customScanResults[found_idx].ht_mcs0;
        targetHtMcs1 = customScanResults[found_idx].ht_mcs1;
        targetHtPrimaryChannel = customScanResults[found_idx].ht_primary_channel;
        targetHtOperationInfo = customScanResults[found_idx].ht_operation_info;
        targetVhtCapInfo = customScanResults[found_idx].vht_cap_info;
        targetVhtRxMcsMap = customScanResults[found_idx].vht_rx_mcs_map;
        targetVhtRxHighest = customScanResults[found_idx].vht_rx_highest;
        targetVhtTxMcsMap = customScanResults[found_idx].vht_tx_mcs_map;
        targetVhtTxHighest = customScanResults[found_idx].vht_tx_highest;
        targetVhtChannelWidth = customScanResults[found_idx].vht_channel_width;
        targetVhtCenterSegment0 = customScanResults[found_idx].vht_center_segment0;
        targetVhtCenterSegment1 = customScanResults[found_idx].vht_center_segment1;
        char linkCapability[320];
        snprintf(linkCapability, sizeof(linkCapability),
                 "ap_ht=%d ap_vht=%d ap_wmm=%d ht_cap=%04x ampdu_param=%02x mcs=%02x/%02x ht_primary=%u ht_op=%02x vht_cap=%08x vht_rxmap=%04x vht_txmap=%04x vht_op=%u/%u/%u assoc=%s ra_mask=%08x ampdu=0",
                 targetSupportsHt ? 1 : 0, targetSupportsVht ? 1 : 0,
                 targetSupportsWmm ? 1 : 0,
                 targetHtCapInfo, targetHtAmpduParams, targetHtMcs0,
                 targetHtMcs1, targetHtPrimaryChannel, targetHtOperationInfo,
                 (unsigned int)targetVhtCapInfo, targetVhtRxMcsMap,
                 targetVhtTxMcsMap, targetVhtChannelWidth,
                 targetVhtCenterSegment0, targetVhtCenterSegment1,
                 targetChannel > 14 ? "legacy-g" : "legacy-bg",
                 targetChannel > 14 ? 0x00000ff0U : 0x00000fffU);
        RTW_DEBUG_PROPERTY("Debug_Link_Capability", linkCapability);
        traceEvent("connect:target-selected");

        char securityDb[192];
        snprintf(securityDb, sizeof(securityDb),
                 "target=%s mode=%s rsn_ie_len=%u port=closed pairwise=0 group=0 cam=empty next=wpa-handshake",
                 targetSsid,
                 targetSecurityMode == SECURITY_WPA2 ? "wpa2" :
                 (targetSecurityMode == SECURITY_WPA ? "wpa" : "open"),
                 targetRsnIeLen);
        RTW_DEBUG_PROPERTY("Debug_Security_State", securityDb);
        portAuthorized = false;
        if (targetSecurityMode != SECURITY_OPEN) {
            if (!wpaCryptoReady) {
                traceEvent("connect:wpa-crypto-selftest-failed");
                setProperty("WiFiStatus", "WPACryptoFailed");
                return kIOReturnNotReady;
            }
            OSString* password = OSDynamicCast(OSString, dict->getObject("Password"));
            if (!parseAndSelectRsn()) {
                traceEvent("connect:wpa-suite-unsupported");
                setProperty("WiFiStatus", "WPAUnsupported");
                setProperty("DriverStatus", "Only WPA2-PSK with CCMP group/pairwise cipher and optional PMF is supported");
                return kIOReturnUnsupported;
            }
            const char* passwordText = password ? password->getCStringNoCopy() : nullptr;
            size_t passwordLength = passwordText ? strlen(passwordText) : 0;
            if (passwordLength > 64) passwordLength = 65;
            char passwordCopy[65];
            memset(passwordCopy, 0, sizeof(passwordCopy));
            if (passwordText && passwordLength <= 64)
                memcpy(passwordCopy, passwordText, passwordLength);
            bool pmkReady = passwordLength <= 64 && prepareWpaPmk(passwordCopy);
            RtwWpaCrypto::secureZero(passwordCopy, sizeof(passwordCopy));
            if (!pmkReady) {
                traceEvent("connect:wpa-password-invalid");
                setProperty("WiFiStatus", "WPAPasswordRequired");
                setProperty("DriverStatus", "WPA2 password must be 8-63 characters or a 64-digit hexadecimal PMK");
                resetWpaState(true);
                return kIOReturnBadArgument;
            }
            traceEvent("connect:wpa-pmk-ready");
            setProperty("DriverStatus", "WPA2 credentials accepted; starting authentication");
            publishWpaState("pmk-derived");
        } else {
            resetWpaState(true);
            setProperty("DriverStatus", "Open-network authentication started");
        }

        bool allowed5GPrimary = targetChannel == 36 || targetChannel == 40 ||
                                targetChannel == 44 || targetChannel == 48 ||
                                targetChannel == 149 || targetChannel == 153 ||
                                targetChannel == 157 || targetChannel == 161 ||
                                targetChannel == 165;
        if (targetChannel > 14 && !allowed5GPrimary) {
            traceEvent("connect:5g-channel-blocked");
            setProperty("WiFiStatus", "5GHzChannelBlocked");
            setProperty("DriverStatus", "5 GHz TX is restricted to FCC/US non-DFS channels 36/40/44/48 and 149/153/157/161/165");
            return kIOReturnUnsupported;
        }

        // Select the widest peer-advertised mode that stays inside the
        // controlled FCC/US UNII-1 or UNII-3 block. Hardware receives the
        // center channel, while targetChannel remains the BSS primary channel.
        targetCenterChannel = targetChannel;
        targetBandwidth = 0; // RTW_CHANNEL_WIDTH_20
        targetPrimaryChannelIndex = 0; // RTW_SC_DONT_CARE
        UInt8 htSecondary = targetHtOperationInfo & 0x03;
        int peerHt40Center = targetChannel;
        if (htSecondary == 1) peerHt40Center += 2;
        else if (htSecondary == 3) peerHt40Center -= 2;
        bool allowed40Center = peerHt40Center == 38 || peerHt40Center == 46 ||
                               peerHt40Center == 151 || peerHt40Center == 159;
        bool peerHt40 = targetChannel > 14 && (targetHtCapInfo & (1U << 1)) &&
                        (htSecondary == 1 || htSecondary == 3) &&
                        allowed40Center;
        bool allowed80Center = targetVhtCenterSegment0 == 42 ||
                               targetVhtCenterSegment0 == 155;
        bool primaryInSelected80 =
            (targetVhtCenterSegment0 == 42 && targetChannel >= 36 && targetChannel <= 48) ||
            (targetVhtCenterSegment0 == 155 && targetChannel >= 149 && targetChannel <= 161);
        if (targetChannel > 14 && targetSupportsVht &&
            targetVhtChannelWidth == 1 && allowed80Center &&
            primaryInSelected80) {
            targetBandwidth = 2; // RTW_CHANNEL_WIDTH_80
            targetCenterChannel = targetVhtCenterSegment0;
            int primaryOffset = targetChannel - targetCenterChannel;
            if (primaryOffset == -6) targetPrimaryChannelIndex = 4;      // 20 lowest
            else if (primaryOffset == -2) targetPrimaryChannelIndex = 2; // 20 lower
            else if (primaryOffset == 2) targetPrimaryChannelIndex = 1;  // 20 upper
            else targetPrimaryChannelIndex = 3;                          // 20 uppermost
        } else if (peerHt40) {
            targetBandwidth = 1; // RTW_CHANNEL_WIDTH_40
            targetCenterChannel = peerHt40Center;
            if (htSecondary == 1) {
                targetPrimaryChannelIndex = 2; // primary below center
            } else {
                targetPrimaryChannelIndex = 1; // primary above center
            }
        }
        char bandwidthTarget[192];
        snprintf(bandwidthTarget, sizeof(bandwidthTarget),
                 "primary=%d center=%d bw=%u primary_idx=%u peer_ht40=%d peer_vht=%d vht_op=%u/%u/%u",
                 targetChannel, targetCenterChannel,
                 targetBandwidth == 2 ? 80U : (targetBandwidth == 1 ? 40U : 20U),
                 targetPrimaryChannelIndex, peerHt40 ? 1 : 0,
                 targetSupportsVht ? 1 : 0, targetVhtChannelWidth,
                 targetVhtCenterSegment0, targetVhtCenterSegment1);
        RTW_DEBUG_PROPERTY("Debug_Bandwidth_Target", bandwidthTarget);

        // Tune to target channel
        if (!setChannelHw((UInt32)targetCenterChannel, 0, targetBandwidth,
                          targetPrimaryChannelIndex)) {
            traceEvent("connect:channel-program-failed");
            setProperty("WiFiStatus", "ChannelProgramFailed");
            return kIOReturnIOError;
        }
        if (targetChannel > 14 &&
            !configure5GTxPower((UInt32)targetChannel,
                                (UInt32)targetCenterChannel,
                                targetBandwidth)) {
            traceEvent("connect:5g-tx-power-invalid");
            setProperty("WiFiStatus", "5GHzPowerInvalid");
            setProperty("DriverStatus", "5 GHz association blocked because the calibrated FCC TX power profile is invalid");
            return kIOReturnNotReady;
        }
        RTW_DEBUG_PROPERTY("Debug_TXDMA_Status_AfterChannel", (uint64_t)read32(0x0210), 32);

        // Force WLAN-only Coexistence settings
        initCoexWifiOnly();

        bool useCachedCalibration = channelCalibrationValid &&
                                    calibratedChannel == targetCenterChannel &&
                                    calibratedBandwidth == targetBandwidth;
        if (!useCachedCalibration) {
            // Match Linux's balanced target-channel RFK transaction.
            write32Mask(0x1B00, 0x06, 0);
            write32Mask(0x1B08, 1U << 7, 1);
            write32Mask(0x1B00, 0x06, 1);
            write32Mask(0x1B08, 1U << 7, 1);
            write32Mask(0x1B00, 0x06, 0);
            runTxGapkCalibration();

            char targetRf0PreIqk[64];
            snprintf(targetRf0PreIqk, sizeof(targetRf0PreIqk), "A=%05x B=%05x",
                     (unsigned int)readRfMask(0, 0x00, 0xfffff),
                     (unsigned int)readRfMask(1, 0x00, 0xfffff));
            RTW_DEBUG_PROPERTY("Debug_Target_Rf0_PreIQK", targetRf0PreIqk);

            bool targetIqkOk = kSkipTargetIqkDiagnostic ? true : runIqk();
            char targetRf0PostIqk[64];
            snprintf(targetRf0PostIqk, sizeof(targetRf0PostIqk), "A=%05x B=%05x",
                     (unsigned int)readRfMask(0, 0x00, 0xfffff),
                     (unsigned int)readRfMask(1, 0x00, 0xfffff));
            RTW_DEBUG_PROPERTY("Debug_Target_Rf0_PostIQK", targetRf0PostIqk);
            RTW_DEBUG_PROPERTY("Debug_TXDMA_Status_AfterIQK", (uint64_t)read32(0x0210), 32);
            char targetIqkDb[96];
            snprintf(targetIqkDb, sizeof(targetIqkDb), "ok=%d skipped=%d cached=0 status=0x%02x",
                     targetIqkOk ? 1 : 0, kSkipTargetIqkDiagnostic ? 1 : 0, read8(0x2D9C));
            RTW_DEBUG_PROPERTY("Debug_Target_IQK_Result", targetIqkDb);
            if (targetIqkOk) {
                channelCalibrationValid = true;
                calibratedChannel = targetCenterChannel;
                calibratedBandwidth = targetBandwidth;
            }
        } else {
            char cachedIqkDb[96];
            snprintf(cachedIqkDb, sizeof(cachedIqkDb), "ok=1 skipped=1 cached=1 channel=%d", targetChannel);
            RTW_DEBUG_PROPERTY("Debug_Target_IQK_Result", cachedIqkDb);
            RTW_DEBUG_PROPERTY("Debug_TXGAPK_Status", "cached=target-channel");
        }

        // Return RFK processing to normal power-save state after calibration or reuse.
        write32Mask(0x1B00, 0x06, 0);
        write32Mask(0x1B08, 1U << 7, 0);
        write32Mask(0x1B00, 0x06, 1);
        write32Mask(0x1B08, 1U << 7, 0);
        write32Mask(0x1B00, 0x06, 0);

        // Start Authentication
        authAttempts = 0;
        assocAttempts = 0;
        resetBaState();
        if (sendAuthRequest()) {
            return kIOReturnSuccess;
        } else {
            traceEvent("auth:queue-failed");
            setProperty("WiFiStatus", "Failed");
            return kIOReturnError;
        }
    }

    return kIOReturnUnsupported;
}

const char* RealtekRTL8822C::connectionStateName() const {
    switch (connState) {
        case CONN_STATE_DISCONNECTED: return "disconnected";
        case CONN_STATE_SCANNING: return "scanning";
        case CONN_STATE_CONNECTING_AUTH: return "auth";
        case CONN_STATE_CONNECTING_ASSOC: return "assoc";
        case CONN_STATE_CONNECTED: return "connected";
    }
    return "unknown";
}

void RealtekRTL8822C::traceEvent(const char* event) {
#if RTW_DEBUG
    if (!event) return;

    UInt32 slot = debugTraceNext;
    snprintf(debugTrace[slot], DEBUG_TRACE_ENTRY_SIZE, "%u:%s", ++debugTraceSequence, event);
    debugTraceNext = (debugTraceNext + 1) % DEBUG_TRACE_CAPACITY;
    if (debugTraceCount < DEBUG_TRACE_CAPACITY) debugTraceCount++;

    char trace[DEBUG_TRACE_CAPACITY * DEBUG_TRACE_ENTRY_SIZE];
    trace[0] = '\0';
    UInt32 oldest = (debugTraceNext + DEBUG_TRACE_CAPACITY - debugTraceCount) % DEBUG_TRACE_CAPACITY;
    for (UInt32 i = 0; i < debugTraceCount; i++) {
        UInt32 index = (oldest + i) % DEBUG_TRACE_CAPACITY;
        if (i != 0) strlcat(trace, " | ", sizeof(trace));
        strlcat(trace, debugTrace[index], sizeof(trace));
    }
    RTW_DEBUG_PROPERTY("Debug_EventTrace", trace);
#else
    (void)event;
#endif
}

void RealtekRTL8822C::publishDebugSnapshot() {
#if RTW_DEBUG
    char snapshot[384];
    snprintf(snapshot, sizeof(snapshot),
             "diag=0.0.2 state=%s scan=%d primary=%d center=%d bw=%u rx_rp=%u mgmt_wp=%u mgmt_rp=%u beq_wp=%u beq_rp=%u isr=%d auth_rx=%d mgntdok=%d c2h=%d auth_try=%u assoc_try=%u txba=%d port=%d wpa=%d m1=%u m3=%u",
             connectionStateName(), customScanResultsCount, targetChannel,
             targetCenterChannel,
             targetBandwidth == 2 ? 80U : (targetBandwidth == 1 ? 40U : 20U),
             (unsigned int)rxRp, (unsigned int)mgmtWp, (unsigned int)mgmtRp,
             (unsigned int)beqWp, (unsigned int)beqRp, isrCallsCount,
             debugAuthRxCount, debugMgntdokCount, debugC2hRxCount,
             (unsigned int)authAttempts, (unsigned int)assocAttempts,
             baEstablished ? 1 : 0, portAuthorized ? 1 : 0,
             (int)wpaState, (unsigned int)wpaRxM1, (unsigned int)wpaRxM3);
    RTW_DEBUG_PROPERTY("Debug_Snapshot", snapshot);
    RTW_DEBUG_PROPERTY("Debug_Diagnostics_Revision", "0.0.2");
    RTW_DEBUG_PROPERTY("Debug_HotPath_Diagnostics",
                       "profile=Debug sample=1/1024 ordinary-tx-rx");
#endif
}

void RealtekRTL8822C::publishConnectedScanState(const char* state) {
    if (!state) return;
    setProperty("ConnectedScanState", state);
    RTW_DEBUG_PROPERTY("Debug_Connected_Scan", state);
}

void RealtekRTL8822C::activateNetworkDataPath(const char* reason) {
    IOOutputQueue* queue = getOutputQueue();
    UInt32 sizeBefore = queue ? queue->getSize() : 0;
    UInt32 capacity = queue ? queue->getCapacity() : 0;
    bool queueStarted = queue ? queue->start() : false;
    bool queueServiced = queue ? queue->service() : false;

    // IONetworkController requires a non-null active medium for an active
    // link. Start/service the output queue before publishing carrier so an
    // immediate DHCP/ARP/IP burst cannot land on a stopped queue after wake.
    const IONetworkMedium* medium = getSelectedMedium();
    bool linkPublished = setLinkStatus(kIONetworkLinkValid |
                                      kIONetworkLinkActive,
                                      medium);
    bool postService = queue ? queue->service() : false;

    char db[224];
    snprintf(db, sizeof(db),
             "reason=%s active=1 medium=%s queue_start=%d service=%d/%d size=%u->%u capacity=%u link_publish=%d bsd=%04x/up%d",
             reason ? reason : "unknown", medium ? "selected" : "missing",
             queueStarted ? 1 : 0, queueServiced ? 1 : 0,
             postService ? 1 : 0, (unsigned int)sizeBefore,
             (unsigned int)(queue ? queue->getSize() : 0),
             (unsigned int)capacity, linkPublished ? 1 : 0,
             (unsigned int)(netif ? netif->getFlags() : 0),
             (netif && (netif->getFlags() & IFF_UP)) ? 1 : 0);
    RTW_DEBUG_PROPERTY("Debug_Link_Lifecycle", db);
    traceEvent("link:data-path-active");
}

void RealtekRTL8822C::runConnectionWatchdog() {
    reconcileInterfaceStateFromBsd();
    processRxReorderTimeouts();
    if (connState == CONN_STATE_CONNECTING_AUTH || connState == CONN_STATE_CONNECTING_ASSOC) {
        uint64_t currentTime;
        clock_get_uptime(&currentTime);
        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(currentTime - connStateStartTime, &elapsedNs);
        if (elapsedNs > 3000000000ULL) {
            if (connState == CONN_STATE_CONNECTING_AUTH && authAttempts < 3) {
                traceEvent("auth:timeout-retry");
                sendAuthRequest();
            } else if (connState == CONN_STATE_CONNECTING_ASSOC && assocAttempts < 3) {
                traceEvent("assoc:timeout-retry");
                sendAssocRequest();
            } else {
                bool authTimedOut = connState == CONN_STATE_CONNECTING_AUTH;
                traceEvent(authTimedOut ? "auth:timeout-final" : "assoc:timeout-final");
                disconnectFromNetwork("timeout", false);
                setProperty("WiFiStatus", authTimedOut ? "AuthTimeout" : "AssocTimeout");
            }
        }
    }

    if (connState == CONN_STATE_CONNECTED && targetSecurityMode == SECURITY_WPA2 &&
        !portAuthorized && (wpaState == WPA_STATE_WAIT_M1 || wpaState == WPA_STATE_WAIT_M3)) {
        uint64_t now;
        clock_get_uptime(&now);
        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(now - wpaStateStartTime, &elapsedNs);
        if (wpaState == WPA_STATE_WAIT_M3 && elapsedNs > 1000000000ULL &&
            wpaRetries < 3 && wpaLastTxLen != 0) {
            wpaRetries++;
            clock_get_uptime(&wpaStateStartTime);
            bool sent = sendEapolKeyFrame(wpaLastTx, wpaLastTxLen,
                                          wpaLastTxProtected, false);
            traceEvent(sent ? "wpa:m2-timeout-retry" : "wpa:m2-retry-queue-failed");
            publishWpaState(sent ? "m2-timeout-retry" : "m2-retry-queue-failed");
        } else if ((wpaState == WPA_STATE_WAIT_M1 && elapsedNs > 5000000000ULL) ||
                   (wpaState == WPA_STATE_WAIT_M3 && elapsedNs > 1000000000ULL &&
                    wpaRetries >= 3)) {
            traceEvent("wpa:four-way-timeout");
            disconnectFromNetwork("wpa-timeout", true);
            setProperty("WiFiStatus", "WPAHandshakeTimeout");
        }
    }

    if (connState == CONN_STATE_CONNECTED && htNegotiated && !baEstablished && baAttempts > 0) {
        uint64_t currentTime;
        clock_get_uptime(&currentTime);
        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(currentTime - baStateStartTime, &elapsedNs);
        if (elapsedNs > 1000000000ULL && baAttempts < 3) {
            traceEvent("ba:timeout-retry");
            sendAddBaRequest();
        } else if (elapsedNs > 1000000000ULL && baAttempts >= 3) {
            traceEvent("ba:disabled-no-response");
            char baDb[160];
            snprintf(baDb, sizeof(baDb),
                     "established=0 requests=%u responses=%u attempts=%u agg=0 reason=no-operational-response",
                     (unsigned int)baRequests, (unsigned int)baResponses, (unsigned int)baAttempts);
            RTW_DEBUG_PROPERTY("Debug_BA_State", baDb);
            baAttempts = 0;
        }
    }

    if (connState == CONN_STATE_CONNECTED && lastPeerRxTime != 0) {
        uint64_t now;
        clock_get_uptime(&now);
        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(now - lastPeerRxTime, &elapsedNs);
        if (elapsedNs > 15000000000ULL) {
            traceEvent("link:peer-timeout");
            disconnectFromNetwork("peer-timeout", false);
            setProperty("WiFiStatus", "LinkLost");
        }
    }
}

void RealtekRTL8822C::connectionWatchdog(OSObject* owner, IOTimerEventSource* sender) {
    (void)owner;
    runConnectionWatchdog();
    publishSignalStrength();
    publishDebugSnapshot();
    if (sender) sender->setTimeoutMS(500);
}

void RealtekRTL8822C::publishSignalStrength() {
    if (connState != CONN_STATE_CONNECTED || !portAuthorized ||
        connectedSignalDbm < -120 || connectedSignalDbm > 0) {
        if (publishedSignalDbm != -127) {
            removeProperty("SignalStrength");
            publishedSignalDbm = -127;
        }
        return;
    }

    int delta = connectedSignalDbm - publishedSignalDbm;
    if (publishedSignalDbm != -127 && delta > -2 && delta < 2) return;

    uint64_t now = 0;
    clock_get_uptime(&now);
    if (lastSignalPublishTime != 0) {
        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(now - lastSignalPublishTime, &elapsedNs);
        if (elapsedNs < 3000000000ULL) return;
    }

    char signal[16];
    snprintf(signal, sizeof(signal), "%d", connectedSignalDbm);
    setProperty("SignalStrength", signal);
    RTW_DEBUG_PROPERTY("Debug_RSSI_Last", signal);
    publishedSignalDbm = connectedSignalDbm;
    lastSignalPublishTime = now;
}

bool RealtekRTL8822C::setupRxPacketPool() {
    if (!rxPacketPoolLock || rxPacketPoolWorkLoop || rxPacketPoolTimer)
        return false;

    rxPacketPoolWorkLoop = IOWorkLoop::workLoop();
    if (!rxPacketPoolWorkLoop) return false;

    rxPacketPoolTimer = IOTimerEventSource::timerEventSource(
        this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this,
                             &RealtekRTL8822C::rxPacketPoolTimerFired));
    if (!rxPacketPoolTimer ||
        rxPacketPoolWorkLoop->addEventSource(rxPacketPoolTimer) != kIOReturnSuccess) {
        teardownRxPacketPool();
        return false;
    }

    IOInterruptState interruptState =
        IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
    rxPacketPoolStopping = false;
    rxPacketPoolRefillPending = false;
    rxPacketPoolMinimum = kRtwRxPacketPoolCapacity;
    IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);

    // This one-time reserve is built before hardware interrupts are enabled.
    // Subsequent replacements are allocated only on rxPacketPoolWorkLoop.
    refillRxPacketPool(kRtwRxPacketPoolCapacity);

    interruptState = IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
    UInt32 ready = rxPacketPoolCount;
    bool scheduleRefill = ready < kRtwRxPacketPoolCapacity &&
                          !rxPacketPoolStopping;
    if (scheduleRefill) rxPacketPoolRefillPending = true;
    IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);

    // A small partial reserve would immediately fall back into allocation in
    // the RX poll. Require enough packets to cover several complete slices.
    if (ready < 128) {
        teardownRxPacketPool();
        return false;
    }
    if (scheduleRefill) rxPacketPoolTimer->setTimeoutUS(1000);

    char db[192];
    snprintf(db, sizeof(db),
             "available=1 capacity=%u ready=%u low=%u refill_budget=%u worker=independent",
             (unsigned int)kRtwRxPacketPoolCapacity, (unsigned int)ready,
             (unsigned int)kRtwRxPacketPoolLowWatermark,
             (unsigned int)kRtwRxPacketPoolRefillBudget);
    RTW_DEBUG_PROPERTY("Debug_RX_Packet_Pool", db);
    return true;
}

void RealtekRTL8822C::teardownRxPacketPool() {
    if (rxPacketPoolLock) {
        IOInterruptState interruptState =
            IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
        rxPacketPoolStopping = true;
        rxPacketPoolRefillPending = false;
        IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);
    }

    if (rxPacketPoolTimer) rxPacketPoolTimer->cancelTimeout();
    if (rxPacketPoolTimer && rxPacketPoolWorkLoop)
        rxPacketPoolWorkLoop->removeEventSource(rxPacketPoolTimer);
    if (rxPacketPoolTimer) {
        rxPacketPoolTimer->release();
        rxPacketPoolTimer = nullptr;
    }
    if (rxPacketPoolWorkLoop) {
        rxPacketPoolWorkLoop->release();
        rxPacketPoolWorkLoop = nullptr;
    }

    while (rxPacketPoolLock) {
        mbuf_t packet = nullptr;
        IOInterruptState interruptState =
            IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
        if (rxPacketPoolCount != 0) {
            packet = rxPacketPool[rxPacketPoolHead];
            rxPacketPool[rxPacketPoolHead] = nullptr;
            rxPacketPoolHead = (rxPacketPoolHead + 1) % kRtwRxPacketPoolCapacity;
            rxPacketPoolCount--;
        }
        IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);
        if (!packet) break;
        freePacket(packet);
    }
    rxPacketPoolHead = 0;
    rxPacketPoolTail = 0;
    rxPacketPoolCount = 0;
}

void RealtekRTL8822C::refillRxPacketPool(UInt32 budget) {
    if (!rxPacketPoolLock) return;
    IOInterruptState interruptState =
        IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
    UInt32 missing = rxPacketPoolStopping ? 0 :
        kRtwRxPacketPoolCapacity - rxPacketPoolCount;
    IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);
    UInt32 requested = missing < budget ? missing : budget;
    if (requested == 0) return;

    // Allocate a packet list in one KPI call. v62 proved that the reserve never
    // emptied, but replenishing every consumed packet with an individual
    // mbuf_allocpacket() call merely moved allocator contention onto a second
    // high-priority work loop and regressed uplink. Batch allocation amortizes
    // the allocator/global-cache boundary while preserving nonblocking RX.
    mbuf_t list = nullptr;
    unsigned int chunks = 1;
    debugRxPacketPoolBatchCalls++;
    if (mbuf_allocpacket_list(requested, MBUF_DONTWAIT,
                              kRtwRxPacketBufferSize,
                              &chunks, &list) != 0 || !list) {
        debugRxPacketPoolAllocFailures++;
        return;
    }

    for (mbuf_t packet = list; packet; packet = mbuf_nextpkt(packet))
        mbuf_adj(packet, ETHER_ALIGN);

    interruptState = IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
    while (list && !rxPacketPoolStopping &&
           rxPacketPoolCount < kRtwRxPacketPoolCapacity) {
        mbuf_t packet = list;
        list = mbuf_nextpkt(packet);
        mbuf_setnextpkt(packet, nullptr);
        rxPacketPool[rxPacketPoolTail] = packet;
        rxPacketPoolTail = (rxPacketPoolTail + 1) % kRtwRxPacketPoolCapacity;
        rxPacketPoolCount++;
        debugRxPacketPoolRefilled++;
    }
    IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);

    while (list) {
        mbuf_t packet = list;
        list = mbuf_nextpkt(packet);
        mbuf_setnextpkt(packet, nullptr);
        freePacket(packet);
    }
}

mbuf_t RealtekRTL8822C::takeRxPacket() {
    if (!rxPacketPoolLock) return nullptr;
    mbuf_t packet = nullptr;
    bool scheduleRefill = false;
    IOInterruptState interruptState =
        IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
    if (!rxPacketPoolStopping && rxPacketPoolCount != 0) {
        packet = rxPacketPool[rxPacketPoolHead];
        rxPacketPool[rxPacketPoolHead] = nullptr;
        rxPacketPoolHead = (rxPacketPoolHead + 1) % kRtwRxPacketPoolCapacity;
        rxPacketPoolCount--;
        if (rxPacketPoolCount < rxPacketPoolMinimum)
            rxPacketPoolMinimum = rxPacketPoolCount;
        debugRxPacketPoolHits++;
        if (rxPacketPoolCount < kRtwRxPacketPoolLowWatermark &&
            !rxPacketPoolRefillPending) {
            rxPacketPoolRefillPending = true;
            scheduleRefill = true;
        }
    } else {
        debugRxPacketPoolMisses++;
    }
    IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);
    if (scheduleRefill && rxPacketPoolTimer)
        rxPacketPoolTimer->setTimeoutUS(1);
    return packet;
}

void RealtekRTL8822C::rxPacketPoolTimerFired(OSObject* owner,
                                             IOTimerEventSource* sender) {
    (void)owner;
    if (!rxPacketPoolLock || !sender) return;

    IOInterruptState interruptState =
        IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
    rxPacketPoolRefillPending = false;
    bool stopping = rxPacketPoolStopping;
    IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);
    if (stopping) return;

    refillRxPacketPool(kRtwRxPacketPoolRefillBudget);

    interruptState = IOSimpleLockLockDisableInterrupt(rxPacketPoolLock);
    // Preserve hysteresis: after one batch reaches the high side of the low
    // watermark, stay idle until RX consumes another full batch. Chasing exact
    // capacity every 100 us degenerates back into tiny allocator calls.
    bool more = !rxPacketPoolStopping &&
                rxPacketPoolCount < kRtwRxPacketPoolLowWatermark;
    if (more) rxPacketPoolRefillPending = true;
    IOSimpleLockUnlockEnableInterrupt(rxPacketPoolLock, interruptState);
    if (more) sender->setTimeoutUS(100);
}

void RealtekRTL8822C::free(void) {
    teardownRxPacketPool();
    resetRxBaSessions(false);
    resetWpaState(true);
    if (rxPacketPoolLock) {
        IOSimpleLockFree(rxPacketPoolLock);
        rxPacketPoolLock = nullptr;
    }
    if (beqLock) {
        IOSimpleLockFree(beqLock);
        beqLock = nullptr;
    }
    super::free();
}

IOService* RealtekRTL8822C::probe(IOService* provider, SInt32* score) {
    return super::probe(provider, score);
}

// ======= IO80211Controller WorkLoop =======





// ======= IO80211Controller createInterface =======



bool RealtekRTL8822C::configureInterface(IONetworkInterface* iface) {
    if (!super::configureInterface(iface)) return false;
    netif = OSDynamicCast(IOEthernetInterface, iface);
    if (netif) {
        netif->retain();
        RTW_DEBUG_PROPERTY("Debug_Netif_Config", "Configured OK");
    }
    return true;
}



// ======= IO80211Controller Pure Virtuals =======













bool RealtekRTL8822C::configure5GTxPower(UInt32 primaryChannel,
                                      UInt32 centerChannel,
                                      UInt8 bandwidth,
                                      bool publishDiagnostics) {
    if (!ioBase || countryCode[0] != 'U' || countryCode[1] != 'S') return false;
    UInt8 group;
    if (centerChannel <= 42) group = 0;
    else if (centerChannel <= 50) group = 1;
    else if (centerChannel >= 149 && centerChannel <= 155) group = 10;
    else if (centerChannel >= 157 && centerChannel <= 161) group = 11;
    else if (centerChannel == 165) group = 12;
    else return false;

    SInt8 ofdmLimit;
    SInt8 ht20OneLimit;
    switch (primaryChannel) {
        case 36: ofdmLimit = 74 - 68; ht20OneLimit = 72 - 64; break;
        case 40: ofdmLimit = 76 - 68; ht20OneLimit = 76 - 64; break;
        case 44:
        case 48: ofdmLimit = 76 - 68; ht20OneLimit = 76 - 64; break;
        case 149:
        case 153:
        case 157:
        case 161:
        case 165: ofdmLimit = 76 - 68; ht20OneLimit = 76 - 64; break;
        default: return false;
    }
    const SInt8 ht20TwoLimit = primaryChannel >= 149 ? 76 - 64 : 68 - 64;
    SInt8 htOneLimit = ht20OneLimit;
    SInt8 htTwoLimit = ht20TwoLimit;
    if (bandwidth >= 1) {
        if (centerChannel == 38) {
            htOneLimit = 66 - 64;
            htTwoLimit = 60 - 64;
        } else if (centerChannel == 46) {
            htOneLimit = 72 - 64;
            htTwoLimit = 68 - 64;
        } else if (centerChannel == 151 || centerChannel == 159) {
            htOneLimit = 72 - 64;
            htTwoLimit = 72 - 64;
        } else if (bandwidth == 2 && centerChannel == 42) {
            // Minimum across the constituent center-38/46 BW40 limits.
            htOneLimit = 66 - 64;
            htTwoLimit = 60 - 64;
        } else if (bandwidth == 2 && centerChannel == 155) {
            // Both constituent center-151/159 BW40 limits are 72/72.
            htOneLimit = 72 - 64;
            htTwoLimit = 72 - 64;
        } else {
            return false;
        }
    }
    SInt8 vhtOneLimit = htOneLimit;
    SInt8 vhtTwoLimit = htTwoLimit;
    if (bandwidth == 2) {
        if (centerChannel == 42) {
            vhtOneLimit = 64 - 64;
            vhtTwoLimit = 54 - 64;
        } else if (centerChannel == 155) {
            vhtOneLimit = 72 - 64;
            vhtTwoLimit = 62 - 64;
        } else {
            return false;
        }
    } else if (bandwidth > 2) {
        return false;
    }

    const UInt32 pathOffset[2] = { 0x22, 0x4c };
    const SInt8 ofdmByRate[8] = { 20, 20, 20, 16, 12, 8, 4, 0 };
    const SInt8 ht1sByRate[8] = { 24, 24, 20, 16, 12, 8, 4, 0 };
    const SInt8 vhtByRate[10] = { 24, 24, 20, 16, 12, 8, 4, 0, -4, -8 };
    SInt8 pathDiff[2][44];
    UInt8 reference[2];
    UInt8 rawDiff[2];
    UInt8 rawHt2Diff[2];
    UInt8 rawVht1Diff[2];
    UInt8 rawVht2Diff[2];
    SInt8 ht2Delta[2];
    SInt8 ht2Bw40Delta[2];
    SInt8 vht1Bw80Delta[2];
    SInt8 vht2Bw80Delta[2];
    UInt8 base[2];
    UInt8 upperBase[2];
    bool valid = true;

    for (UInt8 path = 0; path < 2; path++) {
        base[path] = logicalEfuseMap[pathOffset[path] + group];
        upperBase[path] = bandwidth == 2 ?
            logicalEfuseMap[pathOffset[path] + group + 1] : base[path];
        rawDiff[path] = logicalEfuseMap[pathOffset[path] + 14];
        rawHt2Diff[path] = logicalEfuseMap[pathOffset[path] + 15];
        rawVht1Diff[path] = logicalEfuseMap[pathOffset[path] + 20];
        rawVht2Diff[path] = logicalEfuseMap[pathOffset[path] + 21];
        if (base[path] == 0xff || upperBase[path] == 0xff ||
            rawDiff[path] == 0xff || rawHt2Diff[path] == 0xff ||
            (bandwidth == 2 &&
             (rawVht1Diff[path] == 0xff || rawVht2Diff[path] == 0xff))) {
            valid = false;
            continue;
        }
        SInt8 ofdmDelta = (SInt8)(rawDiff[path] & 0x0f);
        SInt8 ht20Delta = (SInt8)((rawDiff[path] >> 4) & 0x0f);
        if (ofdmDelta & 0x08) ofdmDelta = (SInt8)(ofdmDelta - 16);
        if (ht20Delta & 0x08) ht20Delta = (SInt8)(ht20Delta - 16);
        ht2Delta[path] = (SInt8)(rawHt2Diff[path] & 0x0f);
        if (ht2Delta[path] & 0x08) ht2Delta[path] = (SInt8)(ht2Delta[path] - 16);
        ht2Bw40Delta[path] = (SInt8)((rawHt2Diff[path] >> 4) & 0x0f);
        if (ht2Bw40Delta[path] & 0x08)
            ht2Bw40Delta[path] = (SInt8)(ht2Bw40Delta[path] - 16);
        vht1Bw80Delta[path] = (SInt8)((rawVht1Diff[path] >> 4) & 0x0f);
        if (vht1Bw80Delta[path] & 0x08)
            vht1Bw80Delta[path] = (SInt8)(vht1Bw80Delta[path] - 16);
        vht2Bw80Delta[path] = (SInt8)((rawVht2Diff[path] >> 4) & 0x0f);
        if (vht2Bw80Delta[path] & 0x08)
            vht2Bw80Delta[path] = (SInt8)(vht2Bw80Delta[path] - 16);
        int ofdmBase = (int)base[path] + (int)ofdmDelta * 2;
        int oneStreamBase;
        int twoStreamBase;
        if (bandwidth == 2) {
            int bw80Base = ((int)base[path] + (int)upperBase[path]) / 2;
            oneStreamBase = bw80Base + (int)vht1Bw80Delta[path] * 2;
            twoStreamBase = oneStreamBase + (int)vht2Bw80Delta[path] * 2;
        } else if (bandwidth == 1) {
            oneStreamBase = base[path];
            twoStreamBase = oneStreamBase + (int)ht2Bw40Delta[path] * 2;
        } else {
            oneStreamBase = (int)base[path] + (int)ht20Delta * 2;
            twoStreamBase = oneStreamBase + (int)ht2Delta[path] * 2;
        }
        SInt8 oneStreamLimit = htOneLimit < vhtOneLimit ? htOneLimit : vhtOneLimit;
        int ref = oneStreamBase + (oneStreamLimit < 0 ? oneStreamLimit : 0);
        if (ref < 0 || ref > 0x7f) {
            valid = false;
            continue;
        }
        reference[path] = (UInt8)ref;
        for (UInt8 i = 0; i < 8; i++) {
            SInt8 limit = ofdmByRate[i] < ofdmLimit ? ofdmByRate[i] : ofdmLimit;
            int power = ofdmBase + limit - (i < 2 ? 12 : 0);
            int diff = power - ref;
            if (power < 0 || power > 0x7f || diff < -64 || diff > 63) valid = false;
            pathDiff[path][i] = (SInt8)diff;
        }
        for (UInt8 i = 0; i < 8; i++) {
            SInt8 limit = ht1sByRate[i] < htOneLimit ? ht1sByRate[i] : htOneLimit;
            int power = oneStreamBase + limit - (i < 2 ? 12 : 0);
            int diff = power - ref;
            if (power < 0 || power > 0x7f || diff < -64 || diff > 63) valid = false;
            pathDiff[path][8 + i] = (SInt8)diff;
        }
        for (UInt8 i = 0; i < 8; i++) {
            SInt8 limit = ht1sByRate[i] < htTwoLimit ? ht1sByRate[i] : htTwoLimit;
            int power = twoStreamBase + limit - (i < 2 ? 12 : 0);
            int diff = power - ref;
            if (power < 0 || power > 0x7f || diff < -64 || diff > 63) valid = false;
            pathDiff[path][16 + i] = (SInt8)diff;
        }
        // VHT uses the same ten-rate by-rate sections at every bandwidth;
        // the selected base/delta and cross-width FCC limit differ above.
        for (UInt8 i = 0; i < 10; i++) {
            SInt8 limit = vhtByRate[i] < vhtOneLimit ? vhtByRate[i] : vhtOneLimit;
            int power = oneStreamBase + limit - (i < 2 ? 12 : 0);
            int diff = power - ref;
            if (power < 0 || power > 0x7f || diff < -64 || diff > 63) valid = false;
            pathDiff[path][24 + i] = (SInt8)diff;
        }
        for (UInt8 i = 0; i < 10; i++) {
            SInt8 limit = vhtByRate[i] < vhtTwoLimit ? vhtByRate[i] : vhtTwoLimit;
            int power = twoStreamBase + limit - (i < 2 ? 12 : 0);
            int diff = power - ref;
            if (power < 0 || power > 0x7f || diff < -64 || diff > 63) valid = false;
            pathDiff[path][34 + i] = (SInt8)diff;
        }
    }
    fiveGTwoStreamPowerValid = valid;
    if (!valid) return false;

    UInt8 commonDiff[44];
    for (UInt8 i = 0; i < 44; i++)
        commonDiff[i] = (UInt8)((pathDiff[0][i] < pathDiff[1][i]
                               ? pathDiff[0][i] : pathDiff[1][i]) & 0x7f);
    UInt32 ofdmLow = (UInt32)commonDiff[0] | ((UInt32)commonDiff[1] << 8) |
                       ((UInt32)commonDiff[2] << 16) | ((UInt32)commonDiff[3] << 24);
    UInt32 ofdmHigh = (UInt32)commonDiff[4] | ((UInt32)commonDiff[5] << 8) |
                        ((UInt32)commonDiff[6] << 16) | ((UInt32)commonDiff[7] << 24);
    UInt32 htLow = (UInt32)commonDiff[8] | ((UInt32)commonDiff[9] << 8) |
                     ((UInt32)commonDiff[10] << 16) | ((UInt32)commonDiff[11] << 24);
    UInt32 htHigh = (UInt32)commonDiff[12] | ((UInt32)commonDiff[13] << 8) |
                      ((UInt32)commonDiff[14] << 16) | ((UInt32)commonDiff[15] << 24);
    UInt32 ht2Low = (UInt32)commonDiff[16] | ((UInt32)commonDiff[17] << 8) |
                      ((UInt32)commonDiff[18] << 16) | ((UInt32)commonDiff[19] << 24);
    UInt32 ht2High = (UInt32)commonDiff[20] | ((UInt32)commonDiff[21] << 8) |
                       ((UInt32)commonDiff[22] << 16) | ((UInt32)commonDiff[23] << 24);
    UInt32 vht1Low = (UInt32)commonDiff[24] | ((UInt32)commonDiff[25] << 8) |
                       ((UInt32)commonDiff[26] << 16) | ((UInt32)commonDiff[27] << 24);
    UInt32 vht1High = (UInt32)commonDiff[28] | ((UInt32)commonDiff[29] << 8) |
                        ((UInt32)commonDiff[30] << 16) | ((UInt32)commonDiff[31] << 24);
    UInt32 vhtMix = (UInt32)commonDiff[32] | ((UInt32)commonDiff[33] << 8) |
                      ((UInt32)commonDiff[34] << 16) | ((UInt32)commonDiff[35] << 24);
    UInt32 vht2Mid = (UInt32)commonDiff[36] | ((UInt32)commonDiff[37] << 8) |
                       ((UInt32)commonDiff[38] << 16) | ((UInt32)commonDiff[39] << 24);
    UInt32 vht2High = (UInt32)commonDiff[40] | ((UInt32)commonDiff[41] << 8) |
                        ((UInt32)commonDiff[42] << 16) | ((UInt32)commonDiff[43] << 24);

    write32Mask(0x1c90, BIT(15), 0);
    write32Mask(0x18e8, 0x0001fc00, reference[0]);
    write32Mask(0x41e8, 0x0001fc00, reference[1]);
    write32(0x3a04, ofdmLow);
    write32(0x3a08, ofdmHigh);
    write32(0x3a0c, htLow);
    write32(0x3a10, htHigh);
    write32(0x3a14, ht2Low);
    write32(0x3a18, ht2High);
    write32(0x3a2c, vht1Low);
    write32(0x3a30, vht1High);
    write32(0x3a34, vhtMix);
    write32(0x3a38, vht2Mid);
    write32(0x3a3c, vht2High);
    OSSynchronizeIO();

    if (publishDiagnostics) {
        write32Mask(0x1c7c, BIT(23), 0);
        write32Mask(0x1c7c, 0x7f000000, 0x04);
        write32Mask(0x1c7c, BIT(23), 1);
        UInt8 report6m = (UInt8)read32Mask(0x2de8, 0xff);
        write32Mask(0x1c7c, BIT(23), 0);
        write32Mask(0x1c7c, 0x7f000000, 0x13);
        write32Mask(0x1c7c, BIT(23), 1);
        UInt8 reportMcs7 = (UInt8)read32Mask(0x2de8, 0xff);
        write32Mask(0x1c7c, BIT(23), 0);
        write32Mask(0x1c7c, 0x7f000000, 0x1b);
        write32Mask(0x1c7c, BIT(23), 1);
        UInt8 reportMcs15 = (UInt8)read32Mask(0x2de8, 0xff);
        write32Mask(0x1c7c, BIT(23), 0);
        write32Mask(0x1c7c, 0x7f000000, 0x35);
        write32Mask(0x1c7c, BIT(23), 1);
        UInt8 reportVht1Mcs9 = (UInt8)read32Mask(0x2de8, 0xff);
        write32Mask(0x1c7c, BIT(23), 0);
        write32Mask(0x1c7c, 0x7f000000, 0x3f);
        write32Mask(0x1c7c, BIT(23), 1);
        UInt8 reportVht2Mcs9 = (UInt8)read32Mask(0x2de8, 0xff);
        write32Mask(0x1c7c, BIT(23), 0);

        char db[1024];
        snprintf(db, sizeof(db),
             "fcc-us primary=%u center=%u bw=%u group=%u base=%02x/%02x upper=%02x/%02x rawdiff=%02x/%02x ht2raw=%02x/%02x vht80raw=%02x/%02x/%02x/%02x ht2delta20=%d/%d ht2delta40=%d/%d vhtdelta80=%d/%d/%d/%d ref=%u/%u limits=%d/%d/%d/%d/%d legacy=%08x/%08x ht=%08x/%08x/%08x/%08x vht=%08x/%08x/%08x/%08x/%08x report6m=%02x reportmcs7=%02x reportmcs15=%02x reportvht1mcs9=%02x reportvht2mcs9=%02x valid=1 streams=2",
             (unsigned int)primaryChannel, (unsigned int)centerChannel,
             bandwidth == 2 ? 80U : (bandwidth == 1 ? 40U : 20U), group,
             base[0], base[1], upperBase[0], upperBase[1], rawDiff[0], rawDiff[1],
             rawHt2Diff[0], rawHt2Diff[1], rawVht1Diff[0], rawVht1Diff[1],
             rawVht2Diff[0], rawVht2Diff[1],
             (int)ht2Delta[0], (int)ht2Delta[1],
             (int)ht2Bw40Delta[0], (int)ht2Bw40Delta[1],
             (int)vht1Bw80Delta[0], (int)vht1Bw80Delta[1],
             (int)vht2Bw80Delta[0], (int)vht2Bw80Delta[1],
             reference[0], reference[1], (int)ofdmLimit, (int)htOneLimit,
             (int)htTwoLimit, (int)vhtOneLimit, (int)vhtTwoLimit,
             (unsigned int)ofdmLow, (unsigned int)ofdmHigh,
             (unsigned int)htLow, (unsigned int)htHigh,
             (unsigned int)ht2Low, (unsigned int)ht2High,
             (unsigned int)vht1Low, (unsigned int)vht1High,
             (unsigned int)vhtMix, (unsigned int)vht2Mid,
             (unsigned int)vht2High, report6m, reportMcs7, reportMcs15,
             reportVht1Mcs9, reportVht2Mcs9);
        RTW_DEBUG_PROPERTY("Debug_5G_TXAGC", db);
        char powerDb[256];
        snprintf(powerDb, sizeof(powerDb),
             "tx-enabled primary=%u center=%u bw=%u group=%u base=%02x/%02x upper=%02x/%02x ref=%u/%u ht2raw=%02x/%02x vht1raw=%02x/%02x vht2raw=%02x/%02x valid=1",
             (unsigned int)primaryChannel, (unsigned int)centerChannel,
             bandwidth == 2 ? 80U : (bandwidth == 1 ? 40U : 20U), group,
             base[0], base[1], upperBase[0], upperBase[1], reference[0],
             reference[1], rawHt2Diff[0], rawHt2Diff[1], rawVht1Diff[0],
             rawVht1Diff[1], rawVht2Diff[0], rawVht2Diff[1]);
        RTW_DEBUG_PROPERTY("Debug_5G_Power", powerDb);
        char channelDb[160];
        snprintf(channelDb, sizeof(channelDb),
             "tx-enabled fcc-us primary=%u center=%u bw=%u ht=2ss vht=2ss non-dfs=1 primary_idx=%u rf18=%05x/%05x",
             (unsigned int)primaryChannel, (unsigned int)centerChannel,
             bandwidth == 2 ? 80U : (bandwidth == 1 ? 40U : 20U),
             targetPrimaryChannelIndex,
             (unsigned int)readRfMask(0, 0x18, 0xfffff),
             (unsigned int)readRfMask(1, 0x18, 0xfffff));
        RTW_DEBUG_PROPERTY("Debug_5G_Channel", channelDb);
    }
    return true;
}

// Channel switch hardware implementation
UInt32 RealtekRTL8822C::programChannelRf(UInt32 rfReg18A, UInt32 rfReg18B,
                                     UInt32 rfRxbb) {
    // Linux rtw8822c_set_channel_rf(): RF is programmed only after the BB and
    // MAC width/primary-subchannel state is complete.
    write32Mask(0x1c90, 0x100, 0); // REG_RSTB, BIT_RSTB_3WIRE = 0

    writeRfMask(0, 0xee, 0x04, 0x01); // RF_LUTWE2
    writeRfMask(0, 0x33, 0x1f, 0x12); // RF_LUTWA
    writeRfMask(0, 0x3f, 0xfffff, rfRxbb); // RF_LUTWD0
    writeRfMask(0, 0xee, 0x04, 0x00);
    writeRfMask(1, 0xee, 0x04, 0x01);
    writeRfMask(1, 0x33, 0x1f, 0x12);
    writeRfMask(1, 0x3f, 0xfffff, rfRxbb);
    writeRfMask(1, 0xee, 0x04, 0x00);

    writeRfMask(0, 0x18, 0xfffff, rfReg18A);
    writeRfMask(1, 0x18, 0xfffff, rfReg18B);
    write32Mask(0x1c90, 0x100, 1);
    write32Mask(0x1830, 0x20000000, 1);
    write32Mask(0x4130, 0x20000000, 1);
    OSSynchronizeIO();

    // Linux rtw8822c_toggle_igi(). Keep the guard used by the existing port
    // so an erased/uninitialised gain value cannot underflow.
    UInt32 igi = read32(0x1d70) & 0x7f;
    if (igi > 2 && igi < 0x7f) {
        write32Mask(0x1d70, 0x7f, igi - 2);
        write32Mask(0x1d70, 0x7f00, igi - 2);
        write32Mask(0x1d70, 0x7f, igi);
        write32Mask(0x1d70, 0x7f00, igi);
        OSSynchronizeIO();
    }
    return igi;
}

bool RealtekRTL8822C::setChannelHw(UInt32 channel, UInt32 flags, UInt8 bandwidth,
                                UInt8 primaryChannelIndex, bool scanSwitch) {
    if (!ioBase) return false;
    bool publishDiagnostics = !scanSwitch;
    bool is5GHz = channel > 14 || (flags & APPLE80211_C_FLAG_5GHZ) != 0;
    if (channel == 0 || channel > 177) return false;

    UInt32 orig_a = readRfMask(0, 0x18, 0xfffff);
    UInt32 orig_b = readRfMask(1, 0x18, 0xfffff);

    UInt32 rf_reg18_a = orig_a;
    UInt32 rf_reg18_b = orig_b;

    // Clear band/channel/BW fields
    rf_reg18_a &= ~(0x10300U | 0xffU | 0x60000U | 0x3000U);
    rf_reg18_b &= ~(0x10300U | 0xffU | 0x60000U | 0x3000U);

    if (is5GHz) {
        // Linux RF18_BAND_5G: BIT(16) | BIT(8).
        rf_reg18_a |= 0x10100U;
        rf_reg18_b |= 0x10100U;
        if (channel > 140) {
            rf_reg18_a |= 1U << 18;
            rf_reg18_b |= 1U << 18;
        } else if (channel >= 80) {
            rf_reg18_a |= 1U << 17;
            rf_reg18_b |= 1U << 17;
        }
    }
    // Channel number
    rf_reg18_a |= (channel & 0xff);
    rf_reg18_b |= (channel & 0xff);
    UInt32 rf_rxbb = 0x18;
    if (bandwidth == 1) {
        rf_reg18_a |= 0x002000U;
        rf_reg18_b |= 0x002000U;
        rf_rxbb = 0x10;
    } else if (bandwidth == 2) {
        rf_reg18_a |= 0x001000U;
        rf_reg18_b |= 0x001000U;
        rf_rxbb = 0x08;
    } else {
        bandwidth = 0;
        rf_reg18_a |= 0x003000U;
        rf_reg18_b |= 0x003000U;
    }

    // CCK TX Power Reference and full BB/MAC setup for 2.4 GHz CCK 1 Mbps rate (Channel 1-14)
    if (!is5GHz) {
        // --- 1. Linux 2.4 GHz + 20 MHz BB setup ---
        write32_clr(0x1a14, BIT(8) | BIT(9));       // BGCTRL
        write32_set(0x1a9c, BIT(20));               // TXF4
        write32_clr(0x0454, BIT(7));                // CCK_CHECK
        write32_clr(0x1a80, BIT(18));               // CCKTXONLY
        write32Mask(0x1c80, 0x3f000000, 0x0f);      // CCAMSK
        write32Mask(0x18ac, 0x0000f000, 0x05);      // RXAGC CCK A
        write32Mask(0x41ac, 0x0000f000, 0x05);      // RXAGC CCK B
        write32Mask(0x18ac, 0x000001f0, 0x06);      // RXAGC OFDM A
        write32Mask(0x41ac, 0x000001f0, 0x06);      // RXAGC OFDM B

        if (channel == 13 || channel == 14)
            write32Mask(0x0c30, 0x00000fff, 0x969); // SCOTRK
        else if (channel == 11 || channel == 12)
            write32Mask(0x0c30, 0x00000fff, 0x96a); // SCOTRK
        else
            write32Mask(0x0c30, 0x00000fff, 0x9aa); // SCOTRK

        if (channel == 14) {
            write32Mask(0x1a20, 0xFFFF0000, 0x3da0);
            write32Mask(0x1a24, 0xFFFFFFFF, 0x4962c931);
            write32Mask(0x1a28, 0x0000FFFF, 0x6aa3);
            write32Mask(0x1a98, 0xFFFF0000, 0xaa7b);
            write32Mask(0x1a9c, 0x0000FFFF, 0xf3d7);
            write32Mask(0x1aa0, 0xFFFFFFFF, 0x0);
            write32Mask(0x1aac, 0xFFFFFFFF, 0xff012455);
            write32Mask(0x1ab0, 0xFFFFFFFF, 0xffff);
        } else {
            write32Mask(0x1a20, 0xFFFF0000, 0x5284);
            write32Mask(0x1a24, 0xFFFFFFFF, 0x3e18fec8);
            write32Mask(0x1a28, 0x0000FFFF, 0x0a88);
            write32Mask(0x1a98, 0xFFFF0000, 0xacc4);
            write32Mask(0x1a9c, 0x0000FFFF, 0xc8b2);
            write32Mask(0x1aa0, 0xFFFFFFFF, 0x00faf0de);
            write32Mask(0x1aac, 0xFFFFFFFF, 0x00122344);
            write32Mask(0x1ab0, 0xFFFFFFFF, 0x0fffffff);
        }

        if (channel == 13)
            write32Mask(0x0808, 0x00000070, 0x03);  // TXDFIR0
        else
            write32Mask(0x0808, 0x00000070, 0x01);  // TXDFIR0
        write32Mask(0x0810, 0x00003ff0, 0x19b);     // DFIRBW
        write32Mask(0x09b0, 0x0000ffff, 0);         // TXBWCTL 20 MHz
        write32Mask(0x09b4, 0x00000700, 0x07);      // TXCLK
        write32Mask(0x09b4, 0x00700000, 0x06);
        write32Mask(0x1abc, BIT(30), 0);
        write32Mask(0x088c, 0x0000f000, 1);
        write32Mask(0x0cbc, BIT(21), 0);

        // --- 2. MAC setup ---
        write8(0x0483, 0);                           // DATA_SC
        write32(0x0668, read32(0x0668) & ~(BIT(7) | BIT(8)));
        write32Mask(0x0024, 0x00300000, 0);          // MAC clock 80 MHz
        write8(0x055c, 80);
        write8(0x0638, 80);

        // Linux order: BB -> MAC -> RF -> IGI -> TX power.
        programChannelRf(rf_reg18_a, rf_reg18_b, rf_rxbb);

        // --- 3. CCK & OFDM TX Power Reference setup ---
        UInt8 cck_a = logicalEfuseMap[0x10];
        UInt8 ofdm_a = logicalEfuseMap[0x16];
        UInt8 cck_b = logicalEfuseMap[0x3a];
        UInt8 ofdm_b = logicalEfuseMap[0x40];

        if (cck_a != 0xFF && cck_a <= 0x7F) {
            write32Mask(0x18a0, 0x007f0000, cck_a); // CCK reference path A
            if (publishDiagnostics)
                RTW_DEBUG_LOG("RealtekRTL8822C: Applied Path A CCK Power Ref: %u\n", cck_a);
        }
        if (ofdm_a != 0xFF && ofdm_a <= 0x7F) {
            write32Mask(0x18e8, 0x0001fc00, ofdm_a); // OFDM reference path A
            if (publishDiagnostics)
                RTW_DEBUG_LOG("RealtekRTL8822C: Applied Path A OFDM Power Ref: %u\n", ofdm_a);
        }

        if (cck_b != 0xFF && cck_b <= 0x7F) {
            write32Mask(0x41a0, 0x007f0000, cck_b); // CCK reference path B
            if (publishDiagnostics)
                RTW_DEBUG_LOG("RealtekRTL8822C: Applied Path B CCK Power Ref: %u\n", cck_b);
        }
        if (ofdm_b != 0xFF && ofdm_b <= 0x7F) {
            write32Mask(0x41e8, 0x0001fc00, ofdm_b); // OFDM reference path B
            if (publishDiagnostics)
                RTW_DEBUG_LOG("RealtekRTL8822C: Applied Path B OFDM Power Ref: %u\n", ofdm_b);
        }

        // Keep the Linux type0 +0x0c delta for 1 Mbps CCK; other rates use their reference.
        write32Mask(0x1c90, 1U << 15, 0); // REG_TXAGC_CTRL
        write32(0x3a00, 0x0000000c); // CCK 1 Mbps delta
        write32(0x3a04, 0); // OFDM legacy low (6M/9M/12M/18M)
        write32(0x3a08, 0); // OFDM legacy high (24M/36M/48M/54M)
        write32(0x3a0c, 0); // HT MCS0-3
        write32(0x3a10, 0); // HT MCS4-7

        if (publishDiagnostics) {
            // TXAGC table memory is read through the BB report mux, not 0x3a00.
            write32Mask(0x1c7c, BIT(23), 0);
            write32Mask(0x1c7c, 0x7f000000, 0); // DESC_RATE1M
            write32Mask(0x1c7c, BIT(23), 1);
            UInt32 txAgcReportCtrlEnabled = read32(0x1c7c);
            UInt8 txAgcReportRaw = (UInt8)read32Mask(0x2de8, 0xff);
            SInt8 txAgcReportDiff = (txAgcReportRaw & BIT(6))
                ? (SInt8)(txAgcReportRaw | BIT(7))
                : (SInt8)txAgcReportRaw;
            write32Mask(0x1c7c, BIT(23), 0);

            write32Mask(0x1c7c, 0x7f000000, 0x04); // DESC_RATE6M
            write32Mask(0x1c7c, BIT(23), 1);
            UInt32 txAgcOfdmReportCtrlEnabled = read32(0x1c7c);
            UInt8 txAgcOfdmReportRaw = (UInt8)read32Mask(0x2de8, 0xff);
            SInt8 txAgcOfdmReportDiff = (txAgcOfdmReportRaw & BIT(6))
                ? (SInt8)(txAgcOfdmReportRaw | BIT(7))
                : (SInt8)txAgcOfdmReportRaw;
            write32Mask(0x1c7c, BIT(23), 0);

            char txAgcDb[256];
            snprintf(txAgcDb, sizeof(txAgcDb),
                     "3a00=%08x rate0_raw=%02x rate0_diff=%d rate0_ctrl=%08x rate4_raw=%02x rate4_diff=%d rate4_ctrl=%08x report_ctrl_off=%08x txagc_ctrl=%08x",
                     (unsigned int)read32(0x3a00), (unsigned int)txAgcReportRaw,
                     (int)txAgcReportDiff, (unsigned int)txAgcReportCtrlEnabled,
                     (unsigned int)txAgcOfdmReportRaw, (int)txAgcOfdmReportDiff,
                     (unsigned int)txAgcOfdmReportCtrlEnabled,
                     (unsigned int)read32(0x1c7c),
                     (unsigned int)read32(0x1c90));
            RTW_DEBUG_PROPERTY("Debug_CCK_TxAgc_Write", txAgcDb);
        }
    } else {
        // Linux rtw8822c_set_channel_bb(), 5 GHz / 20 MHz receive path.
        write32_set(0x1a80, BIT(18));
        write32_set(0x0454, BIT(7));
        write32_set(0x1a14, BIT(8) | BIT(9));
        write32_clr(0x1a9c, BIT(20));
        write32Mask(0x1c80, 0x3f000000, 0x22);
        write32Mask(0x0808, 0x00000070, 0x03);

        UInt32 agcBand = 1;
        if (channel >= 100 && channel <= 144) agcBand = 2;
        else if (channel > 144) agcBand = 3;
        write32Mask(0x18ac, 0x000001f0, agcBand);
        write32Mask(0x41ac, 0x000001f0, agcBand);

        UInt32 scoTrack = 0x411;
        if (channel <= 51) scoTrack = 0x494;
        else if (channel <= 55) scoTrack = 0x493;
        else if (channel <= 111) scoTrack = 0x453;
        else if (channel <= 119) scoTrack = 0x452;
        else if (channel <= 172) scoTrack = 0x412;
        write32Mask(0x0c30, 0x00000fff, scoTrack);

        if (bandwidth == 1) {
            write32Mask(0x1a00, BIT(4), primaryChannelIndex == 1 ? 1 : 0);
            write32Mask(0x09b0, 0x0000000f, 0x5);
            write32Mask(0x09b0, 0x000000c0, 0);
            write32Mask(0x09b0, 0x0000ff00,
                        (UInt32)primaryChannelIndex |
                        ((UInt32)primaryChannelIndex << 4));
            write32Mask(0x1abc, BIT(30), 1);
            write32Mask(0x088c, 0x0000f000, 1);
            write32Mask(0x0cbc, BIT(21), 1);
        } else if (bandwidth == 2) {
            write32Mask(0x09b0, 0x0000000f, 0xa);
            write32Mask(0x09b0, 0x000000c0, 0);
            write32Mask(0x09b0, 0x0000ff00,
                        (UInt32)primaryChannelIndex |
                        ((UInt32)primaryChannelIndex << 4));
            write32Mask(0x088c, 0x0000f000, 6);
            write32Mask(0x0cbc, BIT(21), 1);
        } else {
            write32Mask(0x0810, 0x00003ff0, 0x19b);
            write32Mask(0x09b0, 0x0000000f, 0);
            write32Mask(0x09b0, 0x0000ffc0, 0);
            write32Mask(0x09b4, 0x00000700, 0x07);
            write32Mask(0x09b4, 0x00700000, 0x06);
            write32Mask(0x1abc, BIT(30), 0);
            write32Mask(0x088c, 0x0000f000, 1);
            write32Mask(0x0cbc, BIT(21), 0);
        }

        // Linux rtw_set_channel_mac(). DATA_SC identifies the primary 20 MHz
        // subchannel inside the configured 40/80 MHz center channel.
        UInt8 txsc40 = 0;
        if (bandwidth == 2)
            txsc40 = (primaryChannelIndex == 1 || primaryChannelIndex == 3) ? 9 : 10;
        write8(0x0483, (UInt8)(primaryChannelIndex | (txsc40 << 4)));
        UInt32 trxProtocol = read32(0x0668) & ~(BIT(7) | BIT(8));
        if (bandwidth == 1) trxProtocol |= BIT(7);
        else if (bandwidth == 2) trxProtocol |= BIT(8);
        write32(0x0668, trxProtocol);
        write32Mask(0x0024, 0x00300000, 0);
        write8(0x055c, 80);
        write8(0x0638, 80);

        // Linux order: BB -> MAC -> RF -> IGI. 5 GHz TX power is installed by
        // configure5GTxPower() immediately after this function returns.
        programChannelRf(rf_reg18_a, rf_reg18_b, rf_rxbb);

        // RTL8822C logical EFUSE contains a packed 18-byte 2.4 GHz power
        // block followed by this path's 24-byte 5 GHz block. Mirror Linux's
        // 5 GHz base selection and signed four-bit HT/OFDM deltas, but keep
        // TX blocked until these board-specific values are runtime-verified.
        UInt8 powerGroup = 0xff;
        if (channel <= 42) powerGroup = 0;
        else if (channel <= 50) powerGroup = 1;
        else if (channel <= 58) powerGroup = 2;
        else if (channel <= 64) powerGroup = 3;
        else if (channel <= 106) powerGroup = 4;
        else if (channel <= 114) powerGroup = 5;
        else if (channel <= 122) powerGroup = 6;
        else if (channel <= 130) powerGroup = 7;
        else if (channel <= 138) powerGroup = 8;
        else if (channel <= 144) powerGroup = 9;
        else if (channel <= 155) powerGroup = 10;
        else if (channel <= 161) powerGroup = 11;
        else if (channel <= 171) powerGroup = 12;
        else if (channel <= 177) powerGroup = 13;

        const UInt32 pathA5g = 0x22;
        const UInt32 pathB5g = 0x4c;
        UInt8 baseA = powerGroup < 14 ? logicalEfuseMap[pathA5g + powerGroup] : 0xff;
        UInt8 baseB = powerGroup < 14 ? logicalEfuseMap[pathB5g + powerGroup] : 0xff;
        UInt8 diffA = logicalEfuseMap[pathA5g + 14];
        UInt8 diffB = logicalEfuseMap[pathB5g + 14];
        SInt8 ofdmDiffA = (SInt8)(diffA & 0x0f);
        SInt8 ofdmDiffB = (SInt8)(diffB & 0x0f);
        SInt8 ht20DiffA = (SInt8)((diffA >> 4) & 0x0f);
        SInt8 ht20DiffB = (SInt8)((diffB >> 4) & 0x0f);
        if (ofdmDiffA & 0x08) ofdmDiffA = (SInt8)(ofdmDiffA - 16);
        if (ofdmDiffB & 0x08) ofdmDiffB = (SInt8)(ofdmDiffB - 16);
        if (ht20DiffA & 0x08) ht20DiffA = (SInt8)(ht20DiffA - 16);
        if (ht20DiffB & 0x08) ht20DiffB = (SInt8)(ht20DiffB - 16);
        int ofdmA = (int)baseA + (int)ofdmDiffA * 2;
        int ofdmB = (int)baseB + (int)ofdmDiffB * 2;
        int ht20A = (int)baseA + (int)ht20DiffA * 2;
        int ht20B = (int)baseB + (int)ht20DiffB * 2;
        bool powerValid = powerGroup < 14 && baseA != 0xff && baseB != 0xff &&
                          diffA != 0xff && diffB != 0xff &&
                          ofdmA >= 0 && ofdmA <= 0x7f && ofdmB >= 0 && ofdmB <= 0x7f &&
                          ht20A >= 0 && ht20A <= 0x7f && ht20B >= 0 && ht20B <= 0x7f;

        if (publishDiagnostics) {
            char powerDb[256];
            snprintf(powerDb, sizeof(powerDb),
                     "ch=%u group=%u A:base=%02x diff=%02x ofdm_d=%d ht20_d=%d ofdm=%d ht20=%d B:base=%02x diff=%02x ofdm_d=%d ht20_d=%d ofdm=%d ht20=%d valid=%d tx=blocked",
                     (unsigned int)channel, (unsigned int)powerGroup,
                     baseA, diffA, (int)ofdmDiffA, (int)ht20DiffA, ofdmA, ht20A,
                     baseB, diffB, (int)ofdmDiffB, (int)ht20DiffB, ofdmB, ht20B,
                     powerValid ? 1 : 0);
            RTW_DEBUG_PROPERTY("Debug_5G_Power", powerDb);

            char tableDb[256];
            snprintf(tableDb, sizeof(tableDb),
                     "A=%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x d=%02x B=%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x/%02x d=%02x",
                     logicalEfuseMap[pathA5g + 0], logicalEfuseMap[pathA5g + 1],
                     logicalEfuseMap[pathA5g + 2], logicalEfuseMap[pathA5g + 3],
                     logicalEfuseMap[pathA5g + 4], logicalEfuseMap[pathA5g + 5],
                     logicalEfuseMap[pathA5g + 6], logicalEfuseMap[pathA5g + 7],
                     logicalEfuseMap[pathA5g + 8], logicalEfuseMap[pathA5g + 9],
                     logicalEfuseMap[pathA5g + 10], logicalEfuseMap[pathA5g + 11],
                     logicalEfuseMap[pathA5g + 12], logicalEfuseMap[pathA5g + 13], diffA,
                     logicalEfuseMap[pathB5g + 0], logicalEfuseMap[pathB5g + 1],
                     logicalEfuseMap[pathB5g + 2], logicalEfuseMap[pathB5g + 3],
                     logicalEfuseMap[pathB5g + 4], logicalEfuseMap[pathB5g + 5],
                     logicalEfuseMap[pathB5g + 6], logicalEfuseMap[pathB5g + 7],
                     logicalEfuseMap[pathB5g + 8], logicalEfuseMap[pathB5g + 9],
                     logicalEfuseMap[pathB5g + 10], logicalEfuseMap[pathB5g + 11],
                     logicalEfuseMap[pathB5g + 12], logicalEfuseMap[pathB5g + 13], diffB);
            RTW_DEBUG_PROPERTY("Debug_5G_EFUSE_Table", tableDb);

            char fiveGDb[192];
            snprintf(fiveGDb, sizeof(fiveGDb),
                     "rx-only ch=%u agc=%u sco=%03x rf18=%05x/%05x tx=blocked-pending-5g-power",
                     (unsigned int)channel, (unsigned int)agcBand,
                     (unsigned int)scoTrack, (unsigned int)rf_reg18_a,
                     (unsigned int)rf_reg18_b);
            RTW_DEBUG_PROPERTY("Debug_5G_Channel", fiveGDb);
        }
    }

    if (publishDiagnostics) {
        UInt32 igi = read32(0x1d70) & 0x7f;
        char rfDb[256];
        snprintf(rfDb, sizeof(rfDb), "center=%u bw=%u primary_idx=%u (5G=%d) origA=0x%05x origB=0x%05x newA=0x%05x newB=0x%05x rxbb=%02x igi=%u",
                 (unsigned int)channel, bandwidth == 2 ? 80 : (bandwidth == 1 ? 40 : 20),
                 primaryChannelIndex, is5GHz, (unsigned int)orig_a,
                 (unsigned int)orig_b, (unsigned int)rf_reg18_a,
                 (unsigned int)rf_reg18_b, (unsigned int)rf_rxbb,
                 (unsigned int)igi);
        RTW_DEBUG_PROPERTY("Debug_Last_RF_Reg18", rfDb);
    }

    UInt32 reportedChannel = channel;
    if (bandwidth == 1) {
        if (primaryChannelIndex == 1) reportedChannel = channel + 2;
        else if (primaryChannelIndex == 2) reportedChannel = channel - 2;
    } else if (bandwidth == 2) {
        if (primaryChannelIndex == 1) reportedChannel = channel + 2;
        else if (primaryChannelIndex == 2) reportedChannel = channel - 2;
        else if (primaryChannelIndex == 3) reportedChannel = channel + 6;
        else if (primaryChannelIndex == 4) reportedChannel = channel - 6;
    }
    currentChannel.channel = reportedChannel;
    // apple80211_var.h values; the private IO80211 compatibility headers used
    // by this target do not expose the 40/80 symbolic names consistently.
    UInt32 widthFlag = bandwidth == 2 ? 0x400U :
                         (bandwidth == 1 ? 0x004U : APPLE80211_C_FLAG_20MHZ);
    currentChannel.flags = (is5GHz ? APPLE80211_C_FLAG_5GHZ : APPLE80211_C_FLAG_2GHZ) |
                           widthFlag;

    if (publishDiagnostics) {
        char bandwidthHw[256];
        snprintf(bandwidthHw, sizeof(bandwidthHw),
                 "primary=%u center=%u bw=%u primary_idx=%u data_sc=%02x trx=%08x txbw=%08x sbd=%08x pt=%08x rf18=%05x/%05x",
                 (unsigned int)reportedChannel, (unsigned int)channel,
                 bandwidth == 2 ? 80U : (bandwidth == 1 ? 40U : 20U),
                 primaryChannelIndex, read8(0x0483), (unsigned int)read32(0x0668),
                 (unsigned int)read32(0x09b0), (unsigned int)read32(0x088c),
                 (unsigned int)read32(0x0cbc),
                 (unsigned int)readRfMask(0, 0x18, 0xfffff),
                 (unsigned int)readRfMask(1, 0x18, 0xfffff));
        RTW_DEBUG_PROPERTY("Debug_Bandwidth_Hardware", bandwidthHw);
    }

    // Linux rtw8822c_set_channel() returns immediately after BB/MAC/RF and
    // IGI programming. Scans provide their dwell with a timer instead of
    // busy-waiting on the controller work loop.
    if (!scanSwitch) IODelay(20000);
    return true;
}


#define MASKDWORD 0xffffffff
#define GENMASK(h, l) (((1U << ((h) - (l) + 1U)) - 1U) << (l))
#include "rtw8822c_tables.h"

bool RealtekRTL8822C::parsePowerSeq(const struct rtw_pwr_seq_cmd *cmd_seq) {
    const struct rtw_pwr_seq_cmd *cmd = cmd_seq;
    while (cmd->cmd != RTW_PWR_CMD_END) {
        if (!(cmd->intf_msk & RTW_PWR_INTF_PCI_MSK)) {
            cmd++;
            continue; // Skip non-PCI commands
        }
        if (cmd->base_addr != RTW_PWR_ADDR_MAC) {
            cmd++;
            continue; // Skip SDIO/USB bases
        }

        UInt16 offset = cmd->offset;
        UInt8 value;
        int poll = 10000;

        switch (cmd->cmd) {
            case RTW_PWR_CMD_WRITE:
                value = read8(offset);
                value &= ~(cmd->mask);
                value |= (cmd->value & cmd->mask);
                write8(offset, value);
                break;
            case RTW_PWR_CMD_POLLING:
                do {
                    value = read8(offset);
                    if ((value & cmd->mask) == (cmd->value & cmd->mask))
                        break;
                    IODelay(100);
                    poll--;
                } while (poll > 0);
                if (poll == 0) return false;
                break;
            default:
                break;
        }
        cmd++;
    }
    return true;
}

bool RealtekRTL8822C::failStart(IOService* provider, const char* status,
                               const char* event) {
    if (status) setProperty("DriverStatus", status);
    if (event) traceEvent(event);
    stop(provider);
    return false;
}

bool RealtekRTL8822C::prepareDmaMapping(IOBufferMemoryDescriptor* descriptor,
                                       IODMACommand* command,
                                       addr64_t* address) {
    if (!descriptor || !command || !address) return false;
    *address = 0;
    if (descriptor->prepare() != kIOReturnSuccess) return false;
    if (command->setMemoryDescriptor(descriptor) != kIOReturnSuccess)
        return false;
    if (command->prepare() != kIOReturnSuccess) return false;
    IODMACommand::Segment32 segment;
    UInt32 segmentCount = 1;
    UInt64 offset = 0;
    if (command->gen32IOVMSegments(&offset, &segment, &segmentCount) !=
            kIOReturnSuccess ||
        segmentCount != 1 || segment.fLength < descriptor->getLength() ||
        segment.fIOVMAddr == 0) return false;
    *address = segment.fIOVMAddr;
    return true;
}

bool RealtekRTL8822C::start(IOService* provider) {
    IOPCIDevice* earlyPciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!earlyPciDevice) return false;

    // IONetworkController::start() joins the controller to the provider's PM
    // tree. Mark the PCI provider before that can trigger an eligibility scan.
    earlyPciDevice->setProperty("IOPMIsPowerManaged", kOSBooleanTrue);

    if (!super::start(provider)) {
        return false;
    }

    RTW_DEBUG_LOG("RealtekRTL8822C: Starting up...\n");

    pciDevice = earlyPciDevice;

    pciDevice->retain();

    IOOutputQueue* outputQueue = getOutputQueue();
    IOBasicOutputQueue* basicOutputQueue =
        OSDynamicCast(IOBasicOutputQueue, outputQueue);
    if (!outputQueue || !basicOutputQueue) {
        return failStart(provider, "Failed to create output queue",
                         "start:output-queue-failed");
    }
    char outputQueueDb[128];
    snprintf(outputQueueDb, sizeof(outputQueueDb),
             "type=basic capacity=%u size=%u state=%u",
             (unsigned int)basicOutputQueue->getCapacity(),
             (unsigned int)basicOutputQueue->getSize(),
             (unsigned int)basicOutputQueue->getState());
    RTW_DEBUG_PROPERTY("Debug_Output_Queue", outputQueueDb);

    setProperty("DriverStatus", "Started IOEthernetController");
    setProperty("DriverVersion", RTW_VERSION);
    setProperty("BuildConfiguration", RTW_DEBUG ? "Debug" : "Release");
    setProperty("InterfaceUserEnabled", kOSBooleanTrue);
    setProperty("PowerState", "Awake");
    RTW_DEBUG_PROPERTY("Debug_PM_Provider_Managed", true);
    setProperty("InterfaceState", "Enabled");
    RTW_DEBUG_PROPERTY("Debug_C2H_Kernel_Log", "failures-only; full last C2H remains in Debug_C2H_Last_Rpt");

    pciDevice->setMemoryEnable(true);
    pciDevice->setBusMasterEnable(true);

    // Configure PCIe capability: Relaxed Ordering and No Snoop
    UInt8 pcie_cap_offset = 0;
    UInt8 cap_ptr = pciDevice->configRead8(0x34); // Capabilities Pointer
    while (cap_ptr != 0) {
        UInt8 cap_id = pciDevice->configRead8(cap_ptr);
        if (cap_id == 0x10) { // PCI_CAP_ID_EXP (PCI Express Capability)
            pcie_cap_offset = cap_ptr;
            break;
        }
        cap_ptr = pciDevice->configRead8(cap_ptr + 1);
    }
    if (pcie_cap_offset != 0) {
        UInt16 dev_ctl = pciDevice->configRead16(pcie_cap_offset + 0x08);
        RTW_DEBUG_PROPERTY("Debug_PCIe_DevCtl_Before", dev_ctl, 16);
        dev_ctl |= (1 << 11); // Enable No Snoop
        dev_ctl |= (1 << 4);  // Enable Relaxed Ordering
        pciDevice->configWrite16(pcie_cap_offset + 0x08, dev_ctl);
#if RTW_DEBUG
        UInt16 dev_ctl_after = pciDevice->configRead16(pcie_cap_offset + 0x08);
        RTW_DEBUG_PROPERTY("Debug_PCIe_DevCtl_After", dev_ctl_after, 16);
#endif

        UInt16 link_ctl = pciDevice->configRead16(pcie_cap_offset + 0x10);
        RTW_DEBUG_PROPERTY("Debug_PCIe_LinkCtl_Before", link_ctl, 16);
        link_ctl &= ~0x0003; // Disable ASPM (clear bits 0-1)
        pciDevice->configWrite16(pcie_cap_offset + 0x10, link_ctl);
#if RTW_DEBUG
        UInt16 link_ctl_after = pciDevice->configRead16(pcie_cap_offset + 0x10);
        RTW_DEBUG_PROPERTY("Debug_PCIe_LinkCtl_After", link_ctl_after, 16);
#endif
    }

    bar2Desc = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!bar2Desc) {
        return failStart(provider, "Failed to get BAR2 descriptor",
                         "start:bar2-descriptor-failed");
    }

    bar2Map = bar2Desc->map();
    if (!bar2Map) {
        return failStart(provider, "Failed to map BAR2",
                         "start:bar2-map-failed");
    }

    ioBase = (volatile UInt8*)bar2Map->getVirtualAddress();
    traceEvent("start:bar2-mapped");

    // Configure and publish network media
    OSDictionary* mediumDict = OSDictionary::withCapacity(1);
    IONetworkMedium* medium = IONetworkMedium::medium(kIOMediumIEEE80211Auto, 0);
    if (mediumDict && medium) {
        IONetworkMedium::addMedium(mediumDict, medium);
        publishMediumDictionary(mediumDict);
        setSelectedMedium(medium);
        setLinkStatus(kIONetworkLinkValid, medium);
        medium->release();
    }
    if (mediumDict) {
        mediumDict->release();
    }
    if (!mediumDict || !medium) {
        if (medium) medium->release();
        return failStart(provider, "Failed to publish network medium",
                         "start:medium-failed");
    }

    // Setup Interrupts - use IO80211WorkLoop (createWorkLoop already called by super)
    if (!workLoop) {
        workLoop = getWorkLoop();
    }
    if (workLoop) {
        int interruptType = 0;
        if (pciDevice->getInterruptType(0, &interruptType) == kIOReturnSuccess) {
            if (interruptType & kIOInterruptTypePCIMessaged) {
                RTW_DEBUG_PROPERTY("Debug_MSI", "MSI automatically enabled by OS");
            } else {
                RTW_DEBUG_PROPERTY("Debug_MSI", "Using legacy pin interrupt");
            }
        }

        interruptSource = IOInterruptEventSource::interruptEventSource(
            this,
            OSMemberFunctionCast(IOInterruptEventAction, this, &RealtekRTL8822C::handleInterrupt),
            pciDevice,
            0
        );
        if (interruptSource && workLoop->addEventSource(interruptSource) == kIOReturnSuccess) {
            // Hardware interrupts stay masked and the work-loop source stays
            // disabled until firmware, DMA, MAC, PHY and the interface are
            // fully initialized. This prevents ISR access to partial rings.
            RTW_DEBUG_PROPERTY("Debug_InterruptSource", "Setup OK; enable deferred until hardware ready");
        } else {
            RTW_DEBUG_PROPERTY("Debug_InterruptSource", "Setup Failed");
            if (interruptSource) {
                interruptSource->release();
                interruptSource = nullptr;
            }
            return failStart(provider, "Failed to create interrupt source",
                             "start:interrupt-source-failed");
        }

        scanTimer = IOTimerEventSource::timerEventSource(
            this,
            OSMemberFunctionCast(IOTimerEventSource::Action, this, &RealtekRTL8822C::connectionWatchdog)
        );
        if (scanTimer && workLoop->addEventSource(scanTimer) == kIOReturnSuccess) {
            RTW_DEBUG_PROPERTY("Debug_Connection_Watchdog", "armed=0 deferred=hardware-ready interval_ms=500 auth_max=3 assoc_max=3 ba_max=3");
        } else {
            if (scanTimer) {
                scanTimer->release();
                scanTimer = nullptr;
            }
            RTW_DEBUG_PROPERTY("Debug_Connection_Watchdog", "armed=0");
            return failStart(provider, "Failed to create connection watchdog",
                             "start:watchdog-source-failed");
        }

        offchannelScanTimer = IOTimerEventSource::timerEventSource(
            this,
            OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                 &RealtekRTL8822C::connectedScanTimerFired)
        );
        if (!offchannelScanTimer ||
            workLoop->addEventSource(offchannelScanTimer) != kIOReturnSuccess) {
            if (offchannelScanTimer) {
                offchannelScanTimer->release();
                offchannelScanTimer = nullptr;
            }
            publishConnectedScanState("available=0 reason=timer-setup-failed");
            return failStart(provider, "Failed to create connected-scan timer",
                             "start:connected-scan-source-failed");
        } else {
            publishConnectedScanState("available=1 active=0");
        }

        rxPollTimer = IOTimerEventSource::timerEventSource(
            this,
            OSMemberFunctionCast(IOTimerEventSource::Action, this,
                                 &RealtekRTL8822C::rxPollTimerFired)
        );
        if (!rxPollTimer ||
            workLoop->addEventSource(rxPollTimer) != kIOReturnSuccess) {
            if (rxPollTimer) {
                rxPollTimer->release();
                rxPollTimer = nullptr;
            }
            RTW_DEBUG_PROPERTY("Debug_RX_Poll", "available=0 fallback=drain-512");
            return failStart(provider, "Failed to create bounded RX poll timer",
                             "start:rx-poll-source-failed");
        } else {
            RTW_DEBUG_PROPERTY("Debug_RX_Poll", "available=1 active=0 budget=8 delay_us=500 rdu_masked=1 input=immediate stage_timing=1 ccmp_view=1 direct_mbuf=1");
        }
    } else {
        return failStart(provider, "Failed to obtain controller work loop",
                         "start:workloop-failed");
    }

    RTW_DEBUG_PROPERTY("Debug_BAR2_Length", bar2Map->getLength(), 32);

    // Read Chip Version (SYS_CFG1 = 0x00F0)
    UInt32 chip_version = read32(0x00F0);
    setProperty("Hardware_CHIP_VERSION", chip_version, 32);
    cutVersion = (chip_version >> 12) & 0x0F;

    // Test read32(0x1208) early
    // (Moved to downloadFirmware as Debug_DDMA_After_Init)

    // Phase 2: DMA (Direct Memory Access) and Ring Buffers. Descriptor rings
    // remain uncached; packet arenas use coherent copy-back mappings.
    bcnqDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kRtwDmaRingMemoryOptions,
        8192,
        0x00000000FFFFFFFFULL
    );
    if (!bcnqDesc) {
        return failStart(provider, "Failed to allocate BCNQ",
                         "start:bcnq-allocation-failed");
    }

    IOReturn descPrepRet = bcnqDesc->prepare();
    RTW_DEBUG_PROPERTY("Debug_Desc_Prep_Ret", (uint64_t)(UInt32)descPrepRet, 32);
    if (descPrepRet != kIOReturnSuccess)
        return failStart(provider, "Failed to prepare BCNQ memory",
                         "start:bcnq-prepare-failed");

    // Create IODMACommand in kBypassed mode (standard for Hackintosh drivers)
    bcnqDmaCmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost32,
        32, 0, IODMACommand::kBypassed, 0, 0,
        nullptr
    );

    if (!bcnqDmaCmd) {
        return failStart(provider, "Failed to create BCNQ DMA command",
                         "start:bcnq-dma-command-failed");
    }
    IOReturn setMemoryRet = bcnqDmaCmd->setMemoryDescriptor(bcnqDesc);
    if (setMemoryRet != kIOReturnSuccess)
        return failStart(provider, "Failed to bind BCNQ DMA memory",
                         "start:bcnq-dma-bind-failed");
    IOReturn prepRet = bcnqDmaCmd->prepare();
    RTW_DEBUG_PROPERTY("Debug_DMA_Prep_Ret", (uint64_t)(UInt32)prepRet, 32);
    if (prepRet != kIOReturnSuccess)
        return failStart(provider, "Failed to prepare BCNQ DMA command",
                         "start:bcnq-dma-prepare-failed");


    // Phase 2.5: Allocate H2CQ ring and H2C payload descriptors (aligned with Linux: 128 slots)
    h2cqDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kRtwDmaRingMemoryOptions,
        2048, // 128 slots * 16 bytes = 2048 bytes
        0x00000000FFFFFFFFULL
    );
    if (!h2cqDesc) {
        return failStart(provider, "Failed to allocate H2CQ descriptor",
                         "start:h2cq-allocation-failed");
    }
    h2cqDmaCmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost32,
        32, 0, IODMACommand::kBypassed, 0, 0,
        nullptr
    );
    if (!h2cqDmaCmd) {
        return failStart(provider, "Failed to create H2CQ DMA command",
                         "start:h2cq-dma-command-failed");
    }
    if (!prepareDmaMapping(h2cqDesc, h2cqDmaCmd, &h2cqPhysAddr))
        return failStart(provider, "Failed to map H2CQ DMA memory",
                         "start:h2cq-dma-map-failed");

    h2cPayloadDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kRtwDmaPayloadMemoryOptions,
        16384, // 128 slots * 128 bytes = 16KB (each slot gets a 128-byte contiguous block)
        0x00000000FFFFFFFFULL
    );
    if (!h2cPayloadDesc) {
        return failStart(provider, "Failed to allocate H2C payload",
                         "start:h2c-payload-allocation-failed");
    }
    h2cPayloadDmaCmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost32,
        32, 0, IODMACommand::kBypassed, 0, 0,
        nullptr
    );
    if (!h2cPayloadDmaCmd) {
        return failStart(provider, "Failed to create H2C payload DMA command",
                         "start:h2c-payload-dma-command-failed");
    }
    if (!prepareDmaMapping(h2cPayloadDesc, h2cPayloadDmaCmd,
                           &h2cPayloadPhysAddr))
        return failStart(provider, "Failed to map H2C payload DMA memory",
                         "start:h2c-payload-dma-map-failed");

    // CRITICAL H2CQ PCI Ring configuration (aligned with Linux):
    write32(0x1320, (UInt32)(h2cqPhysAddr & 0xFFFFFFFF));
    write16(0x1328, 128); // 128 slots in the Ring
    write32(0x1330, read32(0x1330) | (1 << 16) | (1 << 8)); // Clear Host and HW indices for H2CQ

        // === BEQ Allocation ===
    beqDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaRingMemoryOptions, 256 * 16, 0x00000000FFFFFFFFULL);
    if (!beqDesc) return failStart(provider, "Failed to allocate BEQ descriptor", "start:beq-allocation-failed");
    beqDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!beqDmaCmd) return failStart(provider, "Failed to create BEQ DMA command", "start:beq-dma-command-failed");
    if (!prepareDmaMapping(beqDesc, beqDmaCmd, &beqPhysAddr))
        return failStart(provider, "Failed to map BEQ DMA memory", "start:beq-dma-map-failed");
    beqPayloadDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaPayloadMemoryOptions, 256 * 2048, 0x00000000FFFFFFFFULL);
    if (!beqPayloadDesc) return failStart(provider, "Failed to allocate BEQ payload", "start:beq-payload-allocation-failed");
    beqPayloadDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!beqPayloadDmaCmd) return failStart(provider, "Failed to create BEQ payload DMA command", "start:beq-payload-dma-command-failed");
    if (!prepareDmaMapping(beqPayloadDesc, beqPayloadDmaCmd, &beqPayloadPhysAddr))
        return failStart(provider, "Failed to map BEQ payload DMA memory", "start:beq-payload-dma-map-failed");
    write32(0x0328, (UInt32)(beqPhysAddr & 0xFFFFFFFF));
    write16(0x0388, 256);

    // === BKQ Allocation ===
    bkqDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaRingMemoryOptions, 128 * 16, 0x00000000FFFFFFFFULL);
    if (!bkqDesc) return failStart(provider, "Failed to allocate BKQ descriptor", "start:bkq-allocation-failed");
    bkqDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!bkqDmaCmd) return failStart(provider, "Failed to create BKQ DMA command", "start:bkq-dma-command-failed");
    if (!prepareDmaMapping(bkqDesc, bkqDmaCmd, &bkqPhysAddr))
        return failStart(provider, "Failed to map BKQ DMA memory", "start:bkq-dma-map-failed");
    bkqPayloadDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaPayloadMemoryOptions, 128 * 2048, 0x00000000FFFFFFFFULL);
    if (!bkqPayloadDesc) return failStart(provider, "Failed to allocate BKQ payload", "start:bkq-payload-allocation-failed");
    bkqPayloadDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!bkqPayloadDmaCmd) return failStart(provider, "Failed to create BKQ payload DMA command", "start:bkq-payload-dma-command-failed");
    if (!prepareDmaMapping(bkqPayloadDesc, bkqPayloadDmaCmd, &bkqPayloadPhysAddr))
        return failStart(provider, "Failed to map BKQ payload DMA memory", "start:bkq-payload-dma-map-failed");
    write32(0x0330, (UInt32)(bkqPhysAddr & 0xFFFFFFFF));
    write16(0x038A, 128);

    // === VIQ Allocation ===
    viqDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaRingMemoryOptions, 128 * 16, 0x00000000FFFFFFFFULL);
    if (!viqDesc) return failStart(provider, "Failed to allocate VIQ descriptor", "start:viq-allocation-failed");
    viqDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!viqDmaCmd) return failStart(provider, "Failed to create VIQ DMA command", "start:viq-dma-command-failed");
    if (!prepareDmaMapping(viqDesc, viqDmaCmd, &viqPhysAddr))
        return failStart(provider, "Failed to map VIQ DMA memory", "start:viq-dma-map-failed");
    viqPayloadDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaPayloadMemoryOptions, 128 * 2048, 0x00000000FFFFFFFFULL);
    if (!viqPayloadDesc) return failStart(provider, "Failed to allocate VIQ payload", "start:viq-payload-allocation-failed");
    viqPayloadDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!viqPayloadDmaCmd) return failStart(provider, "Failed to create VIQ payload DMA command", "start:viq-payload-dma-command-failed");
    if (!prepareDmaMapping(viqPayloadDesc, viqPayloadDmaCmd, &viqPayloadPhysAddr))
        return failStart(provider, "Failed to map VIQ payload DMA memory", "start:viq-payload-dma-map-failed");
    write32(0x0320, (UInt32)(viqPhysAddr & 0xFFFFFFFF));
    write16(0x0386, 128);

    // === VOQ Allocation ===
    voqDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaRingMemoryOptions, 128 * 16, 0x00000000FFFFFFFFULL);
    if (!voqDesc) return failStart(provider, "Failed to allocate VOQ descriptor", "start:voq-allocation-failed");
    voqDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!voqDmaCmd) return failStart(provider, "Failed to create VOQ DMA command", "start:voq-dma-command-failed");
    if (!prepareDmaMapping(voqDesc, voqDmaCmd, &voqPhysAddr))
        return failStart(provider, "Failed to map VOQ DMA memory", "start:voq-dma-map-failed");
    voqPayloadDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaPayloadMemoryOptions, 128 * 2048, 0x00000000FFFFFFFFULL);
    if (!voqPayloadDesc) return failStart(provider, "Failed to allocate VOQ payload", "start:voq-payload-allocation-failed");
    voqPayloadDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!voqPayloadDmaCmd) return failStart(provider, "Failed to create VOQ payload DMA command", "start:voq-payload-dma-command-failed");
    if (!prepareDmaMapping(voqPayloadDesc, voqPayloadDmaCmd, &voqPayloadPhysAddr))
        return failStart(provider, "Failed to map VOQ payload DMA memory", "start:voq-payload-dma-map-failed");
    write32(0x0318, (UInt32)(voqPhysAddr & 0xFFFFFFFF));
    write16(0x0384, 128);

    // === MGMTQ Allocation ===
    mgmtDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaRingMemoryOptions, 128 * 16, 0x00000000FFFFFFFFULL);
    if (!mgmtDesc) return failStart(provider, "Failed to allocate MGMTQ descriptor", "start:mgmtq-allocation-failed");
    mgmtDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!mgmtDmaCmd) return failStart(provider, "Failed to create MGMTQ DMA command", "start:mgmtq-dma-command-failed");
    if (!prepareDmaMapping(mgmtDesc, mgmtDmaCmd, &mgmtPhysAddr))
        return failStart(provider, "Failed to map MGMTQ DMA memory", "start:mgmtq-dma-map-failed");
    mgmtPayloadDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kRtwDmaPayloadMemoryOptions, 128 * 2048, 0x00000000FFFFFFFFULL);
    if (!mgmtPayloadDesc) return failStart(provider, "Failed to allocate MGMTQ payload", "start:mgmtq-payload-allocation-failed");
    mgmtPayloadDmaCmd = IODMACommand::withSpecification(kIODMACommandOutputHost32, 32, 0, IODMACommand::kBypassed, 0, 0, nullptr);
    if (!mgmtPayloadDmaCmd) return failStart(provider, "Failed to create MGMTQ payload DMA command", "start:mgmtq-payload-dma-command-failed");
    if (!prepareDmaMapping(mgmtPayloadDesc, mgmtPayloadDmaCmd, &mgmtPayloadPhysAddr))
        return failStart(provider, "Failed to map MGMTQ payload DMA memory", "start:mgmtq-payload-dma-map-failed");
    write32(0x0310, (UInt32)(mgmtPhysAddr & 0xFFFFFFFF));
    write16(0x0380, 128);
    mgmtWp = 0;

    // ===================== RX Ring Allocation and Initialization =====================
    // Allocate an uncached RX descriptor ring (512 descriptors * 8 bytes).
    rxRingDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kRtwDmaRingMemoryOptions,
        4096,
        0x00000000FFFFFFFFULL
    );
    if (!rxRingDesc) {
        return failStart(provider, "Failed to allocate RX ring descriptor",
                         "start:rx-ring-allocation-failed");
    }
    rxRingDmaCmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost32,
        32, 0, IODMACommand::kBypassed, 0, 0,
        nullptr
    );
    if (!rxRingDmaCmd) {
        return failStart(provider, "Failed to create RX ring DMA command",
                         "start:rx-ring-dma-command-failed");
    }
    if (!prepareDmaMapping(rxRingDesc, rxRingDmaCmd, &rxRingPhysAddr))
        return failStart(provider, "Failed to map RX ring DMA memory",
                         "start:rx-ring-dma-map-failed");

    // Allocate 6 MB of coherent, CPU-cacheable RX packet storage. The device
    // writes it and the CPU consumes it after IODMACommand synchronization.
    rxBufferDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kRtwDmaPayloadMemoryOptions,
        6291456,
        0x00000000FFFFFFFFULL
    );
    if (!rxBufferDesc) {
        return failStart(provider, "Failed to allocate RX buffers",
                         "start:rx-buffer-allocation-failed");
    }
    rxBufferDmaCmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost32,
        32, 0, IODMACommand::kBypassed, 0, 0,
        nullptr
    );
    if (!rxBufferDmaCmd) {
        return failStart(provider, "Failed to create RX buffer DMA command",
                         "start:rx-buffer-dma-command-failed");
    }
    if (!prepareDmaMapping(rxBufferDesc, rxBufferDmaCmd, &rxBufferPhysAddr))
        return failStart(provider, "Failed to map RX buffer DMA memory",
                         "start:rx-buffer-dma-map-failed");

    rxBufferVirtAddr = (UInt8*)rxBufferDesc->getBytesNoCopy();
    rxRp = 0;
    RTW_DEBUG_PROPERTY("Debug_DMA_Cache_Policy",
                       "mapping=bypassed-coherent rings=inhibit payload=copyback sync=explicit");

    // Initialize RX Ring descriptors in memory
    UInt8* rxRingVirtAddr = (UInt8*)rxRingDesc->getBytesNoCopy();
    memset(rxRingVirtAddr, 0, 4096);
    for (UInt32 i = 0; i < 512; i++) {
        UInt32 buf_dma = (UInt32)(rxBufferPhysAddr + (addr64_t)i * 12288U);
        OSWriteLittleInt16(rxRingVirtAddr + i * 8, 0, 12288); // buf_size
        OSWriteLittleInt16(rxRingVirtAddr + i * 8, 2, 0);     // total_pkt_size
        OSWriteLittleInt32(rxRingVirtAddr + i * 8, 4, buf_dma); // dma
    }

    // Configure RX Ring registers
    write32(0x0338, (UInt32)(rxRingPhysAddr & 0xFFFFFFFF)); // RTK_PCI_RXBD_DESA_MPDUQ
    write16(0x0382, 512); // RTK_PCI_RXBD_NUM_MPDUQ (512 descriptors)
    write16(0x03B4, 0);   // Clear Host Read Pointer

    if (downloadFirmware()) {
        if (readEfuse()) {
            if (initMac()) {
                initCoexWifiOnly();
                if (initPhy()) {
                    // Run the pre-OTA TXAGC diagnostic on a known 2.4 GHz channel.
                    bool channelReady = setChannelHw(1, 0);
                    bool txdmaReady = channelReady &&
                        clearTxdmaLifecycleStatus("cold-start");
                    if (channelReady && txdmaReady) {
                        setProperty("DriverStatus", "Firmware loaded, EFUSE parsed, MAC/PHY initialized, IQK success!");
                        traceEvent("start:hw-init-complete");
                        hardwareReady = true;
                        hardwareSuspended = false;
                    } else {
                        setProperty("DriverStatus", channelReady ?
                                    "Cold-start TXDMA status did not clear" :
                                    "Cold-start channel setup failed");
                        traceEvent(channelReady ? "start:txdma-status-failed" :
                                                  "start:channel-failed");
                    }
                } else {
                    setProperty("DriverStatus", "PHY Initialization or IQK Failed!");
                    traceEvent("start:phy-init-failed");
                }
            }
        }
    } else {
        // DriverStatus already set by downloadFirmware with the specific error
    }

    if (!hardwareReady) {
        traceEvent("start:hardware-init-failed");
        return failStart(provider, nullptr, nullptr);
    }

    // Attach Network Interface (IO80211Interface)
    if (!attachInterface((IONetworkInterface**)&netif, true)) {
        return failStart(provider, "Failed to attach Ethernet interface",
                         "start:interface-attach-failed");
    }
    netif->registerService();
    setLinkStatus(kIONetworkLinkValid);
    RTW_DEBUG_PROPERTY("Debug_netif_val", (uint64_t)netif, 64);

    if (!setupRxPacketPool()) {
        return failStart(provider, "Failed to prepare RX packet reserve",
                         "start:rx-packet-pool-failed");
    }

    // Clear all pending HISR bits first (Write-1-to-Clear: write 0xFFFFFFFF)
    // This acknowledges any stale interrupt state from init/IQK so the first
    // ISR call we receive is truly a new event from the card.
    if (interruptSource) interruptSource->enable();
    enableHardwareInterrupts();
    if (scanTimer) {
        scanTimer->setTimeoutMS(500);
        RTW_DEBUG_PROPERTY("Debug_Connection_Watchdog", "armed=1 interval_ms=500 auth_max=3 assoc_max=3 ba_max=3");
    }

    traceEvent("start:interrupts-enabled");
    publishLifecycleInvariant("start-complete", true);
    publishDebugSnapshot();


    registerService();
    return true;
}

void RealtekRTL8822C::stop(IOService* provider) {
    RTW_DEBUG_LOG("RealtekRTL8822C: Stopping...\n");
    if (hardwareReady && !hardwareSuspended) suspendHardware();
    hardwareReady = false;
    portAuthorized = false;
    resetRxBaSessions(false);
    resetWpaState(true);

    if (interruptSource) interruptSource->disable();
    if (interruptSource && workLoop) {
        workLoop->removeEventSource(interruptSource);
    }
    if (interruptSource) {
        interruptSource->release();
        interruptSource = nullptr;
    }
    if (scanTimer && workLoop) {
        scanTimer->cancelTimeout();
        workLoop->removeEventSource(scanTimer);
    }
    if (scanTimer) {
        scanTimer->release();
        scanTimer = nullptr;
    }
    if (offchannelScanTimer && workLoop) {
        offchannelScanTimer->cancelTimeout();
        workLoop->removeEventSource(offchannelScanTimer);
    }
    if (offchannelScanTimer) {
        offchannelScanTimer->release();
        offchannelScanTimer = nullptr;
    }
    if (rxPollTimer && workLoop) {
        rxPollTimer->cancelTimeout();
        workLoop->removeEventSource(rxPollTimer);
    }
    if (rxPollTimer) {
        rxPollTimer->release();
        rxPollTimer = nullptr;
    }
    rxPollActive = false;
    teardownRxPacketPool();
    workLoop = nullptr;

    // No DMA-visible allocation may be completed or released while the PCI
    // function can still initiate bus-master transactions. Partial start()
    // failures use this same boundary before unwinding their first resource.
    if (ioBase) maskAndAckInterrupts();
    if (pciDevice) pciDevice->setBusMasterEnable(false);

    if (beqDmaCmd) { beqDmaCmd->complete(); beqDmaCmd->release(); beqDmaCmd = nullptr; }
    if (beqDesc) { beqDesc->complete(); beqDesc->release(); beqDesc = nullptr; }
    if (beqPayloadDmaCmd) { beqPayloadDmaCmd->complete(); beqPayloadDmaCmd->release(); beqPayloadDmaCmd = nullptr; }
    if (beqPayloadDesc) { beqPayloadDesc->complete(); beqPayloadDesc->release(); beqPayloadDesc = nullptr; }

    if (bkqDmaCmd) { bkqDmaCmd->complete(); bkqDmaCmd->release(); bkqDmaCmd = nullptr; }
    if (bkqDesc) { bkqDesc->complete(); bkqDesc->release(); bkqDesc = nullptr; }
    if (bkqPayloadDmaCmd) { bkqPayloadDmaCmd->complete(); bkqPayloadDmaCmd->release(); bkqPayloadDmaCmd = nullptr; }
    if (bkqPayloadDesc) { bkqPayloadDesc->complete(); bkqPayloadDesc->release(); bkqPayloadDesc = nullptr; }

    if (viqDmaCmd) { viqDmaCmd->complete(); viqDmaCmd->release(); viqDmaCmd = nullptr; }
    if (viqDesc) { viqDesc->complete(); viqDesc->release(); viqDesc = nullptr; }
    if (viqPayloadDmaCmd) { viqPayloadDmaCmd->complete(); viqPayloadDmaCmd->release(); viqPayloadDmaCmd = nullptr; }
    if (viqPayloadDesc) { viqPayloadDesc->complete(); viqPayloadDesc->release(); viqPayloadDesc = nullptr; }

    if (voqDmaCmd) { voqDmaCmd->complete(); voqDmaCmd->release(); voqDmaCmd = nullptr; }
    if (voqDesc) { voqDesc->complete(); voqDesc->release(); voqDesc = nullptr; }
    if (voqPayloadDmaCmd) { voqPayloadDmaCmd->complete(); voqPayloadDmaCmd->release(); voqPayloadDmaCmd = nullptr; }
    if (voqPayloadDesc) { voqPayloadDesc->complete(); voqPayloadDesc->release(); voqPayloadDesc = nullptr; }

    if (mgmtDmaCmd) { mgmtDmaCmd->complete(); mgmtDmaCmd->release(); mgmtDmaCmd = nullptr; }
    if (mgmtDesc) { mgmtDesc->complete(); mgmtDesc->release(); mgmtDesc = nullptr; }
    if (mgmtPayloadDmaCmd) { mgmtPayloadDmaCmd->complete(); mgmtPayloadDmaCmd->release(); mgmtPayloadDmaCmd = nullptr; }
    if (mgmtPayloadDesc) { mgmtPayloadDesc->complete(); mgmtPayloadDesc->release(); mgmtPayloadDesc = nullptr; }
    if (bcnqDmaCmd) { bcnqDmaCmd->complete(); bcnqDmaCmd->release(); bcnqDmaCmd = nullptr; }
    if (bcnqDesc) { bcnqDesc->complete(); bcnqDesc->release(); bcnqDesc = nullptr; }
    if (h2cqDmaCmd) { h2cqDmaCmd->complete(); h2cqDmaCmd->release(); h2cqDmaCmd = nullptr; }
    if (h2cqDesc) { h2cqDesc->complete(); h2cqDesc->release(); h2cqDesc = nullptr; }
    if (h2cPayloadDmaCmd) { h2cPayloadDmaCmd->complete(); h2cPayloadDmaCmd->release(); h2cPayloadDmaCmd = nullptr; }
    if (h2cPayloadDesc) { h2cPayloadDesc->complete(); h2cPayloadDesc->release(); h2cPayloadDesc = nullptr; }
    if (rxRingDmaCmd) { rxRingDmaCmd->complete(); rxRingDmaCmd->release(); rxRingDmaCmd = nullptr; }
    if (rxRingDesc) { rxRingDesc->complete(); rxRingDesc->release(); rxRingDesc = nullptr; }
    if (rxBufferDmaCmd) { rxBufferDmaCmd->complete(); rxBufferDmaCmd->release(); rxBufferDmaCmd = nullptr; }
    if (rxBufferDesc) { rxBufferDesc->complete(); rxBufferDesc->release(); rxBufferDesc = nullptr; }

    beqWp = 0; beqRp = 0; beqLastHwRp = 0; beqOutstanding = 0; beqQueueStalled = false;
    h2cqWp = mgmtWp = mgmtRp = bkqWp = viqWp = voqWp = rxRp = 0;
    h2cqPhysAddr = h2cPayloadPhysAddr = 0;
    beqPhysAddr = beqPayloadPhysAddr = 0;
    bkqPhysAddr = bkqPayloadPhysAddr = 0;
    viqPhysAddr = viqPayloadPhysAddr = 0;
    voqPhysAddr = voqPayloadPhysAddr = 0;
    mgmtPhysAddr = mgmtPayloadPhysAddr = 0;
    rxRingPhysAddr = rxBufferPhysAddr = 0;
    rxBufferVirtAddr = nullptr;
    customScanResultsCount = 0;
    lastScanCompletedTime = 0;
    connState = CONN_STATE_DISCONNECTED;
    memset(targetSsid, 0, sizeof(targetSsid));
    memset(targetBssid, 0, sizeof(targetBssid));
    baEstablished = false;
    baDialogToken = 0;
    baTid = 0;
    baBufferSize = 0;
    baTimeout = 0;
    baRequests = 0;
    baResponses = 0;
    targetChannel = 1;
    targetCenterChannel = 1;
    targetBandwidth = 0;
    targetPrimaryChannelIndex = 0;
    isrCallsCount = 0;
    debugAuthRxCount = 0;
    debugBedokCount = 0;
    debugTxerrCount = 0;
    debugTxfovwCount = 0;
    debugC2hRxCount = 0;
    txSeqNum = 0;
    debugMgntdokCount = 0;
    debugBcndmaintCount = 0;
    mgmtRp = 0;
    connStateCounter = 0;
    targetSupportsCck = true;
    targetSupportsHt = false;
    targetSupportsVht = false;
    targetSupportsWmm = false;
    targetHtCapInfo = 0;
    targetHtAmpduParams = 0;
    targetHtMcs0 = targetHtMcs1 = 0;
    targetHtPrimaryChannel = targetHtOperationInfo = 0;

    if (netif) {
        detachInterface(netif);
        netif->release();
        netif = nullptr;
    }
    if (bar2Map) {
        ioBase = nullptr;
        bar2Map->release();
        bar2Map = nullptr;
    }
    bar2Desc = nullptr;
    if (pciDevice) {
        pciDevice->release();
        pciDevice = nullptr;
    }
    super::stop(provider);
}

bool RealtekRTL8822C::reprogramDmaRings() {
    if (!h2cqDesc || !h2cPayloadDesc || !beqDesc || !bkqDesc || !viqDesc || !voqDesc ||
        !mgmtDesc || !rxRingDesc || !rxBufferDesc ||
        h2cqPhysAddr == 0 || h2cPayloadPhysAddr == 0 ||
        beqPhysAddr == 0 || bkqPhysAddr == 0 ||
        viqPhysAddr == 0 || voqPhysAddr == 0 || mgmtPhysAddr == 0 ||
        rxRingPhysAddr == 0 || rxBufferPhysAddr == 0) {
        setProperty("DriverStatus", "Cannot restore DMA rings: allocation or IOVA missing");
        return false;
    }

    memset(h2cqDesc->getBytesNoCopy(), 0, 128U * 16U);
    memset(h2cPayloadDesc->getBytesNoCopy(), 0, 128U * 128U);
    memset(beqDesc->getBytesNoCopy(), 0, 256U * 16U);
    memset(bkqDesc->getBytesNoCopy(), 0, 128U * 16U);
    memset(viqDesc->getBytesNoCopy(), 0, 128U * 16U);
    memset(voqDesc->getBytesNoCopy(), 0, 128U * 16U);
    memset(mgmtDesc->getBytesNoCopy(), 0, 128U * 16U);

    UInt8* rxRing = static_cast<UInt8*>(rxRingDesc->getBytesNoCopy());
    memset(rxRing, 0, 512U * 8U);
    for (UInt32 i = 0; i < 512; i++) {
        UInt8* bd = rxRing + i * 8U;
        OSWriteLittleInt16(bd, 0, 12288);
        OSWriteLittleInt16(bd, 2, 0);
        OSWriteLittleInt32(bd, 4,
                           static_cast<UInt32>(rxBufferPhysAddr +
                                               static_cast<addr64_t>(i) * 12288U));
    }

    h2cqWp = 0;
    beqWp = beqRp = beqLastHwRp = beqOutstanding = 0;
    beqQueueStalled = false;
    bkqWp = viqWp = voqWp = mgmtWp = mgmtRp = rxRp = 0;

    write32(0x1320, static_cast<UInt32>(h2cqPhysAddr));
    write16(0x1328, 128);
    write32(0x1330, read32(0x1330) | (1U << 16) | (1U << 8));
    write32(0x0328, static_cast<UInt32>(beqPhysAddr));
    write16(0x0388, 256);
    write32(0x0330, static_cast<UInt32>(bkqPhysAddr));
    write16(0x038a, 128);
    write32(0x0320, static_cast<UInt32>(viqPhysAddr));
    write16(0x0386, 128);
    write32(0x0318, static_cast<UInt32>(voqPhysAddr));
    write16(0x0384, 128);
    write32(0x0310, static_cast<UInt32>(mgmtPhysAddr));
    write16(0x0380, 128);
    write32(0x0338, static_cast<UInt32>(rxRingPhysAddr));
    write16(0x0382, 512);
    write16(0x03b4, 0);

    h2cqDmaCmd->synchronize(kIODirectionOut);
    h2cPayloadDmaCmd->synchronize(kIODirectionOut);
    beqDmaCmd->synchronize(kIODirectionOut);
    bkqDmaCmd->synchronize(kIODirectionOut);
    viqDmaCmd->synchronize(kIODirectionOut);
    voqDmaCmd->synchronize(kIODirectionOut);
    mgmtDmaCmd->synchronize(kIODirectionOut);
    rxRingDmaCmd->synchronize(kIODirectionOut);
    OSSynchronizeIO();
    return true;
}

void RealtekRTL8822C::resetFirmwareH2cState(const char* reason) {
    UInt16 seqBefore = h2cSeq;
    UInt8 boxBefore = lastBoxNum;
    UInt32 wpBefore = h2cqWp;
    UInt8 mailboxState = ioBase ? read8(0x01cc) : 0xff;

    // Linux resets both software namespaces after every successful firmware
    // download. A restarted firmware must never inherit mailbox rotation or
    // packet sequence state from the previous firmware instance.
    h2cSeq = 0;
    lastBoxNum = 0;
    h2cqWp = 0;
    RTW_DEBUG_REMOVE_PROPERTY("Debug_H2C_Mailbox_Error");

    char db[192];
    snprintf(db, sizeof(db),
             "reason=%s seq=%u->0 box=%u->0 wp=%u->0 hmetfr=%02x",
             reason ? reason : "unknown", (unsigned int)seqBefore,
             (unsigned int)boxBefore, (unsigned int)wpBefore,
             (unsigned int)mailboxState);
    RTW_DEBUG_PROPERTY("Debug_H2C_Lifecycle_Reset", db);
}

void RealtekRTL8822C::maskAndAckInterrupts() {
    if (!ioBase) return;
    write32(0x00b0, 0);
    write32(0x00b8, 0);
    write32(0x10b8, 0);
    OSSynchronizeIO();
    UInt32 h0 = read32(0x00b4);
    UInt32 h1 = read32(0x00bc);
    UInt32 h2 = read32(0x10b4);
    UInt32 h3 = read32(0x10bc);
    if (h0) write32(0x00b4, h0);
    if (h1) write32(0x00bc, h1);
    if (h2) write32(0x10b4, h2);
    if (h3) write32(0x10bc, h3);
    OSSynchronizeIO();
}

void RealtekRTL8822C::enableHardwareInterrupts() {
    if (!ioBase) return;
    maskAndAckInterrupts();
    restoreHardwareInterruptMasks();
}

void RealtekRTL8822C::restoreHardwareInterruptMasks() {
    if (!ioBase) return;
    UInt32 himr0 = kRtwHimr0;
    UInt32 himr1 = kRtwHimr1;
    if (rxPollActive) {
        // Match Linux rtw_pci_enable_interrupt(..., exclude_rx=true): RX stays
        // masked while a bounded poll owns the ring, but TX/C2H/H2C remain live.
        himr0 &= ~0x00000003U; // ROK | RDU
        himr1 &= ~0x00000100U; // RXFOVW (not currently in the base mask)
    }
    write32(0x00b0, himr0);
    write32(0x00b8, himr1);
    write32(0x10b8, kRtwHimr3);
    OSSynchronizeIO();
}

bool RealtekRTL8822C::clearTxdmaLifecycleStatus(const char* reason) {
    if (!ioBase) return false;
    UInt32 before = read32(0x0210);
    if (before) write32(0x0210, before);
    OSSynchronizeIO();
    UInt32 after = read32(0x0210);
    char db[160];
    snprintf(db, sizeof(db), "reason=%s before=%08x after=%08x ok=%d",
             reason ? reason : "unknown", (unsigned int)before,
             (unsigned int)after, after == 0 ? 1 : 0);
    RTW_DEBUG_PROPERTY("Debug_TXDMA_Lifecycle_Clear", db);
    return after == 0;
}

void RealtekRTL8822C::publishLifecycleInvariant(const char* event,
                                             bool powerSequenceOk) {
    UInt16 pciCommand = pciDevice ? pciDevice->configRead16(kIOPCIConfigCommand) : 0;
    UInt16 bsdFlags = netif ? netif->getFlags() : 0;
    UInt32 irq0 = 0, irq1 = 0, irq3 = 0, beq = 0, mgmt = 0;
    if (ioBase && !hardwareSuspended) {
        irq0 = read32(0x00b0);
        irq1 = read32(0x00b8);
        irq3 = read32(0x10b8);
        beq = read32(0x03a8);
        mgmt = read32(0x03b0);
    }
    char db[320];
    snprintf(db, sizeof(db),
             "event=%s suspended=%d ready=%d iface=%d bsd=%04x/up%d powerseq=%d pci_mem=%d busmaster=%d irq=%08x/%08x/%08x rings=%08x/%08x cam_clears=%u suspend=%u resume=%u failures=%u",
             event ? event : "unknown", hardwareSuspended ? 1 : 0,
             hardwareReady ? 1 : 0, interfaceEnabled ? 1 : 0,
             bsdFlags, (bsdFlags & IFF_UP) ? 1 : 0,
             powerSequenceOk ? 1 : 0, (pciCommand & (1U << 1)) ? 1 : 0,
             (pciCommand & (1U << 2)) ? 1 : 0,
             (unsigned int)irq0, (unsigned int)irq1, (unsigned int)irq3,
             (unsigned int)mgmt, (unsigned int)beq,
             (unsigned int)lifecycleCamClearCount,
             (unsigned int)lifecycleSuspendCount,
             (unsigned int)lifecycleResumeCount,
             (unsigned int)lifecycleResumeFailures);
    RTW_DEBUG_PROPERTY("Debug_Lifecycle_Invariant", db);
    if (event && strncmp(event, "interface-", 10) == 0)
        RTW_DEBUG_PROPERTY("Debug_Interface_Lifecycle", db);
    else
        RTW_DEBUG_PROPERTY("Debug_Power_Lifecycle", db);
}

bool RealtekRTL8822C::rollbackFailedResume(const char* stage) {
    maskAndAckInterrupts();
    if (interruptSource) interruptSource->disable();
    if (ioBase) {
        clearAllSecurityCam("resume-rollback");
        write8(0x0522, 0xff);
    }
    bool enteredCardEmu = ioBase && parsePowerSeq(trans_act_to_cardemu_8822c);
    bool enteredCardDis = ioBase && parsePowerSeq(trans_cardemu_to_carddis_8822c);
    bool poweredOff = enteredCardEmu && enteredCardDis;
    if (pciDevice) pciDevice->setBusMasterEnable(false);
    hardwareSuspended = true;
    interfaceEnabled = false;
    portAuthorized = false;
    powerState = APPLE80211_POWER_OFF;
    lifecycleResumeFailures++;
    setLinkStatus(kIONetworkLinkValid);
    setProperty("PowerState", "ResumeFailed");
    setProperty("InterfaceState", "SuspendedAfterResumeFailure");
    char status[192];
    snprintf(status, sizeof(status),
             "Hardware resume failed at %s; rollback power-off=%d",
             stage ? stage : "unknown", poweredOff ? 1 : 0);
    setProperty("DriverStatus", status);
    publishLifecycleInvariant("resume-rollback", poweredOff);
    return false;
}

bool RealtekRTL8822C::suspendHardware() {
    if (hardwareSuspended) return true;

    interfaceEnabledBeforeSleep = interfaceEnabled;
    interfaceEnabled = false;
    bool hadConnectionState = connState != CONN_STATE_DISCONNECTED;
    if (hadConnectionState)
        disconnectFromNetwork("system-sleep", true);
    else
        clearAllSecurityCam("system-sleep");
    resetWpaState(true);
    portAuthorized = false;
    resetRxBaSessions(false);

    IOOutputQueue* queue = getOutputQueue();
    if (queue) {
        queue->stop();
        queue->flush();
    }
    if (scanTimer) scanTimer->cancelTimeout();
    if (offchannelScanTimer) offchannelScanTimer->cancelTimeout();
    if (rxPollTimer) rxPollTimer->cancelTimeout();
    rxPollActive = false;

    maskAndAckInterrupts();
    if (interruptSource) interruptSource->disable();
    write8(0x0522, 0xff);
    OSSynchronizeIO();

    bool enteredCardEmu = parsePowerSeq(trans_act_to_cardemu_8822c);
    bool enteredCardDis = parsePowerSeq(trans_cardemu_to_carddis_8822c);
    bool poweredOff = enteredCardEmu && enteredCardDis;
    pciDevice->setBusMasterEnable(false);
    hardwareSuspended = true;
    powerState = APPLE80211_POWER_OFF;
    lifecycleSuspendCount++;
    setLinkStatus(kIONetworkLinkValid);
    setProperty("PowerState", poweredOff ? "Suspended" : "SuspendPowerSequenceFailed");
    setProperty("InterfaceState", "Suspended");
    setProperty("DriverStatus", poweredOff ? "Hardware suspended" :
                                            "Hardware suspended with power-sequence failure");
    publishLifecycleInvariant("suspend-complete", poweredOff);
    return poweredOff;
}

bool RealtekRTL8822C::resumeHardware() {
    if (!hardwareSuspended) return true;

    pciDevice->setMemoryEnable(true);
    pciDevice->setBusMasterEnable(true);
    channelCalibrationValid = false;
    calibratedChannel = 0;
    calibratedBandwidth = 0;
    if (!downloadFirmware()) return rollbackFailedResume("firmware");
    if (!reprogramDmaRings()) return rollbackFailedResume("dma-rings");
    if (!readEfuse()) return rollbackFailedResume("efuse");
    if (!initMac()) return rollbackFailedResume("mac");

    initCoexWifiOnly();
    if (!initPhy()) return rollbackFailedResume("phy");
    if (!setChannelHw(1, 0)) return rollbackFailedResume("channel-1");
    if (!clearTxdmaLifecycleStatus("resume"))
        return rollbackFailedResume("txdma-status");

    hardwareSuspended = false;
    rxPollActive = false;
    interfaceEnabled = interfaceEnabledBeforeSleep;
    powerState = APPLE80211_POWER_ON;
    lifecycleResumeCount++;
    if (interruptSource) interruptSource->enable();
    enableHardwareInterrupts();
    if (scanTimer) scanTimer->setTimeoutMS(500);
    setLinkStatus(kIONetworkLinkValid);
    setProperty("PowerState", "Awake");
    setProperty("InterfaceState", interfaceEnabled ? "Enabled" : "Disabled");
    setProperty("WiFiStatus", "Idle");
    setProperty("DriverStatus", "Hardware resumed; reconnect required");
    publishLifecycleInvariant("resume-complete", true);
    return true;
}

IOReturn RealtekRTL8822C::registerWithPolicyMaker(IOService* policyMaker) {
    static IOPMPowerState powerStateArray[2] = {
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { 1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
    };
    IOReturn result = policyMaker->registerPowerDriver(this, powerStateArray, 2);
    RTW_DEBUG_PROPERTY("Debug_PM_Registration", static_cast<UInt32>(result), 32);
    return result;
}

IOReturn RealtekRTL8822C::setPowerState(unsigned long powerStateOrdinal, IOService* whatDevice) {
    (void)whatDevice;
    if (!hardwareReady) {
        powerState = powerStateOrdinal == 0 ? APPLE80211_POWER_OFF : APPLE80211_POWER_ON;
        return IOPMAckImplied;
    }
    IOReturn result = executeCommand(this, &RealtekRTL8822C::powerStateAction,
                                     this,
                                     reinterpret_cast<void*>(
                                         static_cast<uintptr_t>(powerStateOrdinal)));
    bool ok = result == kIOReturnSuccess;
    if (!ok) traceEvent(powerStateOrdinal == 0 ? "power:suspend-failed" :
                                                "power:resume-failed");
    else traceEvent(powerStateOrdinal == 0 ? "power:suspended" : "power:resumed");
    return IOPMAckImplied;
}

bool RealtekRTL8822C::sendFirmwarePacket(UInt16 page, const UInt8* data, UInt32 size) {
    if (!bcnqDesc) return false;

    // The BCNQ ring has 32 descriptors (16 bytes each) -> 512 bytes total.
    // However, the BCNQ length is hardcoded/assumed to be 1 entry.
    // We will always write to offset 0, just like Linux (since ring->r.len is 1).
    UInt32 bcnq_wp = 0;

    UInt8* bcnqVirtAddr = (UInt8*)bcnqDesc->getBytesNoCopy();

    IODMACommand::Segment32 segments[1];
    UInt32 numSegments = 1;
    UInt64 offset = 0;
    bcnqDmaCmd->gen32IOVMSegments(&offset, segments, &numSegments);
    addr64_t bcnqPhysAddr = segments[0].fIOVMAddr;
    UInt32 purePhys = (UInt32)(bcnqDesc->getPhysicalSegment(0, NULL) & 0xFFFFFFFF);
    char iovaDb[128];
    snprintf(iovaDb, sizeof(iovaDb), "IOVA=0x%08llx PHYS=0x%08x", bcnqPhysAddr, purePhys);
    RTW_DEBUG_PROPERTY("Debug_IOMMU_Addr_Pkt", iovaDb);

    // CRITICAL FIX: Tell the hardware where the BCNQ ring is!
    write32(0x0308, (UInt32)(bcnqPhysAddr & 0xFFFFFFFF));
#if RTW_DEBUG
    UInt32 readback_0308 = read32(0x0308);
    RTW_DEBUG_PROPERTY("Debug_0308_Readback", readback_0308, 32);
#endif

    char txbdNumDbg[160];
    snprintf(txbdNumDbg, sizeof(txbdNumDbg),
             "VO=0x%04x VI=0x%04x BE=0x%04x BK=0x%04x MGMT=0x%04x HI0=0x%04x",
             read16(0x0384), read16(0x0386), read16(0x0388),
             read16(0x038A), read16(0x0380), read16(0x038C));
    RTW_DEBUG_PROPERTY("Debug_TXBD_NUM_Readback", txbdNumDbg);

    // We will place our payload at offset 512.
    UInt32 payload_offset = 512;
    UInt8* payload_virt = bcnqVirtAddr + payload_offset;
    UInt32 payload_phys = (UInt32)((bcnqPhysAddr + payload_offset) & 0xFFFFFFFF);

    // 1. Prepare Buffer Descriptor (TX BD) in the ring at bcnq_wp
    volatile UInt32* buf_desc = (volatile UInt32*)(bcnqVirtAddr + bcnq_wp * 16);


    // BCNQ buffer descriptor length is 8 bytes. We use 2 descriptors (16 bytes).
    // Descriptor 1: MAC TX Descriptor (48 bytes)
    // Descriptor 2: Firmware Payload (size bytes)

    UInt32 psb_len = ((size + 48 + 127) / 128) | 0x8000; // 0x8000 is the OWN bit (Bit 15 of psb_len, Bit 31 of UInt32)

    // First 8-byte descriptor
    buf_desc[0] = (48 & 0xFFFF) | (psb_len << 16);
    buf_desc[1] = payload_phys;

    // Second 8-byte descriptor (no flags — psb_len of second entry is 0)
    buf_desc[2] = (size & 0xFFFF);
    buf_desc[3] = payload_phys + 48;

    // Clear OWN bit of the next descriptor so hardware stops reading further
    UInt32 next_wp = (bcnq_wp + 1) % 32;
    volatile UInt32* next_desc = (volatile UInt32*)(bcnqVirtAddr + next_wp * 16);
    next_desc[0] &= ~0x80000000;

    // 2. Prepare TX Descriptor (48 bytes)
    volatile UInt32* tx_desc = (volatile UInt32*)payload_virt;
    memset((void*)tx_desc, 0, 48);

    // W0: TXPKTSIZE (0-15), OFFSET (16-23), LS (26), DISQSELSEQ (31)
    tx_desc[0] = (size & 0xFFFF) | (48 << 16) | (1U << 26) | (1U << 31);

    // W1: QSEL (8-12) -> BEACON = 0x10
    tx_desc[1] = (16 << 8);

    // W3: USE_RATE (8), DISDATAFB (10)
    tx_desc[3] = (1 << 8) | (1 << 10);

    // W4: DATARATE (0-6) -> DESC_RATE1M = 0x00
    tx_desc[4] = 0x00;

    // W8: EN_HWSEQ (15)
    tx_desc[8] = (1U << 15);

    // Calculate TX descriptor checksum (RTW_TX_DESC_W7_TXDESC_CHECKSUM) over 24 words (48 bytes)
    tx_desc[7] &= ~0x0000FFFFU; // clear checksum field
    UInt16 chksum = 0;
    volatile UInt16* desc_words = (volatile UInt16*)tx_desc;
    for (int i = 0; i < 24; i++) {
        chksum ^= desc_words[i];
    }
    tx_desc[7] |= chksum;

    // 3. Copy Payload
    UInt8* payload_ptr = payload_virt + 48;
    memcpy(payload_ptr, data, size);

    // 4. Setup MAC for RSVD page write (like rtw_fw_write_data_rsvd_page)
    UInt8 bckp_bcn_ctrl = read8(0x0550); // REG_BCN_CTRL (0x0550)

    UInt16 pg_addr = page;
    pg_addr &= 0x0FFF; // BIT_MASK_BCN_HEAD_1_V1
    // BIT_BCN_VALID_V1 (bit 15) is W1C: writing 1 clears it to 0,
    // then hardware sets it back to 1 when BCNQ transfer to TXBUF completes.

    write16(0x0204, pg_addr | 0x8000); // Clears BCN_VALID (bit 15 is 0)
    OSSynchronizeIO();

    UInt8 bckp_cr1 = read8(0x0101); // REG_CR + 1
    write8(0x0101, bckp_cr1 | 0x01); // BIT_ENSWBCN >> 8

#if RTW_DEBUG
    UInt8 reg_cr1_check = read8(0x0101);
    RTW_DEBUG_PROPERTY("Debug_REG_CR1_After_Write", reg_cr1_check, 32);
#endif

    write8(0x0550, (bckp_bcn_ctrl & ~0x08) | 0x10); // ~BIT_EN_BCN_FUNCTION | BIT_DIS_TSF_UDT (BIT 4)

    UInt8 bckp_txq = read8(0x0422); // REG_FWHW_TXQ_CTRL (0x0420) + 2
    write8(0x0422, bckp_txq & ~0x40); // ~(BIT_EN_BCNQ_DL >> 16)

    // Dump descriptor before kick
    char debug_db_desc[128];
    snprintf(debug_db_desc, sizeof(debug_db_desc), "BD0=0x%08x BD1=0x%08x BD2=0x%08x BD3=0x%08x", buf_desc[0], buf_desc[1], buf_desc[2], buf_desc[3]);
    RTW_DEBUG_PROPERTY("Debug_BCNQ_Desc", debug_db_desc);

    char debug_tx_desc[128];
    snprintf(debug_tx_desc, sizeof(debug_tx_desc), "TX0=0x%08x TX1=0x%08x TX3=0x%08x TX8=0x%08x", tx_desc[0], tx_desc[1], tx_desc[3], tx_desc[8]);
    RTW_DEBUG_PROPERTY("Debug_TX_Desc", debug_tx_desc);

    // Check Bus Master Enable
    UInt16 pci_cmd = pciDevice->configRead16(0x04);
    RTW_DEBUG_PROPERTY("Debug_PCI_CMD", pci_cmd, 16);
    if (!(pci_cmd & 0x0004)) {
        setProperty("DriverStatus", "Bus Master was disabled! Re-enabling...");
        pciDevice->setBusMasterEnable(true);
        pci_cmd = pciDevice->configRead16(0x04);
        RTW_DEBUG_PROPERTY("Debug_PCI_CMD_After", pci_cmd, 16);
    }



    // Kick Off (PCI DMA will automatically start because OWN bit is set, but we must ring the doorbell)
#if RTW_DEBUG
    IOReturn syncRet = bcnqDmaCmd->synchronize(kIODirectionOut);
    RTW_DEBUG_PROPERTY("Debug_DMA_Sync_Out", (uint64_t)(UInt32)syncRet, 32);
#else
    bcnqDmaCmd->synchronize(kIODirectionOut);
#endif
    OSSynchronizeIO();

    UInt32 bd0_before = buf_desc[0];
    UInt8 work_before = read8(0x0383);
    asm volatile("sfence" ::: "memory");
    write8(0x0383, work_before | 0x10);
    OSSynchronizeIO();
    UInt8 work_after0 = read8(0x0383);
    IODelay(10);
    UInt8 work_after10us = read8(0x0383);
    IODelay(1000);
    UInt32 bd0_after1ms = buf_desc[0];
    UInt16 bcn_valid_after1ms = read16(0x0204);

    char kickDbg[192];
    snprintf(kickDbg, sizeof(kickDbg),
             "work=%02x/%02x/%02x bd0=%08x->%08x bcn=0x%04x",
             work_before, work_after0, work_after10us,
             bd0_before, bd0_after1ms, bcn_valid_after1ms);
    RTW_DEBUG_PROPERTY("Debug_BCNQ_KickTrace", kickDbg);

    // Wait for 0x0204 (BIT_BCN_VALID_V1) to become 1.
    // Hardware sets this when the BCNQ packet is transferred to TXBUF.
    // Match Linux timeout: 1000 iterations * 10 us = 10 ms
    bool bcn_ok = false;
    int bcn_iters = 0;
    for (int i = 0; i < 1000; i++) {
        UInt16 val = read16(0x0204);
        if (val & (1 << 15)) {
            bcn_ok = true;
            bcn_iters = i;
            break;
        }
        IODelay(10);
    }

    char bcn_db[128];
    snprintf(bcn_db, sizeof(bcn_db), "BCN_VALID=%s iters=%d reg=0x%04x",
             bcn_ok ? "OK" : "TIMEOUT", bcn_iters, read16(0x0204));
    RTW_DEBUG_PROPERTY("Debug_BCN_VALID", bcn_db);

    if (!bcn_ok) {
        // BCN_VALID didn't fire. Use a conservative fallback delay
        // to give PCI DMA time to complete the transfer.
        IODelay(2000); // 2ms should be more than enough for 4KB PCI DMA
    }

    // Check for PCIe bus-level transaction errors (AER and Device Status)
    UInt8 cap_ptr = pciDevice->configRead8(0x34) & 0xFC;
    UInt16 pcie_status = 0;
    while (cap_ptr != 0) {
        UInt8 cap_id = pciDevice->configRead8(cap_ptr);
        if (cap_id == 0x10) { // PCI Express Capability
            pcie_status = pciDevice->configRead16(cap_ptr + 0x0A); // Device Status Register
            break;
        }
        cap_ptr = pciDevice->configRead8(cap_ptr + 1) & 0xFC;
    }

    UInt32 aer_uncorr = 0;
    UInt16 ext_cap_ptr = 0x100;
    while (ext_cap_ptr != 0) {
        UInt32 ext_cap_header = pciDevice->configRead32(ext_cap_ptr);
        UInt16 ext_cap_id = ext_cap_header & 0xFFFF;
        if (ext_cap_id == 0x0000 || ext_cap_id == 0xFFFF) break;

        if (ext_cap_id == 0x0001) { // AER Capability
            aer_uncorr = pciDevice->configRead32(ext_cap_ptr + 0x04); // Uncorrectable Error Status
            break;
        }
        ext_cap_ptr = (ext_cap_header >> 20) & 0xFFF;
    }

    char pciErr[128];
    snprintf(pciErr, sizeof(pciErr), "PCIe_DevSta=0x%04x (UR=%d), AER_Uncorr=0x%08x (CTO=%d, CA=%d, UR=%d)",
             pcie_status, (pcie_status >> 3) & 1,
             aer_uncorr, (aer_uncorr >> 14) & 1, (aer_uncorr >> 15) & 1, (aer_uncorr >> 20) & 1);
    RTW_DEBUG_PROPERTY("Debug_PCIe_Errors", pciErr);

    // Restore MAC registers
    write16(0x0204, page | 0x8000); // Restore with BCN_VALID per Linux
    write8(0x0550, bckp_bcn_ctrl);
    write8(0x0422, bckp_txq);
    write8(0x0101, bckp_cr1);

    // No need to advance write pointer since we always use wp=0

    return true;
}

bool RealtekRTL8822C::testBcnqRoundtrip() {
    if (!bcnqDesc) return false;
    UInt8* bcnqVA = (UInt8*)bcnqDesc->getBytesNoCopy();

    // Clear the payload area
    memset(bcnqVA + 512, 0, 4096);

    // Write 48-byte TX descriptor manually (minimal)
    // Word 0: 16 (size), offset 40 (psb)
    *(UInt16*)(bcnqVA + 512 + 0) = 16;
    *(UInt16*)(bcnqVA + 512 + 2) = 40; // OFFSET
    // Word 1: QSEL_BEACON (0x10) in bits 8-12
    *(UInt32*)(bcnqVA + 512 + 4) = (0x10 << 8);
    // Other words = 0
    for(int i=8; i<48; i+=4) *(UInt32*)(bcnqVA + 512 + i) = 0;

    // Write pattern to payload (+48)
    UInt32* payload = (UInt32*)(bcnqVA + 512 + 48);
    payload[0] = 0xDEADBEEF;
    payload[1] = 0xBAADF00D;
    payload[2] = 0xCAFEBABE;
    payload[3] = 0x12345678;

    // Set up ring buffer descriptors
    IOPhysicalAddress paddr = bcnqDesc->getPhysicalSegment(512, NULL);
    UInt32* bdVA = (UInt32*)bcnqVA;
    bdVA[0] = (1U << 31) | 48 | (40 << 16);
    bdVA[1] = (UInt32)paddr;
    bdVA[2] = (1U << 31) | 16;
    bdVA[3] = (UInt32)(paddr + 48);

    OSSynchronizeIO();

    // Sync R/W pointers
    write32(0x039C, 1);
    OSSynchronizeIO();

    // Select Page 0 (RTK_PCI_TXBD_BCN_WORK)
    write8(0x0383, 0x10); // Ring doorbell

    // Wait for PCI DMA to complete
    IODelay(10000); // 10ms

    // Now read back from TX FIFO using DBG_CTRL
    // fw_fifo_addr for TX is 0x780
    UInt16 ctl = read16(0x0140) & 0xF000;

    // TEMPORARILY ENABLE MAC TO ALLOW DEBUG FIFO READ
    UInt8 old_cr = read8(0x0100);
    write8(0x0100, 0x77); // MAC TX/RX, DMA TX/RX enabled

    write16(0x0140, 0x0780 | ctl);

    // Disable RX clock gate
    write32(0x0608, read32(0x0608) | (1U << 19)); // BIT_DISGCLK

    // Read 16 bytes of TX desc, 16 bytes of payload
    UInt32 rx_desc[4];
    UInt32 rx_payload[4];
    for (UInt32 i = 0; i < 4; i++) rx_desc[i] = read32(0x8000U + i * 4U);
    for (UInt32 i = 0; i < 4; i++) rx_payload[i] = read32(0x8030U + i * 4U);

    // Restore
    write16(0x0140, ctl);
    write32(0x0608, read32(0x0608) & ~(1U << 19));
    write8(0x0100, old_cr);

    char dbg[128];
    snprintf(dbg, sizeof(dbg), "Desc=[%08x %08x...] Pay=[%08x %08x %08x %08x]",
        rx_desc[0], rx_desc[1], rx_payload[0], rx_payload[1], rx_payload[2], rx_payload[3]);
    RTW_DEBUG_PROPERTY("Debug_Roundtrip_TXBUF", dbg);

    return true;
}

bool RealtekRTL8822C::downloadFirmware() {
    if (!bcnqDesc) return false;

    UInt8* bcnqVirtAddr = (UInt8*)bcnqDesc->getBytesNoCopy();

    // Phase 3: Firmware Loading
    if (rtw8822c_fw_len <= 0x20) {
        setProperty("DriverStatus", "Firmware Header too small!");
        return false;
    }

    UInt32 dmem_addr = *(UInt32*)(&rtw8822c_fw[0x20]);
    UInt32 dmem_size = *(UInt32*)(&rtw8822c_fw[0x24]);
    UInt32 imem_size = *(UInt32*)(&rtw8822c_fw[0x30]);
    UInt32 emem_size = *(UInt32*)(&rtw8822c_fw[0x34]);
    UInt32 emem_addr = *(UInt32*)(&rtw8822c_fw[0x38]);
    UInt32 imem_addr = *(UInt32*)(&rtw8822c_fw[0x3C]);

    UInt8 mem_usage = rtw8822c_fw[0x18];
    if (!(mem_usage & 0x10)) emem_size = 0; // BIT(4) for EMEM

    setProperty("Firmware_DMEM_Size", dmem_size, 32);
    setProperty("Firmware_IMEM_Size", imem_size, 32);
    setProperty("Firmware_EMEM_Size", emem_size, 32);

    if (!macPreSystemCfg()) {
        setProperty("DriverStatus", "Failed macPreSystemCfg");
        return false;
    }

    // Run minimal round-trip test
    // testBcnqRoundtrip();

    RTW_DEBUG_PROPERTY("Debug_DDMA_After_PreCfg", read32(0x1208), 32);

    // Execute MAC Power On sequence for RTW8822C (card_enable_flow_8822c)
    if (!parsePowerSeq(trans_carddis_to_cardemu_8822c)) {
        setProperty("DriverStatus", "Failed PowerOn: carddis_to_cardemu");
        return false;
    }
    if (!parsePowerSeq(trans_cardemu_to_act_8822c)) {
        setProperty("DriverStatus", "Failed PowerOn: cardemu_to_act");
        return false;
    }

    if (!macInitSystemCfg()) {
        setProperty("DriverStatus", "Failed macInitSystemCfg");
        return false;
    }

    IODelay(10000); // Allow hardware to stabilize after power on

    // Test DDMA accessibility AFTER initialization
    RTW_DEBUG_PROPERTY("Debug_DDMA_After_Init", read32(0x1208), 32);

    // === wlan_cpu_enable(false) ===
    write8(0x0003, read8(0x0003) & ~(1 << 2)); // REG_SYS_FUNC_EN + 1, clear BIT_FEN_CPUEN
    write8(0x001D, read8(0x001D) & ~(1 << 0)); // REG_RSV_CTRL + 1, clear BIT_WLMCU_IOIF

    // === download_firmware_reg_backup ===
    UInt8 bckp_txdma_pq_map = read8(0x010D); // REG_TXDMA_PQ_MAP (0x010C) + 1
    write8(0x010D, 0xC0); // RTW_DMA_MAPPING_HIGH << 6

    UInt8 bckp_cr = read8(0x0100); // REG_CR
    // Linux sets BIT_HCI_TXDMA_EN (bit0) | BIT_TXDMA_EN (bit2) = 0x05 during fw download.
    // This enables the BCNQ DMA engine to transfer packets into chip TXBUF.
    // Writing 0x00 disabled TXDMA — BCNQ could never deliver data to TXBUF!
    write8(0x0100, 0x05); // BIT_HCI_TXDMA_EN | BIT_TXDMA_EN

#if RTW_DEBUG
    UInt8 reg_cr_check = read8(0x0100);
    RTW_DEBUG_PROPERTY("Debug_REG_CR_After_Write", reg_cr_check, 32);
#endif

    UInt32 bckp_h2cq_csr = read32(0x1330); // REG_H2CQ_CSR (0x1330)
    write32(0x1330, (1U << 31)); // OVERWRITE with BIT_H2CQ_FULL

    UInt16 bckp_fifopage_info_1 = read16(0x0230); // REG_FIFOPAGE_INFO_1 (0x0230)
    UInt32 bckp_rqpn_ctrl_2 = read32(0x022C); // REG_RQPN_CTRL_2 (0x022C)

    write16(0x0230, 0x200);
    write32(0x022C, bckp_rqpn_ctrl_2 | (1U << 31)); // BIT_LD_RQPN (BIT 31)

    // === download_firmware_reset_platform ===
    write8(0x1082, read8(0x1082) & ~0x01); // REG_CPU_DMEM_CON clear BIT_WL_PLATFORM_RST
    write8(0x0009, read8(0x0009) & ~0x40); // REG_SYS_CLK_CTRL clear BIT_CPU_CLK_EN (BIT 6 of +1)

    write8(0x1082, read8(0x1082) | 0x01); // set BIT_WL_PLATFORM_RST
    write8(0x0009, read8(0x0009) | 0x40); // set BIT_CPU_CLK_EN



    // Firmware Download Enable
    UInt16 fw_ctrl_backup = read16(0x0080) & 0x3800; // REG_MCUFW_CTRL
    write16(0x0080, fw_ctrl_backup | 0x01); // BIT_MCUFWDL_EN

    // 4. Map the physical address to the BCNQ Register
    memset(bcnqVirtAddr, 0, 8192);

    // Restore RTK_PCI_CTRL + 3 (0x0303) as it might enable TRXDMA channels!
    write32(0x0300, read32(0x0300) | 0x00108000); // BIT_RST_TRXDMA_INTF (bit 20) | BIT_RX_TAG_EN (bit 15)
    write8(0x0303, read8(0x0303) | 0xF7);
    RTW_DEBUG_PROPERTY("Debug_0300_Readback", read32(0x0300), 32);
    RTW_DEBUG_PROPERTY("Debug_0303_Readback", read8(0x0303), 8);

    UInt16 rsvd_boundary = 1938;
    write16(0x0204, rsvd_boundary);
    write16(0x0424, rsvd_boundary);
    write16(0x0206, rsvd_boundary);
    write16(0x0456, rsvd_boundary);


    IODMACommand::Segment32 segments[1];
    UInt32 numSegments = 1;
    UInt64 offset = 0;
    bcnqDmaCmd->gen32IOVMSegments(&offset, segments, &numSegments);
    addr64_t bcnqPhysAddr = segments[0].fIOVMAddr;

    UInt32 purePhys = (UInt32)(bcnqDesc->getPhysicalSegment(0, NULL) & 0xFFFFFFFF);
    char iovaDb[128];
    snprintf(iovaDb, sizeof(iovaDb), "IOVA=0x%08llx PHYS=0x%08x", bcnqPhysAddr, purePhys);
    RTW_DEBUG_PROPERTY("Debug_IOMMU_Addr_Init", iovaDb);

    write32(0x0308, (UInt32)(bcnqPhysAddr & 0xFFFFFFFF));
    setProperty("Hardware_BCNQ_PHYS_ADDR", (uint64_t)bcnqPhysAddr, 64);

    // Reset ALL read/write pointers (RTK_PCI_TXBD_RWPTR_CLR)
    // This is critical! Without this, the hardware BCNQ write pointer
    // is out of sync with our software wp=0, and BCNQ DMA won't fire.
    write32(0x039C, 0xFFFFFFFF);
    OSSynchronizeIO();

    char rwptr_db[64];
    snprintf(rwptr_db, sizeof(rwptr_db), "RWPTR_CLR done, 039C readback=0x%08x", read32(0x039C));
    RTW_DEBUG_PROPERTY("Debug_RWPTR_CLR", rwptr_db);

    UInt32 mem_sizes[3] = {dmem_size, imem_size, emem_size};
    UInt32 mem_addrs[3] = {dmem_addr, imem_addr, emem_addr};
    UInt32 cur_offset = 64; // FW_HDR_SIZE

    for (int sec = 0; sec < 3; sec++) {
        UInt32 section_size = mem_sizes[sec];
        if (section_size == 0) continue;

        UInt32 target_addr = mem_addrs[sec] & ~0x80000000;
        section_size += 8; // Add 8 bytes checksum size

        // Clear DDMA Checksum status
        UInt32 ddmaval = read32(0x1208);
        ddmaval |= (1U << 25);
        write32(0x1208, ddmaval);
        IODelay(500);

        for (UInt32 mem_offset = 0; mem_offset < section_size; ) {
            UInt32 chunk_size = section_size - mem_offset;
            if (chunk_size > 4096) chunk_size = 4096;

            bool first = (mem_offset == 0);

            // Wait for DDMA ready if not first
            if (!first) {
                bool ddma_ok = false;
                for (int i = 0; i < 1000; i++) {
                    if (!(read32(0x1208) & (1U << 31))) { // BIT_DDMACH0_OWN
                        ddma_ok = true;
                        break;
                    }
                    IODelay(100);
                }
                if (!ddma_ok) {
                    setProperty("DriverStatus", "DDMA timeout waiting for OWN bit (Post-Send)");
                    return false;
                }
            }

            // Send chunk via BCNQ page 0 (matching Linux: src=0, page = src>>7 = 0)
            if (!sendFirmwarePacket(0, &rtw8822c_fw[cur_offset], chunk_size)) {
                setProperty("DriverStatus", "Firmware Download Failed at BCNQ!");
                return false;
            }

            // SA = OCPBASE_TXBUF_88XX + 0 + TX_DESC_SIZE(48) = 0x18780030 (matching Linux exactly)
            write32(0x1200, 0x18780000U + 48U); // REG_DDMA_CH0SA = 0x18780030
            write32(0x1204, target_addr + mem_offset); // REG_DDMA_CH0DA

            UInt32 ctrl = (1U << 31) | (1U << 29); // BIT_DDMACH0_OWN | BIT_DDMACH0_CHKSUM_EN
            ctrl |= (chunk_size & 0x3FFFFU); // BIT_MASK_DDMACH0_DLEN
            if (!first) ctrl |= (1U << 24); // BIT_DDMACH0_CHKSUM_CONT

            write32(0x1208, ctrl); // triggers DDMA

            // Wait for DDMA to complete
            bool chunk_ok = false;
            UInt32 last_ddma_ctrl = 0;
            int iters = 0;
            for (int i = 0; i < 500; i++) {
                last_ddma_ctrl = read32(0x1208);
                if (!(last_ddma_ctrl & (1U << 31))) { // OWN bit cleared
                    chunk_ok = true;
                    iters = i;
                    break;
                }
                IODelay(1000);
            }

            char debugStr[128];
            if (!chunk_ok) {
                snprintf(debugStr, sizeof(debugStr), "Chunk Timeout! sec=%d, offset=%u, size=%u, ctrl=0x%08x", sec, mem_offset, chunk_size, last_ddma_ctrl);
                setProperty("DriverStatus", debugStr);
                return false;
            } else {
                snprintf(debugStr, sizeof(debugStr), "Chunk OK! sec=%d, offset=%u, iters=%d, ctrl=0x%08x", sec, mem_offset, iters, last_ddma_ctrl);
                RTW_DEBUG_PROPERTY("Debug_Last_Chunk", debugStr);
            }

            mem_offset += chunk_size;
            cur_offset += chunk_size;
        }

        // Software XOR-16 checksum verification (host side)
        // This proves whether the data in our DMA buffer is correct
        if (sec == 0) {
            UInt32 sw_data_offset = 64; // FW_HDR_SIZE, start of DMEM
            UInt16 xor16 = 0;
            for (UInt32 i = 0; i < section_size; i += 2) {
                UInt16 word = *(UInt16*)(&rtw8822c_fw[sw_data_offset + i]);
                xor16 ^= word;
            }
            char swChk[128];
            snprintf(swChk, sizeof(swChk), "SW_XOR16=0x%04x (should be 0x0000), sec_size=%u", xor16, section_size);
            RTW_DEBUG_PROPERTY("Debug_SW_Checksum", swChk);

            // Dump first 16 bytes of DMA buffer payload area
            // This is what PCI DMA should send to TXBUF
            UInt8* bcnqVA = (UInt8*)bcnqDesc->getBytesNoCopy();
            UInt8* dump_ptr = bcnqVA + 512 + 48; // payload_offset + tx_desc_size
            char dmpStr[128];
            snprintf(dmpStr, sizeof(dmpStr),
                "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                dump_ptr[0], dump_ptr[1], dump_ptr[2], dump_ptr[3],
                dump_ptr[4], dump_ptr[5], dump_ptr[6], dump_ptr[7],
                dump_ptr[8], dump_ptr[9], dump_ptr[10], dump_ptr[11],
                dump_ptr[12], dump_ptr[13], dump_ptr[14], dump_ptr[15]);
            RTW_DEBUG_PROPERTY("Debug_DMA_Payload_Dump", dmpStr);

            // Also dump first 16 bytes of what SHOULD be there from FW
            UInt8* expected = (UInt8*)&rtw8822c_fw[64 + (section_size - 2952)]; // last chunk data
            char expStr[128];
            snprintf(expStr, sizeof(expStr),
                "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
                expected[0], expected[1], expected[2], expected[3],
                expected[4], expected[5], expected[6], expected[7],
                expected[8], expected[9], expected[10], expected[11],
                expected[12], expected[13], expected[14], expected[15]);
            RTW_DEBUG_PROPERTY("Debug_Expected_FW_Data", expStr);
        }

        // Validate Checksum for this section (DDMA already completed all chunks for this section)
        UInt32 chksum_val = read32(0x1208);
        char debugChk[128];
        if (chksum_val & (1 << 27)) { // BIT_DDMACH0_CHKSUM_STS
            snprintf(debugChk, sizeof(debugChk), "Checksum FAILED! sec=%d, val=0x%08x", sec, chksum_val);
            setProperty("DriverStatus", debugChk);
            return false;
        } else {
            snprintf(debugChk, sizeof(debugChk), "Checksum OK! sec=%d, val=0x%08x", sec, chksum_val);
            RTW_DEBUG_PROPERTY("Debug_Last_Checksum", debugChk);
        }

        UInt8 fw_ctrl8 = read8(0x0080);
        if (target_addr < 0x00200000) { // OCPBASE_DMEM_88XX
            fw_ctrl8 |= (1 << 3) | (1 << 4); // BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK
        } else {
            fw_ctrl8 |= (1 << 5) | (1 << 6); // BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK
        }
        write8(0x0080, fw_ctrl8);
    }

    // End Flow
    // REG_TXDMA_STATUS is W1C. Linux clears BTI_PAGE_OVF at 0x0210 here.
    write32(0x0210, (1U << 2));

    // === download_firmware_reg_restore ===
    write32(0x022C, bckp_rqpn_ctrl_2);
    write16(0x0230, bckp_fifopage_info_1);
    write32(0x1330, bckp_h2cq_csr);
    write8(0x0100, bckp_cr);
    write8(0x010D, bckp_txdma_pq_map);

    UInt16 fw_ctrl = read16(0x0080);
    if ((fw_ctrl & 0x0050) != 0x0050) { // BIT_CHECK_SUM_OK (BIT 4 | BIT 6)
        setProperty("DriverStatus", "Firmware Checksum OK bits not set!");
        return false;
    }

    fw_ctrl = (fw_ctrl | (1 << 14)) & ~0x01; // BIT_FW_DW_RDY (14) and ~BIT_MCUFWDL_EN (0)
    write16(0x0080, fw_ctrl);

    // CPU Enable
    write8(0x001D, read8(0x001D) | 0x01);
    write8(0x0003, read8(0x0003) | 0x04);

    // Validate (FW_READY)
    int fw_rdy_timeout = 1000;
    UInt32 fw_ready = (1 << 15) | (1 << 14) | (1 << 3) | (1 << 5) | (1 << 4) | (1 << 6); // 0xC078
    UInt32 fw_ready_mask = 0xFFFF & ~( (1 << 12) | (1 << 13) ); // 0xCFFF

    while ((read32(0x0080) & fw_ready_mask) != fw_ready && fw_rdy_timeout > 0) {
        IODelay(20000); // 20ms
        fw_rdy_timeout--;
    }

    if (fw_rdy_timeout <= 0) {
        setProperty("DriverStatus", "Firmware Validation Failed (FW_READY timeout)!");
        return false;
    }

    resetFirmwareH2cState("firmware-ready");

    setProperty("DriverStatus", "Firmware Download Completed Successfully!");
    return true;
}

IOReturn RealtekRTL8822C::setInterfaceEnabledGated(bool enabled) {
    RTW_DEBUG_LOG("RealtekRTL8822C: interface state=%d\n", enabled ? 1 : 0);
    if (hardwareSuspended) {
        interfaceEnabled = enabled;
        interfaceEnabledBeforeSleep = enabled;
        setProperty("InterfaceState", enabled ? "EnabledOnWake" : "Disabled");
        return kIOReturnSuccess;
    }

    if (enabled) {
        interfaceEnabled = true;
        setLinkStatus(kIONetworkLinkValid);
        setProperty("InterfaceState", "Enabled");
        setProperty("DriverStatus", "Network interface enabled; reconnect required");
        publishLifecycleInvariant("interface-enable", true);
        return kIOReturnSuccess;
    }

    interfaceEnabled = false;
    IOOutputQueue* queue = getOutputQueue();
    if (queue) queue->stop();
    if (connState != CONN_STATE_DISCONNECTED)
        disconnectFromNetwork("interface-disabled", true);
    else
        clearAllSecurityCam("interface-disabled");
    if (queue) {
        queue->flush();
    }
    setLinkStatus(kIONetworkLinkValid);
    setProperty("InterfaceState", "Disabled");
    setProperty("DriverStatus", "Network interface disabled");
    publishLifecycleInvariant("interface-disable", true);
    return kIOReturnSuccess;
}

IOReturn RealtekRTL8822C::setUserInterfaceEnabledGated(bool enabled) {
    interfaceUserDisabled = !enabled;
    setProperty("InterfaceUserEnabled", enabled ? kOSBooleanTrue :
                                                 kOSBooleanFalse);
    return setInterfaceEnabledGated(enabled);
}

void RealtekRTL8822C::reconcileInterfaceStateFromBsd() {
    if (!netif || hardwareSuspended || interfaceEnabled ||
        interfaceUserDisabled) return;
    UInt16 bsdFlags = netif->getFlags();
    if ((bsdFlags & IFF_UP) == 0) return;
    setInterfaceEnabledGated(true);
    char db[128];
    snprintf(db, sizeof(db),
             "action=enable-from-bsd-up flags=%04x driver_enabled=%d",
             bsdFlags, interfaceEnabled ? 1 : 0);
    RTW_DEBUG_PROPERTY("Debug_Interface_Reconcile", db);
    traceEvent("interface:reconciled-bsd-up");
}

IOReturn RealtekRTL8822C::enable(IONetworkInterface *interface) {
    (void)interface;
    traceEvent("interface:enable-callback");
    if (interfaceUserDisabled) {
        traceEvent("interface:enable-suppressed-user-disabled");
        return setInterfaceEnabledGated(false);
    }
    return setInterfaceEnabledGated(true);
}

IOReturn RealtekRTL8822C::disable(IONetworkInterface *interface) {
    (void)interface;
    traceEvent("interface:disable-callback");
    return setInterfaceEnabledGated(false);
}

IOReturn RealtekRTL8822C::selectMedium(const IONetworkMedium* medium) {
    if (medium) {
        setSelectedMedium(medium);
        return kIOReturnSuccess;
    }
    return kIOReturnError;
}

IOReturn RealtekRTL8822C::getPacketFilters(UInt32* filters) const {
    if (!filters) return kIOReturnBadArgument;
    // The RX path accepts the station address, broadcast and all multicast.
    // Do not advertise promiscuous mode: linked software admission deliberately
    // rejects foreign unicast frames.
    *filters = kIOPacketFilterUnicast |
               kIOPacketFilterBroadcast |
               kIOPacketFilterMulticast;
    return kIOReturnSuccess;
}

IOReturn RealtekRTL8822C::setMulticastMode(bool active) {
    if (!ioBase) return kIOReturnNotReady;
    UInt32 rcr = read32(0x0608);
    if (active) rcr |= (1U << 2);   // RCR_AM: accept multicast
    else rcr &= ~(1U << 2);
    write32(0x0608, rcr);
    OSSynchronizeIO();

    char db[128];
    snprintf(db, sizeof(db), "operation=multicast-mode active=%d rcr=%08x",
             active ? 1 : 0, (unsigned int)read32(0x0608));
    RTW_DEBUG_PROPERTY("Debug_Filter_Lifecycle", db);
    traceEvent(active ? "filter:multicast-on" : "filter:multicast-off");
    return kIOReturnSuccess;
}

IOReturn RealtekRTL8822C::setMulticastList(IOEthernetAddress* addrs,
                                        UInt32 count) {
    (void)addrs;
    // RTL8822C is intentionally kept in accept-all-multicast mode here. The
    // software RX admission path still requires a group destination address.
    char db[128];
    snprintf(db, sizeof(db),
             "operation=multicast-list count=%u policy=accept-all-multicast",
             (unsigned int)count);
    RTW_DEBUG_PROPERTY("Debug_Filter_Lifecycle", db);
    return kIOReturnSuccess;
}







IOReturn RealtekRTL8822C::getHardwareAddress(IOEthernetAddress* addr) {
    memcpy(addr->bytes, macAddress, 6);
    return kIOReturnSuccess;
}

IOReturn RealtekRTL8822C::setHardwareAddress(const IOEthernetAddress* addr) {
    memcpy(macAddress, addr->bytes, 6);
    if (ioBase) {
        for (UInt32 i = 0; i < 6; i++) write8(0x0610U + i, macAddress[i]);
        OSSynchronizeIO();
    }
    return kIOReturnSuccess;
}

const OSString * RealtekRTL8822C::newVendorString() const {
    return OSString::withCString("Apple");
}

const OSString * RealtekRTL8822C::newModelString() const {
    return OSString::withCString("RTL8822CE");
}







UInt32 RealtekRTL8822C::dropOutputPacket(mbuf_t m, const char* reason) {
    debugOutputDropped++;
    strlcpy(debugOutputLastDrop, reason ? reason : "unknown", sizeof(debugOutputLastDrop));
    if (m) freePacket(m);
    return kIOReturnOutputDropped;
}

// Caller holds beqLock. Keep this limited to software accounting and the
// BEQ host index: this helper is used both from outputPacket() and BEDOK.
void RealtekRTL8822C::updateBeqCompletionLocked() {
    UInt32 bdIdx = read32(0x03A8);
    UInt32 hwRp = (bdIdx >> 16) & 0x0fff;
    hwRp %= 256;
    UInt32 consumed = (hwRp + 256 - beqLastHwRp) % 256;
    if (consumed > beqOutstanding) consumed = beqOutstanding;
    beqOutstanding -= consumed;
    beqLastHwRp = hwRp;
    beqRp = hwRp;

    // With no descriptors in flight, hardware RP is the authoritative slot.
    if (beqOutstanding == 0 && beqWp != hwRp) {
        beqWp = hwRp;
        write16(0x03A8, (UInt16)beqWp);
        OSSynchronizeIO();
    }
}

void RealtekRTL8822C::updateBeqCompletion() {
    if (!beqLock) return;
    IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
    updateBeqCompletionLocked();
    IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
}

void RealtekRTL8822C::serviceBeqCompletion(bool fromRxPoll) {
    if (!beqLock) return;
#if RTW_DEBUG
    uint64_t serviceStarted = 0;
    if (fromRxPoll) clock_get_uptime(&serviceStarted);
#endif
    bool restartOutput = false;
    IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
    updateBeqCompletionLocked();
    // Linux rtw_pci_tx_isr() wakes only when avail_desc() > 4. One slot is
    // permanently reserved, so a 256-entry ring has 255 - outstanding free
    // descriptors and the matching condition is outstanding <= 250.
    if (beqQueueStalled && beqOutstanding < 251) {
        beqQueueStalled = false;
        restartOutput = true;
    }
    IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);

    if (fromRxPoll) debugRxTxServiceCalls++;
    if (restartOutput) {
        if (fromRxPoll) debugRxTxQueueRestarts++;
        IOOutputQueue* queue = getOutputQueue();
        // The queue is already running; kIOReturnOutputStall only paused its
        // service path. IOBasicOutputQueue::start() may synchronously drain
        // the retained mbufs on this caller, which used to execute on the RX
        // work loop and produced 100+ ms receive-poll outliers. Resume it on
        // its dedicated service thread exactly as IOBasicOutputQueue requires.
        if (queue) queue->service(IOBasicOutputQueue::kServiceAsync);
    }
#if RTW_DEBUG
    if (fromRxPoll) {
        uint64_t serviceFinished = 0;
        uint64_t serviceElapsedNs = 0;
        clock_get_uptime(&serviceFinished);
        absolutetime_to_nanoseconds(serviceFinished - serviceStarted,
                                    &serviceElapsedNs);
        debugRxTxServiceTotalNs += serviceElapsedNs;
        if (serviceElapsedNs > debugRxTxServiceMaxNs)
            debugRxTxServiceMaxNs = serviceElapsedNs;
    }
#endif
}

// Recover only the BEQ indices after a confirmed TXDMA payload overflow.
// RTL8822C uses bit 4 for the BEQ host index and bit 20 for its hardware
// index in REG_BD_RWPTR_CLR (0x039c). Do not use the boot-time all-queue
// reset here: RX, MGMTQ, and H2CQ must remain live during a data-path fault.
bool RealtekRTL8822C::recoverBeqPayloadOverflow() {
    if (!beqLock || !ioBase) return false;

    UInt32 beforeIdx = 0;
    UInt32 beforeStatus = 0;
    UInt32 afterIdx = 0;
    UInt32 afterStatus = 0;
    UInt32 resetMask = (1U << 20) | (1U << 4);

    IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
    beforeIdx = read32(0x03A8);
    beforeStatus = read32(0x0210);

    // Prevent the AC scheduler from consuming a descriptor while its BEQ
    // indices are reset. Bit 2 is BE; restore the exact prior pause state.
    UInt8 savedPause = read8(0x0522);
    write8(0x0522, savedPause | (1U << 2));
    OSSynchronizeIO();

    write32(0x039c, resetMask);
    OSSynchronizeIO();

    // TXDMA_STATUS is W1C. Clear only bits reported by hardware after the
    // index reset, then resynchronise the software ring to the new HW RP.
    UInt32 pendingStatus = read32(0x0210);
    if (pendingStatus) write32(0x0210, pendingStatus);
    OSSynchronizeIO();

    afterIdx = read32(0x03A8);
    UInt32 hwRp = ((afterIdx >> 16) & 0x0fff) % 256;
    beqWp = hwRp;
    beqRp = hwRp;
    beqLastHwRp = hwRp;
    beqOutstanding = 0;
    beqQueueStalled = false;
    // Any status report for the discarded ring is no longer attributable to
    // a post-recovery descriptor.
    debugDataReportPending = false;
    write16(0x03A8, (UInt16)hwRp);
    write8(0x0522, savedPause);
    OSSynchronizeIO();

    afterStatus = read32(0x0210);
    debugBeqRecoveries++;
    IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);

    char recovery[256];
    snprintf(recovery, sizeof(recovery),
             "count=%u reset=%08x idx=%08x->%08x status=%08x->%08x rp=%u",
             (unsigned int)debugBeqRecoveries, (unsigned int)resetMask,
             (unsigned int)beforeIdx, (unsigned int)afterIdx,
             (unsigned int)beforeStatus, (unsigned int)afterStatus,
             (unsigned int)hwRp);
    RTW_DEBUG_PROPERTY("Debug_BEQ_Recovery", recovery);
    traceEvent("beq:payload-overflow-recovered");
    return true;
}

void RealtekRTL8822C::captureBeqPayloadOverflow(const char* reason) {
    if (!beqDesc || !beqPayloadDesc) return;

    // Stop new queued output before capturing the two slots. The descriptor
    // dump stays valid evidence for the fault even though recovery follows.
    IOOutputQueue* queue = getOutputQueue();
    if (queue) queue->stop();

    UInt32 swWp = 0, outstanding = 0, lastSlot = 0, lastWifiLen = 0, lastPsbLen = 0;
    if (beqLock) {
        IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
        swWp = beqWp;
        outstanding = beqOutstanding;
        lastSlot = debugBeqLastSlot;
        lastWifiLen = debugBeqLastWifiLen;
        lastPsbLen = debugBeqLastPsbLen;
        IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
    }
    UInt32 idx = read32(0x03a8);
    UInt32 hwRp = ((idx >> 16) & 0x0fff) % 256;
    UInt32 inspectSlots[2] = { lastSlot % 256, hwRp };
    char fault[1024];
    int pos = snprintf(fault, sizeof(fault),
                       "reason=%s status=%08x idx=%08x num=%04x base=%08x sw_wp=%u hw_rp=%u outstanding=%u expected_slot=%u len=%u psb=%u",
                       reason ? reason : "unknown", (unsigned int)read32(0x0210), (unsigned int)idx,
                       (unsigned int)read16(0x0388), (unsigned int)read32(0x0328),
                       (unsigned int)swWp, (unsigned int)hwRp, (unsigned int)outstanding,
                       (unsigned int)lastSlot, (unsigned int)lastWifiLen,
                       (unsigned int)lastPsbLen);
    for (int n = 0; n < 2 && pos > 0 && (UInt32)pos < sizeof(fault); n++) {
        UInt32 slot = inspectSlots[n];
        UInt8* payload = (UInt8*)beqPayloadDesc->getBytesNoCopy() + slot * 2048;
        volatile UInt32* desc = (volatile UInt32*)payload;
        volatile UInt32* bd = (volatile UInt32*)((UInt8*)beqDesc->getBytesNoCopy() + slot * 16);
        int written = snprintf(fault + pos, sizeof(fault) - (size_t)pos,
                               " | s%u W=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x BD=%08x/%08x/%08x/%08x fc=%02x%02x",
                               (unsigned int)slot,
                               (unsigned int)desc[0], (unsigned int)desc[1], (unsigned int)desc[2],
                               (unsigned int)desc[3], (unsigned int)desc[4], (unsigned int)desc[5],
                               (unsigned int)desc[6], (unsigned int)desc[7], (unsigned int)desc[8],
                               (unsigned int)desc[9], (unsigned int)bd[0], (unsigned int)bd[1],
                               (unsigned int)bd[2], (unsigned int)bd[3], payload[48], payload[49]);
        if (written > 0) pos += written;
    }
    RTW_DEBUG_PROPERTY("Debug_BEQ_Fault", fault);
    debugBeqFaults++;
    strlcpy(debugOutputLastDrop, "txdma-payload-overflow", sizeof(debugOutputLastDrop));
    if (recoverBeqPayloadOverflow()) {
        if (queue) queue->start();
    } else {
        if (beqLock) {
            IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
            beqQueueStalled = true;
            IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
        }
    }
}

RealtekRTL8822C::DataEnqueueResult RealtekRTL8822C::enqueueEthernetData(
    const UInt8* ethFrame, UInt32 ethLen, bool protect,
    bool allowAggregation, bool requestReport) {
    if (!ethFrame || ethLen < 14 || ethLen > 1514 || !beqPayloadDesc ||
        !beqDesc || !beqLock)
        return !ethFrame || ethLen < 14 || ethLen > 1514 ?
            DATA_ENQUEUE_INVALID_FRAME : DATA_ENQUEUE_NOT_READY;
    if (protect && !wpaPtkInstalled) return DATA_ENQUEUE_NOT_READY;
    UInt16 ethType = (UInt16)(((UInt16)ethFrame[12] << 8) | (UInt16)ethFrame[13]);
    UInt8 wifi[1544];
    UInt32 wifiLen = 0;
    if (!translate8023to80211(ethFrame, ethLen, wifi, &wifiLen))
        return DATA_ENQUEUE_INVALID_FRAME;

    IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
    updateBeqCompletionLocked();
    // Linux rtw_pci_tx_write() stops the software queue once fewer than two
    // descriptors remain after a submission. Since IOBasicOutputQueue asks
    // us for the next packet before it can observe that state, stall that next
    // call at 254 outstanding rather than consuming the final reserved slot.
    if (beqOutstanding >= 254) {
        beqQueueStalled = true;
        IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
        return DATA_ENQUEUE_RING_FULL;
    }

    UInt16 dataSn = dataSeqNum++ & 0x0fff;
    UInt16 seqControl = (UInt16)(dataSn << 4);
    wifi[22] = (UInt8)seqControl;
    wifi[23] = (UInt8)(seqControl >> 8);
    UInt64 packetPn = 0;
    if (protect) {
        UInt8 subtype = (wifi[0] >> 4) & 0x0f;
        UInt32 headerLen = (subtype & 0x08) ? 26 : 24;
        if (wifiLen + 8 > sizeof(wifi)) {
            IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
            return DATA_ENQUEUE_INVALID_FRAME;
        }
        memmove(wifi + headerLen + 8, wifi + headerLen, wifiLen - headerLen);
        packetPn = ++wpaTxPn;
        UInt8* ccmp = wifi + headerLen;
        ccmp[0] = (UInt8)packetPn;
        ccmp[1] = (UInt8)(packetPn >> 8);
        ccmp[2] = 0;
        ccmp[3] = 0x20; // KeyID 0, ExtIV
        ccmp[4] = (UInt8)(packetPn >> 16);
        ccmp[5] = (UInt8)(packetPn >> 24);
        ccmp[6] = (UInt8)(packetPn >> 32);
        ccmp[7] = (UInt8)(packetPn >> 40);
        wifi[1] |= 0x40;
        wifiLen += 8;
    }

    UInt32 wp = beqWp;
    volatile UInt32* bufferDescriptor = (volatile UInt32*)((UInt8*)beqDesc->getBytesNoCopy() + wp * 16);
    UInt8* slot = (UInt8*)beqPayloadDesc->getBytesNoCopy() + wp * 2048;
#if RTW_DEBUG
    UInt32 slotPhysical = (UInt32)(beqPayloadDesc->getPhysicalSegment(wp * 2048, NULL) & 0xffffffff);
#endif
    UInt32 slotDma = (UInt32)(beqPayloadPhysAddr + wp * 2048);
    volatile UInt32* descriptor = (volatile UInt32*)slot;
    memset((void*)descriptor, 0, 48);
    descriptor[0] = (wifiLen & 0xffff) | (48U << 16) | (1U << 26);
    if (wifi[4] & 1) descriptor[0] |= 1U << 24;

    bool aggregate = allowAggregation && htNegotiated && baEstablished && baTid == 0;
    UInt8 rateId = htNegotiated ? negotiatedRateId : (targetChannel > 14 ? 7 : 6);
    UInt8 initialRate = vhtNegotiated ?
        (negotiatedRateId == 9 ? (targetBandwidth == 0 ? 0x3e : 0x3f) :
                                 (targetBandwidth == 0 ? 0x34 : 0x35)) :
        (htNegotiated ?
         ((negotiatedRateId == 2 || negotiatedRateId == 4) ? 0x1b : 0x13) : 0x04);
    descriptor[1] = (UInt32)rateId << 16;
    if (protect) descriptor[1] |= 3U << 22; // Linux RTW CAM AES security type
    descriptor[4] = initialRate;
    descriptor[5] = ((UInt32)targetBandwidth & 0x03U) << 5;
    descriptor[9] = ((UInt32)dataSn & 0x0fff) << 12;
    if (aggregate) {
        UInt8 peerExp = targetHtAmpduParams & 0x03;
        UInt8 peerDensity = (targetHtAmpduParams >> 2) & 0x07;
        UInt8 maxAggregate = (UInt8)((4U << peerExp) - 1U);
        if (maxAggregate > 31) maxAggregate = 31;
        descriptor[2] |= (1U << 12) | ((UInt32)peerDensity << 20);
        descriptor[3] |= (UInt32)maxAggregate << 17;
    }

    UInt8 reportSn = 0;
    bool isIcmp = ethType == 0x0800 && ethLen >= 35 && ethFrame[23] == 1;
    if (requestReport && !debugDataReportPending && (ethType == 0x0806 || isIcmp)) {
        txSeqNum = (txSeqNum + 1) % 64;
        reportSn = (UInt8)((txSeqNum << 2) & 0xfc);
        debugDataReportSn = reportSn;
        debugDataReportEtherType = ethType;
        debugDataReportSeq = dataSn;
        debugDataReportPending = true;
        descriptor[2] |= 1U << 19;
        descriptor[6] = reportSn;
    }

    UInt16 checksum = 0;
    volatile UInt16* words = (volatile UInt16*)descriptor;
    for (int i = 0; i < 24; i++) checksum ^= words[i];
    descriptor[7] = (descriptor[7] & 0xffff0000U) | checksum;
    memcpy(slot + 48, wifi, wifiLen);
    UInt32 psbLen = (wifiLen + 48 + 127) / 128;
    debugBeqLastSlot = wp;
    debugBeqLastWifiLen = wifiLen;
    debugBeqLastPsbLen = psbLen;
    bufferDescriptor[0] = 48 | (psbLen << 16);
    bufferDescriptor[1] = slotDma;
    bufferDescriptor[2] = wifiLen;
    bufferDescriptor[3] = slotDma + 48;
    beqPayloadDmaCmd->synchronize(kIODirectionOut);
    beqDmaCmd->synchronize(kIODirectionOut);
    OSSynchronizeIO();
    beqWp = (wp + 1) % 256;
    beqOutstanding++;
    write16(0x03a8, (UInt16)beqWp);
    OSSynchronizeIO();

#if RTW_DEBUG
    UInt32 debugW0 = descriptor[0], debugW1 = descriptor[1], debugW2 = descriptor[2];
    UInt32 debugW3 = descriptor[3], debugW4 = descriptor[4];
    UInt32 debugW5 = descriptor[5], debugW9 = descriptor[9];
    UInt32 debugBd0 = bufferDescriptor[0], debugBd1 = bufferDescriptor[1];
#endif
    IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);

#if RTW_DEBUG
    bool importantTxDiagnostic = ethType == 0x888e || ethType == 0x0806 || isIcmp || reportSn != 0;
    if (importantTxDiagnostic || ((UInt32)debugOutputCalls & kRtwHotDiagnosticMask) == 1U) {
        char txDb[512];
        snprintf(txDb, sizeof(txDb),
                 "eth=%04x len=%u wifi=%u sn=%u rpt=%02x ht=%d vht=%d bw=%u agg=%d rateid=%u protected=%d sec=%u pn=%llu W0=%08x W1=%08x W2=%08x W3=%08x W4=%08x W5=%08x W9=%08x BD0=%08x BD1=%08x phys=%08x iova=%08x match=%d",
                 (unsigned int)ethType, (unsigned int)ethLen, (unsigned int)wifiLen,
                 (unsigned int)dataSn, reportSn, htNegotiated ? 1 : 0,
                 vhtNegotiated ? 1 : 0,
                 targetBandwidth == 2 ? 80U : (targetBandwidth == 1 ? 40U : 20U),
                 aggregate ? 1 : 0, rateId,
                 protect ? 1 : 0, protect ? 3 : 0,
                 (unsigned long long)packetPn, (unsigned int)debugW0,
                 (unsigned int)debugW1, (unsigned int)debugW2, (unsigned int)debugW3,
                 (unsigned int)debugW4, (unsigned int)debugW5,
                 (unsigned int)debugW9, (unsigned int)debugBd0,
                 (unsigned int)debugBd1, (unsigned int)slotPhysical, (unsigned int)slotDma,
                 slotPhysical == slotDma ? 1 : 0);
        RTW_DEBUG_PROPERTY("Debug_Tx_Last_Data", txDb);
    }
#endif
    return DATA_ENQUEUE_SUCCESS;
}

IOOutputQueue* RealtekRTL8822C::createOutputQueue() {
    // Without an IOOutputQueue, kIOReturnOutputStall is returned directly to
    // the network stack as error 0x102 instead of retaining and retrying the
    // same mbuf. BEQ submission is already protected by beqLock, so use the
    // independent basic queue service thread. A gated queue shares the RX work
    // loop and v59 proved it can stretch one receive slice to 189 ms.
    return IOBasicOutputQueue::withTarget(this, 1024);
}

UInt32 RealtekRTL8822C::outputPacket(mbuf_t m, void *param) {
    (void)param;
    if (!m) return kIOReturnOutputSuccess;
    debugOutputCalls++;
    if (!ioBase) return dropOutputPacket(m, "no-io-base");
    if (connState != CONN_STATE_CONNECTED) return dropOutputPacket(m, "not-connected");
    size_t packetLen = mbuf_pkthdr_len(m);
    if (packetLen < 14 || packetLen > 1514) return dropOutputPacket(m, "invalid-length");
    UInt32 ethLen = (UInt32)packetLen;
    UInt8 ethernet[1514];
    if (mbuf_copydata(m, 0, ethLen, ethernet) != 0)
        return dropOutputPacket(m, "copy-failed");
    UInt16 ethType = (UInt16)(((UInt16)ethernet[12] << 8) | (UInt16)ethernet[13]);
    if (!portAuthorized && ethType != 0x888e)
        return dropOutputPacket(m, "controlled-port-closed");
    bool protect = targetSecurityMode == SECURITY_WPA2 && portAuthorized;
    DataEnqueueResult enqueueResult = enqueueEthernetData(
        ethernet, ethLen, protect, ethType != 0x888e, true);
    if (enqueueResult != DATA_ENQUEUE_SUCCESS) {
        if (enqueueResult == DATA_ENQUEUE_RING_FULL) {
            debugOutputStalled++;
            strlcpy(debugOutputLastDrop, "beq-stalled",
                    sizeof(debugOutputLastDrop));
            return kIOReturnOutputStall;
        }
        return dropOutputPacket(
            m, enqueueResult == DATA_ENQUEUE_NOT_READY ?
                "tx-path-not-ready" : "tx-translate-failed");
    }
    freePacket(m);
    debugOutputSuccess++;
    return kIOReturnOutputSuccess;
}

bool RealtekRTL8822C::macPreSystemCfg() {
    write8(0x001C, 0); // REG_RSV_CTRL (0x001C)

    // PCI specific: REG_HCI_OPT_CTRL (0x0074) |= BIT_USB_SUS_DIS (BIT 4)
    write32(0x0074, read32(0x0074) | (1 << 4));

    // Route PAPE/LNA control to WLAN: BIT_PAPE_WLBT_SEL | BIT_LNAON_WLBT_SEL.
    write32(0x0064, read32(0x0064) | (1U << 29) | (1U << 28));

    // Disable LED ownership of PAPE/LNA pins: BIT_PAPE_SEL_EN | BIT_LNAON_SEL_EN.
    write32(0x004C, read32(0x004C) & ~((1U << 25) | (1U << 26)));

    write32(0x0040, read32(0x0040) | (1U << 2)); // BIT_WLRFE_4_5_EN

    char pinmuxDb[128];
    snprintf(pinmuxDb, sizeof(pinmuxDb), "PAD64=%08x LED4C=%08x MUX40=%08x",
             (unsigned int)read32(0x0064), (unsigned int)read32(0x004C),
             (unsigned int)read32(0x0040));
    RTW_DEBUG_PROPERTY("Debug_Pinmux_PreCfg", pinmuxDb);

    // REG_SYS_FUNC_EN (0x0002) &= ~(BIT_FEN_BB_RSTB (BIT 1) | BIT_FEN_BB_GLB_RST (BIT 0))
    write8(0x0002, read8(0x0002) & ~((1 << 1) | (1 << 0)));

    // REG_RF_CTRL (0x001F) &= ~(BIT_RF_SDM_RSTB (BIT 2) | BIT_RF_RSTB (BIT 1) | BIT_RF_EN (BIT 0))
    write8(0x001F, read8(0x001F) & ~((1 << 2) | (1 << 1) | (1 << 0)));

    // REG_WLRF1 (0x00EC) &= ~BIT_WLRF1_BBRF_EN (BIT 24 | BIT 25 | BIT 26)
    write32(0x00EC, read32(0x00EC) & ~((1U << 24) | (1U << 25) | (1U << 26)));

    return true;
}

bool RealtekRTL8822C::macInitSystemCfg() {
    // REG_CPU_DMEM_CON (0x1080) |= BIT_WL_PLATFORM_RST (BIT 16) | BIT_DDMA_EN (BIT 8)
    UInt32 cpu_dmem = read32(0x1080);
    cpu_dmem |= (1 << 16) | (1 << 8);
    write32(0x1080, cpu_dmem);

    // REG_SYS_FUNC_EN + 1 (0x0003) = 0xD8 (sys_func_en for 8822c)
    write8(0x0003, read8(0x0003) | 0xD8);

    // REG_CR_EXT + 3 (0x1103)
    write8(0x1103, (read8(0x1103) & 0xF0) | 0x0C);

    // REG_MCUFW_CTRL (0x0080)
    UInt32 mcufw_ctrl = read32(0x0080);
    if (mcufw_ctrl & (1 << 20)) { // BIT_BOOT_FSPI_EN (BIT 20)
        write32(0x0080, mcufw_ctrl & ~(1U << 20));
        UInt32 gpio_mux = read32(0x0040); // REG_GPIO_MUXCFG (0x0040)
        write32(0x0040, gpio_mux & ~(1U << 19)); // BIT_FSPI_EN (BIT 19)
    }

    return true;
}

bool RealtekRTL8822C::readEfuse() {
    // Enable Efuse Bank / Grant
    // 1. switch bank to wifi (bank 0)
    UInt32 ldo_efuse = read32(0x0034);
    ldo_efuse &= ~(0x3U << 8); // clear BIT_MASK_EFUSE_BANK_SEL (bits 8-9)
    write32(0x0034, ldo_efuse);

    // 2. disable 2.5V LDO (cfg_ldo25(false))
    UInt8 ldo_pwr = read8(0x0029); // REG_ANAPARLDO_POW_MAC
    ldo_pwr &= ~0x01; // clear BIT_LDOE25_PON (bit 0)
    write8(0x0029, ldo_pwr);

    // 3. Dump physical efuse (512 bytes)
    UInt8* phy_map = new UInt8[512];
    if (!phy_map) return false;
    memset(phy_map, 0, 512);

    UInt32 efuse_ctl = read32(0x0030); // REG_EFUSE_CTRL

    for (UInt32 addr = 0; addr < 512; addr++) {
        efuse_ctl &= ~0x3FF00U; // clear BITS_EF_ADDR (bits 8-17)
        efuse_ctl &= ~0xFFU;    // clear BIT_MASK_EF_DATA (bits 0-7)
        efuse_ctl |= (addr & 0x3FF) << 8;
        write32(0x0030, efuse_ctl & ~0x80000000); // clear BIT_EF_FLAG (bit 31)

        int cnt = 100000;
        bool done = false;
        while (cnt > 0) {
            IODelay(1);
            efuse_ctl = read32(0x0030);
            if (efuse_ctl & 0x80000000) { // BIT_EF_FLAG
                done = true;
                break;
            }
            cnt--;
        }
        if (!done) {
            setProperty("DriverStatus", "EFUSE Physical Dump Timeout!");
            delete[] phy_map;
            return false;
        }
        phy_map[addr] = (UInt8)(efuse_ctl & 0xFF);
    }

    char phyDb[128];
    snprintf(phyDb, sizeof(phyDb), "%02x %02x %02x %02x %02x %02x %02x %02x",
             phy_map[0], phy_map[1], phy_map[2], phy_map[3],
             phy_map[4], phy_map[5], phy_map[6], phy_map[7]);
    RTW_DEBUG_PROPERTY("Debug_EFUSE_Physical_Header", phyDb);

    // 4. Decode physical map to logical map (768 bytes)
    UInt8* log_map = new UInt8[768];
    if (!log_map) {
        delete[] phy_map;
        return false;
    }
    memset(log_map, 0xff, 768);

    UInt32 phy_idx = 0;
    // protect_size = 124, physical_size = 512 -> physical_size - protect_size = 388
    while (phy_idx < 388) {
        UInt8 hdr1 = phy_map[phy_idx];
        UInt8 hdr2 = phy_map[phy_idx + 1];
        if (hdr1 == 0xff || ((hdr1 & 0x1f) == 0x0f && hdr2 == 0xff)) {
            break;
        }

        UInt8 blk_idx;
        UInt8 word_en;
        if ((hdr1 & 0x1f) == 0x0f) {
            /* 2-byte header format */
            blk_idx = (((hdr2 & 0xf0) >> 1) | ((hdr1 >> 5) & 0x07));
            word_en = hdr2 & 0x0f;
            phy_idx += 2;
        } else {
            /* 1-byte header format */
            blk_idx = (hdr1 & 0xf0) >> 4;
            word_en = hdr1 & 0x0f;
            phy_idx += 1;
        }

        for (UInt32 i = 0; i < 4; i++) {
            if (((UInt32)word_en & (1U << i)) != 0) {
                continue; // 1 -> not write
            }

            UInt32 log_idx = ((UInt32)blk_idx << 3) + (i << 1);
            if (phy_idx + 1 > 388 || log_idx + 1 > 768) {
                setProperty("DriverStatus", "EFUSE Logical Decode Bounds Error!");
                delete[] phy_map;
                delete[] log_map;
                return false;
            }

            log_map[log_idx] = phy_map[phy_idx];
            log_map[log_idx + 1] = phy_map[phy_idx + 1];
            phy_idx += 2;
        }
    }

    // 5. Extract Unicast MAC address from offset 0x120
    memcpy(macAddress, &log_map[0x120], 6);
    rfeOption = log_map[0xCA];
    memcpy(physEfuseMap, phy_map, 512);
    memcpy(logicalEfuseMap, log_map, 768);

    char macDb[128];
    snprintf(macDb, sizeof(macDb), "%02x:%02x:%02x:%02x:%02x:%02x",
             macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
    RTW_DEBUG_PROPERTY("Debug_EFUSE_MAC", macDb);

    RTW_DEBUG_PROPERTY("Debug_RFE_Option", (UInt64)rfeOption);

    char logMapDump[128];
    snprintf(logMapDump, sizeof(logMapDump), "%02x %02x %02x %02x %02x %02x %02x %02x | CA=%02x",
             log_map[0xC0], log_map[0xC1], log_map[0xC2], log_map[0xC3],
             log_map[0xC4], log_map[0xC5], log_map[0xC6], log_map[0xC7], log_map[0xCA]);
    RTW_DEBUG_PROPERTY("Debug_EFUSE_log_map_C0_C7", logMapDump);

    // 6. Validate MAC Address
    bool is_valid = false;
    for (int i = 0; i < 6; i++) {
        if (macAddress[i] != 0x00 && macAddress[i] != 0xff) {
            is_valid = true;
            break;
        }
    }
    if (macAddress[0] & 0x01) { // Multicast address
        is_valid = false;
    }

    if (!is_valid) {
        macAddress[0] = 0x00;
        macAddress[1] = 0x11;
        macAddress[2] = 0x22;
        macAddress[3] = 0x33;
        macAddress[4] = 0x44;
        macAddress[5] = 0x55;
        RTW_DEBUG_PROPERTY("Debug_EFUSE_MAC_Status", "Invalid, using dummy 00:11:22:33:44:55");
    } else {
        RTW_DEBUG_PROPERTY("Debug_EFUSE_MAC_Status", "Valid");
    }

    // Clean up
    delete[] phy_map;
    delete[] log_map;
    return true;
}

void RealtekRTL8822C::clearSecurityCam(UInt8 slot) {
    if (slot >= 32) return;
    write32(0x0674, 0);
    write32(0x0670, (1U << 31) | (1U << 16) | ((UInt32)slot << 3));
}

void RealtekRTL8822C::clearAllSecurityCam(const char* reason) {
    if (!ioBase) return;
    for (UInt8 slot = 0; slot < 32; slot++) clearSecurityCam(slot);
    OSSynchronizeIO();
    lifecycleCamClearCount++;
#if RTW_DEBUG
    char db[160];
    snprintf(db, sizeof(db), "clears=%u slots=32 reason=%s command=issued",
             (unsigned int)lifecycleCamClearCount,
             reason ? reason : "unspecified");
    RTW_DEBUG_PROPERTY("Debug_CAM_Lifecycle", db);
#else
    (void)reason;
#endif
}

bool RealtekRTL8822C::writeSecurityCam(UInt8 slot, UInt8 keyIndex, UInt8 keyType,
                                    bool group, const UInt8* address, const UInt8* key) {
    if (slot >= 32 || !address || !key) return false;
    UInt32 base = (UInt32)slot << 3;
    for (int word = 7; word >= 0; word--) {
        UInt32 content = 0;
        if (word == 0) {
            content = (keyIndex & 0x03) | ((UInt32)(keyType & 0x07) << 2) |
                      (group ? (1U << 6) : 0) | (1U << 15) |
                      ((UInt32)address[0] << 16) | ((UInt32)address[1] << 24);
        } else if (word == 1) {
            content = address[2] | ((UInt32)address[3] << 8) |
                      ((UInt32)address[4] << 16) | ((UInt32)address[5] << 24);
        } else if (word >= 2 && word <= 5) {
            UInt32 offset = (UInt32)(word - 2) * 4;
            content = key[offset] | ((UInt32)key[offset + 1] << 8) |
                      ((UInt32)key[offset + 2] << 16) | ((UInt32)key[offset + 3] << 24);
        }
        write32(0x0674, content);
        write32(0x0670, (1U << 31) | (1U << 16) | base + (UInt32)word);
    }
    return true;
}

void RealtekRTL8822C::initSecurityEngine() {
    // Linux rtw_sec_enable_sec_engine(): enable the MAC security block and
    // default-key lookup. No protected data is admitted until controlled-port
    // authorization, and the CAM starts empty.
    write16(0x0100, read16(0x0100) | (1U << 9));
    UInt16 config = read16(0x0680);
    config |= (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3) |
              (1U << 6) | (1U << 7);
    write16(0x0680, config);
    clearAllSecurityCam("security-engine-init");
    bool cryptoOk = RtwWpaCrypto::selfTest();
    wpaCryptoReady = cryptoOk;
#if RTW_DEBUG
    char db[128];
    snprintf(db, sizeof(db), "engine=1 cr=%04x config=%04x cam=empty slots=32 ciphers=ccmp crypto=%s",
             read16(0x0100), read16(0x0680), cryptoOk ? "selftest-ok" : "selftest-failed");
    RTW_DEBUG_PROPERTY("Debug_Security_Engine", db);
#endif
    if (!cryptoOk) setProperty("DriverStatus", "WPA crypto self-test failed");
}

bool RealtekRTL8822C::parseAndSelectRsn() {
    assocRsnIeLen = 0;
    memset(assocRsnIe, 0, sizeof(assocRsnIe));
    if (targetSecurityMode != SECURITY_WPA2 || targetRsnIeLen < 20 ||
        targetRsnIe[0] != 48 || targetRsnIe[1] + 2 != targetRsnIeLen) {
        RTW_DEBUG_PROPERTY("Debug_Security_State", "unsupported: WPA2 RSN IE required");
        return false;
    }

    const UInt8* body = targetRsnIe + 2;
    UInt32 left = targetRsnIeLen - 2;
    if (left < 2 || body[0] != 1 || body[1] != 0) return false;
    body += 2; left -= 2;
    if (left < 4) return false;
    UInt8 groupCipher[4];
    memcpy(groupCipher, body, 4);
    body += 4; left -= 4;

    if (left < 2) return false;
    UInt16 pairwiseCount = (UInt16)(body[0] | ((UInt16)body[1] << 8));
    body += 2; left -= 2;
    if (pairwiseCount == 0 || left < (UInt32)pairwiseCount * 4) return false;
    bool pairwiseCcmp = false;
    for (UInt16 i = 0; i < pairwiseCount; i++) {
        const UInt8* suite = body + i * 4;
        if (suite[0] == 0x00 && suite[1] == 0x0f && suite[2] == 0xac && suite[3] == 4)
            pairwiseCcmp = true;
    }
    body += pairwiseCount * 4; left -= pairwiseCount * 4;

    if (left < 2) return false;
    UInt16 akmCount = (UInt16)(body[0] | ((UInt16)body[1] << 8));
    body += 2; left -= 2;
    if (akmCount == 0 || left < (UInt32)akmCount * 4) return false;
    bool psk = false;
    for (UInt16 i = 0; i < akmCount; i++) {
        const UInt8* suite = body + i * 4;
        if (suite[0] == 0x00 && suite[1] == 0x0f && suite[2] == 0xac && suite[3] == 2)
            psk = true;
    }
    body += akmCount * 4; left -= akmCount * 4;
    UInt16 capabilities = left >= 2 ? (UInt16)(body[0] | ((UInt16)body[1] << 8)) : 0;
    bool managementProtectionRequired = (capabilities & (1U << 6)) != 0;
    bool groupCcmp = groupCipher[0] == 0x00 && groupCipher[1] == 0x0f &&
                     groupCipher[2] == 0xac && groupCipher[3] == 4;
    if (!pairwiseCcmp || !groupCcmp || !psk || managementProtectionRequired) {
        char unsupported[192];
        snprintf(unsupported, sizeof(unsupported),
                 "unsupported: pairwise_ccmp=%d group_ccmp=%d psk=%d pmf_required=%d",
                 pairwiseCcmp ? 1 : 0, groupCcmp ? 1 : 0, psk ? 1 : 0,
                 managementProtectionRequired ? 1 : 0);
        RTW_DEBUG_PROPERTY("Debug_Security_State", unsupported);
        return false;
    }

    // One exact negotiated suite: RSN v1, CCMP group, CCMP pairwise,
    // PSK AKM, no PMF. This is the IE used in Association and Message 2.
    const UInt8 selected[] = {
        48, 20, 1, 0,
        0x00,0x0f,0xac,0x04,
        1,0, 0x00,0x0f,0xac,0x04,
        1,0, 0x00,0x0f,0xac,0x02,
        0,0
    };
    memcpy(assocRsnIe, selected, sizeof(selected));
    assocRsnIeLen = sizeof(selected);
    return true;
}

bool RealtekRTL8822C::prepareWpaPmk(const char* password) {
    if (!password) return false;
    size_t length = strlen(password);
    RtwWpaCrypto::secureZero(wpaPmk, sizeof(wpaPmk));
    if (length == 64) {
        for (size_t i = 0; i < 32; i++) {
            int high = rtwHexValue(password[i * 2]);
            int low = rtwHexValue(password[i * 2 + 1]);
            if (high < 0 || low < 0) {
                RtwWpaCrypto::secureZero(wpaPmk, sizeof(wpaPmk));
                return false;
            }
            wpaPmk[i] = (UInt8)((high << 4) | low);
        }
        wpaPmkValid = true;
        return true;
    }
    if (length < 8 || length > 63) return false;
    wpaPmkValid = RtwWpaCrypto::derivePmk(password,
        reinterpret_cast<const UInt8*>(targetSsid), strlen(targetSsid), wpaPmk);
    return wpaPmkValid;
}

void RealtekRTL8822C::resetWpaState(bool clearPmk) {
    wpaState = WPA_STATE_DISABLED;
    RtwWpaCrypto::secureZero(wpaPtk, sizeof(wpaPtk));
    RtwWpaCrypto::secureZero(wpaInstalledPtk, sizeof(wpaInstalledPtk));
    RtwWpaCrypto::secureZero(wpaAnonce, sizeof(wpaAnonce));
    RtwWpaCrypto::secureZero(wpaSnonce, sizeof(wpaSnonce));
    RtwWpaCrypto::secureZero(wpaGtk, sizeof(wpaGtk));
    if (clearPmk) {
        RtwWpaCrypto::secureZero(wpaPmk, sizeof(wpaPmk));
        wpaPmkValid = false;
    }
    wpaGtkKeyId = 0;
    wpaEapolVersion = 2;
    wpaPtkInstalled = false;
    wpaNewPtkPending = false;
    wpaGtkInstalled = false;
    wpaM1Replay = 0;
    wpaLastReplay = 0;
    wpaReplayValid = false;
    wpaTxPn = 0;
    memset(wpaPairwiseRxPn, 0, sizeof(wpaPairwiseRxPn));
    memset(wpaGroupRxPn, 0, sizeof(wpaGroupRxPn));
    RtwWpaCrypto::secureZero(wpaLastTx, sizeof(wpaLastTx));
    wpaLastTxLen = 0;
    wpaLastTxProtected = false;
    wpaRetries = 0;
    wpaStateStartTime = 0;
}

void RealtekRTL8822C::publishWpaState(const char* event) {
#if RTW_DEBUG
    const char* state = "disabled";
    if (wpaState == WPA_STATE_WAIT_M1) state = "wait-m1";
    else if (wpaState == WPA_STATE_WAIT_M3) state = "wait-m3";
    else if (wpaState == WPA_STATE_COMPLETED) state = "completed";
    char security[320];
    snprintf(security, sizeof(security),
             "mode=%s state=%s event=%s port=%s pmk=%d ptk=%d gtk=%d gtk_id=%u replay=%llu txpn=%llu m1=%u m3=%u group1=%u mic_fail=%u replay_drop=%u unwrap_fail=%u",
             targetSecurityMode == SECURITY_WPA2 ? "wpa2-psk-ccmp" :
             (targetSecurityMode == SECURITY_OPEN ? "open" : "unsupported"),
             state, event ? event : "none", portAuthorized ? "authorized" : "eapol-only",
             wpaPmkValid ? 1 : 0, wpaPtkInstalled ? 1 : 0,
             wpaGtkInstalled ? 1 : 0, wpaGtkKeyId,
             (unsigned long long)wpaLastReplay, (unsigned long long)wpaTxPn,
             (unsigned int)wpaRxM1, (unsigned int)wpaRxM3,
             (unsigned int)wpaRxGroupM1, (unsigned int)wpaMicFailures,
             (unsigned int)wpaReplayDrops, (unsigned int)wpaDecryptFailures);
    RTW_DEBUG_PROPERTY("Debug_Security_State", security);
#else
    (void)event;
#endif
}

bool RealtekRTL8822C::sendEapolKeyFrame(const UInt8* eapol, UInt32 len,
                                     bool protect, bool remember) {
    if (!eapol || len < 99 || len > sizeof(wpaLastTx)) return false;
    UInt8 ethernet[14 + 384];
    memcpy(ethernet, currentBssid, 6);
    memcpy(ethernet + 6, macAddress, 6);
    ethernet[12] = 0x88;
    ethernet[13] = 0x8e;
    memcpy(ethernet + 14, eapol, len);
    if (enqueueEthernetData(ethernet, len + 14, protect, false, false) !=
        DATA_ENQUEUE_SUCCESS) return false;
    if (remember) {
        memcpy(wpaLastTx, eapol, len);
        wpaLastTxLen = len;
        wpaLastTxProtected = protect;
        wpaRetries = 0;
        clock_get_uptime(&wpaStateStartTime);
    }
    return true;
}

bool RealtekRTL8822C::sendWpaMessage2(UInt64 replay) {
    if (!wpaPmkValid || assocRsnIeLen == 0) return false;
    UInt32 len = 99 + assocRsnIeLen;
    UInt8 eapol[384];
    memset(eapol, 0, sizeof(eapol));
    eapol[0] = wpaEapolVersion;
    eapol[1] = 3;
    rtwWriteBe16(eapol + 2, (UInt16)(len - 4));
    eapol[4] = 2;
    rtwWriteBe16(eapol + 5, 2U | (1U << 3) | (1U << 8));
    rtwWriteBe64(eapol + 9, replay);
    memcpy(eapol + 17, wpaSnonce, sizeof(wpaSnonce));
    rtwWriteBe16(eapol + 97, assocRsnIeLen);
    memcpy(eapol + 99, assocRsnIe, assocRsnIeLen);
    UInt8 mic[20];
    RtwWpaCrypto::hmacSha1(wpaPtk, 16, eapol, len, mic);
    memcpy(eapol + 81, mic, 16);
    RtwWpaCrypto::secureZero(mic, sizeof(mic));
    bool sent = sendEapolKeyFrame(eapol, len, false, true);
    RtwWpaCrypto::secureZero(eapol, sizeof(eapol));
    if (sent) traceEvent("wpa:m2-tx");
    return sent;
}

bool RealtekRTL8822C::sendWpaMessage4(UInt64 replay) {
    UInt8 eapol[99];
    memset(eapol, 0, sizeof(eapol));
    eapol[0] = wpaEapolVersion;
    eapol[1] = 3;
    rtwWriteBe16(eapol + 2, 95);
    eapol[4] = 2;
    rtwWriteBe16(eapol + 5, 2U | (1U << 3) | (1U << 8) | (1U << 9));
    rtwWriteBe64(eapol + 9, replay);
    UInt8 mic[20];
    RtwWpaCrypto::hmacSha1(wpaPtk, 16, eapol, sizeof(eapol), mic);
    memcpy(eapol + 81, mic, 16);
    RtwWpaCrypto::secureZero(mic, sizeof(mic));
    bool sent = sendEapolKeyFrame(eapol, sizeof(eapol), false, true);
    RtwWpaCrypto::secureZero(eapol, sizeof(eapol));
    if (sent) traceEvent("wpa:m4-tx");
    return sent;
}

bool RealtekRTL8822C::sendWpaGroupMessage2(UInt64 replay) {
    UInt8 eapol[99];
    memset(eapol, 0, sizeof(eapol));
    eapol[0] = wpaEapolVersion;
    eapol[1] = 3;
    rtwWriteBe16(eapol + 2, 95);
    eapol[4] = 2;
    rtwWriteBe16(eapol + 5, 2U | (1U << 8) | (1U << 9));
    rtwWriteBe64(eapol + 9, replay);
    UInt8 mic[20];
    RtwWpaCrypto::hmacSha1(wpaInstalledPtk, 16, eapol, sizeof(eapol), mic);
    memcpy(eapol + 81, mic, 16);
    RtwWpaCrypto::secureZero(mic, sizeof(mic));
    bool sent = sendEapolKeyFrame(eapol, sizeof(eapol), true, true);
    RtwWpaCrypto::secureZero(eapol, sizeof(eapol));
    if (sent) traceEvent("wpa:group-m2-tx");
    return sent;
}

bool RealtekRTL8822C::installPairwiseKey() {
    if (wpaPtkInstalled && !wpaNewPtkPending) return true;
    // Linux default-key CAM mode reserves slots 0-3 for GTK and starts
    // pairwise keys at slot 4. RTW_CAM_AES is type 4.
    if (!writeSecurityCam(4, 0, 4, false, currentBssid, wpaPtk + 32)) return false;
    memcpy(wpaInstalledPtk, wpaPtk, sizeof(wpaInstalledPtk));
    wpaPtkInstalled = true;
    wpaNewPtkPending = false;
    wpaTxPn = 0;
    memset(wpaPairwiseRxPn, 0, sizeof(wpaPairwiseRxPn));
    return true;
}

bool RealtekRTL8822C::installGroupKey(UInt8 keyId, const UInt8* key, UInt64 rsc) {
    if (!key || keyId > 3) return false;
    if (wpaGtkInstalled && wpaGtkKeyId == keyId &&
        RtwWpaCrypto::constantTimeEqual(wpaGtk, key, sizeof(wpaGtk))) return true;
    UInt8 broadcast[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
    if (!writeSecurityCam(keyId, keyId, 4, true, broadcast, key)) return false;
    memcpy(wpaGtk, key, sizeof(wpaGtk));
    wpaGtkKeyId = keyId;
    wpaGtkInstalled = true;
    for (int tid = 0; tid < 16; tid++) wpaGroupRxPn[keyId][tid] = rsc;
    return true;
}

static bool rtwWpaCheckMic(const UInt8* eapol, UInt32 len, const UInt8* kck) {
    if (!eapol || len < 99 || !kck) return false;
    UInt8* copy = (UInt8*)IOMalloc(len);
    if (!copy) return false;
    memcpy(copy, eapol, len);
    UInt8 received[16], digest[20];
    memcpy(received, copy + 81, sizeof(received));
    memset(copy + 81, 0, 16);
    RtwWpaCrypto::hmacSha1(kck, 16, copy, len, digest);
    bool valid = RtwWpaCrypto::constantTimeEqual(received, digest, sizeof(received));
    RtwWpaCrypto::secureZero(received, sizeof(received));
    RtwWpaCrypto::secureZero(digest, sizeof(digest));
    RtwWpaCrypto::secureZero(copy, len);
    IOFree(copy, len);
    return valid;
}

bool RealtekRTL8822C::handleWpaMessage1(const UInt8* eapol, UInt32 len,
                                     UInt16 keyInfo, UInt64 replay) {
    (void)len;
    if (!wpaPmkValid || (keyInfo & (1U << 8)) || !(keyInfo & (1U << 7))) return false;
    if (wpaReplayValid && replay <= wpaLastReplay) {
        wpaReplayDrops++;
        publishWpaState("m1-replay-drop");
        return false;
    }
    if (wpaState == WPA_STATE_WAIT_M3 && replay == wpaM1Replay &&
        memcmp(wpaAnonce, eapol + 17, 32) == 0 && wpaLastTxLen != 0) {
        bool sent = sendEapolKeyFrame(wpaLastTx, wpaLastTxLen, false, false);
        if (sent) traceEvent("wpa:m2-retransmit-on-m1");
        return sent;
    }
    memcpy(wpaAnonce, eapol + 17, sizeof(wpaAnonce));
    if (random_buf(wpaSnonce, sizeof(wpaSnonce)) != 0) {
        setProperty("DriverStatus", "WPA SNonce generation failed");
        return false;
    }
    RtwWpaCrypto::derivePtk(wpaPmk, currentBssid, macAddress,
                            wpaAnonce, wpaSnonce, wpaPtk);
    wpaNewPtkPending = true;
    wpaM1Replay = replay;
    wpaState = WPA_STATE_WAIT_M3;
    wpaRxM1++;
    publishWpaState("m1-validated");
    return sendWpaMessage2(replay);
}

bool RealtekRTL8822C::handleWpaMessage3(const UInt8* eapol, UInt32 len,
                                     UInt16 keyInfo, UInt64 replay) {
    if (wpaState == WPA_STATE_COMPLETED && wpaReplayValid && replay == wpaLastReplay &&
        memcmp(wpaAnonce, eapol + 17, 32) == 0) {
        if (!rtwWpaCheckMic(eapol, len, wpaInstalledPtk)) return false;
        traceEvent("wpa:m3-idempotent-repeat");
        return sendWpaMessage4(replay);
    }
    if (wpaState != WPA_STATE_WAIT_M3 || replay < wpaM1Replay ||
        (wpaReplayValid && replay <= wpaLastReplay)) {
        wpaReplayDrops++;
        publishWpaState("m3-replay-drop");
        return false;
    }
    if (!(keyInfo & (1U << 6)) || !(keyInfo & (1U << 7)) ||
        !(keyInfo & (1U << 8)) || !(keyInfo & (1U << 9)) ||
        memcmp(wpaAnonce, eapol + 17, 32) != 0 ||
        rtwReadBe16(eapol + 7) != 16) return false;
    if (!rtwWpaCheckMic(eapol, len, wpaPtk)) {
        wpaMicFailures++;
        publishWpaState("m3-mic-failed");
        return false;
    }

    UInt16 keyDataLen = rtwReadBe16(eapol + 97);
    if (99U + keyDataLen > len || keyDataLen > 256) return false;
    UInt8 plain[256];
    size_t plainLen = keyDataLen;
    if (keyInfo & (1U << 12)) {
        if (keyDataLen < 24) return false;
        plainLen = sizeof(plain);
        if (!RtwWpaCrypto::aesKeyUnwrap(wpaPtk + 16, eapol + 99, keyDataLen,
                                        plain, &plainLen)) {
            wpaDecryptFailures++;
            publishWpaState("m3-key-unwrap-failed");
            return false;
        }
    } else if (keyDataLen != 0) {
        memcpy(plain, eapol + 99, keyDataLen);
    }
    const UInt8* gtk = nullptr;
    UInt8 gtkId = 0;
    bool matchingRsn = false;
    size_t offset = 0;
    while (offset + 2 <= plainLen) {
        UInt8 id = plain[offset], elementLen = plain[offset + 1];
        if (offset + 2 + elementLen > plainLen) break;
        const UInt8* element = plain + offset;
        if (id == 48 && elementLen + 2 == targetRsnIeLen &&
            memcmp(element, targetRsnIe, targetRsnIeLen) == 0) matchingRsn = true;
        if (id == 221 && elementLen == 22 && element[2] == 0x00 &&
            element[3] == 0x0f && element[4] == 0xac && element[5] == 0x01) {
            gtkId = element[6] & 3;
            gtk = element + 8;
        }
        offset += 2 + elementLen;
    }
    if (!matchingRsn || (gtk && !(keyInfo & (1U << 12)))) {
        RtwWpaCrypto::secureZero(plain, sizeof(plain));
        publishWpaState(!matchingRsn ? "m3-rsn-mismatch" : "m3-gtk-not-encrypted");
        return false;
    }
    UInt8 gtkCopy[16];
    memset(gtkCopy, 0, sizeof(gtkCopy));
    if (gtk) memcpy(gtkCopy, gtk, sizeof(gtkCopy));
    UInt64 groupRsc = rtwReadLe48(eapol + 65);
    RtwWpaCrypto::secureZero(plain, sizeof(plain));

    // Queue M4 without protection first. Only after the descriptor is owned
    // by hardware do we make the new PTK/GTK visible in CAM.
    if (!sendWpaMessage4(replay)) {
        RtwWpaCrypto::secureZero(gtkCopy, sizeof(gtkCopy));
        return false;
    }
    if (!installPairwiseKey() || (gtk && !installGroupKey(gtkId, gtkCopy, groupRsc))) {
        RtwWpaCrypto::secureZero(gtkCopy, sizeof(gtkCopy));
        disconnectFromNetwork("wpa-cam-install-failed", true);
        setProperty("WiFiStatus", "WPAKeyInstallFailed");
        return false;
    }
    if (!gtk) {
        for (int tid = 0; tid < 16; tid++) wpaPairwiseRxPn[tid] = groupRsc;
    }
    RtwWpaCrypto::secureZero(gtkCopy, sizeof(gtkCopy));
    wpaLastReplay = replay;
    wpaReplayValid = true;
    wpaState = WPA_STATE_COMPLETED;
    wpaRxM3++;
    portAuthorized = true;
    setProperty("WiFiStatus", "Connected");
    setProperty("DriverStatus", "WPA2-PSK/CCMP handshake complete; PTK and GTK installed");
    setProperty("ConnectedSSID", targetSsid);
    char engine[192];
    snprintf(engine, sizeof(engine),
             "engine=1 cr=%04x config=%04x cam=programmed pairwise=4 group=%u ciphers=ccmp crypto=%s",
             read16(0x0100), read16(0x0680), (unsigned int)wpaGtkKeyId,
             wpaCryptoReady ? "selftest-ok" : "selftest-failed");
    RTW_DEBUG_PROPERTY("Debug_Security_Engine", engine);
    activateNetworkDataPath("wpa-four-way-complete");
    publishWpaState("four-way-complete");
    traceEvent("wpa:four-way-complete");
    if (htNegotiated) sendAddBaRequest();
    return true;
}

bool RealtekRTL8822C::handleWpaGroupMessage1(const UInt8* eapol, UInt32 len,
                                          UInt16 keyInfo, UInt64 replay) {
    if (wpaState != WPA_STATE_COMPLETED || !wpaPtkInstalled ||
        replay <= wpaLastReplay || !(keyInfo & (1U << 7)) ||
        !(keyInfo & (1U << 8)) || !(keyInfo & (1U << 9)) ||
        !(keyInfo & (1U << 12))) {
        wpaReplayDrops++;
        return false;
    }
    if (!rtwWpaCheckMic(eapol, len, wpaInstalledPtk)) {
        wpaMicFailures++;
        publishWpaState("group-m1-mic-failed");
        return false;
    }
    UInt16 wrappedLen = rtwReadBe16(eapol + 97);
    if (wrappedLen < 24 || 99U + wrappedLen > len || wrappedLen > 256) return false;
    UInt8 plain[256];
    size_t plainLen = sizeof(plain);
    if (!RtwWpaCrypto::aesKeyUnwrap(wpaInstalledPtk + 16, eapol + 99, wrappedLen,
                                    plain, &plainLen)) {
        wpaDecryptFailures++;
        return false;
    }
    const UInt8* gtk = nullptr;
    UInt8 gtkId = 0;
    for (size_t offset = 0; offset + 2 <= plainLen;) {
        UInt8 elementLen = plain[offset + 1];
        if (offset + 2 + elementLen > plainLen) break;
        const UInt8* element = plain + offset;
        if (element[0] == 221 && elementLen == 22 && element[2] == 0x00 &&
            element[3] == 0x0f && element[4] == 0xac && element[5] == 0x01) {
            gtkId = element[6] & 3;
            gtk = element + 8;
            break;
        }
        offset += 2 + elementLen;
    }
    if (!gtk) {
        RtwWpaCrypto::secureZero(plain, sizeof(plain));
        return false;
    }
    UInt8 gtkCopy[16];
    memcpy(gtkCopy, gtk, sizeof(gtkCopy));
    RtwWpaCrypto::secureZero(plain, sizeof(plain));
    if (!installGroupKey(gtkId, gtkCopy, rtwReadLe48(eapol + 65))) {
        RtwWpaCrypto::secureZero(gtkCopy, sizeof(gtkCopy));
        return false;
    }
    RtwWpaCrypto::secureZero(gtkCopy, sizeof(gtkCopy));
    wpaLastReplay = replay;
    wpaReplayValid = true;
    wpaRxGroupM1++;
    bool sent = sendWpaGroupMessage2(replay);
    publishWpaState(sent ? "group-rekey-complete" : "group-m2-queue-failed");
    return sent;
}

bool RealtekRTL8822C::handleEapolKey(const UInt8* eapol, UInt32 len) {
    if (targetSecurityMode != SECURITY_WPA2 || !eapol || len < 99 ||
        eapol[1] != 3 || eapol[4] != 2) return false;
    UInt32 total = 4U + rtwReadBe16(eapol + 2);
    if (total < 99 || total > len || 99U + rtwReadBe16(eapol + 97) > total) return false;
    UInt16 keyInfo = rtwReadBe16(eapol + 5);
    if ((keyInfo & 7) != 2 || (keyInfo & ((1U << 10) | (1U << 11) | (1U << 13)))) return false;
    wpaEapolVersion = eapol[0] == 1 ? 1 : 2;
    UInt64 replay = rtwReadBe64(eapol + 9);
    bool pairwise = (keyInfo & (1U << 3)) != 0;
    bool mic = (keyInfo & (1U << 8)) != 0;
    bool ack = (keyInfo & (1U << 7)) != 0;
    if (pairwise && ack && !mic)
        return handleWpaMessage1(eapol, total, keyInfo, replay);
    if (pairwise && ack && mic)
        return handleWpaMessage3(eapol, total, keyInfo, replay);
    if (!pairwise && ack && mic)
        return handleWpaGroupMessage1(eapol, total, keyInfo, replay);
    return false;
}

bool RealtekRTL8822C::initMac() {
    // 1. TXDMA queue mapping
    UInt16 pq_map = 0;
    pq_map |= (3 & 0x03) << 14; // HIQ (HIGH)
    pq_map |= (0 & 0x03) << 12; // MGQ (EXTRA)
    pq_map |= (1 & 0x03) << 10; // BKQ (LOW)
    pq_map |= (1 & 0x03) << 8;  // BEQ (LOW)
    pq_map |= (2 & 0x03) << 6;  // VIQ (NORMAL)
    pq_map |= (2 & 0x03) << 4;  // VOQ (NORMAL)
    write16(0x010C, pq_map); // REG_TXDMA_PQ_MAP

    write8(0x0100, 0x00); // REG_CR = 0
    write8(0x0100, 0xFF); // REG_CR = MAC_TRX_ENABLE (0xFF)

    write32(0x1330, 0x80000000); // REG_H2CQ_CSR = BIT_H2CQ_FULL

    // 2. Priority Queue configuration
    write16(0x0230, 64); // REG_FIFOPAGE_INFO_1 (hq)
    write16(0x0234, 64); // REG_FIFOPAGE_INFO_2 (lq)
    write16(0x0238, 64); // REG_FIFOPAGE_INFO_3 (nq)
    write16(0x023C, 64); // REG_FIFOPAGE_INFO_4 (exq)
    write16(0x0240, 1681); // REG_FIFOPAGE_INFO_5 (pubq_num)

    write32(0x022C, read32(0x022C) | 0x80000000); // REG_RQPN_CTRL_2 |= BIT_LD_RQPN

    UInt16 rsvd_boundary = 1938;
    write16(0x0204, rsvd_boundary); // REG_FIFOPAGE_CTRL_2

    write8(0x0422, read8(0x0422) | 0x10); // REG_FWHW_TXQ_CTRL + 2

    write16(0x0424, rsvd_boundary); // REG_BCNQ_BDNY_V1
    write16(0x0206, rsvd_boundary); // REG_FIFOPAGE_CTRL_2 + 2
    write16(0x0456, rsvd_boundary); // REG_BCNQ1_BDNY_V1

    write32(0x011C, 24319); // REG_RXFF_BNDY

    // 3. LLT initialization
    write8(0x0208, read8(0x0208) | 0x01); // REG_AUTO_LLT_V1 |= BIT_AUTO_INIT_LLT_V1 (BIT 0)
    int llt_timeout = 1000;
    while (llt_timeout > 0) {
        if (!(read8(0x0208) & 0x01)) {
            break;
        }
        IODelay(10);
        llt_timeout--;
    }
    if (llt_timeout <= 0) {
        setProperty("DriverStatus", "LLT Auto Init Timeout!");
        return false;
    }
    write8(0x0103, 0); // Clear REG_CR + 3

    // 4. H2C Queue initialization
    UInt32 h2cq_addr = 1986 * 128;
    UInt32 h2cq_size = 1024;
    write32(0x0244, (read32(0x0244) & 0xFFFC0000) | h2cq_addr); // REG_H2C_HEAD
    write32(0x024C, (read32(0x024C) & 0xFFFC0000) | h2cq_addr); // REG_H2C_READ_ADDR
    write32(0x0248, (read32(0x0248) & 0xFFFC0000) | (h2cq_addr + h2cq_size)); // REG_H2C_TAIL
    write8(0x0254, (read8(0x0254) & 0xFC) | 0x01); // REG_H2C_INFO
    write8(0x0254, (read8(0x0254) & 0xFB) | 0x04); // REG_H2C_INFO
    write8(0x020D, (read8(0x020D) & 0x7F) | 0x80); // REG_TXDMA_OFFSET_CHK + 1

    UInt32 wp = read32(0x10D4) & 0x3FFFF;
    UInt32 rp = read32(0x10D0) & 0x3FFFF;
    UInt32 h2cq_free = wp >= rp ? h2cq_size - (wp - rp) : rp - wp;
    char h2cDb[128];
    snprintf(h2cDb, sizeof(h2cDb), "free=%u size=%u wp=0x%05x rp=0x%05x", h2cq_free, h2cq_size, wp, rp);
    RTW_DEBUG_PROPERTY("Debug_H2C_Queue_Status", h2cDb);

    if (h2cq_size != h2cq_free) {
        setProperty("DriverStatus", "H2C Queue mismatch!");
        return false;
    }

    // 5. rtw8822c specific MAC init registers
    // txq control
    UInt8 fwhw_txq_ctrl = read8(0x0420);
    fwhw_txq_ctrl |= (1 << 7);
    fwhw_txq_ctrl &= ~((1 << 1) | (1 << 2));
    write8(0x0420, fwhw_txq_ctrl);
    write8(0x0421, 0x1F); // WLAN_TXQ_RPT_EN

    // sifs control
    write16(0x0428, 0x100A); // REG_SPEC_SIFS = WLAN_SIFS_DUR_TUNE
    write32(0x0514, 0x100A0E0A); // REG_SIFS = WLAN_SIFS_CFG
    write16(0x063C, 0x0A0A); // REG_RESP_SIFS_CCK
    write16(0x063E, 0x0E0E); // REG_RESP_SIFS_OFDM

    // rate fallback control
    write32(0x0430, 0x01000000); // REG_DARFRC
    write32(0x0434, 0x08070504); // REG_DARFRCH
    write32(0x043C, 0x08070504); // REG_RARFRCH
    write32(0x0444, 0xFE01F010); // REG_ARFR0
    write32(0x0448, 0x40000000); // REG_ARFRH0
    write32(0x044C, 0x003FF010); // REG_ARFR1_V1
    write32(0x0450, 0x40000000); // REG_ARFRH1_V1
    write32(0x049C, 0x0600F010); // REG_ARFR4
    write32(0x04A0, 0x400003E0); // REG_ARFRH4
    write32(0x04A4, 0x0600F015); // REG_ARFR5
    write32(0x04A8, 0x000000E0); // REG_ARFRH5

    // protocol configuration
    write8(0x0455, 0x70); // REG_AMPDU_MAX_TIME_V1
    write8(0x045E, read8(0x045E) | 0x04); // REG_TX_HANG_CTRL |= BIT_EN_EOF_V1 (bit 2 = 0x04)
    write16(0x04E5, 0x09E0); // REG_PRECNT_CTRL
    write32(0x04C8, 0x3F3F08FF); // REG_PROT_MODE_CTRL
    write16(0x04CE, 0x0801); // REG_BAR_MODE_CTRL + 2
    write8(0x1448, 0x06); // FAST_EDCA_VOVI_SETTING VO
    write8(0x144A, 0x06); // FAST_EDCA_VOVI_SETTING VI
    write8(0x144C, 0x06); // FAST_EDCA_BEBK_SETTING BE
    write8(0x144E, 0x06); // FAST_EDCA_BEBK_SETTING BK

    // close BA parser
    write8(0x0426, read8(0x0426) & ~0x20); // REG_LIFETIME_EN &= ~BIT_BA_PARSER_EN (BIT 5 = 0x20)
    write32(0x0440, read32(0x0440) & ~0x00600000U); // REG_RRSR &= ~BITS_RRSR_RSC

    // EDCA configuration
    write32(0x0500, 0x002FA226); // REG_EDCA_VO_PARAM
    write32(0x0504, 0x005EA328); // REG_EDCA_VI_PARAM
    write32(0x0508, 0x005EA42B); // REG_EDCA_BE_PARAM
    write32(0x050C, 0x0000A44F); // REG_EDCA_BK_PARAM
    write8(0x0512, 0x1C); // REG_PIFS
    write8(0x0521, read8(0x0521) & ~0x10); // REG_TX_PTCL_CTRL + 1 &= ~(BIT_SIFS_BK_EN >> 8) (0x1000 >> 8 = 0x10)
    write8(0x0525, read8(0x0525) | 0x07); // REG_RD_CTRL + 1

    // MAC clock configuration
    write32(0x0024, read32(0x0024) & ~0x00300000U); // REG_AFE_CTRL1 &= ~BIT_MAC_CLK_SEL (bits 20-21 = 0x00300000)
    write8(0x055C, 80); // REG_USTIME_TSF
    write8(0x0638, 80); // REG_USTIME_EDCA

    write8(0x0577, read8(0x0577) | 0x0B); // REG_MISC_CTRL |= BIT_EN_FREE_CNT (0x08) | BIT_DIS_SECOND_CCA (0x03) = 0x0B
    write8(0x05B4, read8(0x05B4) & ~0x70); // REG_TIMER0_SRC_SEL &= ~BIT_TSFT_SEL_TIMER0 (bits 4-6 = 0x70)
    write16(0x0522, 0x0000); // REG_TXPAUSE
    write8(0x051B, 0x09); // REG_SLOT
    write32(0x0544, 0x001B0005); // REG_RD_NAV_NXT
    write16(0x055E, 0x3030); // REG_RXTSF_OFFSET_CCK

    // Set beacon control
    write8(0x0550, read8(0x0550) | 0x08); // REG_BCN_CTRL |= BIT_EN_BCN_FUNCTION (bit 3 = 0x08)

    // Set send beacon related registers
    write32(0x0540, 0x6404); // REG_TBTT_PROHIBIT
    write8(0x0558, 0x04); // REG_DRVERLYINT
    write8(0x0551, 0x10); // REG_BCN_CTRL_CLINT0
    write8(0x0559, 0x02); // REG_BCNDMATIM
    write8(0x055D, 0xFF); // REG_BCN_MAX_ERR

    // WMAC configuration
    write32(0x0620, 0xFFFFFFFF); // REG_MAR low
    write32(0x0624, 0xFFFFFFFF); // REG_MAR high
    write8(0x06DE, 0x84); // REG_BBPSF_CTRL + 2
    write8(0x0640, 0x21); // REG_ACKTO
    write8(0x0639, 0x6A); // REG_ACKTO_CCK
    write16(0x0642, 0x40); // REG_EIFS
    write8(0x0652, 0xC8); // REG_NAV_CTRL + 2
    write8(0x066E, 0x05); // REG_WMAC_TRXPTCL_CTL_H + 2
    write16(0x06A0, 0xFFFF); // REG_RXFLTMAP0
    write16(0x06A2, 0xFFFF); // REG_RXFLTMAP1
    write16(0x06A4, 0xFFFF); // REG_RXFLTMAP2
    write32(0x0608, 0xE400220E); // REG_RCR
    write8(0x060C, 24); // REG_RX_PKT_LIMIT
    write8(0x0606, 0x30); // REG_TCR + 2
    write8(0x0605, 0x30); // REG_TCR + 1
    write32(0x1664, read32(0x1664) | 0x0200); // REG_GENERAL_OPTION |= BIT_DUMMY_FCS_READY_MASK_EN (0x0200)
    write32(0x07D8, 0xb0810041); // REG_WMAC_OPTION_FUNCTION + 8
    write8(0x07D4, 0x98); // REG_WMAC_OPTION_FUNCTION_1

    // init low power
    UInt16 rxpsf_ctrl_2 = read16(0x1612) & 0xF00F;
    rxpsf_ctrl_2 |= 0x0550;
    write16(0x1612, rxpsf_ctrl_2); // REG_RXPSF_CTRL + 2
    write16(0x1610, 0x3F87); // REG_RXPSF_CTRL
    write32(0x1614, 0xFFFFFFFF); // REG_RXPSF_TYPE_CTRL
    write8(0x0462, read8(0x0462) | 0x40); // REG_SND_PTCL_CTRL |= BIT_DIS_CHK_VHTSIGB_CRC (bit 6 = 0x40)

    // Interrupt migration configuration
    write32(0x0304, 0x33330000); // REG_INT_MIG

    // Write real MAC address to REG_MACID_CFG (0x0610)
    for (UInt32 i = 0; i < 6; i++) {
        write8(0x0610U + i, macAddress[i]);
    }

    // 6. rtw_drv_info_cfg
    write8(0x060F, 4); // REG_RX_DRVINFO_SZ
    UInt8 trxff_bndy_1 = read8(0x0115);
    trxff_bndy_1 &= 0xF0;
    trxff_bndy_1 |= 0x0F;
    write8(0x0115, trxff_bndy_1);
    write32(0x0608, read32(0x0608) | 0x10000000); // REG_RCR |= BIT_APP_PHYSTS
    write32(0x07D4, read32(0x07D4) & ~((1U << 8) | (1U << 9))); // REG_WMAC_OPTION_FUNCTION + 4

    initSecurityEngine();

    // 7. PCIe interface cfg
    write32(0x03FC, read32(0x03FC) | 0x04000000);

    return true;
}

void RealtekRTL8822C::handleInterrupt(OSObject* owner, IOInterruptEventSource* sender, int count) {
    (void)owner;
    (void)count;
    bool timerPoll = sender == nullptr;
    if (timerPoll) {
        if (!rxPollActive) return;
        debugRxTimerPolls++;
    } else {
        isrCallsCount++;
    }
    if (!ioBase) return;

    // === Step 1: Mask ALL hardware interrupts immediately (NAPI-style) ===
    // For level-triggered legacy pin interrupts, the IRQ line stays asserted
    // as long as any HISR bit is set. We must mask HIMR first to prevent
    // an interrupt storm (10M+ calls/sec observed without this fix).
    write32(0x00B0, 0); // HIMR0 = 0
    write32(0x00B8, 0); // HIMR1 = 0
    write32(0x10B8, 0); // HIMR3 = 0
    OSSynchronizeIO();

    // === Step 2: Read and ACK interrupt status ===
    UInt32 h0 = read32(0x00B4); // HISR0 (RTK_PCI_HISR0 = 0x0B4)
    UInt32 h1 = read32(0x00BC); // HISR1 (RTK_PCI_HISR1 = 0x0BC)
    UInt32 h3 = read32(0x10BC); // HISR3 (RTK_PCI_HISR3 = 0x10BC)
    if (h0) write32(0x00B4, h0); // Write 1 to clear pending bits
    if (h1) write32(0x00BC, h1);
    if (h3) write32(0x10BC, h3);
    OSSynchronizeIO();

    if (!timerPoll) {
        if (h0 & 0x00000001U) debugRxRokInterrupts++;
        if (h0 & 0x00000002U) debugRxRduInterrupts++;
    }

    if (h0 == 0 && h1 == 0 && h3 == 0 && !timerPoll) {
        // Spurious interrupt - re-enable and bail
        restoreHardwareInterruptMasks();
        return;
    }

    if (h0 & 0x00000010) { // IMR_BEDOK (BIT 4)
        debugBedokCount++;
        serviceBeqCompletion(false);
    }
    if (h0 & 0x00000040) { // IMR_MGNTDOK (BIT 6)
        debugMgntdokCount++;
        UInt32 bd_idx = read32(0x03B0);
        mgmtRp = (bd_idx >> 16) & 0x0FFF;
    }
    if (h0 & 0x00004000) { // IMR_BCNDMAINT_E (BIT 14)
        debugBcndmaintCount++;
    }
    if (h1 & 0x00000800) { // IMR_TXERR (BIT 11)
        debugTxerrCount++;
        if (read32(0x0210) & (1U << 13)) {
            captureBeqPayloadOverflow("txerr-payload-overflow");
        }
    }
    if (h1 & 0x00000200) { // IMR_TXFOVW (BIT 9)
        debugTxfovwCount++;
    }

    // Throttled debug logging
    if (!timerPoll && (isrCallsCount & 0x3FF) == 1) {
        char intDb[128];
        snprintf(intDb, sizeof(intDb), "ISR H0=0x%08x H1=0x%08x calls=%d", h0, h1, isrCallsCount);
        RTW_DEBUG_PROPERTY("Debug_Last_ISR", intDb);
    }

    // === Step 3: Process RX if ROK, RDU or RXFOVW ===
    if ((timerPoll || (h0 & 0x03) || (h1 & 0x0100)) && rxBufferVirtAddr) {
#if RTW_DEBUG
        uint64_t rxPollStarted = 0;
        clock_get_uptime(&rxPollStarted);
#endif
        debugRxPollCalls++;
        // Linux services TX completions before handing receive work to NAPI.
        // Our timer poll shares one serialized IOWorkLoop with the hardware
        // interrupt source, so a latched BEDOK cannot run until this callback
        // returns. Poll the authoritative hardware RP here and wake a stalled
        // output queue before consuming another RX slice.
        serviceBeqCompletion(true);
        UInt32 tmp = read32(0x03B4);
        UInt32 cur_wp = (tmp >> 16) & 0x0FFF;

        UInt32 rx_count = (cur_wp >= rxRp) ? (cur_wp - rxRp) : (512 - rxRp + cur_wp);

        UInt32 packets_processed = 0;
        UInt32 rxBudget = rxPollTimer ? kRtwRxPollBudget : 512U;
#if RTW_DEBUG
        uint64_t rxSyncStarted = 0;
        uint64_t rxSyncFinished = 0;
        uint64_t rxSyncElapsedNs = 0;
        clock_get_uptime(&rxSyncStarted);
#endif
        rxBufferDmaCmd->synchronize(kIODirectionIn);
#if RTW_DEBUG
        clock_get_uptime(&rxSyncFinished);
        absolutetime_to_nanoseconds(rxSyncFinished - rxSyncStarted,
                                    &rxSyncElapsedNs);
        debugRxSyncCalls++;
        debugRxSyncTotalNs += rxSyncElapsedNs;
        if (rxSyncElapsedNs > debugRxSyncMaxNs)
            debugRxSyncMaxNs = rxSyncElapsedNs;
#endif
        while (rx_count > 0 && packets_processed < rxBudget) {
            UInt8* rx_buf = rxBufferVirtAddr + rxRp * 12288;

            UInt32 rw0 = OSReadLittleInt32(rx_buf, 0);
            UInt32 rw2 = OSReadLittleInt32(rx_buf, 8);
#if RTW_DEBUG
            UInt32 rw3 = OSReadLittleInt32(rx_buf, 12);
            UInt32 rw4 = OSReadLittleInt32(rx_buf, 16);
#endif

            UInt32 pkt_len     = rw0 & 0x3FFF;
            UInt32 crc_err     = (rw0 >> 14) & 1;
            UInt32 icv_err     = (rw0 >> 15) & 1;
            UInt32 drv_info_sz = ((rw0 >> 16) & 0x0F) * 8;
            UInt8 enc_type     = (UInt8)((rw0 >> 20) & 0x07);
            UInt32 shift       = (rw0 >> 24) & 0x03;
            bool decrypted     = ((rw0 & (1U << 27)) == 0) && enc_type != 0;
            UInt32 is_c2h      = (rw2 >> 28) & 1;
            UInt32 pkt_offset  = 24 + drv_info_sz + shift;

            // Validate packet bounds within the 12288-byte DMA slot
            if (pkt_offset + pkt_len > 12288) {
                rxRp = (rxRp + 1) % 512;
                rx_count--;
                packets_processed++;
                continue;
            }

            // Debug dump of received packet metadata and raw descriptor (throttled)
            if (!timerPoll && (isrCallsCount & 0x3FF) == 1) {
                char pkt_info[256];
                UInt8* rx_payload = rx_buf + pkt_offset;
                snprintf(pkt_info, sizeof(pkt_info),
                         "idx=%d len=%u crc=%u icv=%u enc=%u dec=%u drv_sz=%u shift=%u c2h=%u w0=0x%08x w2=0x%08x fc=0x%02x 0x%02x 0x%02x 0x%02x",
                         (int)rxRp, (unsigned int)pkt_len, (unsigned int)crc_err,
                         (unsigned int)icv_err, enc_type, decrypted ? 1U : 0U,
                         (unsigned int)drv_info_sz,
                         (unsigned int)shift, (unsigned int)is_c2h, (unsigned int)rw0, (unsigned int)rw2,
                         pkt_len > 0 ? rx_payload[0] : 0,
                         pkt_len > 1 ? rx_payload[1] : 0,
                         pkt_len > 2 ? rx_payload[2] : 0,
                         pkt_len > 3 ? rx_payload[3] : 0);
                RTW_DEBUG_PROPERTY("Debug_Rx_Last_Pkt", pkt_info);

                char raw_desc[256];
                snprintf(raw_desc, sizeof(raw_desc),
                         "rp=%d: %02x %02x %02x %02x | %02x %02x %02x %02x | %02x %02x %02x %02x | %02x %02x %02x %02x | %02x %02x %02x %02x",
                         (int)rxRp,
                         rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3],
                         rx_buf[4], rx_buf[5], rx_buf[6], rx_buf[7],
                         rx_buf[8], rx_buf[9], rx_buf[10], rx_buf[11],
                         rx_buf[12], rx_buf[13], rx_buf[14], rx_buf[15],
                         rx_buf[16], rx_buf[17], rx_buf[18], rx_buf[19]);
                RTW_DEBUG_PROPERTY("Debug_Rx_Raw_Desc", raw_desc);
            }

            if (pkt_len > 0 && !crc_err && !icv_err && !is_c2h) {
                UInt8* rx_frame   = rx_buf + pkt_offset;
                UInt32 payload_len = pkt_len;
                if (payload_len > 4) payload_len -= 4; // Strip FCS

                UInt8 fc0    = rx_frame[0];
                UInt8 type   = (fc0 >> 2) & 0x03;
                UInt8 subtype = (fc0 >> 4) & 0x0F;

                if (type == 0) { // Management
                    if (subtype == 8 || subtype == 5) {
                        // Beacon or Probe Response
                        int signalDbm = -127;
                        if ((rw0 & (1U << 26)) && drv_info_sz != 0)
                            signalDbm = extractRxSignalDbm(rx_buf + 24 + shift, drv_info_sz);
                        parseBeaconOrProbeResponse(rx_frame, payload_len, signalDbm);
                    }
                    handleMgmtFrame(rx_frame, payload_len);
                } else if (type == 1) { // Control (BAR is relevant to RX reorder)
                    handleControlFrame(rx_frame, payload_len);
                } else if (type == 2) { // Data
                    if ((rw0 & BIT(26)) && drv_info_sz != 0 && payload_len >= 16 &&
                        connState == CONN_STATE_CONNECTED &&
                        memcmp(rx_frame + 10, currentBssid, 6) == 0) {
                        int signalDbm = extractRxSignalDbm(rx_buf + 24 + shift,
                                                          drv_info_sz);
                        if (signalDbm >= -120 && signalDbm <= 0) {
                            connectedSignalDbm = connectedSignalDbm >= -120 ?
                                (connectedSignalDbm * 7 + signalDbm) / 8 :
                                signalDbm;
                        }
                    }
#if RTW_DEBUG
                    UInt8 rxRate = (UInt8)(rw3 & 0x7fU);
                    UInt8 rxDescBw = (UInt8)((rw4 >> 4) & 0x03U);
                    UInt8 rxBw = rxDescBw;
                    UInt8 rxSc = 0xff;
                    UInt8 ppduCount = (UInt8)((rw2 >> 29) & 0x03U);
                    bool phyBwKnown = false;

                    // Linux first reads W4.BW, then RTL8822C PHY-status page
                    // 1 replaces it with RXSC-derived PPDU width. RXSC zero
                    // means the current channel width; 1..8 are 20 MHz,
                    // 9..12 are 40 MHz and 13..15 are 80 MHz. Descriptor BW
                    // alone therefore under-reported every wide-channel PPDU.
                    if ((rw0 & BIT(26)) && drv_info_sz >= 8) {
                        const UInt8* phyStatus = rx_buf + 24 + shift;
                        UInt8 page = phyStatus[0] & 0x0f;
                        if (page == 0) {
                            rxBw = 0;
                            phyBwKnown = true;
                        } else if (page == 1) {
                            UInt8 rxScByte = phyStatus[5];
                            rxSc = (rxRate > 0x03 && rxRate < 0x0c) ?
                                (rxScByte & 0x0f) : (rxScByte >> 4);
                            if (rxSc == 0) rxBw = targetBandwidth <= 2 ? targetBandwidth : 0;
                            else if (rxSc <= 8) rxBw = 0;
                            else if (rxSc <= 12) rxBw = 1;
                            else rxBw = 2;
                            phyBwKnown = true;
                        }
                    }
                    if (phyBwKnown) {
                        debugRxPpduBw[ppduCount] = rxBw;
                        debugRxPpduBwValid[ppduCount] = true;
                    } else if (debugRxPpduBwValid[ppduCount]) {
                        // Only the first MPDU of an aggregate commonly carries
                        // PHY status. PPDU_CNT identifies the remaining MPDUs,
                        // so inherit that PPDU's already decoded RXSC width.
                        rxBw = debugRxPpduBw[ppduCount];
                    }
                    debugRxLastRate = rxRate;
                    debugRxLastBw = rxBw;
                    if (rxRate < 84U) debugRxRateCounts[rxRate]++;
                    if (rxBw < 3U) debugRxBwCounts[rxBw]++;
                    if (rxDescBw < 3U) debugRxDescBwCounts[rxDescBw]++;
                    if (rxSc < 16U) debugRxScCounts[rxSc]++;
#endif
                    handleRxDataFrame(rx_frame, payload_len, enc_type, decrypted);
                }
            } else if (is_c2h && !crc_err && pkt_len >= 2) {
                const UInt8* c2h = rx_buf + pkt_offset;
                UInt8 c2h_id = c2h[0];
                UInt8 c2h_seq = c2h[1];

                debugC2hRxCount++;

                char raw_c2h[37];
                UInt32 raw_len = pkt_len < 12 ? pkt_len : 12;
                UInt32 raw_pos = 0;
                raw_c2h[0] = '\0';
                for (UInt32 i = 0; i < raw_len; i++) {
                    int written = snprintf(raw_c2h + raw_pos, sizeof(raw_c2h) - raw_pos,
                                           "%02x%s", c2h[i], i + 1 == raw_len ? "" : " ");
                    if (written > 0) raw_pos += (UInt32)written;
                }

                char c2h_db[384];
                bool isCcxReport = false;
                UInt8 ccxReportSn = 0;
                UInt8 ccxReportStatus = 0xff;
                if (c2h_id == 0x03 && pkt_len >= 9) { // Legacy C2H_CCX_TX_RPT
                    const UInt8* payload = c2h + 2;
                    UInt8 rpt_sn = payload[6] & 0xFC;
                    UInt8 rpt_st = payload[0] & 0xC0;
                    isCcxReport = true;
                    ccxReportSn = rpt_sn;
                    ccxReportStatus = rpt_st;
                    snprintf(c2h_db, sizeof(c2h_db), "CCX_TX_RPT_V0: id=0x%02x seq=0x%02x len=%u rpt_sn=0x%02x rpt_st=0x%02x status=%s raw=%s",
                             c2h_id, c2h_seq, (unsigned int)pkt_len, rpt_sn, rpt_st,
                             rpt_st == 0 ? "ACK" : "NO-ACK", raw_c2h);
                } else if (c2h_id == 0xff && pkt_len >= 12 && c2h[2] == 0x0f) {
                    // HALMAC extended C2H CCX report: payload[0] is the subtype.
                    const UInt8* payload = c2h + 2;
                    UInt8 rpt_sn = payload[8] & 0xFC;
                    UInt8 rpt_st = payload[9] & 0xC0;
                    isCcxReport = true;
                    ccxReportSn = rpt_sn;
                    ccxReportStatus = rpt_st;
                    snprintf(c2h_db, sizeof(c2h_db), "CCX_TX_RPT_V1: id=0x%02x seq=0x%02x len=%u subtype=0x%02x rpt_sn=0x%02x rpt_st=0x%02x status=%s raw=%s",
                             c2h_id, c2h_seq, (unsigned int)pkt_len, payload[0], rpt_sn, rpt_st,
                             rpt_st == 0 ? "ACK" : "NO-ACK", raw_c2h);
                } else {
                    UInt8 subtype = pkt_len >= 3 ? c2h[2] : 0;
                    snprintf(c2h_db, sizeof(c2h_db), "C2H generic: id=0x%02x seq=0x%02x len=%u subtype=0x%02x raw=%s",
                             c2h_id, c2h_seq, (unsigned int)pkt_len, subtype, raw_c2h);
                }
                if (isCcxReport ||
                    (((UInt32)debugC2hRxCount & kRtwHotDiagnosticMask) == 1U))
                    RTW_DEBUG_PROPERTY("Debug_C2H_Last_Rpt", c2h_db);
                // Successful CCX and ordinary firmware C2H notifications are
                // high-frequency telemetry. Keep them in the registry/report,
                // but avoid flooding the kernel console (notably during
                // shutdown). A failed CCX remains actionable and is logged.
                if (isCcxReport && ccxReportStatus != 0) {
                    RTW_ERROR_LOG("RealtekRTL8822C: %s\n", c2h_db);
                }

                bool dataReportMatched = false;
                UInt16 dataReportEtherType = 0;
                UInt16 dataReportSeq = 0;
                if (isCcxReport && beqLock) {
                    IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
                    if (debugDataReportPending && ccxReportSn == debugDataReportSn) {
                        dataReportMatched = true;
                        dataReportEtherType = debugDataReportEtherType;
                        dataReportSeq = debugDataReportSeq;
                        debugDataReportPending = false;
                    }
                    IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
                }
                if (dataReportMatched) {
                    char dataReport[256];
                    snprintf(dataReport, sizeof(dataReport),
                             "sn=%02x eth=%04x seq=%u status=%02x ack=%d beq=%08x txdma=%08x",
                             ccxReportSn, dataReportEtherType, dataReportSeq,
                             ccxReportStatus, ccxReportStatus == 0 ? 1 : 0,
                             (unsigned int)read32(0x03a8), (unsigned int)read32(0x0210));
                    RTW_DEBUG_PROPERTY("Debug_Data_CCX", dataReport);
                }

                if (isCcxReport && debugMgmtShadowValid && ccxReportSn == debugMgmtReportSn && mgmtPayloadDesc) {
                    UInt8* currentSlot = (UInt8*)mgmtPayloadDesc->getBytesNoCopy() + debugMgmtSlot * 2048;
                    bool descSame = memcmp(currentSlot, debugMgmtShadow, 48) == 0;
                    bool frameSame = memcmp(currentSlot + 48, debugMgmtShadow + 48, 30) == 0;
                    UInt16 descXor = 0;
                    for (UInt32 i = 0; i < 24; i++) {
                        descXor ^= OSReadLittleInt16(currentSlot, i * 2);
                    }
                    UInt32 ofdmNow = read32(0x2de0);
                    char postTx[768];
                    snprintf(postTx, sizeof(postTx),
                             "sn=%02x status=%02x ack=%d idx=%08x rp=%u mgntdok=%d txdma=%08x pause=%02x txfifo=%08x edcca=%u ofdm=%08x delta=%u wire=%08x/%08x cca=%08x rf0=%05x/%05x rfmode=%x/%x same=%d/%d xor=%04x",
                             ccxReportSn, ccxReportStatus, ccxReportStatus == 0 ? 1 : 0,
                             (unsigned int)read32(0x03b0), (unsigned int)mgmtRp,
                             debugMgntdokCount, (unsigned int)read32(0x0210), read8(0x0522),
                             (unsigned int)read32(0x1e70),
                             (unsigned int)read32Mask(0x2d38, 1U << 24),
                             (unsigned int)ofdmNow, (unsigned int)(ofdmNow - debugMgmtOfdmTxBaseline),
                             (unsigned int)read32(0x180c), (unsigned int)read32(0x410c),
                             (unsigned int)read32(0x1d58),
                             (unsigned int)readRfMask(0, 0x00, 0xfffff),
                             (unsigned int)readRfMask(1, 0x00, 0xfffff),
                             (unsigned int)readRfMask(0, 0x00, 0xf0000),
                             (unsigned int)readRfMask(1, 0x00, 0xf0000),
                             descSame ? 1 : 0, frameSame ? 1 : 0, descXor);
                    RTW_DEBUG_PROPERTY("Debug_MGMT_PostCCX", postTx);
                }
            }

            rxRp = (rxRp + 1) % 512;
            rx_count--;
            packets_processed++;
        }

        write16(0x03B4, (UInt16)rxRp);
        OSSynchronizeIO();

        // Descriptors may have completed while this bounded RX slice ran.
        // Reconcile again so TCP ACK/output traffic is not left waiting for a
        // BEDOK callback behind a continuously rescheduled RX timer.
        serviceBeqCompletion(true);

        debugRxPollPackets += packets_processed;
        if (packets_processed > debugRxPollMaxBatch)
            debugRxPollMaxBatch = packets_processed;

        UInt32 nextIdx = read32(0x03B4);
        UInt32 nextWp = (nextIdx >> 16) & 0x0fff;
        bool moreRx = nextWp != rxRp;
        if (!moreRx && rxPollTimer) {
            // Clear only latent RX causes, then close the producer-index race
            // before RX interrupts are unmasked again.
            UInt32 pending0 = read32(0x00b4) & 0x00000003U;
            UInt32 pending1 = read32(0x00bc) & 0x00000100U;
            if (pending0) write32(0x00b4, pending0);
            if (pending1) write32(0x00bc, pending1);
            OSSynchronizeIO();
            nextIdx = read32(0x03B4);
            nextWp = (nextIdx >> 16) & 0x0fff;
            moreRx = nextWp != rxRp;
        }

        if (moreRx && rxPollTimer) {
            rxPollActive = true;
            if (packets_processed == rxBudget) debugRxPollBudgetHits++;
            rxPollTimer->setTimeoutUS(kRtwRxPollDelayUs);
        } else {
            rxPollActive = false;
        }

#if RTW_DEBUG
        uint64_t rxPollFinished = 0;
        uint64_t rxPollElapsedNs = 0;
        clock_get_uptime(&rxPollFinished);
        absolutetime_to_nanoseconds(rxPollFinished - rxPollStarted,
                                    &rxPollElapsedNs);
        debugRxPollTotalNs += rxPollElapsedNs;
        if (rxPollElapsedNs > debugRxPollMaxNs)
            debugRxPollMaxNs = rxPollElapsedNs;
#endif
    }

    // === Step 4: Re-enable interrupts ===
    restoreHardwareInterruptMasks();
}

void RealtekRTL8822C::rxPollTimerFired(OSObject* owner,
                                    IOTimerEventSource* sender) {
    (void)sender;
    // Null distinguishes deferred polling from a hardware IRQ while sharing
    // the exact same RX descriptor parser and state transitions.
    handleInterrupt(owner, nullptr, 0);
}

// PHY condition table parser
void RealtekRTL8822C::loadTable(const UInt32* data, UInt32 size, UInt8 type, UInt8 rf_path) {
    UInt32 entries = size / 2;
    bool is_matched = true;
    bool is_skipped = false;

    UInt32 pos_cond_val = 0;
    UInt32 pos_cond2_val = 0;

    for (UInt32 i = 0; i < entries; i++) {
        UInt32 w0 = data[i * 2];
        UInt32 w1 = data[i * 2 + 1];

        bool pos = (w0 >> 31) & 1;
        bool neg = (w0 >> 30) & 1;
        UInt32 branch = (w0 >> 28) & 3;

        if (pos) {
            switch (branch) {
                case 3: // BRANCH_ENDIF
                    is_matched = true;
                    is_skipped = false;
                    break;
                case 2: // BRANCH_ELSE
                    is_matched = is_skipped ? false : true;
                    break;
                case 0: // BRANCH_IF
                case 1: // BRANCH_ELIF
                default:
                    pos_cond_val = w0;
                    pos_cond2_val = w1;
                    break;
            }
        } else if (neg) {
            if (!is_skipped) {
                if (checkPositive(pos_cond_val, pos_cond2_val)) {
                    is_matched = true;
                    is_skipped = true;
                } else {
                    is_matched = false;
                    is_skipped = false;
                }
            } else {
                is_matched = false;
            }
        } else if (is_matched) {
            if (type == 0) { // MAC
                write8(w0, (UInt8)w1);
            } else if (type == 1) { // AGC
                write32(w0, w1);
            } else if (type == 2) { // BB
                if (w0 == 0xfe) IODelay(50000);
                else if (w0 == 0xfd) IODelay(5000);
                else if (w0 == 0xfc) IODelay(1000);
                else if (w0 == 0xfb) IODelay(50);
                else if (w0 == 0xfa) IODelay(5);
                else if (w0 == 0xf9) IODelay(1);
                else write32(w0, w1);
            } else if (type == 3) { // RF
                if (w0 == 0xffe) IODelay(50000);
                else if (w0 == 0xfe) IODelay(100);
                else {
                    writeRfMask(rf_path, w0, 0xFFFFF, w1);
                    IODelay(1);
                }
            }
        }
    }
}

bool RealtekRTL8822C::checkPositive(UInt32 cond_w0, UInt32 cond2_w1) {
    (void)cond2_w1;
    UInt32 cond_rfe = cond_w0 & 0xFF;
    UInt32 cond_intf = (cond_w0 >> 8) & 0x0F;
    UInt32 cond_pkg = (cond_w0 >> 12) & 0x0F;
    UInt32 cond_cut = (cond_w0 >> 24) & 0x0F;

    if (cond_cut && cond_cut != cutVersion)
        return false;

    if (cond_pkg && cond_pkg != 15)
        return false;

    if (cond_intf && cond_intf != 1) // INTF_PCIE is 1
        return false;

    if (cond_rfe != rfeOption)
        return false;

    return true;
}

void RealtekRTL8822C::writeRfMask(UInt8 path, UInt32 addr, UInt32 mask, UInt32 data) {
    if (addr == 0x00) {
        UInt32 sipi_addr = (path == 0) ? 0x1808 : 0x4108;
        UInt32 data_and_addr = (((addr & 0xFF) << 20) | (data & 0xFFFFF)) & 0x0FFFFFFF;
        write32(sipi_addr, data_and_addr);
        IODelay(13);
    } else {
        UInt32 base_addr = (path == 0) ? 0x3C00 : 0x4C00;
        UInt32 direct_addr = base_addr + ((addr & 0xFF) << 2);
        write32Mask(direct_addr, mask & 0xFFFFF, data);
    }
}

UInt32 RealtekRTL8822C::readRfMask(UInt8 path, UInt32 addr, UInt32 mask) {
    UInt32 base_addr = (path == 0) ? 0x3C00 : 0x4C00;
    UInt32 direct_addr = base_addr + ((addr & 0xFF) << 2);
    return read32Mask(direct_addr, mask & 0xFFFFF);
}

void RealtekRTL8822C::headerFileInit(bool pre) {
    write32(0x180c, read32(0x180c) | 0x03 | 0x10000000); // REG_3WIRE
    write32(0x410c, read32(0x410c) | 0x03 | 0x10000000); // REG_3WIRE2
    if (pre) {
        write32(0x1c3c, read32(0x1c3c) & ~0x03U); // REG_ENCCK
    } else {
        write32(0x1c3c, read32(0x1c3c) | 0x03); // REG_ENCCK
    }
}

void RealtekRTL8822C::configTrxMode(UInt8 tx_path, UInt8 rx_path, bool is_tx2_path) {
    if ((tx_path | rx_path) & 0x01) {
        write32Mask(0x1800, 0xfffff, 0x33312); // REG_ORITXCODE
    } else {
        write32Mask(0x1800, 0xfffff, 0x11111);
    }

    if ((tx_path | rx_path) & 0x02) {
        write32Mask(0x4100, 0xfffff, 0x33312); // REG_ORITXCODE2
    } else {
        write32Mask(0x4100, 0xfffff, 0x11111);
    }

    if (rx_path == 0x01 || rx_path == 0x02) {
        write32Mask(0x1a2c, 0x00060000, 0); // REG_CCANRX
        write32Mask(0x1a2c, 0x00600000, 0);
    } else if (rx_path == 0x03) {
        write32Mask(0x1a2c, 0x00600000, 1);
        write32Mask(0x1a2c, 0x00060000, 1);
    }

    if (rx_path == 0x01) {
        write32Mask(0x1a04, 0x0f000000, 0); // REG_RXCCKSEL
    } else if (rx_path == 0x02) {
        write32Mask(0x1a04, 0x0f000000, 5);
    } else if (rx_path == 0x03) {
        write32Mask(0x1a04, 0x0f000000, 1);
    }

    if (rx_path == 0x01 || rx_path == 0x02) {
        write32Mask(0x1d30, 0x300, 0); // REG_RXFNCTL
        write32Mask(0x1d30, 0x600000, 0);
        write32Mask(0xc44, 1 << 17, 0); // REG_AGCSWSH
        write32Mask(0xc54, 1 << 20, 0); // REG_ANTWTPD
        write32Mask(0xc38, 1 << 24, 0); // REG_MRCM
    } else if (rx_path == 0x03) {
        write32Mask(0x1d30, 0x300, 1);
        write32Mask(0x1d30, 0x600000, 1);
        write32Mask(0xc44, 1 << 17, 1);
        write32Mask(0xc54, 1 << 20, 1);
        write32Mask(0xc38, 1 << 24, 1);
    }
    write32Mask(0x824, 0x0f000000, rx_path);
    write32Mask(0x824, 0x000f0000, rx_path);

    if (tx_path == 0x01) {
        write32Mask(0x1a04, 0xf0000000, 8);
    } else if (tx_path == 0x02) {
        write32Mask(0x1a04, 0xf0000000, 4);
    } else {
        if (is_tx2_path) {
            write32Mask(0x1a04, 0xf0000000, 12);
        } else {
            write32Mask(0x1a04, 0xf0000000, 8); // Match Linux 1SS CCK path A
        }
    }

    if (tx_path == 0x01) {
        write32Mask(0x820, 0xff, 0x11); // REG_ANTMAP0
        write32Mask(0x1e2c, 0xff, 0);  // REG_TXLGMAP
    } else if (tx_path == 0x02) {
        write32Mask(0x820, 0xff, 0x12);
        write32Mask(0x1e2c, 0xff, 0);
    } else {
        write32Mask(0x820, 0xff, 0x31);
        write32Mask(0x1e2c, 0xffff, 0x0400);
    }

    write16(0x0002, read16(0x0002) | 0x01); // REG_SYS_FUNC_EN |= BIT_FEN_BB_RSTB
    write16(0x0002, read16(0x0002) & ~0x01);
    write16(0x0002, read16(0x0002) | 0x01);

    UInt32 igi = read32(0x1d70) & 0x7f; // REG_RXIGI
    write32Mask(0x1d70, 0x7f, igi - 2);
    write32Mask(0x1d70, 0x7f00, igi - 2);
    write32Mask(0x1d70, 0x7f, igi);
    write32Mask(0x1d70, 0x7f00, igi);
}

bool RealtekRTL8822C::sendH2CCommand(const UInt8* h2c) {
    UInt32 msg = *(const UInt32*)h2c;
    UInt32 msg_ext = *(const UInt32*)(h2c + 4);

    UInt8 box = lastBoxNum;
    UInt32 box_reg, box_ex_reg;
    switch (box) {
        case 0: box_reg = 0x01D0; box_ex_reg = 0x01F0; break;
        case 1: box_reg = 0x01D4; box_ex_reg = 0x01F4; break;
        case 2: box_reg = 0x01D8; box_ex_reg = 0x01F8; break;
        case 3: box_reg = 0x01DC; box_ex_reg = 0x01FC; break;
        default: return false;
    }

    int timeout = 1000;
    bool is_free = false;
    UInt8 box_state = 0;
    while (timeout > 0) {
        box_state = read8(0x01CC);
        if (!((box_state >> box) & 0x1)) {
            is_free = true;
            break;
        }
        IODelay(10);
        timeout--;
    }
    if (!is_free) {
        char db[160];
        snprintf(db, sizeof(db),
                 "busy-timeout box=%u hmetfr=%02x seq=%u h2cq_wp=%u msg=%08x",
                 (unsigned int)box, (unsigned int)box_state,
                 (unsigned int)h2cSeq, (unsigned int)h2cqWp,
                 (unsigned int)msg);
        RTW_DEBUG_PROPERTY("Debug_H2C_Mailbox_Error", db);
        RTW_DEBUG_PROPERTY("Debug_H2C_Mailbox_State", db);
        return false;
    }

    write32(box_ex_reg, msg_ext);
    write32(box_reg, msg);

    lastBoxNum++;
    if (lastBoxNum >= 4) {
        lastBoxNum = 0;
    }

    char db[160];
    snprintf(db, sizeof(db),
             "sent box=%u hmetfr_before=%02x next=%u seq=%u h2cq_wp=%u msg=%08x",
             (unsigned int)box, (unsigned int)box_state,
             (unsigned int)lastBoxNum, (unsigned int)h2cSeq,
             (unsigned int)h2cqWp, (unsigned int)msg);
    RTW_DEBUG_PROPERTY("Debug_H2C_Mailbox_State", db);

    return true;
}

bool RealtekRTL8822C::sendH2CPacket(const UInt8* packet, UInt32 size) {
    if (!h2cqDesc || !h2cPayloadDesc) {
        RTW_DEBUG_PROPERTY("Debug_H2C_Step", "FAIL: h2c descriptors not allocated");
        return false;
    }

    static constexpr UInt32 H2C_TX_DESC_SIZE = 48;

    UInt32 slot = h2cqWp % 128;
    UInt32 payload_offset = slot * 128; // Use separate 128-byte segments for each slot to prevent overwrite races

    // 1. Construct 48-byte TX packet descriptor (aligned with Linux)
    UInt32 tx_desc[12];
    memset(tx_desc, 0, sizeof(tx_desc));
    // w0: tx_pkt_size = size (no offset, no ls/fs flags for H2C in Linux)
    tx_desc[0] = size & 0xFFFFU;
    // w1: qsel = TX_DESC_QSEL_H2C (19)
    tx_desc[1] = 19U << 8;

    // Copy descriptor and data to the physical payload buffer at the slot offset
    UInt8* payloadVirt = (UInt8*)h2cPayloadDesc->getBytesNoCopy();
    memcpy(payloadVirt + payload_offset, tx_desc, H2C_TX_DESC_SIZE);
    memcpy(payloadVirt + payload_offset + H2C_TX_DESC_SIZE, packet, size);

    // Sync DMA for payload (Only output direction!)
    h2cPayloadDmaCmd->synchronize(kIODirectionOut);

    // 2. Format a 16-byte PCIe Buffer Descriptor in H2CQ Ring (2x 8-byte descriptors)
    struct __attribute__((packed)) {
        UInt16 buf_size;
        UInt16 psb_len;
        UInt32 dma;
    } bd[2];

    UInt16 psb_len = (UInt16)(((H2C_TX_DESC_SIZE + size) - 1) / 128 + 1);

    bd[0].buf_size = H2C_TX_DESC_SIZE;
    bd[0].psb_len  = psb_len;
    bd[0].dma      = (UInt32)((h2cPayloadPhysAddr + payload_offset) & 0xFFFFFFFF);

    bd[1].buf_size = (UInt16)size;
    bd[1].psb_len  = 0;
    bd[1].dma      = (UInt32)((h2cPayloadPhysAddr + payload_offset + H2C_TX_DESC_SIZE) & 0xFFFFFFFF);

    UInt8* ringVirt = (UInt8*)h2cqDesc->getBytesNoCopy();
    UInt32 entry_offset = slot * 16;
    memcpy(ringVirt + entry_offset, bd, 16);

    // Sync DMA for H2CQ Ring (Only output direction!)
    h2cqDmaCmd->synchronize(kIODirectionOut);

    // 3. Write incremented WP to H2CQ index register (0x132C)
    h2cqWp++;
    UInt32 host_idx = h2cqWp & 0xFFF;
    write16(0x132C, (UInt16)host_idx);

    // Wait up to 10ms for hardware to fetch the packet (hw_rp catches up to host_wp)
    bool dma_ok = false;
    UInt32 last_idx_reg = 0;
    for (int i = 0; i < 1000; i++) {
        last_idx_reg = read32(0x132C);
        UInt32 hw_idx = (last_idx_reg >> 16) & 0xFFF;
        if (hw_idx == host_idx) {
            dma_ok = true;
            break;
        }
        IODelay(10);
    }

    char slotStr[80];
    snprintf(slotStr, sizeof(slotStr), "slot=%u entry_off=0x%x payload_off=0x%x", slot, entry_offset, payload_offset);
    RTW_DEBUG_PROPERTY("Debug_H2C_LastSlot", slotStr);

    char bdStr[160];
    snprintf(bdStr, sizeof(bdStr), "bd0_size=%u bd0_psb=%u bd0_dma=0x%x bd1_size=%u bd1_dma=0x%x",
             bd[0].buf_size, bd[0].psb_len, bd[0].dma, bd[1].buf_size, bd[1].dma);
    RTW_DEBUG_PROPERTY("Debug_H2C_LastBD", bdStr);

    char txdStr[120];
    snprintf(txdStr, sizeof(txdStr), "w0=0x%08x w1=0x%08x w2=0x%08x w3=0x%08x", tx_desc[0], tx_desc[1], tx_desc[2], tx_desc[3]);
    RTW_DEBUG_PROPERTY("Debug_H2C_LastTXD", txdStr);

    char pktDumpStr[80];
    snprintf(pktDumpStr, sizeof(pktDumpStr), "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
             packet[0], packet[1], packet[2], packet[3],
             packet[4], packet[5], packet[6], packet[7],
             packet[8], packet[9], packet[10], packet[11],
             packet[12], packet[13], packet[14], packet[15]);
    RTW_DEBUG_PROPERTY("Debug_H2C_LastPkt", pktDumpStr);

    char dmaDb[96];
    snprintf(dmaDb, sizeof(dmaDb), "%s H2C (host_wp=%u hw_rp=%u, reg=0x%08x)",
             dma_ok ? "OK" : "TIMEOUT", host_idx, (last_idx_reg >> 16) & 0xFFF, last_idx_reg);
    RTW_DEBUG_PROPERTY("Debug_H2C_DMA_Status", dmaDb);

    return dma_ok;
}

bool RealtekRTL8822C::runIqk() {
    // Send General Info and PhyDm Info before IQK (required by firmware calibration engine)
    if (!sendGeneralInfo()) {
        RTW_DEBUG_PROPERTY("Debug_IQK_SendFail", "sendGeneralInfo returned false");
        return false;
    }
    IODelay(50000); // Give the firmware 50ms to process GeneralInfo

    if (!sendPhyDmInfo()) {
        RTW_DEBUG_PROPERTY("Debug_IQK_SendFail", "sendPhyDmInfo returned false");
        return false;
    }
    IODelay(50000); // Give the firmware 50ms to process PhyDmInfo

    // 1. WiFi / BT RFK handshake start
    // Poll REG_PMC_DBG_CTRL1 (0xA8) BITS_PMC_BT_IQK_STS (0x600000) == 0
    UInt32 pmc_before = read32(0xA8);
    int bt_iqk_timeout = 60000; // 600ms (60000 * 10us)
    bool bt_iqk_idle = false;
    while (bt_iqk_timeout > 0) {
        if ((read32(0xA8) & 0x600000) == 0) {
            bt_iqk_idle = true;
            break;
        }
        IODelay(10);
        bt_iqk_timeout--;
    }
    UInt32 pmc_after = read32(0xA8);
    char pmcDb[128];
    snprintf(pmcDb, sizeof(pmcDb), "before=0x%08X after=0x%08X idle=%d iters=%d",
             (unsigned int)pmc_before, (unsigned int)pmc_after, bt_iqk_idle ? 1 : 0, 60000 - bt_iqk_timeout);
    RTW_DEBUG_PROPERTY("Debug_RFK_BT_IQK_Wait", pmcDb);

    UInt32 arfr4_before_start = read32(0x049C);

    UInt8 rfk_start_pkt[8];
    memset(rfk_start_pkt, 0, 8);
    rfk_start_pkt[0] = 0x6D; // H2C_CMD_WIFI_CALIBRATION (0x6d)
    rfk_start_pkt[1] = 0x01; // start = true (bit 8 = 1)

    bool start_cmd_ok = sendH2CCommand(rfk_start_pkt);

    // Poll REG_ARFR4 (0x049C) bit 0 (BIT_WL_RFK) for 1
    int rfk_timeout = 10000;
    bool rfk_ok = false;
    while (rfk_timeout > 0) {
        if (read32(0x049C) & 0x01) {
            rfk_ok = true;
            break;
        }
        IODelay(10);
        rfk_timeout--;
    }

    UInt32 arfr4_after_start = read32(0x049C);
    char handshakeStartDb[128];
    snprintf(handshakeStartDb, sizeof(handshakeStartDb), "cmd_ok=%d rfk=%d, before=0x%08X after=0x%08X, iters=%d",
             start_cmd_ok ? 1 : 0, rfk_ok ? 1 : 0, (unsigned int)arfr4_before_start,
             (unsigned int)arfr4_after_start, 10000 - rfk_timeout);
    RTW_DEBUG_PROPERTY("Debug_RFK_Handshake_Start", handshakeStartDb);

    UInt8 h2c_pkt[32];
    memset(h2c_pkt, 0, 32);

    // Match Linux rtw_h2c_pkt_set_header() + rtw_fw_do_iqk() exactly:
    // Word 0: category[6:0]=0x01, cmd_id[15:8]=0xFF, sub_id[31:16]=0x0E
    // Word 1: total_len[15:0]=9 (HDR_SIZE=8 + 1 payload byte), seq[31:16]=h2cSeq
    // Word 2: IQK_CLEAR[0]=1, IQK_SEGMENT_IQK[1]=0
    UInt32* pkt32 = (UInt32*)h2c_pkt;
    pkt32[0] = 0x01U | (0xFFU << 8) | ((UInt32)0x0EU << 16); // = 0x000EFF01
    pkt32[1] = 9U | ((UInt32)h2cSeq << 16);
    pkt32[2] = 0x1U; // IQK_SET_CLEAR = bit[0]
    h2cSeq++;

    // Log the H2C packet bytes for debug
    char pktStr[128];
    snprintf(pktStr, sizeof(pktStr), "%08x %08x %08x %08x", pkt32[0], pkt32[1], pkt32[2], pkt32[3]);
    RTW_DEBUG_PROPERTY("Debug_IQK_H2C_Pkt", pktStr);

    if (!sendH2CPacket(h2c_pkt, 32)) {
        RTW_DEBUG_PROPERTY("Debug_IQK_SendFail", "sendH2CPacket returned false");
        // Revert RFK handshake before returning
        UInt8 rfk_stop_pkt[8];
        memset(rfk_stop_pkt, 0, 8);
        rfk_stop_pkt[0] = 0x6D;
        rfk_stop_pkt[1] = 0x00;
        sendH2CCommand(rfk_stop_pkt);
        return false;
    }

    // Log post-send queue status
    UInt32 post_idx = read32(0x132C);
    UInt32 host_idx = post_idx & 0xFFF;
    UInt32 hw_idx = (post_idx >> 16) & 0xFFF;
    UInt32 post_fifo_rp = read32(0x10D0) & 0x3FFFF;
    UInt32 post_fifo_wp = read32(0x10D4) & 0x3FFFF;
    char postH2c[128];
    snprintf(postH2c, sizeof(postH2c), "host_wp=%u hw_rp=%u | SRAM FIFO wp=0x%05x rp=0x%05x",
             host_idx, hw_idx, post_fifo_wp, post_fifo_rp);
    RTW_DEBUG_PROPERTY("Debug_H2C_Post_Queue", postH2c);

    // Poll REG_RPT_CIP (0x2D9C) for IQK done = 0xAA
    int timeout = 30000;
    bool iqk_done = false;
    UInt8 iqk_chk = 0;
    while (timeout > 0) {
        iqk_chk = read8(0x2D9C);
        if (iqk_chk == 0xAA) {
            iqk_done = true;
            break;
        }
        IODelay(10);
        timeout--;
    }

    char iqkStr[64];
    snprintf(iqkStr, sizeof(iqkStr), "0x%02x (done=%d, iters=%d)", iqk_chk, iqk_done ? 1 : 0, 30000 - timeout);
    RTW_DEBUG_PROPERTY("Debug_IQK_Status", iqkStr);
    write8(0x1B10, 0x00); // REG_IQKSTAT = 0

    // 2. RFK Handshake - Stop
    UInt32 arfr4_before_stop = read32(0x049C);

    UInt8 rfk_stop_pkt[8];
    memset(rfk_stop_pkt, 0, 8);
    rfk_stop_pkt[0] = 0x6D; // H2C_CMD_WIFI_CALIBRATION (0x6d)
    rfk_stop_pkt[1] = 0x00; // start = false (bit 8 = 0)

    bool stop_cmd_ok = sendH2CCommand(rfk_stop_pkt);

    int rfk_stop_timeout = 10000;
    bool rfk_stop_ok = false;
    while (rfk_stop_timeout > 0) {
        if (!(read32(0x049C) & 0x01)) {
            rfk_stop_ok = true;
            break;
        }
        IODelay(10);
        rfk_stop_timeout--;
    }

    UInt32 arfr4_after_stop = read32(0x049C);
    char handshakeStopDb[128];
    snprintf(handshakeStopDb, sizeof(handshakeStopDb), "cmd_ok=%d rfk=%d, before=0x%08X after=0x%08X, iters=%d",
             stop_cmd_ok ? 1 : 0, rfk_stop_ok ? 1 : 0, (unsigned int)arfr4_before_stop,
             (unsigned int)arfr4_after_stop, 10000 - rfk_stop_timeout);
    RTW_DEBUG_PROPERTY("Debug_RFK_Handshake_Stop", handshakeStopDb);

    return iqk_done;
}

bool RealtekRTL8822C::dackWait(UInt32 offset, UInt32 mask, UInt32 target) {
    for (UInt32 count = 0; count < 1000; count++) {
        if (read32Mask(offset, mask) == target)
            return true;
        IODelay(10);
    }
    return false;
}

void RealtekRTL8822C::dackSample(UInt32* iv, UInt32* qv, bool* complete) {
    UInt32 sample = 0;
    UInt32 count = 0;
    UInt32 accepted = 0;
    while (accepted < 100 && count++ < 10000) {
        sample = read32Mask(0x2dbc, 0x003fffff);
        UInt32 i = (sample & 0x003ff000) >> 12;
        UInt32 q = sample & 0x3ff;
        bool iValid = !((i >= 0x200 && (0x400 - i) > 0x64) || (i < 0x200 && i > 0x64));
        bool qValid = !((q >= 0x200 && (0x400 - q) > 0x64) || (q < 0x200 && q > 0x64));
        if (iValid && qValid) {
            iv[accepted] = i;
            qv[accepted] = q;
            accepted++;
        }
    }
    *complete = accepted == 100;
    while (accepted < 100) {
        iv[accepted] = 0;
        qv[accepted] = 0;
        accepted++;
    }
}

void RealtekRTL8822C::dackSearch(UInt32* iv, UInt32* qv, UInt32* iValue, UInt32* qValue) {
    for (UInt32 retry = 0; retry <= 100; retry++) {
        UInt32 iMin = iv[0], iMax = iv[0], qMin = qv[0], qMax = qv[0];
        for (UInt32 i = 0; i < 100; i++) {
            UInt32 values[2] = { iv[i], qv[i] };
            UInt32* mins[2] = { &iMin, &qMin };
            UInt32* maxs[2] = { &iMax, &qMax };
            for (UInt32 component = 0; component < 2; component++) {
                UInt32 value = values[component];
                UInt32& min = *mins[component];
                UInt32& max = *maxs[component];
                if (value >= 0x200) {
                    if (min >= 0x200) { if (min > value) min = value; } else min = value;
                    if (max >= 0x200) { if (max < value) max = value; }
                } else {
                    if (min < 0x200) { if (min > value) min = value; }
                    if (max >= 0x200) max = value;
                    else if (max < value) max = value;
                }
            }
        }
        UInt32 iDelta = (iMax < 0x200 || iMin >= 0x200) ? iMax - iMin : iMax + (0x400 - iMin);
        UInt32 qDelta = (qMax < 0x200 || qMin >= 0x200) ? qMax - qMin : qMax + (0x400 - qMin);
        for (UInt32 i = 0; i < 99; i++) {
            for (UInt32 j = 0; j < 99 - i; j++) {
                if ((iv[j] < 0x200 && iv[j + 1] >= 0x200) ||
                    ((iv[j] < 0x200) == (iv[j + 1] < 0x200) && iv[j] > iv[j + 1])) {
                    UInt32 temp = iv[j]; iv[j] = iv[j + 1]; iv[j + 1] = temp;
                }
                if ((qv[j] < 0x200 && qv[j + 1] >= 0x200) ||
                    ((qv[j] < 0x200) == (qv[j + 1] < 0x200) && qv[j] > qv[j + 1])) {
                    UInt32 temp = qv[j]; qv[j] = qv[j + 1]; qv[j + 1] = temp;
                }
            }
        }
        if (iDelta <= 5 && qDelta <= 5)
            break;
        UInt32 sample = read32Mask(0x2dbc, 0x003fffff);
        iv[0] = (sample & 0x003ff000) >> 12;
        qv[0] = sample & 0x3ff;
        sample = read32Mask(0x2dbc, 0x003fffff);
        iv[99] = (sample & 0x003ff000) >> 12;
        qv[99] = sample & 0x3ff;
    }
    UInt32* vectors[2] = { iv, qv };
    UInt32* results[2] = { iValue, qValue };
    for (UInt32 component = 0; component < 2; component++) {
        UInt32 positive = 0, negative = 0;
        for (UInt32 i = 10; i < 90; i++) {
            if (vectors[component][i] > 0x200) negative += 0x400 - vectors[component][i];
            else positive += vectors[component][i];
        }
        UInt32 value = positive > negative ? (positive - negative) / 80 : (negative - positive) / 80;
        *results[component] = positive < negative && value != 0 ? 0x400 - value : value;
    }
}

void RealtekRTL8822C::dackRfMode(UInt32* iValue, UInt32* qValue, bool* complete) {
    UInt32 iv[100], qv[100];
    dackSample(iv, qv, complete);
    dackSearch(iv, qv, iValue, qValue);
}

bool RealtekRTL8822C::dackAdc(UInt8 path, UInt32* adcI, UInt32* adcQ, UInt32* attempts) {
    UInt32 base = path == 0 ? 0x1800 : 0x4100;
    UInt32 pathSelect = path == 0 ? 0xa0000 : 0x80000;
    bool samplesComplete = true;
    *attempts = 0;
    write32Mask(base + 0x30, 1U << 30, 0);
    if (path == 1) write32(base + 0x30, 0x30db8041);
    write32(base + 0x60, 0xf0040ff0);
    write32(base + 0x0c, 0xdff00220);
    write32(base + 0x10, 0x02dd08c4);
    write32(base + 0x0c, 0x10000260);
    writeRfMask(0, 0x0, 0xfffff, 0x10000);
    writeRfMask(1, 0x0, 0xfffff, 0x10000);
    for (UInt32 i = 0; i < 10; i++) {
        UInt32 ic = 0, qc = 0;
        *attempts = i + 1;
        write32(0x1c3c, pathSelect + 0x8003);
        write32(0x1c24, 0x00010002);
        bool complete = true;
        dackRfMode(&ic, &qc, &complete);
        samplesComplete = samplesComplete && complete;
        if (ic != 0) { ic = 0x400 - ic; *adcI = ic; }
        if (qc != 0) { qc = 0x400 - qc; *adcQ = qc; }
        write32(base + 0x68, (ic & 0x3ff) | ((qc & 0x3ff) << 10));
        dackAdck[path] = (ic & 0x3ff) | ((qc & 0x3ff) << 10);
        write32(0x1c3c, pathSelect + 0x8103);
        dackRfMode(&ic, &qc, &complete);
        samplesComplete = samplesComplete && complete;
        if (ic >= 0x200) ic = 0x400 - ic;
        if (qc >= 0x200) qc = 0x400 - qc;
        if (ic < 5 && qc < 5) break;
    }
    write32(0x1c3c, 0x00000003);
    write32(base + 0x0c, 0x10000260);
    write32(base + 0x10, 0x02d508c4);
    writeRfMask(path, 0x8f, 1U << 13, 1);
    return samplesComplete;
}

bool RealtekRTL8822C::dackStep1(UInt8 path) {
    UInt32 base = path == 0 ? 0x1800 : 0x4100;
    UInt32 read = path == 0 ? 0x2800 : 0x4500;
    write32(base + 0x68, dackAdck[path]);
    write32(base + 0x0c, 0xdff00220);
    if (path == 0) { write32(base + 0x60, 0xf0040ff0); write32(0x1c38, 0xffffffff); }
    write32(base + 0x10, 0x02d508c5); write32(0x9b4, 0xdb66db00);
    write32(base + 0xb0, 0x0a11fb88); write32(base + 0xbc, 0x0008ff81); write32(base + 0xc0, 0x0003d208);
    write32(base + 0xcc, 0x0a11fb88); write32(base + 0xd8, 0x0008ff81); write32(base + 0xdc, 0x0003d208);
    write32(base + 0xb8, 0x60000000); IODelay(2000); write32(base + 0xbc, 0x000aff8d); IODelay(2000);
    write32(base + 0xb0, 0x0a11fb89); write32(base + 0xcc, 0x0a11fb89); IODelay(1000);
    write32(base + 0xb8, 0x62000000); write32(base + 0xd4, 0x62000000); IODelay(20000);
    bool ready = dackWait(read + 0x08, 0x007fff80, 0xffff) && dackWait(read + 0x34, 0x007fff80, 0xffff);
    write32(base + 0xb8, 0x02000000); IODelay(1000); write32(base + 0xbc, 0x0008ff87);
    write32(0x9b4, 0xdb6db600); write32(base + 0x10, 0x02d508c5); write32(base + 0xbc, 0x0008ff87);
    write32(base + 0x60, 0xf0000000);
    return ready;
}

void RealtekRTL8822C::dackStep2(UInt8 path, UInt32* iOut, UInt32* qOut, bool* samplesComplete) {
    UInt32 base = path == 0 ? 0x1800 : 0x4100;
    write32Mask(base + 0xbc, 0xf0000000, 0); write32Mask(base + 0xc0, 0xf, 8);
    write32Mask(base + 0xd8, 0xf0000000, 0); write32Mask(base + 0xdc, 0xf, 8);
    write32(0x1b00, 0x00000008); write8(0x1bcc, 0x3f); write32(base + 0x0c, 0xdff00220);
    write32(base + 0x10, 0x02d508c5); write32(0x1c3c, 0x00088103);
    UInt32 ic = 0, qc = 0;
    dackRfMode(&ic, &qc, samplesComplete);
    if (ic != 0) ic = 0x400 - ic;
    if (qc != 0) qc = 0x400 - qc;
    *iOut = ic < 0x300 ? ic * 12 / 5 + 0x80 : 0x7f - (0x400 - ic) * 12 / 5;
    *qOut = qc < 0x300 ? qc * 12 / 5 + 0x80 : 0x7f - (0x400 - qc) * 12 / 5;
}

bool RealtekRTL8822C::dackStep3(UInt8 path, UInt32 adcI, UInt32 adcQ, UInt32* iIn, UInt32* qIn,
                              UInt32* iOut, UInt32* qOut, bool* samplesComplete) {
    UInt32 base = path == 0 ? 0x1800 : 0x4100;
    UInt32 read = path == 0 ? 0x2800 : 0x4500;
    UInt32 ic = *iIn, qc = *qIn;
    write32(base + 0x0c, 0xdff00220); write32(base + 0x10, 0x02d508c5); write32(0x9b4, 0xdb66db00);
    write32(base + 0xb0, 0x0a11fb88); write32(base + 0xbc, 0xc008ff81); write32(base + 0xc0, 0x0003d208);
    write32Mask(base + 0xbc, 0xf0000000, ic & 0xf); write32Mask(base + 0xc0, 0xf, (ic & 0xf0) >> 4);
    write32(base + 0xcc, 0x0a11fb88); write32(base + 0xd8, 0xe008ff81); write32(base + 0xdc, 0x0003d208);
    write32Mask(base + 0xd8, 0xf0000000, qc & 0xf); write32Mask(base + 0xdc, 0xf, (qc & 0xf0) >> 4);
    write32(base + 0xb8, 0x60000000); IODelay(2000); write32Mask(base + 0xbc, 0xe, 6); IODelay(2000);
    write32(base + 0xb0, 0x0a11fb89); write32(base + 0xcc, 0x0a11fb89); IODelay(1000);
    write32(base + 0xb8, 0x62000000); write32(base + 0xd4, 0x62000000); IODelay(20000);
    bool ready = dackWait(read + 0x24, 0x07f80000, ic) && dackWait(read + 0x50, 0x07f80000, qc);
    write32(base + 0xb8, 0x02000000); IODelay(1000); write32Mask(base + 0xbc, 0xe, 3); write32(0x9b4, 0xdb6db600);
    write32(base + 0x68, ((adcI + 0x10) & 0x3ff) | (((adcQ + 0x10) & 0x3ff) << 10));
    write32(base + 0x10, 0x02d508c5); write32(base + 0x60, 0xf0000000);
    dackRfMode(&ic, &qc, samplesComplete);
    *iOut = ic >= 0x10 ? ic - 0x10 : 0x400 - (0x10 - ic);
    *qOut = qc >= 0x10 ? qc - 0x10 : 0x400 - (0x10 - qc);
    *iIn = *iOut >= 0x200 ? 0x400 - *iOut : *iOut;
    *qIn = *qOut >= 0x200 ? 0x400 - *qOut : *qOut;
    return ready;
}

void RealtekRTL8822C::dackStep4(UInt8 path) {
    UInt32 base = path == 0 ? 0x1800 : 0x4100;
    write32(base + 0x68, 0); write32(base + 0x10, 0x02d508c4);
    write32Mask(base + 0xbc, 0x1, 0); write32Mask(base + 0x30, 1U << 30, 1);
}

void RealtekRTL8822C::dackBackupResults() {
    const UInt32 dckRegs[2][2][2][2] = {
        { { {0x18bc, 0xf0000000}, {0x18c0, 0xf} }, { {0x18d8, 0xf0000000}, {0x18dc, 0xf} } },
        { { {0x41bc, 0xf0000000}, {0x41c0, 0xf} }, { {0x41d8, 0xf0000000}, {0x41dc, 0xf} } }
    };
    UInt32 savedA = read32(0x1860), savedB = read32(0x4160), savedClock = read32(0x9b4);
    write32(0x9b4, 0xdb66db00);
    for (UInt8 path = 0; path < 2; path++) {
        UInt32 base = path == 0 ? 0x1800 : 0x4100;
        UInt32 read = path == 0 ? 0x2800 : 0x4500;
        write32Mask(base + 0x30, 1U << 30, 0); write32Mask(base + 0x60, 0xfc000000, 0x3c);
        for (UInt8 vector = 0; vector < 2; vector++) {
            UInt32 writeAddr = base + 0xb0 + (vector ? 0x1c : 0);
            UInt32 readAddr = read + 0x10 + (vector ? 0x2c : 0);
            for (UInt32 i = 0; i < 15; i++) {
                write32Mask(writeAddr, 0xf0000000, i);
                dackMsbk[path][vector][i] = (UInt16)read32Mask(readAddr, 0x07fc0000);
            }
        }
        for (UInt8 component = 0; component < 2; component++)
            for (UInt8 part = 0; part < 2; part++)
                dackDck[path][component][part] = (UInt8)read32Mask(dckRegs[path][component][part][0], dckRegs[path][component][part][1]);
        write32Mask(base + 0x30, 1U << 30, 1);
    }
    write32(0x1860, savedA); write32(0x4160, savedB); write32(0x9b4, savedClock);
}

void RealtekRTL8822C::runDackCalibration() {
    const UInt32 regs[] = { 0x180c, 0x1810, 0x410c, 0x4110, 0x1c3c, 0x1c24, 0x1d70, 0x9b4,
                            0x1a00, 0x1a14, 0x1d58, 0x1c38, 0x1e24, 0x1e28, 0x1860, 0x4160 };
    UInt32 saved[16], rf8f[2];
    for (UInt32 i = 0; i < 16; i++) saved[i] = read32(regs[i]);
    for (UInt8 path = 0; path < 2; path++) rf8f[path] = readRfMask(path, 0x8f, 0xfffff);
    write32Mask(0x1d58, 0xff8, 0x1ff); write32Mask(0x1a00, 0x3, 2); write32Mask(0x1a14, 0x300, 3);
    write32(0x1d70, 0x7e7e7e7e); write32Mask(0x180c, 0x3, 0); write32Mask(0x410c, 0x3, 0);
    write32(0x1b00, 0x00000008); write8(0x1bcc, 0x3f); write32(0x1b00, 0x0000000a); write8(0x1bcc, 0x3f);
    write32Mask(0x1e24, 1U << 31, 0); write32Mask(0x1e28, 0xf, 3);
    UInt32 adcI[2] = {}, adcQ[2] = {}, adcAttempts[2] = {}, residualI[2] = {}, residualQ[2] = {};
    bool adcSamples[2] = {}, ready[2] = {}, residualSamples[2] = {};
    UInt32 stageI[2] = {}, stageQ[2] = {};
    for (UInt8 path = 0; path < 2; path++) {
        adcSamples[path] = dackAdc(path, &adcI[path], &adcQ[path], &adcAttempts[path]);
        ready[path] = true;
        residualSamples[path] = true;
        for (UInt32 attempt = 0; attempt < 10; attempt++) {
            ready[path] = dackStep1(path) && ready[path];
            bool complete = true;
            dackStep2(path, &stageI[path], &stageQ[path], &complete);
            residualSamples[path] = residualSamples[path] && complete;
            bool stepReady = dackStep3(path, adcI[path], adcQ[path], &stageI[path], &stageQ[path],
                                       &residualI[path], &residualQ[path], &complete);
            ready[path] = ready[path] && stepReady;
            residualSamples[path] = residualSamples[path] && complete;
            if (stageI[path] < 5 && stageQ[path] < 5) break;
        }
        dackStep4(path);
    }
    write32(0x1b00, 0x00000008); write32Mask(0x4130, 1U << 30, 1); write8(0x1bcc, 0);
    write32(0x1b00, 0x0000000a); write8(0x1bcc, 0);
    for (UInt32 i = 0; i < 16; i++) write32(regs[i], saved[i]);
    for (UInt8 path = 0; path < 2; path++) writeRfMask(path, 0x8f, 0xfffff, rf8f[path]);
    dackBackupResults();
    char status[256];
    snprintf(status, sizeof(status),
             "fresh=1 A:adc=%03x/%03x n=%u res=%03x/%03x sample_fail=%d timeout=%d B:adc=%03x/%03x n=%u res=%03x/%03x sample_fail=%d timeout=%d",
             adcI[0], adcQ[0], adcAttempts[0], residualI[0], residualQ[0], (!adcSamples[0] || !residualSamples[0]) ? 1 : 0, ready[0] ? 0 : 1,
             adcI[1], adcQ[1], adcAttempts[1], residualI[1], residualQ[1], (!adcSamples[1] || !residualSamples[1]) ? 1 : 0, ready[1] ? 0 : 1);
    RTW_DEBUG_PROPERTY("Debug_DACK_Status", status);
    RTW_DEBUG_LOG("RealtekRTL8822C: DACK %s\n", status);
}

void RealtekRTL8822C::runRfX2Check() {
    UInt32 before = readRfMask(0, 0xb8, 1U << 15);
    if (before) {
        writeRfMask(0, 0xb8, 0xfffff, 0xc4440);
        writeRfMask(0, 0xba, 0xfffff, 0x6840d);
        writeRfMask(0, 0xb8, 0xfffff, 0x80440);
        IODelay(1000);
    }
    char state[64];
    snprintf(state, sizeof(state), "busy=%u after=%u", (unsigned int)before,
             (unsigned int)readRfMask(0, 0xb8, 1U << 15));
    RTW_DEBUG_PROPERTY("Debug_RF_X2_Check", state);
}

bool RealtekRTL8822C::txGapkRfkHandshake(bool start, bool* btIdle, UInt32* polls) {
    if (btIdle) *btIdle = true;
    if (polls) *polls = 0;

    if (start) {
        bool idle = false;
        for (UInt32 i = 0; i < 60000; i++) {
            if ((read32(0x00a8) & 0x00600000U) == 0) {
                idle = true;
                if (polls) *polls = i;
                break;
            }
            IODelay(10);
        }
        if (!idle) {
            if (btIdle) *btIdle = false;
            if (polls) *polls = 60000;
        }
    }

    UInt8 h2c[8] = {};
    h2c[0] = 0x6d; // H2C_CMD_WIFI_CALIBRATION, as used by the IQK RFK flow.
    h2c[1] = start ? 1 : 0;
    if (!sendH2CCommand(h2c)) return false;

    for (UInt32 i = 0; i < 10000; i++) {
        bool active = (read8(0x049c) & 0x01) != 0;
        if (active == start) {
            if (polls) *polls = i;
            return true;
        }
        IODelay(10);
    }
    if (polls) *polls = 10000;
    return false;
}

void RealtekRTL8822C::runTxGapkCalibration() {
    // Fresh-only port of rtw8822c_do_gapk()/rtw8822c_txgapk(); no cached LUT reload.
    static const UInt32 bbRegs[] = {
        0x0520, 0x1e70, 0x0820, 0x1e2c, 0x1e28, 0x1800, 0x4100,
        0x1e24, 0x1cd0, 0x1d58, 0x1864, 0x4164, 0x180c, 0x410c,
        0x186c, 0x416c, 0x1a00, 0x1c38, 0x1b00, 0x1b20, 0x1bb8,
        0x1bcc, 0x1b10, 0x1b98, 0x1b9c, 0x1bd4, 0x001c, 0x00ec
    };
    static const UInt32 rfRegs[] = { 0x00, 0x18, 0x1a, 0x55, 0x58, 0x87, 0x8f, 0x9e, 0xde, 0xdf };
    static const UInt8 bandChannel[] = { 1, 1, 36, 100, 149 };
    static const UInt8 bandValue[] = { 0, 0, 1, 3, 5 };
    static const UInt8 bandCck[] = { 1, 0, 0, 0, 0 };
    UInt32 bbSaved[sizeof(bbRegs) / sizeof(bbRegs[0])];
    UInt32 rfSaved[2][sizeof(rfRegs) / sizeof(rfRegs[0])];
    UInt32 gainTable[5][11][2] = {};
    SInt8 offsets[10][2] = {};
    UInt8 channel[2] = {};
    bool pathReportOk[2] = { true, true };
    bool pathChannelOk[2] = { true, true };
    bool btIdle = true;
    UInt32 startPolls = 0, stopPolls = 0;
    bool startOk = txGapkRfkHandshake(true, &btIdle, &startPolls);
    bool pauseOk = true;

    for (UInt32 i = 0; i < sizeof(bbRegs) / sizeof(bbRegs[0]); i++) bbSaved[i] = read32(bbRegs[i]);
    UInt8 txPauseSaved = read8(0x0522);
    for (UInt8 path = 0; path < 2; path++)
        for (UInt32 i = 0; i < sizeof(rfRegs) / sizeof(rfRegs[0]); i++)
            rfSaved[path][i] = readRfMask(path, rfRegs[i], 0xfffff);

    if (startOk) {
        // Linux save_all_tx_gain_table(): capture every band/path before calibration.
        for (UInt8 band = 0; band < 5; band++) {
            for (UInt8 path = 0; path < 2; path++) {
                UInt32 cfgch = readRfMask(path, 0x18, 0xfffff);
                write32Mask(path == 0 ? 0x180c : 0x410c, 0x3, 0);
                writeRfMask(path, 0x18, 0xff, bandChannel[band]);
                writeRfMask(path, 0x18, 0x00000300, bandValue[band]);
                writeRfMask(path, 0x1a, 1U << 1, bandCck[band]);
                writeRfMask(path, 0x1a, 1U << 16, bandCck[band]);
                for (UInt8 gain = 0, rf0 = 1; gain < 11; gain++, rf0 += 3) {
                    writeRfMask(path, 0x00, 0xff, rf0);
                    gainTable[band][gain][path] = readRfMask(path, 0x5f, 0x0fff);
                }
                writeRfMask(path, 0x18, 0xfffff, cfgch);
                write32Mask(path == 0 ? 0x180c : 0x410c, 0x3, 3);
            }
        }

        // Linux write_gain_bb_table(): install the captured baseline for each band.
        for (UInt8 band = 0; band < 5; band++) {
            UInt32 qGainSel = band == 0 ? 0 : (band == 2 ? 2 : (band == 3 ? 3 : 4));
            for (UInt8 path = 0; path < 2; path++) {
                UInt32 retained = 0;
                bool haveRetained = false;
                write32Mask(0x1b00, 0x00000006, path);
                write32Mask(0x1b98, 0x00007000, qGainSel);
                write32Mask(0x1b9c, 0x000000ff, 0x88);
                for (UInt8 gain = 0; gain < 11; gain++) {
                    UInt32 value = gainTable[band][gain][path];
                    bool saturated = ((value >> 8) & 0xf) >= 0xc && ((value >> 4) & 0xf) >= 0xe;
                    if (!saturated || !haveRetained) { retained = value; haveRetained = true; }
                    write32Mask(0x1b98, 0x00000fff, retained);
                    write32Mask(0x1b98, 0x000f0000, gain);
                    write32Mask(0x1b98, 1U << 15, 1);
                    write32Mask(0x1b98, 1U << 15, 0);
                }
            }
        }

        // Linux txgapk_tx_pause(), retaining and restoring both MAC pause controls.
        write8(0x0522, 0xff);
        write32Mask(0x1e70, 0x0000000f, 2);
        for (UInt32 i = 0; i < 2500; i++) {
            if (((readRfMask(0, 0x00, 0x000f0000) != 2) &&
                 (readRfMask(1, 0x00, 0x000f0000) != 2))) break;
            if (i == 2499) pauseOk = false;
            IODelay(2);
        }

        for (UInt8 path = 0; path < 2; path++) {
            channel[path] = (UInt8)readRfMask(path, 0x18, 0xff);
            UInt8 activeBand = channel[path] <= 14 ? 0 :
                (channel[path] <= 64 ? 2 : (channel[path] <= 144 ? 3 : (channel[path] <= 177 ? 4 : 0xff)));
            if (channel[path] < 1 || activeBand == 0xff) {
                pathChannelOk[path] = false;
                continue;
            }
            UInt32 pathSetting = path == 0 ? 0x1800 : 0x4100;
            UInt32 setPi = path == 0 ? 0x001c : 0x00ec;
            UInt32 cfg1 = path == 0 ? 0x00000d18 : 0x00000d2a;
            UInt32 cfg2 = path == 0 ? 0x00000d19 : 0x00000d2b;

            write32Mask(0x1e24, 1U << 17, 1);
            write32Mask(0x1cd0, 1U << 28, 1);
            write32Mask(0x1cd0, 1U << 29, 1);
            write32Mask(0x1cd0, 1U << 30, 1);
            write32Mask(0x1cd0, 1U << 31, 0);
            write32Mask(0x1d58, 0x00000ff8, 0x1ff);
            write32Mask(path == 0 ? 0x1864 : 0x4164, 1U << 31, 1);
            write32Mask(path == 0 ? 0x180c : 0x410c, 1U << 27, 1);
            write32Mask(path == 0 ? 0x186c : 0x416c, 1U << 7, 1);
            write32Mask(path == 0 ? 0x180c : 0x410c, 0x3, 0);
            write32Mask(0x1a00, 0x00000006, 2);
            write32(0x1c38, 0xffffffff);
            UInt32 ana = path == 0 ? 0x1830 : 0x4130;
            for (UInt32 afe = 0; afe < 16; afe++) write32(ana, 0x700f0001 + (afe << 20));
            write32(ana, 0x70ff0001);
            write32Mask(0x0820, 0x3, path + 1);
            write32(0x1e2c, 0xe4e40000);
            write32Mask(0x1e28, 0xf, 3);
            write32Mask(pathSetting, 0x000fffff, 0x33312);
            write32Mask(pathSetting, 1U << 31, 1);
            write32Mask(setPi, 0xc0000000, 0);
            writeRfMask(path, 0xdf, 1U << 4, 1);
            writeRfMask(path, 0x58, 0x000fff00, 0x820);
            write32Mask(0x1b00, 0x00000006, path);
            write32Mask(0x1b10, 0xff, 0);
            write32Mask(0x1bb8, 1U << 20, 0);
            write32Mask(0x1bcc, 0x3f, 0x3f);
            write32Mask(0x1b20, 0xc0000000, 0);
            writeRfMask(path, 0xde, 1U << 16, 1);
            if (activeBand == 0) {
                writeRfMask(path, 0x00, 0xfffff, 0x5000f);
                writeRfMask(path, 0x55, 0x0000001c, 0);
                writeRfMask(path, 0x87, 1U << 18, 1);
                writeRfMask(path, 0x00, 0x000003e0, 0x0f);
                writeRfMask(path, 0xde, 1U << 2, 1);
                writeRfMask(path, 0x1a, 0x00007000, 1);
                writeRfMask(path, 0x1a, 0x00000c00, 0);
                writeRfMask(path, 0x8f, 1U << 1, 1);
                write32Mask(0x1b98, 0x00007000, 0);
            } else {
                writeRfMask(path, 0x00, 0xfffff, 0x50011);
                writeRfMask(path, 0x63, 0x0000c000, 3);
                writeRfMask(path, 0x63, 0x0000001c, 3);
                writeRfMask(path, 0x63, 0x00003000, 1);
                writeRfMask(path, 0x8a, 0x00000018, 2);
                writeRfMask(path, 0x00, 0x000003e0, 0x12);
                writeRfMask(path, 0xde, 1U << 2, 1);
                writeRfMask(path, 0x1a, 0x00000c00, 0);
                writeRfMask(path, 0x8f, 1U << 1, 1);
                writeRfMask(path, 0x00, 0x000f0000, 5);
                write32Mask(0x1b98, 0x00007000, activeBand == 2 ? 2 : (activeBand == 3 ? 3 : 4));
            }
            write32Mask(0x1b2c, 0xff, 0x18);
            IODelay(1000);
            write32Mask(0x1bcc, 0xff, activeBand == 0 ? 0x2d : 0x36);
            IODelay(1000);
            write32(0x1b00, cfg1);
            write32(0x1b00, cfg2);
            bool reportReady = false;
            for (UInt32 i = 0; i < 100; i++) {
                if (read32Mask(0x2d9c, 0xff) == 0x55) { reportReady = true; break; }
                IODelay(1000);
            }
            pathReportOk[path] = reportReady;
            write32Mask(setPi, 0xc0000000, 2);
            write32Mask(0x1b00, 0x00000006, path);
            write32Mask(0x1bd4, 1U << 21, 1);
            write32Mask(0x1bd4, 0x001f0000, 0x12);
            write32Mask(0x1b9c, 0x00000f00, 3);
            UInt32 report = read32(0x1bfc);
            for (UInt8 i = 0; i < 8; i++) {
                SInt8 value = (SInt8)((report >> (i * 4)) & 0xf);
                offsets[i][path] = (value & 0x8) ? (SInt8)(value | 0xf0) : value;
            }
            write32Mask(0x1b9c, 0x00000f00, 4);
            report = read32(0x1bfc);
            for (UInt8 i = 0; i < 2; i++) {
                SInt8 value = (SInt8)((report >> (i * 4)) & 0xf);
                offsets[i + 8][path] = (value & 0x8) ? (SInt8)(value | 0xf0) : value;
            }

            // rtw8822c_txgapk_rf_restore()/afe_dpk_restore()/bb_dpk_restore().
            writeRfMask(path, 0xde, 1U << 16, 0);
            writeRfMask(path, 0x9e, 1U << 5, 0);
            writeRfMask(path, 0x9e, 1U << 10, 0);
            write32Mask(0x1b00, 0x00000006, 0);
            write32Mask(0x1b20, 0xc0000000, 0);
            write32Mask(0x1bb8, 1U << 20, 0);
            write32Mask(0x1bcc, 0xff, 0);
            write32Mask(0x1b00, 0x00000006, 1);
            write32Mask(0x1b20, 0xc0000000, 0);
            write32Mask(0x1bb8, 1U << 20, 0);
            write32Mask(0x1bcc, 0xff, 0);
            write32Mask(0x1b00, 0x00000006, 0);
            write32Mask(0x1d58, 0x00000ff8, 0);
            write32Mask(path == 0 ? 0x1864 : 0x4164, 1U << 31, 0);
            write32Mask(path == 0 ? 0x180c : 0x410c, 1U << 27, 0);
            write32Mask(path == 0 ? 0x186c : 0x416c, 1U << 7, 0);
            write32Mask(path == 0 ? 0x180c : 0x410c, 0x3, 3);
            write32Mask(0x1a00, 0x00000006, 0);
            write32Mask(0x1b20, 0x07000000, 5);
            writeRfMask(path, 0x00, 0x000f0000, 3);
            writeRfMask(path, 0xde, 1U << 2, 0);
            writeRfMask(path, 0x8f, 1U << 1, 0);
            write32(0x1c38, 0xffa1005e);
            static const UInt32 afeRestore[] = {
                0x700b8041, 0x70144041, 0x70244041, 0x70344041,
                0x70444041, 0x705b8041, 0x70644041, 0x707b8041,
                0x708b8041, 0x709b8041, 0x70ab8041, 0x70bb8041,
                0x70cb8041, 0x70db8041, 0x70eb8041, 0x70fb8041
            };
            for (UInt32 afe = 0; afe < sizeof(afeRestore) / sizeof(afeRestore[0]); afe++)
                write32(ana, afeRestore[afe]);
        }

        // Linux write_tx_gain(): apply the measured 2.4 GHz offsets to both RF LUTs.
        for (UInt8 path = 0; path < 2; path++) {
            if (!pathChannelOk[path]) continue;
            UInt8 activeBand = channel[path] <= 14 ? 0 :
                (channel[path] <= 64 ? 2 : (channel[path] <= 144 ? 3 : 4));
            UInt32 lutBase = activeBand == 0 ? 0x20 :
                (activeBand == 2 ? 0x200 : (activeBand == 3 ? 0x280 : 0x300));
            SInt8 accumulated[11] = {};
            for (UInt8 i = 0; i < 11; i++) {
                for (UInt8 j = i; j < 11; j++) {
                    UInt32 value = gainTable[activeBand][j][path];
                    bool saturated = ((value >> 8) & 0xf) >= 0xc && ((value >> 4) & 0xf) >= 0xe;
                    if (!saturated) accumulated[i] = (SInt8)(accumulated[i] + offsets[j][path]);
                }
            }
            writeRfMask(path, 0xee, 0xfffff, 0x10000);
            for (UInt8 i = 0; i < 11; i++) {
                UInt32 value = gainTable[activeBand][i][path];
                bool saturated = ((value >> 8) & 0xf) >= 0xc && ((value >> 4) & 0xf) >= 0xe;
                SInt32 combined = (SInt32)(value << 1) + (SInt32)accumulated[i];
                UInt32 adjusted = saturated ? value :
                    ((UInt32)(combined >> 1) | ((combined & 1) ? (1U << 12) : 0));
                writeRfMask(path, 0x33, 0xfffff, lutBase + i);
                writeRfMask(path, 0x3f, 0x00001fff, adjusted);
            }
            writeRfMask(path, 0xee, 0xfffff, 0);
        }
    }

    // Restore every transient BB/RF/MAC state even when H2C or report polling failed.
    for (UInt8 path = 0; path < 2; path++)
        for (UInt32 i = 0; i < sizeof(rfRegs) / sizeof(rfRegs[0]); i++)
            writeRfMask(path, rfRegs[i], 0xfffff, rfSaved[path][i]);
    for (UInt32 i = 0; i < sizeof(bbRegs) / sizeof(bbRegs[0]); i++) write32(bbRegs[i], bbSaved[i]);
    write8(0x0522, txPauseSaved);

    bool ignoredBt = true;
    bool stopOk = txGapkRfkHandshake(false, &ignoredBt, &stopPolls);
    char status[512];
    snprintf(status, sizeof(status),
             "fresh=1 h2c_start=%d bt_idle=%d start_poll=%u pause=%d A:ch=%u report=%d off=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d B:ch=%u report=%d off=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d h2c_stop=%d stop_poll=%u",
             startOk ? 1 : 0, btIdle ? 1 : 0, (unsigned int)startPolls, pauseOk ? 1 : 0,
             (unsigned int)channel[0], pathReportOk[0] && pathChannelOk[0] ? 1 : 0,
             offsets[0][0], offsets[1][0], offsets[2][0], offsets[3][0], offsets[4][0], offsets[5][0], offsets[6][0], offsets[7][0], offsets[8][0], offsets[9][0],
             (unsigned int)channel[1], pathReportOk[1] && pathChannelOk[1] ? 1 : 0,
             offsets[0][1], offsets[1][1], offsets[2][1], offsets[3][1], offsets[4][1], offsets[5][1], offsets[6][1], offsets[7][1], offsets[8][1], offsets[9][1],
             stopOk ? 1 : 0, (unsigned int)stopPolls);
    RTW_DEBUG_PROPERTY("Debug_TXGAPK_Status", status);
    RTW_DEBUG_LOG("RealtekRTL8822C: TXGAPK %s\n", status);
}

bool RealtekRTL8822C::initPhy() {
    // 1. Power on BB/RF domains
    write8(0x0002, read8(0x0002) | 0x03); // REG_SYS_FUNC_EN |= GLB_RST | RSTB
    write8(0x001F, read8(0x001F) | 0x07); // REG_RF_CTRL
    write32(0x00EC, read32(0x00EC) | 0x07000000); // REG_WLRF1

    // Disable RFK Power Save (enable RF path DPD/processing for path 0 and 1)
    // REG_NCTL0 (0x1b00), REG_DPD_CTL1_S0 (0x1b08)
    write32Mask(0x1B00, 0x06, 0); // Select Path A (0)
    write32Mask(0x1B08, 1 << 7, 1); // Disable power save (set BIT_PS_EN)
    write32Mask(0x1B00, 0x06, 1); // Select Path B (1)
    write32Mask(0x1B08, 1 << 7, 1); // Disable power save (set BIT_PS_EN)
    write32Mask(0x1B00, 0x06, 0); // Restore path select to A

    // Pre init
    headerFileInit(true);

    // Load MAC, BB, and AGC tables
    loadTable(rtw8822c_mac, sizeof(rtw8822c_mac) / sizeof(UInt32), 0, 0);
    loadTable(rtw8822c_bb, sizeof(rtw8822c_bb) / sizeof(UInt32), 2, 0);
    loadTable(rtw8822c_agc, sizeof(rtw8822c_agc) / sizeof(UInt32), 1, 0);

    // Crystal cap
    UInt8 crystal_cap = logicalEfuseMap[0xb9] & 0x7F;
    write32Mask(0x1040, 0xfffc00,
                (UInt32)crystal_cap | ((UInt32)crystal_cap << 7));

    // Post init
    headerFileInit(false);

    // Linux rtw_load_rfk_table() preconditions
    write32Mask(0x1e24, 1U << 17, 1);
    write32Mask(0x1cd0, 1U << 28, 1);
    write32Mask(0x1cd0, 1U << 29, 1);
    write32Mask(0x1cd0, 1U << 30, 1);
    write32Mask(0x1cd0, 1U << 31, 0);

    // Load RFK init table (rtw8822c_array_mp_cal_init is a 2-column table: addr, value)
    loadTable(rtw8822c_array_mp_cal_init,
              sizeof(rtw8822c_array_mp_cal_init) / sizeof(UInt32),
              2, 0);

    char rfkInitStr[64];
    snprintf(rfkInitStr, sizeof(rfkInitStr), "loaded mp_cal_init entries=%lu",
             (unsigned long)(sizeof(rtw8822c_array_mp_cal_init) / sizeof(UInt32) / 2));
    RTW_DEBUG_PROPERTY("Debug_RFK_Init", rfkInitStr);

    char rfkRegsStr[128];
    snprintf(rfkRegsStr, sizeof(rfkRegsStr), "1e24=0x%08X 1cd0=0x%08X 1b10=0x%08X 1b20=0x%08X 1c38=0x%08X",
             (unsigned int)read32(0x1e24), (unsigned int)read32(0x1cd0),
             (unsigned int)read32(0x1b10), (unsigned int)read32(0x1b20),
             (unsigned int)read32(0x1c38));
    RTW_DEBUG_PROPERTY("Debug_RFK_Regs", rfkRegsStr);

    // The generated local arrays are already normalized to their target paths.
    loadTable(rtw8822c_rf_a, sizeof(rtw8822c_rf_a) / sizeof(UInt32), 3, 0);
    loadTable(rtw8822c_rf_b, sizeof(rtw8822c_rf_b) / sizeof(UInt32), 3, 1);

    // Config TRX path
    configTrxMode(3, 3, false);

    // DPK MAC/BB and AFE tables are calibration-only. Linux applies them only
    // after backing up normal state and restores every affected register.
    RTW_DEBUG_PROPERTY("Debug_DPK_Init_State", "skipped-calibration-only-tables");

    // Linux performs fresh DACK before trim and channel/IQK programming.
    runDackCalibration();
    runRfX2Check();
    RTW_DEBUG_PROPERTY("Debug_TXGAPK_Status", "deferred=target-channel");

    // Thermal Trim
    UInt8 pg_therm_a = physEfuseMap[0x1ef];
    if (pg_therm_a != 0xFF) {
        UInt8 therm_val = (pg_therm_a >> 1) & 0x07;
        therm_val |= (pg_therm_a & 0x01) << 3;
        writeRfMask(0, 0x43, 0xF0000, therm_val);
    }
    UInt8 pg_therm_b = physEfuseMap[0x1b0];
    if (pg_therm_b != 0xFF) {
        UInt8 therm_val = (pg_therm_b >> 1) & 0x07;
        therm_val |= (pg_therm_b & 0x01) << 3;
        writeRfMask(1, 0x43, 0xF0000, therm_val);
    }

    // PA Bias Trim
    UInt8 pabias_2ga = physEfuseMap[0x1d6];
    if (pabias_2ga != 0xFF) {
        writeRfMask(0, 0x60, 0xF000, pabias_2ga & 0x0F);
    }
    UInt8 pabias_2gb = physEfuseMap[0x1d5];
    if (pabias_2gb != 0xFF) {
        writeRfMask(1, 0x60, 0xF000, pabias_2gb & 0x0F);
    }
    UInt8 pabias_5ga = physEfuseMap[0x1d8];
    if (pabias_5ga != 0xFF) {
        writeRfMask(0, 0x60, 0xF0000, pabias_5ga & 0x0F);
    }
    UInt8 pabias_5gb = physEfuseMap[0x1d7];
    if (pabias_5gb != 0xFF) {
        writeRfMask(1, 0x60, 0xF0000, pabias_5gb & 0x0F);
    }

    // Apply the Linux factory power-trim LUT when PPG bytes exist.
    UInt8 trim2gLow = physEfuseMap[0x1d4];
    UInt8 trim2gMid = physEfuseMap[0x1ee];
    UInt8 trim2gHigh = physEfuseMap[0x1d2];
    bool has2gPowerTrim = trim2gLow != 0xFF || trim2gMid != 0xFF || trim2gHigh != 0xFF;
    UInt8 trimGain[2][8] = {};
    if (trim2gLow != 0xff) {
        trimGain[0][0] = (UInt8)(trim2gLow & 0x0F);
        trimGain[1][0] = (UInt8)((trim2gLow >> 4) & 0x0F);
    }
    if (trim2gMid != 0xff) {
        trimGain[0][1] = (UInt8)(trim2gMid & 0x0F);
        trimGain[1][1] = (UInt8)((trim2gMid >> 4) & 0x0F);
    }
    if (trim2gHigh != 0xff) {
        trimGain[0][2] = (UInt8)(trim2gHigh & 0x0F);
        trimGain[1][2] = (UInt8)((trim2gHigh >> 4) & 0x0F);
    }
    const UInt16 trim5gOffset[2][5] = {
        { 0x1ec, 0x1e8, 0x1e4, 0x1e0, 0x1dc },
        { 0x1eb, 0x1e7, 0x1e3, 0x1df, 0x1db }
    };
    bool has5gPowerTrim = false;
    for (UInt8 path = 0; path < 2; path++) {
        for (UInt8 i = 0; i < 5; i++) {
            UInt8 value = physEfuseMap[trim5gOffset[path][i]];
            if (value == 0xff) continue;
            has5gPowerTrim = true;
            trimGain[path][i + 3] = value & 0x1f;
        }
    }
    if (has2gPowerTrim || has5gPowerTrim) {
        const UInt8 linuxTrimIndex[] = { 0, 1, 2, 2, 3, 4, 5, 6, 7, 3, 4, 5, 6, 7, 7 };
        for (UInt8 path = 0; path < 2; path++) {
            writeRfMask(path, 0xee, 1U << 19, 1);
            for (UInt8 seq = 0; seq < sizeof(linuxTrimIndex); seq++) {
                writeRfMask(path, 0x33, 0xfffff, seq);
                writeRfMask(path, 0x3f, 0xfffff, trimGain[path][linuxTrimIndex[seq]]);
            }
            writeRfMask(path, 0xee, 1U << 19, 0);
        }
    }
    char powerTrim5gDb[192];
    snprintf(powerTrim5gDb, sizeof(powerTrim5gDb),
             "A=%02x/%02x/%02x/%02x/%02x B=%02x/%02x/%02x/%02x/%02x applied=%d",
             physEfuseMap[0x1ec], physEfuseMap[0x1e8], physEfuseMap[0x1e4],
             physEfuseMap[0x1e0], physEfuseMap[0x1dc], physEfuseMap[0x1eb],
             physEfuseMap[0x1e7], physEfuseMap[0x1e3], physEfuseMap[0x1df],
             physEfuseMap[0x1db], has5gPowerTrim ? 1 : 0);
    RTW_DEBUG_PROPERTY("Debug_5G_Power_Trim", powerTrim5gDb);
    char powerTrimDb[96];
    snprintf(powerTrimDb, sizeof(powerTrimDb), "2g=%02x/%02x/%02x applied=%d",
             trim2gLow, trim2gMid, trim2gHigh, has2gPowerTrim ? 1 : 0);
    RTW_DEBUG_PROPERTY("Debug_2G_Power_Trim", powerTrimDb);

    // Disable DPD for all supported rates, matching Linux rtw8822c_power_trim().
    write32Mask(0x0A70, 0x3FF, 0x3FF);

    // --- WORKAROUND: Lock RF Synthesizer to a valid channel before IQK ---
    // If the RF synthesizer is not locked to a valid channel, the hardware
    // PHY calibration engine will fail (IQK = 0x00).
    // We simulate rtw8822c_set_channel_rf for 2.4GHz, Channel 6, 20MHz bandwidth.

    UInt32 rf_reg18_a = readRfMask(0, 0x18, 0xfffff);
    UInt32 rf_reg18_b = readRfMask(1, 0x18, 0xfffff);

    char dbg_buf[64];
    snprintf(dbg_buf, sizeof(dbg_buf), "A=0x%05X B=0x%05X", (unsigned int)rf_reg18_a, (unsigned int)rf_reg18_b);
    RTW_DEBUG_PROPERTY("Debug_RF_Reg18_Read", dbg_buf);

    write32Mask(0x1c90, 0x100, 0); // REG_RSTB, BIT_RSTB_3WIRE

    UInt32 rf_rxbb = 0x18; // 20MHz

    // Path A
    writeRfMask(0, 0xee, 0x04, 0x01); // RF_LUTWE2
    writeRfMask(0, 0x33, 0x1f, 0x12); // RF_LUTWA
    writeRfMask(0, 0x3f, 0xfffff, rf_rxbb); // RF_LUTWD0
    writeRfMask(0, 0xee, 0x04, 0x00); // RF_LUTWE2

    // Path B
    writeRfMask(1, 0xee, 0x04, 0x01); // RF_LUTWE2
    writeRfMask(1, 0x33, 0x1f, 0x12); // RF_LUTWA
    writeRfMask(1, 0x3f, 0xfffff, rf_rxbb); // RF_LUTWD0
    writeRfMask(1, 0xee, 0x04, 0x00); // RF_LUTWE2

    rf_reg18_a &= ~(0x10300U | 0xffU | 0x60000U | 0x3000U);
    rf_reg18_a |= 0x000000; // BAND_2G (0x00000)
    rf_reg18_a |= 6;        // Channel 6
    rf_reg18_a |= 0x003000; // BW_20M
    writeRfMask(0, 0x18, 0xfffff, rf_reg18_a); // RF_CFGCH

    rf_reg18_b &= ~(0x10300U | 0xffU | 0x60000U | 0x3000U);
    rf_reg18_b |= 0x000000;
    rf_reg18_b |= 6;
    rf_reg18_b |= 0x003000;
    writeRfMask(1, 0x18, 0xfffff, rf_reg18_b);

    // Enable 3-wire
    write32Mask(0x1c90, 0x100, 1);
    write32Mask(0x1830, 0x20000000, 1); // REG_ANAPAR_A, BIT_ANAPAR_UPDATE
    write32Mask(0x4130, 0x20000000, 1); // REG_ANAPAR_B, BIT_ANAPAR_UPDATE

    IODelay(1000);
    // ----------------------------------------------------------------------

    // Snapshot RF0 around firmware IQK to identify the unexpected RF mode writer.
    char rf0PreIqk[64];
    snprintf(rf0PreIqk, sizeof(rf0PreIqk), "A=%05x B=%05x",
             (unsigned int)readRfMask(0, 0x00, 0xfffff),
             (unsigned int)readRfMask(1, 0x00, 0xfffff));
    RTW_DEBUG_PROPERTY("Debug_Rf0_PreIQK", rf0PreIqk);

    bool iqkOk = runIqk();

    // Boot calibration is also a balanced RFK transaction.
    write32Mask(0x1B00, 0x06, 0);
    write32Mask(0x1B08, 1U << 7, 0);
    write32Mask(0x1B00, 0x06, 1);
    write32Mask(0x1B08, 1U << 7, 0);
    write32Mask(0x1B00, 0x06, 0);

    char rf0PostIqk[64];
    snprintf(rf0PostIqk, sizeof(rf0PostIqk), "A=%05x B=%05x",
             (unsigned int)readRfMask(0, 0x00, 0xfffff),
             (unsigned int)readRfMask(1, 0x00, 0xfffff));
    RTW_DEBUG_PROPERTY("Debug_Rf0_PostIQK", rf0PostIqk);

    write32Mask(0x1b00, 0x06, 0);
    UInt32 rfkPsA = read32Mask(0x1b08, 1U << 7);
    write32Mask(0x1b00, 0x06, 1);
    UInt32 rfkPsB = read32Mask(0x1b08, 1U << 7);
    write32Mask(0x1b00, 0x06, 0);
    char phyBaseline[384];
    snprintf(phyBaseline, sizeof(phyBaseline),
             "rfmap=A->A,B->B dpk_tables=skipped wire=%08x/%08x cca=%08x rfgate=%08x/%08x bg=%08x txfifo=%08x pause=%02x psA=%u psB=%u",
             (unsigned int)read32(0x180c), (unsigned int)read32(0x410c),
             (unsigned int)read32(0x1d58), (unsigned int)read32(0x1864),
             (unsigned int)read32(0x4164), (unsigned int)read32(0x1a14),
             (unsigned int)read32(0x1e70), read8(0x0522),
             (unsigned int)rfkPsA, (unsigned int)rfkPsB);
    RTW_DEBUG_PROPERTY("Debug_Phy_Baseline", phyBaseline);
    return iqkOk;
}

void RealtekRTL8822C::loadTable3(const UInt32* data, UInt32 size) {
    // 3-element format: {addr, mask, val}, no condition codes
    UInt32 entries = size / 3;
    for (UInt32 i = 0; i < entries; i++) {
        UInt32 addr = data[i * 3];
        UInt32 mask = data[i * 3 + 1];
        UInt32 val  = data[i * 3 + 2];
        write32Mask(addr, mask, val);
    }
}

bool RealtekRTL8822C::sendGeneralInfo() {
    UInt8 h2c_pkt[32];
    memset(h2c_pkt, 0, 32);

    UInt32* pkt32 = (UInt32*)h2c_pkt;
    // Word 0: category[6:0]=0x01, cmd_id[15:8]=0xFF, sub_id[31:16]=0x0D
    pkt32[0] = 0x01U | (0xFFU << 8) | ((UInt32)0x0DU << 16);
    // Word 1: total_len[15:0]=12, seq[31:16]=h2cSeq
    pkt32[1] = 12U | ((UInt32)h2cSeq << 16);
    // Word 2: GENERAL_INFO_SET_FW_TX_BOUNDARY[23:16]=56 (1994 - 1938)
    pkt32[2] = 56U << 16;
    h2cSeq++;

    return sendH2CPacket(h2c_pkt, 32);
}

bool RealtekRTL8822C::sendPhyDmInfo() {
    UInt8 h2c_pkt[32];
    memset(h2c_pkt, 0, 32);

    UInt32* pkt32 = (UInt32*)h2c_pkt;
    // Word 0: category[6:0]=0x01, cmd_id[15:8]=0xFF, sub_id[31:16]=0x11
    pkt32[0] = 0x01U | (0xFFU << 8) | ((UInt32)0x11U << 16);
    // Word 1: total_len[15:0]=16, seq[31:16]=h2cSeq
    pkt32[1] = 16U | ((UInt32)h2cSeq << 16);
    // Word 2: rfe_option, rf_type=2, cut_ver=cutVersion, rx_ant=3, tx_ant=3
    pkt32[2] = (rfeOption & 0xFFU) | (2U << 8) | ((cutVersion & 0xFFU) << 16) | (3U << 24) | (3U << 28);
    h2cSeq++;

    return sendH2CPacket(h2c_pkt, 32);
}


bool RealtekRTL8822C::translate8023to80211(const UInt8* eth_frame, UInt32 eth_len, UInt8* wifi_frame, UInt32* wifi_len) {
    if (eth_len < 14) return false;

    const UInt8* dst_mac = eth_frame;
    UInt16 eth_type = (UInt16)(((UInt16)eth_frame[12] << 8) | (UInt16)eth_frame[13]);

    // Infrastructure uplink: ToDS is bit 0 of the second FC byte. The old
    // code used 0x08 (Retry), producing a non-DS frame with the wrong flag.
    // HT traffic uses QoS Data/TID0 so that a TX BA session can apply.
    // EAPOL-Key frames stay non-QoS, matching mac80211/net80211 handshake
    // ordering. Ordinary traffic uses TID 0 once HT is negotiated.
    bool qos = htNegotiated && eth_type != 0x888e;
    wifi_frame[0] = qos ? 0x88 : 0x08;
    wifi_frame[1] = 0x01;
    wifi_frame[2] = 0x00; // Duration
    wifi_frame[3] = 0x00;

    // Addr1 (BSSID)
    UInt8 bssid[6];
    memcpy(bssid, currentBssid, 6);
    bool bssid_is_zero = true;
    for (int i = 0; i < 6; i++) {
        if (bssid[i] != 0) { bssid_is_zero = false; break; }
    }
    // A zero BSSID means the connection state changed under the independent
    // output queue. Never transmit to the old bring-up placeholder address;
    // reject this packet so the caller can drop it as a permanent state error
    // instead of poisoning the queue with an endless OutputStall retry.
    if (bssid_is_zero) return false;
    memcpy(wifi_frame + 4, bssid, 6);

    // Addr2 (Source MAC)
    memcpy(wifi_frame + 10, macAddress, 6);

    // Addr3 (Destination MAC)
    memcpy(wifi_frame + 16, dst_mac, 6);

    // Seq Control
    wifi_frame[22] = 0x00;
    wifi_frame[23] = 0x00;

    UInt32 hdrLen = qos ? 26 : 24;
    if (qos) {
        wifi_frame[24] = 0x00; // QoS control: TID 0, normal ACK policy
        wifi_frame[25] = 0x00;
    }

    // LLC/SNAP header (8 bytes)
    wifi_frame[hdrLen + 0] = 0xAA;
    wifi_frame[hdrLen + 1] = 0xAA;
    wifi_frame[hdrLen + 2] = 0x03;
    wifi_frame[hdrLen + 3] = 0x00;
    wifi_frame[hdrLen + 4] = 0x00;
    wifi_frame[hdrLen + 5] = 0x00;
    wifi_frame[hdrLen + 6] = (eth_type >> 8) & 0xFF;
    wifi_frame[hdrLen + 7] = eth_type & 0xFF;

    // Payload
    memcpy(wifi_frame + hdrLen + 8, eth_frame + 14, eth_len - 14);

    *wifi_len = (eth_len - 14) + hdrLen + 8;
    return true;
}

bool RealtekRTL8822C::deliverEthernetFrame(const UInt8* frame, UInt32 len) {
    if (!frame || len < 14 || len > 1514 || !netif) return false;
    UInt16 ethType = (UInt16)(((UInt16)frame[12] << 8) | (UInt16)frame[13]);
    return deliverEthernetPayload(frame, frame + 6, ethType,
                                  frame + 14, len - 14);
}

bool RealtekRTL8822C::deliverEthernetPayload(const UInt8* dst, const UInt8* src,
                                           UInt16 ethType, const UInt8* payload,
                                           UInt32 payloadLen) {
    if (!dst || !src || (!payload && payloadLen != 0) ||
        payloadLen > 1500 || !netif) return false;
    UInt32 len = payloadLen + 14;
    // IEEE 802.1X is the only traffic permitted before the controlled port is
    // authorized. Open networks authorize it at association time; WPA will do
    // so only after the pairwise handshake and key installation.
    if (!portAuthorized && ethType != 0x888e) return false;
    if (ethType == 0x888e && targetSecurityMode == SECURITY_WPA2) {
        bool handled = handleEapolKey(payload, payloadLen);
        if (!handled) publishWpaState("eapol-rejected");
        return handled;
    }

#if RTW_DEBUG
    uint64_t buildStarted = 0;
    uint64_t buildFinished = 0;
    uint64_t buildElapsedNs = 0;
    uint64_t stageStarted = 0;
    uint64_t stageFinished = 0;
    uint64_t stageElapsedNs = 0;
    clock_get_uptime(&buildStarted);
    stageStarted = buildStarted;
#endif
    mbuf_t m = takeRxPacket();
    if (!m) {
        // Preserve connectivity if the independent replenisher briefly falls
        // behind, but never wait indefinitely on the hardware RX work loop.
        unsigned int chunks = 1;
        if (mbuf_allocpacket(MBUF_DONTWAIT, kRtwRxPacketBufferSize,
                             &chunks, &m) != 0 || !m)
            return false;
        mbuf_adj(m, ETHER_ALIGN);
    }
#if RTW_DEBUG
    clock_get_uptime(&stageFinished);
    absolutetime_to_nanoseconds(stageFinished - stageStarted, &stageElapsedNs);
    debugRxPoolTakeTotalNs += stageElapsedNs;
    if (stageElapsedNs > debugRxPoolTakeMaxNs)
        debugRxPoolTakeMaxNs = stageElapsedNs;
    stageStarted = stageFinished;
#endif
    UInt8* ethernet = (UInt8*)mbuf_data(m);
    memcpy(ethernet, dst, 6);
    memcpy(ethernet + 6, src, 6);
    ethernet[12] = (UInt8)(ethType >> 8);
    ethernet[13] = (UInt8)ethType;
    if (payloadLen) memcpy(ethernet + 14, payload, payloadLen);
#if RTW_DEBUG
    clock_get_uptime(&stageFinished);
    absolutetime_to_nanoseconds(stageFinished - stageStarted, &stageElapsedNs);
    debugRxCopyTotalNs += stageElapsedNs;
    if (stageElapsedNs > debugRxCopyMaxNs)
        debugRxCopyMaxNs = stageElapsedNs;
    stageStarted = stageFinished;
#endif
    mbuf_setlen(m, len);
    mbuf_pkthdr_setlen(m, len);
#if RTW_DEBUG
    clock_get_uptime(&buildFinished);
    absolutetime_to_nanoseconds(buildFinished - stageStarted, &stageElapsedNs);
    debugRxFinalizeTotalNs += stageElapsedNs;
    if (stageElapsedNs > debugRxFinalizeMaxNs)
        debugRxFinalizeMaxNs = stageElapsedNs;
    absolutetime_to_nanoseconds(buildFinished - buildStarted, &buildElapsedNs);
    debugRxBuildCalls++;
    debugRxBuildTotalNs += buildElapsedNs;
    if (buildElapsedNs > debugRxBuildMaxNs)
        debugRxBuildMaxNs = buildElapsedNs;
#endif
#if RTW_DEBUG
    uint64_t inputStarted = 0;
    uint64_t inputFinished = 0;
    uint64_t inputElapsedNs = 0;
    clock_get_uptime(&inputStarted);
#endif
    UInt32 inputRet = netif->inputPacket(m, len);
#if RTW_DEBUG
    clock_get_uptime(&inputFinished);
    absolutetime_to_nanoseconds(inputFinished - inputStarted, &inputElapsedNs);
    debugRxInputCalls++;
    debugRxInputTotalNs += inputElapsedNs;
    if (inputElapsedNs > debugRxInputMaxNs)
        debugRxInputMaxNs = inputElapsedNs;
#endif
    if (inputRet > 0) debugDataRxDelivered += inputRet;

#if RTW_DEBUG
    bool isIcmp = ethType == 0x0800 && payloadLen >= 21 && payload[9] == 1;
    bool importantRxDiagnostic = ethType == 0x888e || ethType == 0x0806 || isIcmp;
    if (importantRxDiagnostic || ((UInt32)debugDataRxFrames & kRtwHotDiagnosticMask) == 1U) {
        char deliveredDb[512];
        if (ethType == 0x0800 && payloadLen >= 20) {
            UInt8 ipProto = payload[9];
            UInt8 icmpType = (ipProto == 1 && payloadLen >= 21) ? payload[20] : 0xff;
            snprintf(deliveredDb, sizeof(deliveredDb),
                     "input=%u len=%u type=0800 proto=%u src=%u.%u.%u.%u dst=%u.%u.%u.%u icmp=%u port=%d",
                     (unsigned int)inputRet, (unsigned int)len, ipProto,
                     payload[12], payload[13], payload[14], payload[15],
                     payload[16], payload[17], payload[18], payload[19], icmpType,
                     portAuthorized ? 1 : 0);
        } else if (ethType == 0x0806 && payloadLen >= 28) {
            UInt16 arpOp = (UInt16)(((UInt16)payload[6] << 8) | (UInt16)payload[7]);
            snprintf(deliveredDb, sizeof(deliveredDb),
                     "input=%u len=%u type=0806 arp_op=%u sender=%u.%u.%u.%u target=%u.%u.%u.%u port=%d",
                     (unsigned int)inputRet, (unsigned int)len, arpOp,
                     payload[14], payload[15], payload[16], payload[17],
                     payload[24], payload[25], payload[26], payload[27],
                     portAuthorized ? 1 : 0);
        } else {
            snprintf(deliveredDb, sizeof(deliveredDb), "input=%u len=%u type=%04x port=%d",
                     (unsigned int)inputRet, (unsigned int)len, ethType,
                     portAuthorized ? 1 : 0);
        }
        RTW_DEBUG_PROPERTY("Debug_RX_Delivered_Last", deliveredDb);
    }
#endif
    return inputRet > 0;
}

bool RealtekRTL8822C::validateCcmpView(const UInt8* frame, UInt32 len,
                                    UInt8 encType, bool decrypted,
                                    UInt32* payloadSkip, UInt32* tailTrim) {
    if (!frame || !payloadSkip || !tailTrim || len < 24 || !(frame[1] & 0x40) ||
        encType != 4 || !decrypted || !wpaPtkInstalled) return false;
    UInt8 subtype = (frame[0] >> 4) & 0x0f;
    bool qos = (subtype & 0x08) != 0;
    UInt32 headerLen = qos ? 26 : 24;
    if (qos && (frame[1] & 0x80)) headerLen += 4;
    if (len < headerLen + 16) return false;
    const UInt8* ccmp = frame + headerLen;
    if (ccmp[2] != 0 || !(ccmp[3] & 0x20)) return false;
    UInt8 keyId = (ccmp[3] >> 6) & 3;
    UInt64 pn = (UInt64)ccmp[0] | ((UInt64)ccmp[1] << 8) |
                ((UInt64)ccmp[4] << 16) | ((UInt64)ccmp[5] << 24) |
                ((UInt64)ccmp[6] << 32) | ((UInt64)ccmp[7] << 40);
    UInt8 tid = qos ? (frame[24] & 0x0f) : 0;
    bool group = (frame[4] & 1) != 0;
    UInt64* lastPn = nullptr;
    if (group) {
        if (!wpaGtkInstalled || keyId != wpaGtkKeyId) return false;
        lastPn = &wpaGroupRxPn[keyId][tid];
    } else {
        if (keyId != 0) return false;
        lastPn = &wpaPairwiseRxPn[tid];
    }
    if (pn == 0 || pn <= *lastPn) {
        wpaReplayDrops++;
        publishWpaState("ccmp-replay-drop");
        return false;
    }

#if RTW_DEBUG
    uint64_t ccmpStarted = 0;
    uint64_t ccmpFinished = 0;
    uint64_t ccmpElapsedNs = 0;
    clock_get_uptime(&ccmpStarted);
#endif
    // Hardware has already authenticated and decrypted this MPDU. Keep the
    // DMA slot immutable and pass a plaintext view downstream instead of
    // allocating and copying every protected frame on the interrupt work loop.
    *payloadSkip = 8;
    *tailTrim = 8;
#if RTW_DEBUG
    clock_get_uptime(&ccmpFinished);
    absolutetime_to_nanoseconds(ccmpFinished - ccmpStarted, &ccmpElapsedNs);
    debugRxCcmpCalls++;
    debugRxCcmpTotalNs += ccmpElapsedNs;
    if (ccmpElapsedNs > debugRxCcmpMaxNs)
        debugRxCcmpMaxNs = ccmpElapsedNs;
#endif
    *lastPn = pn;
    return true;
}

bool RealtekRTL8822C::deliverWifiDataFrame(const UInt8* frame, UInt32 len,
                                         UInt8 encType, bool decrypted) {
    if (!frame || len < 24) return false;
    if (!(frame[1] & 0x40)) return deliverPlainWifiDataFrame(frame, len);
    UInt32 payloadSkip = 0;
    UInt32 tailTrim = 0;
    if (!validateCcmpView(frame, len, encType, decrypted,
                          &payloadSkip, &tailTrim))
        return false;
    return deliverPlainWifiDataFrame(frame, len, payloadSkip, tailTrim);
}

bool RealtekRTL8822C::deliverPlainWifiDataFrame(const UInt8* frame, UInt32 len,
                                              UInt32 payloadSkip, UInt32 tailTrim) {
    if (!frame || len < 24) return false;
    UInt8 subtype = (frame[0] >> 4) & 0x0f;
    bool qos = (subtype & 0x08) != 0;
    UInt32 hdrLen = qos ? 26 : 24;
    if (qos && (frame[1] & 0x80)) hdrLen += 4;
    if (len < hdrLen || payloadSkip > len - hdrLen ||
        tailTrim > len - hdrLen - payloadSkip) return false;
    UInt32 payloadStart = hdrLen + payloadSkip;
    UInt32 payloadEnd = len - tailTrim;

    bool amsdu = qos && (frame[24] & 0x80) != 0;
    if (!amsdu) {
        UInt8 fc1 = frame[1];
        bool toDs = (fc1 & 0x01) != 0;
        bool fromDs = (fc1 & 0x02) != 0;
        if (toDs || !fromDs || memcmp(frame + 10, currentBssid, 6) != 0 ||
            (targetSecurityMode == SECURITY_OPEN && (fc1 & 0x40))) return false;
        const UInt8* dst = frame + 4;
        bool group = (dst[0] & 0x01) != 0;
        if (!group && memcmp(dst, macAddress, 6) != 0) return false;
        if (payloadEnd < payloadStart + 8) return false;
        const UInt8* llc = frame + payloadStart;
        if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03) return false;
        UInt16 ethType = (UInt16)(((UInt16)llc[6] << 8) | llc[7]);
        if (targetSecurityMode != SECURITY_OPEN && !(fc1 & 0x40) &&
            ethType != 0x888e) return false;
        UInt32 msduPayloadLen = payloadEnd - payloadStart - 8;
        return deliverEthernetPayload(dst, frame + 16, ethType,
                                      llc + 8, msduPayloadLen);
    }

    // A-MSDU subframes contain DA, SA, a big-endian MSDU length and are
    // padded to a four-byte boundary. Decapsulation must happen after MPDU
    // reordering, matching mac80211's RX ordering.
    if ((frame[1] & 0x03) != 0x02 || memcmp(frame + 10, currentBssid, 6) != 0 ||
        (targetSecurityMode == SECURITY_OPEN && (frame[1] & 0x40)))
        return false;
    if (targetSecurityMode != SECURITY_OPEN && !(frame[1] & 0x40)) return false;
    UInt32 offset = payloadStart;
    UInt32 delivered = 0;
    while (offset + 14 <= payloadEnd) {
        const UInt8* sub = frame + offset;
        UInt32 msduLen = ((UInt32)sub[12] << 8) | sub[13];
        if (msduLen < 8 || offset + 14 + msduLen > payloadEnd) return false;
        bool group = (sub[0] & 0x01) != 0;
        if (!group && memcmp(sub, macAddress, 6) != 0) return false;
        const UInt8* llc = sub + 14;
        if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03) return false;
        UInt32 payloadLen = msduLen - 8;
        if (payloadLen > 1500) return false;
        UInt16 ethType = (UInt16)(((UInt16)llc[6] << 8) | llc[7]);
        if (deliverEthernetPayload(sub, sub + 6, ethType,
                                   llc + 8, payloadLen)) delivered++;
        UInt32 subframeLen = 14 + msduLen;
        offset += (subframeLen + 3) & ~3U;
    }
    rxAmsduDelivered += delivered;
    return delivered != 0;
}

void RealtekRTL8822C::releaseRxReorder(UInt8 tid) {
    if (tid >= RX_BA_TID_COUNT || !rxBa[tid].active) return;
    RxBaSession& session = rxBa[tid];
    for (;;) {
        RxReorderEntry& entry = session.entry[session.headSeq % RX_REORDER_MAX_WINDOW];
        if (!entry.frame || entry.seq != session.headSeq) break;
        UInt8* frame = entry.frame;
        UInt32 len = entry.len;
        UInt8 encType = entry.encType;
        bool decrypted = entry.decrypted;
        entry.frame = nullptr;
        entry.len = 0;
        if (session.buffered) session.buffered--;
        session.headSeq = (session.headSeq + 1) & 0x0fff;
        deliverWifiDataFrame(frame, len, encType, decrypted);
        IOFree(frame, len);
        rxReorderReleased++;
    }
}

void RealtekRTL8822C::advanceRxReorder(UInt8 tid, UInt16 newHead) {
    if (tid >= RX_BA_TID_COUNT || !rxBa[tid].active) return;
    RxBaSession& session = rxBa[tid];
    UInt16 steps = (newHead - session.headSeq) & 0x0fff;
    if (steps >= 2048) return;
    while (steps--) {
        RxReorderEntry& entry = session.entry[session.headSeq % RX_REORDER_MAX_WINDOW];
        if (entry.frame && entry.seq == session.headSeq) {
            UInt8* frame = entry.frame;
            UInt32 len = entry.len;
            UInt8 encType = entry.encType;
            bool decrypted = entry.decrypted;
            entry.frame = nullptr;
            entry.len = 0;
            if (session.buffered) session.buffered--;
            deliverWifiDataFrame(frame, len, encType, decrypted);
            IOFree(frame, len);
            rxReorderReleased++;
        } else {
            rxReorderHoles++;
        }
        session.headSeq = (session.headSeq + 1) & 0x0fff;
    }
    releaseRxReorder(tid);
}

bool RealtekRTL8822C::startRxBaSession(UInt8 tid, UInt8 dialogToken,
                                     UInt16 startSeq, UInt16 windowSize,
                                     UInt16 timeoutTu) {
    if (tid >= RX_BA_TID_COUNT) return false;
    if (windowSize == 0) windowSize = RX_REORDER_MAX_WINDOW;
    if (windowSize > RX_REORDER_MAX_WINDOW) windowSize = RX_REORDER_MAX_WINDOW;
    RxBaSession& session = rxBa[tid];
    // APs may retry the same ADDBA Request until our response is observed.
    // A retry is idempotent: acknowledge it again without destroying the live
    // reorder window or manufacturing holes from already advanced sequence
    // state. A genuinely new agreement replaces the old one without flushing
    // missing sequence numbers as delivered data.
    if (session.active && session.dialogToken == dialogToken &&
        session.windowSize == windowSize && session.timeoutTu == timeoutTu) {
        rxBaRetries++;
        return true;
    }
    stopRxBaSession(tid, false);
    session.active = true;
    session.dialogToken = dialogToken;
    session.headSeq = startSeq & 0x0fff;
    session.windowSize = windowSize;
    session.timeoutTu = timeoutTu;
    session.buffered = 0;
    rxBaAccepted++;
    publishRxBaState();
    return true;
}

void RealtekRTL8822C::stopRxBaSession(UInt8 tid, bool flush) {
    if (tid >= RX_BA_TID_COUNT) return;
    RxBaSession& session = rxBa[tid];
    if (session.active && flush) advanceRxReorder(tid, (session.headSeq + session.windowSize) & 0x0fff);
    for (UInt32 i = 0; i < RX_REORDER_MAX_WINDOW; i++) {
        if (session.entry[i].frame) {
            IOFree(session.entry[i].frame, session.entry[i].len);
            session.entry[i].frame = nullptr;
        }
    }
    if (session.active) rxBaStopped++;
    memset(&session, 0, sizeof(session));
}

void RealtekRTL8822C::resetRxBaSessions(bool flush) {
    for (UInt8 tid = 0; tid < RX_BA_TID_COUNT; tid++) stopRxBaSession(tid, flush);
    publishRxBaState();
}

void RealtekRTL8822C::processRxReorderTimeouts() {
    uint64_t now;
    clock_get_uptime(&now);
    for (UInt8 tid = 0; tid < RX_BA_TID_COUNT; tid++) {
        RxBaSession& session = rxBa[tid];
        if (!session.active || !session.buffered) continue;
        UInt16 bestSeq = 0;
        UInt16 bestDelta = 0xffff;
        uint64_t oldest = now;
        bool found = false;
        for (UInt32 i = 0; i < RX_REORDER_MAX_WINDOW; i++) {
            RxReorderEntry& entry = session.entry[i];
            if (!entry.frame) continue;
            UInt16 delta = (entry.seq - session.headSeq) & 0x0fff;
            if (delta < bestDelta) { bestDelta = delta; bestSeq = entry.seq; }
            if (entry.queuedAt < oldest) oldest = entry.queuedAt;
            found = true;
        }
        if (!found) continue;
        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(now - oldest, &elapsedNs);
        uint64_t timeoutNs = session.timeoutTu ? (uint64_t)session.timeoutTu * 1024000ULL : 100000000ULL;
        if (timeoutNs < 10000000ULL) timeoutNs = 10000000ULL;
        if (timeoutNs > 1000000000ULL) timeoutNs = 1000000000ULL;
        if (elapsedNs >= timeoutNs) advanceRxReorder(tid, bestSeq);
    }
}

void RealtekRTL8822C::publishRxBaState() {
#if RTW_DEBUG
    UInt32 active = 0, buffered = 0;
    char tids[128];
    tids[0] = '\0';
    for (UInt8 tid = 0; tid < RX_BA_TID_COUNT; tid++) {
        if (!rxBa[tid].active) continue;
        active++;
        buffered += rxBa[tid].buffered;
        char item[24];
        snprintf(item, sizeof(item), "%s%u:%u/%u", tids[0] ? "," : "", tid,
                 rxBa[tid].headSeq, rxBa[tid].windowSize);
        strlcat(tids, item, sizeof(tids));
    }
    char db[320];
    snprintf(db, sizeof(db),
             "active=%u tids=%s buffered=%u accepted=%u stopped=%u retries=%u queued=%u released=%u duplicate=%u holes=%u amsdu=%u",
             (unsigned int)active, tids[0] ? tids : "none", (unsigned int)buffered,
             (unsigned int)rxBaAccepted, (unsigned int)rxBaStopped,
             (unsigned int)rxBaRetries,
             (unsigned int)rxReorderBuffered, (unsigned int)rxReorderReleased,
             (unsigned int)rxReorderDuplicates, (unsigned int)rxReorderHoles,
             (unsigned int)rxAmsduDelivered);
    RTW_DEBUG_PROPERTY("Debug_RX_BA_State", db);
#endif
}

void RealtekRTL8822C::handleRxDataFrame(const UInt8* frame, UInt32 len,
                                     UInt8 encType, bool decrypted) {
    debugDataRxFrames++;
    bool delivered = false;
    if (len >= 24 && connState == CONN_STATE_CONNECTED &&
        memcmp(frame + 10, currentBssid, 6) == 0) {
        clock_get_uptime(&lastPeerRxTime);
        UInt8 subtype = (frame[0] >> 4) & 0x0f;
        bool qos = (subtype & 0x08) != 0;
        UInt8 tid = qos && len >= 26 ? frame[24] & 0x0f : 0;
        UInt16 seq = (UInt16)(((UInt16)frame[22] | ((UInt16)frame[23] << 8)) >> 4);
        UInt8 frag = frame[22] & 0x0f;
        if (qos && frag == 0 && tid < RX_BA_TID_COUNT && rxBa[tid].active) {
            RxBaSession& session = rxBa[tid];
            UInt16 delta = (seq - session.headSeq) & 0x0fff;
            if (delta >= 2048) {
                rxReorderDuplicates++;
            } else {
                if (delta >= session.windowSize) {
                    UInt16 newHead = (seq - session.windowSize + 1) & 0x0fff;
                    advanceRxReorder(tid, newHead);
                    delta = (seq - session.headSeq) & 0x0fff;
                }
                if (delta == 0) {
                    delivered = deliverWifiDataFrame(frame, len, encType, decrypted);
                    session.headSeq = (session.headSeq + 1) & 0x0fff;
                    rxReorderReleased++;
                    releaseRxReorder(tid);
                } else {
                    RxReorderEntry& entry = session.entry[seq % RX_REORDER_MAX_WINDOW];
                    if (entry.frame && entry.seq == seq) {
                        rxReorderDuplicates++;
                    } else {
                        if (entry.frame) {
                            IOFree(entry.frame, entry.len);
                            if (session.buffered) session.buffered--;
                        }
                        entry.frame = (UInt8*)IOMalloc(len);
                        if (entry.frame) {
                            memcpy(entry.frame, frame, len);
                            entry.len = len;
                            entry.seq = seq;
                            entry.encType = encType;
                            entry.decrypted = decrypted;
                            clock_get_uptime(&entry.queuedAt);
                            session.buffered++;
                            rxReorderBuffered++;
                        } else {
                            debugDataRxRejected++;
                        }
                    }
                }
            }
        } else {
            delivered = deliverWifiDataFrame(frame, len, encType, decrypted);
        }
    }
    if (!delivered && !(len >= 26 && ((frame[0] >> 4) & 0x08) &&
                        rxBa[frame[24] & 0x0f].active)) {
        debugDataRxRejected++;
    }
#if RTW_DEBUG
    if (((UInt32)debugDataRxFrames & kRtwHotDiagnosticMask) == 1U) {
        char db[320];
        snprintf(db, sizeof(db),
                 "count=%d len=%u fc=%02x/%02x seq=%u tid=%u delivered=%d protected=%d enc=%u decrypted=%d port=%d",
                 debugDataRxFrames, (unsigned int)len, frame[0], frame[1],
                 len >= 24 ? (((UInt16)frame[22] | ((UInt16)frame[23] << 8)) >> 4) : 0,
                 len >= 26 ? frame[24] & 0x0f : 0, delivered ? 1 : 0,
                 (frame[1] & 0x40) ? 1 : 0, encType, decrypted ? 1 : 0,
                 portAuthorized ? 1 : 0);
        RTW_DEBUG_PROPERTY("Debug_RX_Data_Last", db);
    }
#endif
}

void RealtekRTL8822C::handleControlFrame(const UInt8* frame, UInt32 len) {
    if (len < 20 || ((frame[0] >> 4) & 0x0f) != 8) return; // Block Ack Request
    if (memcmp(frame + 10, currentBssid, 6) != 0) return;
    UInt16 control = (UInt16)((UInt16)frame[16] | ((UInt16)frame[17] << 8));
    if (control & (1U << 1)) return; // Multi-TID BAR is outside HT20 STA use.
    UInt8 tid = (control >> 12) & 0x0f;
    UInt16 startSeq = (UInt16)(((UInt16)frame[18] | ((UInt16)frame[19] << 8)) >> 4);
    if (tid < RX_BA_TID_COUNT && rxBa[tid].active) advanceRxReorder(tid, startSeq);
}

bool RealtekRTL8822C::transmitWifiFrame(const UInt8* frame, UInt32 len, UInt8 qsel) {
    IOBufferMemoryDescriptor* desc = beqDesc;
    IOBufferMemoryDescriptor* payloadDesc = beqPayloadDesc;
    IODMACommand* dmaCmd = beqDmaCmd;
    IODMACommand* payloadDmaCmd = beqPayloadDmaCmd;
    UInt32 wp = beqWp;
    UInt32 idx_reg = 0x03A8; // BEQ

    if (qsel == 18) {
        desc = mgmtDesc;
        payloadDesc = mgmtPayloadDesc;
        dmaCmd = mgmtDmaCmd;
        payloadDmaCmd = mgmtPayloadDmaCmd;
        wp = mgmtWp;
        idx_reg = 0x03B0; // MGMTQ
        if (((wp + 1) % 128) == mgmtRp) {
            setProperty("DriverStatus", "MGMT queue full!");
            return false;
        }
    }

    if (!ioBase || !payloadDesc || !desc) return false;

    volatile UInt32* buf_desc = (volatile UInt32*)((UInt8*)desc->getBytesNoCopy() + wp * 16);
    UInt8* slot_virt = (UInt8*)payloadDesc->getBytesNoCopy() + wp * 2048;
    UInt32 slot_phys = (UInt32)(payloadDesc->getPhysicalSegment(wp * 2048, NULL) & 0xFFFFFFFF);
    UInt32 slot_dma = slot_phys;

    if (qsel == 18) {
        UInt32 slot_iova = (UInt32)(mgmtPayloadPhysAddr + wp * 2048);
        slot_dma = slot_iova;
        char dmaDb[128];
        snprintf(dmaDb, sizeof(dmaDb), "phys=0x%08x iova=0x%08x match=%d", slot_phys, slot_iova, slot_phys == slot_iova);
        RTW_DEBUG_PROPERTY("Debug_MGMT_DMA_Addr", dmaDb);
    }

    volatile UInt32* tx_desc = (volatile UInt32*)slot_virt;
    memset((void*)tx_desc, 0, 48);

    // W0: TXPKTSIZE, OFFSET, LS, DISQSELSEQ
    tx_desc[0] = (len & 0xFFFF) | (48 << 16) | (1U << 26) | (1U << 31);

    UInt8 rate_id = 8; // RTW_RATEID_B_20M for non-management traffic
    UInt8 datarate = 0x00; // DESC_RATE1M for non-management traffic
    if (qsel == 18) {
        // Linux uses the G table for 5 GHz management and B_20M on 2.4 GHz.
        rate_id = currentChannel.channel > 14 ? 7 : 8;
        datarate = 0x04;   // DESC_RATE6M
    }

    // W1: QSEL and RATE_ID
    tx_desc[1] = ((UInt32)qsel << 8) | ((UInt32)rate_id << 16);

    // W3: USE_RATE, DISDATAFB
    tx_desc[3] = (1 << 8) | (1 << 10);

    // W4: DATARATE
    tx_desc[4] = datarate;

    // W2: SPE_RPT (BIT 19) to request CCX TX report from firmware
    tx_desc[2] = (1U << 19);

    // W6: SW_DEFINE = unique incrementing serial number (sequence)
    txSeqNum = (txSeqNum + 1) % 64;
    tx_desc[6] = (txSeqNum << 2) & 0xFC;

    // W8: EN_HWSEQ
    tx_desc[8] = (1U << 15);

    // Calculate TX descriptor checksum (RTW_TX_DESC_W7_TXDESC_CHECKSUM) over 24 words (48 bytes)
    tx_desc[7] &= ~0x0000FFFFU; // clear checksum field
    UInt16 chksum = 0;
    volatile UInt16* desc_words = (volatile UInt16*)tx_desc;
    for (int i = 0; i < 24; i++) {
        chksum ^= desc_words[i];
    }
    tx_desc[7] |= chksum;

    // Copy the raw 802.11 frame payload
    memcpy(slot_virt + 48, frame, len);

    // Two-segment buffer descriptor, matching Linux pci.c:843-847:
    // BD0: psb_len (ceil of total/128), buf_size=48 (tx_desc header), dma=slot_phys
    // BD1: buf_size=len (802.11 frame body), dma=slot_phys+48
    UInt32 total_len = len + 48;
    UInt32 psb_len = (total_len + 127) / 128; // ceil((48+len)/128)
    // buf_desc layout per pci.h: [u16 buf_size][u16 psb_len][u32 dma] per entry
    buf_desc[0] = (48 & 0xFFFF) | (psb_len << 16); // buf_size=48, psb_len
    buf_desc[1] = slot_dma;                          // DMA-mapped address of tx_desc
    buf_desc[2] = (len & 0xFFFF);                    // buf_size=len (payload)
    buf_desc[3] = slot_dma + 48;                     // DMA-mapped address of 802.11 frame

    char tx_mgmt_db[256];
    snprintf(tx_mgmt_db, sizeof(tx_mgmt_db), "W0=0x%08x W1=0x%08x W2=0x%08x W3=0x%08x W4=0x%08x W6=0x%08x BD0=0x%08x BD1=0x%08x BD2=0x%08x BD3=0x%08x",
             (unsigned int)tx_desc[0], (unsigned int)tx_desc[1], (unsigned int)tx_desc[2], (unsigned int)tx_desc[3],
             (unsigned int)tx_desc[4], (unsigned int)tx_desc[6],
             (unsigned int)buf_desc[0], (unsigned int)buf_desc[1], (unsigned int)buf_desc[2], (unsigned int)buf_desc[3]);

    if (qsel == 18) {
        RTW_DEBUG_PROPERTY("Debug_MGMT_Last_Desc", tx_mgmt_db);
    } else {
        RTW_DEBUG_PROPERTY("Debug_Tx_Last_Mgmt", tx_mgmt_db);
    }

    payloadDmaCmd->synchronize(kIODirectionOut);
    dmaCmd->synchronize(kIODirectionOut);
    OSSynchronizeIO();

    if (qsel == 18) {
        debugMgmtSlot = wp;
        debugMgmtReportSn = (UInt8)(tx_desc[6] & 0xfc);
        debugMgmtOfdmTxBaseline = read32(0x2de0);
        memcpy(debugMgmtShadow, slot_virt, sizeof(debugMgmtShadow));
        debugMgmtShadowValid = true;

        UInt16 descXor = 0;
        for (UInt32 i = 0; i < 24; i++) {
            descXor ^= OSReadLittleInt16(slot_virt, i * 2);
        }
        char frameHex[128];
        UInt32 framePos = 0;
        frameHex[0] = '\0';
        UInt32 frameDumpLen = len < 30 ? len : 30;
        for (UInt32 i = 0; i < frameDumpLen && framePos + 4 < sizeof(frameHex); i++) {
            int written = snprintf(frameHex + framePos, sizeof(frameHex) - framePos,
                                   "%02x%s", slot_virt[48 + i], i + 1 == frameDumpLen ? "" : " ");
            if (written > 0) framePos += (UInt32)written;
        }

        char preTx[1024];
        snprintf(preTx, sizeof(preTx),
                 "sn=%02x slot=%u len=%u idx=%08x cr=%02x pause=%02x txfifo=%08x txdma=%08x fwhw=%08x retry=%04x ack=%02x/%02x edcca=%u bb=%08x wire=%08x/%08x cca=%08x ant=%08x txmap=%08x path=%08x/%08x rf0=%05x/%05x rf18=%05x/%05x ofdm=%08x xor=%04x frame=%s",
                 debugMgmtReportSn, (unsigned int)debugMgmtSlot, (unsigned int)len,
                 (unsigned int)read32(0x03b0), read8(0x0100), read8(0x0522),
                 (unsigned int)read32(0x1e70), (unsigned int)read32(0x0210),
                 (unsigned int)read32(0x0420), (unsigned int)read16(0x042a),
                 read8(0x0640), read8(0x0639),
                 (unsigned int)read32Mask(0x2d38, 1U << 24),
                 (unsigned int)read32(0x1c3c), (unsigned int)read32(0x180c),
                 (unsigned int)read32(0x410c), (unsigned int)read32(0x1d58),
                 (unsigned int)read32(0x0820), (unsigned int)read32(0x1e2c),
                 (unsigned int)read32(0x1800), (unsigned int)read32(0x4100),
                 (unsigned int)readRfMask(0, 0x00, 0xfffff),
                 (unsigned int)readRfMask(1, 0x00, 0xfffff),
                 (unsigned int)readRfMask(0, 0x18, 0xfffff),
                 (unsigned int)readRfMask(1, 0x18, 0xfffff),
                 (unsigned int)debugMgmtOfdmTxBaseline, descXor, frameHex);
        RTW_DEBUG_PROPERTY("Debug_MGMT_PreDoorbell", preTx);
    }

    if (qsel == 18) {
        wp = (wp + 1) % 128;
        mgmtWp = wp;
    } else {
        wp = (wp + 1) % 256;
        beqWp = wp;
    }

    write16(idx_reg, (UInt16)wp);
    OSSynchronizeIO();

    return true;
}

bool RealtekRTL8822C::restoreConnectedScanHome() {
    if (!ioBase || connState != CONN_STATE_CONNECTED) return false;
    bool channelOk = setChannelHw(connectedScanHomeCenter, 0,
                                  connectedScanHomeBandwidth,
                                  connectedScanHomePrimaryIndex, true);
    bool powerOk = connectedScanHomePrimary <= 14 ||
                   configure5GTxPower(connectedScanHomePrimary,
                                      connectedScanHomeCenter,
                                      connectedScanHomeBandwidth, false);
    write32(0x0608, connectedScanOriginalRcr);
    OSSynchronizeIO();
    return channelOk && powerOk;
}

void RealtekRTL8822C::finishConnectedScan(bool cancelled, const char* reason,
                                       bool restoreHome) {
    if (offchannelScanTimer) offchannelScanTimer->cancelTimeout();
    bool restored = true;
    if (restoreHome && connState == CONN_STATE_CONNECTED)
        restored = restoreConnectedScanHome();
    else if (ioBase)
        write32(0x0608, connectedScanOriginalRcr);

    scanInProgress = false;
    connectedScanPhase = 0;
    publishScanResults();
    if (!cancelled && restored) clock_get_uptime(&lastScanCompletedTime);

    IOOutputQueue* queue = getOutputQueue();
    if (queue && restored && interfaceEnabled &&
        connState == CONN_STATE_CONNECTED && portAuthorized)
        queue->start();

    char scanDb[256];
    snprintf(scanDb, sizeof(scanDb),
             "available=1 active=0 result=%s reason=%s restored=%d count=%d home=%u/%u/%u idx=%u",
             cancelled ? "cancelled" : "complete", reason ? reason : "none",
             restored ? 1 : 0, customScanResultsCount,
             (unsigned int)connectedScanHomePrimary,
             (unsigned int)connectedScanHomeCenter,
             connectedScanHomeBandwidth == 2 ? 80U :
             (connectedScanHomeBandwidth == 1 ? 40U : 20U),
             connectedScanHomePrimaryIndex);
    publishConnectedScanState(scanDb);
    if (!cancelled && restored && connState == CONN_STATE_CONNECTED) {
        setProperty("WiFiStatus", "Connected");
        setProperty("DriverStatus", customScanResultsCount > 0 ?
                    "Connected scan complete" :
                    "Connected scan complete: no networks found");
        traceEvent("scan:connected-complete");
    } else if (!restored && connState == CONN_STATE_CONNECTED) {
        setProperty("DriverStatus", "Connected scan cancelled: home channel restore failed");
        traceEvent("scan:restore-failed");
    } else {
        char cancelledStatus[192];
        snprintf(cancelledStatus, sizeof(cancelledStatus),
                 "Connected scan cancelled: %s",
                 reason ? reason : "unknown reason");
        setProperty("DriverStatus", cancelledStatus);
        traceEvent("scan:connected-cancelled");
    }
}

bool RealtekRTL8822C::startConnectedScan() {
    if (!offchannelScanTimer || scanInProgress ||
        connState != CONN_STATE_CONNECTED || !portAuthorized)
        return false;

    connectedScanOriginalRcr = read32(0x0608);
    connectedScanHomePrimary = (UInt32)targetChannel;
    connectedScanHomeCenter = (UInt32)targetCenterChannel;
    connectedScanHomeBandwidth = targetBandwidth;
    connectedScanHomePrimaryIndex = targetPrimaryChannelIndex;
    pruneScanResults(300000000000ULL);
    connectedScanIndex = 0;
    connectedScanPhase = 1; // drain BEQ before the first excursion
    connectedScanDrainRetries = 0;
    lastScanCompletedTime = 0;
    scanInProgress = true;

    IOOutputQueue* queue = getOutputQueue();
    if (queue) queue->stop();
    setProperty("DriverStatus", "Connected off-channel scan started");
    char scanDb[192];
    snprintf(scanDb, sizeof(scanDb),
             "available=1 active=1 phase=drain channel=0 index=0 home=%u/%u/%u idx=%u",
             (unsigned int)connectedScanHomePrimary,
             (unsigned int)connectedScanHomeCenter,
             connectedScanHomeBandwidth == 2 ? 80U :
             (connectedScanHomeBandwidth == 1 ? 40U : 20U),
             connectedScanHomePrimaryIndex);
    publishConnectedScanState(scanDb);
    traceEvent("scan:connected-start");
    offchannelScanTimer->setTimeoutMS(1);
    return true;
}

void RealtekRTL8822C::advanceConnectedScan(IOTimerEventSource* sender) {
    static const UInt8 scanChannels[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        36, 40, 44, 48, 149, 153, 157, 161, 165
    };
    static const UInt32 scanChannelCount = sizeof(scanChannels) / sizeof(scanChannels[0]);
    if (!scanInProgress || !sender) return;
    if (hardwareSuspended || !interfaceEnabled ||
        connState != CONN_STATE_CONNECTED || !portAuthorized) {
        finishConnectedScan(true, "link-state-changed", false);
        return;
    }

    if (connectedScanPhase == 1) { // drain
        updateBeqCompletion();
        UInt32 outstanding = 0;
        if (beqLock) {
            IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
            outstanding = beqOutstanding;
            IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
        }
        if (outstanding != 0) {
            if (++connectedScanDrainRetries >= 40) {
                finishConnectedScan(true, "beq-drain-timeout", true);
                return;
            }
            sender->setTimeoutMS(5);
            return;
        }
        connectedScanDrainRetries = 0;
        if (connectedScanIndex >= scanChannelCount) {
            finishConnectedScan(false, "complete", true);
            return;
        }

        UInt32 channel = scanChannels[connectedScanIndex];
        scanChannel = channel;
        UInt32 promiscuousRcr = (connectedScanOriginalRcr | 0x0fU) &
                                ~((1U << 6) | (1U << 7));
        write32(0x0608, promiscuousRcr);
        if (channel != connectedScanHomePrimary) {
            UInt32 flags = channel > 14 ? APPLE80211_C_FLAG_5GHZ :
                                         APPLE80211_C_FLAG_2GHZ;
            if (!setChannelHw(channel, flags | APPLE80211_C_FLAG_20MHZ,
                              0, 0, true)) {
                finishConnectedScan(true, "offchannel-program-failed", true);
                return;
            }
        }
        connectedScanPhase = 2; // off-channel dwell
        char scanDb[192];
        snprintf(scanDb, sizeof(scanDb),
                 "available=1 active=1 phase=dwell channel=%u index=%u/%u home=%u/%u/%u",
                 (unsigned int)channel, (unsigned int)(connectedScanIndex + 1),
                 (unsigned int)scanChannelCount,
                 (unsigned int)connectedScanHomePrimary,
                 (unsigned int)connectedScanHomeCenter,
                 connectedScanHomeBandwidth == 2 ? 80U :
                 (connectedScanHomeBandwidth == 1 ? 40U : 20U));
        publishConnectedScanState(scanDb);
        sender->setTimeoutMS(55);
        return;
    }

    if (connectedScanPhase == 2) { // return home after every dwell
        if (!restoreConnectedScanHome()) {
            finishConnectedScan(true, "home-restore-failed", true);
            return;
        }
        IOOutputQueue* queue = getOutputQueue();
        if (queue) queue->start();
        connectedScanPhase = 3;
        publishConnectedScanState("available=1 active=1 phase=home");
        sender->setTimeoutMS(65);
        return;
    }

    if (connectedScanPhase == 3) { // home-channel service window
        IOOutputQueue* queue = getOutputQueue();
        if (queue) queue->stop();
        connectedScanIndex++;
        connectedScanPhase = 1;
        sender->setTimeoutMS(1);
        return;
    }

    finishConnectedScan(true, "invalid-phase", true);
}

void RealtekRTL8822C::connectedScanTimerFired(OSObject* owner,
                                           IOTimerEventSource* sender) {
    (void)owner;
    advanceConnectedScan(sender);
}

bool RealtekRTL8822C::performWifiScan() {
    customScanResultsCount = 0;
    lastScanCompletedTime = 0;
    UInt32 orig_rcr = read32(0x0608);

    // Set promiscuous: accept AP, AM, APM, AB, AAP
    // CRITICAL: do NOT call setChannelHw() to restore at end of scan —
    // the wrong 3-wire control register sequence (0x1c90 bit 8) breaks the
    // RF receiver after the restore call, making subsequent scans deaf.
    // The ISR (handleInterrupt) now handles all beacon parsing; we just
    // need to stay promiscuous for the sweep duration.
    write32(0x0608, (orig_rcr | 0x0F) & ~((1U << 6) | (1U << 7)));
    OSSynchronizeIO();

    RTW_DEBUG_LOG("RealtekRTL8822C: Starting channel sweep...\n");
    static const UInt8 scanChannels[] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        36, 40, 44, 48, 149, 153, 157, 161, 165
    };
    bool scanOk = true;
    for (UInt32 index = 0; index < sizeof(scanChannels); index++) {
        UInt32 chan = scanChannels[index];
        scanChannel = chan;
        UInt32 channelFlags = chan > 14 ? APPLE80211_C_FLAG_5GHZ : APPLE80211_C_FLAG_2GHZ;
        if (!setChannelHw(chan, channelFlags | APPLE80211_C_FLAG_20MHZ,
                          0, 0, true)) {
            scanOk = false;
            break;
        }
        // setProperties() is serialized through the controller command gate.
        // While this scan holds that gate the interrupt event source cannot
        // run on the same work loop, so drain the RX ring synchronously after
        // every dwell. Advancing rxRp prevents the deferred ISR from parsing
        // these frames a second time after the command returns.
        IOSleep(250);
        processRxPacketsForScan();
    }
    processRxPacketsForScan();

    // Restore only RCR (promiscuous -> normal filter).
    // We intentionally leave the RF on the last swept channel to avoid
    // corrupting the RF state with a restore call. The next connect/scan will
    // set the channel correctly via setChannelHw().
    write32(0x0608, orig_rcr);
    OSSynchronizeIO();

    publishScanResults();
    if (scanOk) clock_get_uptime(&lastScanCompletedTime);
    return scanOk;
}

void RealtekRTL8822C::publishScanResults() {
    if (customScanResultsCount == 0) {
        setProperty("ScanResults", "No networks found.");
        return;
    }

    char buf[4096];
    buf[0] = '\0';
    for (int i = 0; i < customScanResultsCount; i++) {
        char entry[192];
        char signalText[24];
        if (customScanResults[i].rssi == -127)
            strlcpy(signalText, "unknown", sizeof(signalText));
        else
            snprintf(signalText, sizeof(signalText), "%ddBm", customScanResults[i].rssi);
        const char* security = customScanResults[i].security_mode == SECURITY_WPA2 ? "WPA2" :
                               (customScanResults[i].security_mode == SECURITY_WPA ? "WPA" : "Open");
        snprintf(entry, sizeof(entry), "\n  - %s (%s, ch=%d) [Signal: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x]",
                 customScanResults[i].ssid,
                 security,
                 customScanResults[i].channel,
                 signalText,
                 customScanResults[i].bssid[0], customScanResults[i].bssid[1], customScanResults[i].bssid[2],
                 customScanResults[i].bssid[3], customScanResults[i].bssid[4], customScanResults[i].bssid[5]);
        strlcat(buf, entry, sizeof(buf));
    }

    setProperty("ScanResults", buf);
}

void RealtekRTL8822C::pruneScanResults(uint64_t maxAgeNs) {
    uint64_t now = 0;
    clock_get_uptime(&now);
    int kept = 0;
    for (int i = 0; i < customScanResultsCount; i++) {
        uint64_t elapsedNs = maxAgeNs + 1;
        if (customScanResults[i].last_seen_time != 0 &&
            now >= customScanResults[i].last_seen_time) {
            absolutetime_to_nanoseconds(
                now - customScanResults[i].last_seen_time, &elapsedNs);
        }
        bool current = connState == CONN_STATE_CONNECTED &&
                       memcmp(customScanResults[i].bssid, currentBssid, 6) == 0;
        if (!current && elapsedNs > maxAgeNs) continue;
        if (kept != i) customScanResults[kept] = customScanResults[i];
        kept++;
    }
    customScanResultsCount = kept;
}

void RealtekRTL8822C::processRxPacketsForScan() {
    UInt32 tmp = read32(0x03B4);
    UInt32 hw_wp = (tmp >> 16) & 0x0FFF;
    UInt32 rp = rxRp;

    int processed = 0;
    while (rp != hw_wp && processed < 512) {
        rxBufferDmaCmd->synchronize(kIODirectionIn);
        UInt8* rx_buf = rxBufferVirtAddr + rp * 12288;

        UInt32 w0 = OSReadLittleInt32(rx_buf, 0);
        UInt32 w2 = OSReadLittleInt32(rx_buf, 8);

        UInt32 pkt_len = w0 & 0x3FFF;
        UInt32 crc_err = (w0 >> 14) & 1;
        UInt32 drv_info_sz = ((w0 >> 16) & 0x0F) * 8;
        UInt32 shift = (w0 >> 24) & 0x03;
        UInt32 is_c2h = (w2 >> 28) & 1;

        UInt32 pkt_offset = 24 + drv_info_sz + shift;

        if (pkt_len > 0 && !crc_err && !is_c2h) {
            UInt8* wifi_frame = rx_buf + pkt_offset;
            UInt32 wifi_len = pkt_len;
            if (wifi_len > 4) wifi_len -= 4; // Strip FCS

            int signalDbm = -127;
            if ((w0 & (1U << 26)) && drv_info_sz != 0)
                signalDbm = extractRxSignalDbm(rx_buf + 24 + shift, drv_info_sz);
            parseBeaconOrProbeResponse(wifi_frame, wifi_len, signalDbm);
        }

        rp = (rp + 1) % 512;
        processed++;
    }

    if (processed > 0) {
        rxRp = rp;
        write16(0x03B4, (UInt16)rp);
        OSSynchronizeIO();
    }
}

int RealtekRTL8822C::extractRxSignalDbm(const UInt8* phyStatus, UInt32 phyStatusLen) {
    if (!phyStatus || phyStatusLen < 3) return -127;
    UInt8 page = phyStatus[0] & 0x0f;
    SInt32 signal = -127;

    if (page == 1) {
        // RTL8822C PHY status page 1: PWDB A/B are bytes 1 and 2.
        // Linux query_phy_status_page1() converts them with PWDB - 110 and
        // reports the stronger receive path.
        SInt8 pathA = (SInt8)((SInt32)phyStatus[1] - 110);
        SInt8 pathB = (SInt8)((SInt32)phyStatus[2] - 110);
        signal = pathA > pathB ? pathA : pathB;
    } else if (page == 0 && phyStatusLen >= 20) {
        // CCK page 0 also applies the gain-index correction used by Linux
        // query_phy_status_page0(). Signal power is reported from path A.
        SInt8 pathA = (SInt8)phyStatus[1];
        UInt8 gainA = phyStatus[2] & 0x3f;
        UInt8 rawB = phyStatus[16];
        UInt8 gainB = phyStatus[19] & 0x3f;
        UInt8 upper = (UInt8)((read32Mask(0x1a98, 0xc000) << 4) |
                              read32Mask(0x1aa8, 0x000f0000));
        UInt8 lower = (UInt8)((read32Mask(0x1a98, 0x00c0) << 4) |
                              read32Mask(0x1a70, 0x0f000000));
        if (gainA < lower)
            pathA = (SInt8)((SInt32)pathA + ((lower - gainA) << 1));
        else if (gainA > upper)
            pathA = (SInt8)((SInt32)pathA - ((gainA - upper) << 1));
        pathA = (SInt8)((SInt32)pathA - 110);
        signal = pathA;
        (void)rawB;
        (void)gainB;
    } else {
        signal = -127;
    }

    if (signal > 0) signal = -127;
    if (signal < -120 && signal != -127) signal = -120;
    return (int)signal;
}

void RealtekRTL8822C::parseBeaconOrProbeResponse(const UInt8* frame, UInt32 len, int signalDbm) {
    if (!frame || len < 24) {
        RTW_DEBUG_PROPERTY("Debug_Scan_Parse", "rejected: null or truncated 802.11 header");
        return;
    }
    UInt8 fc0 = frame[0];
    UInt8 type = (fc0 >> 2) & 0x03;
    UInt8 subtype = (fc0 >> 4) & 0x0F;

    if (type != 0 || (subtype != 8 && subtype != 5)) {
        return;
    }

    const UInt8* bssid = frame + 16;
    if (len < 36) return;

    if (connState == CONN_STATE_CONNECTED &&
        memcmp(bssid, currentBssid, 6) == 0 &&
        signalDbm >= -120 && signalDbm <= 0) {
        connectedSignalDbm = connectedSignalDbm >= -120 ?
            (connectedSignalDbm * 7 + signalDbm) / 8 : signalDbm;
    }

    UInt32 ie_offset = 36;
    char ssid[33] = {0};
    int channel = 0;
    bool is_secure = false;
    bool supports_cck = false;
    bool supports_ht = false;
    bool supports_vht = false;
    bool supports_wmm = false;
    UInt8 security_mode = SECURITY_OPEN;
    UInt8 rsn_ie_len = 0;
    UInt8 rsn_ie[64];
    memset(rsn_ie, 0, sizeof(rsn_ie));
    UInt16 ht_cap_info = 0;
    UInt8 ht_ampdu_params = 0;
    UInt8 ht_mcs0 = 0;
    UInt8 ht_mcs1 = 0;
    UInt8 ht_primary_channel = 0;
    UInt8 ht_operation_info = 0;
    UInt32 vht_cap_info = 0;
    UInt16 vht_rx_mcs_map = 0xffff;
    UInt16 vht_rx_highest = 0;
    UInt16 vht_tx_mcs_map = 0xffff;
    UInt16 vht_tx_highest = 0;
    UInt8 vht_channel_width = 0;
    UInt8 vht_center_segment0 = 0;
    UInt8 vht_center_segment1 = 0;

    while (ie_offset + 2 <= len) {
        UInt8 ie_id = frame[ie_offset];
        UInt8 ie_len = frame[ie_offset + 1];

        if (ie_offset + 2 + ie_len > len) {
            break;
        }

        const UInt8* ie_val = frame + ie_offset + 2;

        if (ie_id == 0) {
            int ssid_len = ie_len > 32 ? 32 : ie_len;
            memcpy(ssid, ie_val, ssid_len);
            ssid[ssid_len] = '\0';
        } else if (ie_id == 1) {
            for (int k = 0; k < ie_len; k++) {
                UInt8 r = ie_val[k] & 0x7F;
                if (r == 2 || r == 4 || r == 11 || r == 22) {
                    supports_cck = true;
                }
            }
        } else if (ie_id == 3 && ie_len == 1) {
            channel = ie_val[0];
        } else if (ie_id == 45 && ie_len >= 19) { // IEEE 802.11 HT Capability
            supports_ht = true;
            ht_cap_info = (UInt16)((UInt16)ie_val[0] | ((UInt16)ie_val[1] << 8));
            ht_ampdu_params = ie_val[2];
            ht_mcs0 = ie_val[3];
            ht_mcs1 = ie_val[4];
        } else if (ie_id == 61 && ie_len >= 2) { // IEEE 802.11 HT Operation
            ht_primary_channel = ie_val[0];
            ht_operation_info = ie_val[1];
        } else if (ie_id == 191 && ie_len >= 12) { // IEEE 802.11 VHT Capability
            supports_vht = true;
            vht_cap_info = (UInt32)ie_val[0] | ((UInt32)ie_val[1] << 8) |
                           ((UInt32)ie_val[2] << 16) | ((UInt32)ie_val[3] << 24);
            vht_rx_mcs_map = (UInt16)((UInt16)ie_val[4] | ((UInt16)ie_val[5] << 8));
            vht_rx_highest = (UInt16)((UInt16)ie_val[6] | ((UInt16)ie_val[7] << 8));
            vht_tx_mcs_map = (UInt16)((UInt16)ie_val[8] | ((UInt16)ie_val[9] << 8));
            vht_tx_highest = (UInt16)((UInt16)ie_val[10] | ((UInt16)ie_val[11] << 8));
        } else if (ie_id == 192 && ie_len >= 5) { // IEEE 802.11 VHT Operation
            vht_channel_width = ie_val[0];
            vht_center_segment0 = ie_val[1];
            vht_center_segment1 = ie_val[2];
        } else if (ie_id == 48) {
            is_secure = true;
            security_mode = SECURITY_WPA2;
            if ((UInt32)ie_len + 2U <= sizeof(rsn_ie)) {
                rsn_ie_len = (UInt8)(ie_len + 2);
                memcpy(rsn_ie, frame + ie_offset, rsn_ie_len);
            } else {
                rsn_ie_len = 0;
            }
        } else if (ie_id == 221 && ie_len >= 4 &&
                   ie_val[0] == 0x00 && ie_val[1] == 0x50 && ie_val[2] == 0xF2) {
            if (ie_val[3] == 0x01) {
                is_secure = true; // WPA
                if (security_mode == SECURITY_OPEN) security_mode = SECURITY_WPA;
                if (rsn_ie_len == 0) {
                    rsn_ie_len = (UInt8)((ie_len + 2) > sizeof(rsn_ie) ? sizeof(rsn_ie) : (ie_len + 2));
                    memcpy(rsn_ie, frame + ie_offset, rsn_ie_len);
                }
            } else if (ie_val[3] == 0x02 && ie_len >= 7 &&
                       (ie_val[4] == 0x00 || ie_val[4] == 0x01) && ie_val[5] == 0x01) {
                supports_wmm = true; // WMM Information or Parameter element
            }
        }

        ie_offset += 2 + ie_len;
    }

    // 5 GHz beacons commonly omit the DS Parameter Set element.
    if (channel == 0)
        channel = ht_primary_channel != 0 ? ht_primary_channel : (int)scanChannel;

    if (ssid[0] == '\0') {
        strlcpy(ssid, "<Hidden>", sizeof(ssid));
    }

    for (int i = 0; i < customScanResultsCount; i++) {
        bool match = true;
        for (int j = 0; j < 6; j++) {
            if (customScanResults[i].bssid[j] != bssid[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            clock_get_uptime(&customScanResults[i].last_seen_time);
            if (ssid[0] != '\0' && strcmp(ssid, "<Hidden>") != 0)
                strlcpy(customScanResults[i].ssid, ssid,
                        sizeof(customScanResults[i].ssid));
            customScanResults[i].channel = channel;
            if (signalDbm >= -120 && signalDbm <= 0) {
                int oldSignal = customScanResults[i].rssi;
                customScanResults[i].rssi = oldSignal >= -120 ?
                    (oldSignal * 3 + signalDbm) / 4 : signalDbm;
            }
            customScanResults[i].is_secure = is_secure;
            customScanResults[i].supports_cck = supports_cck;
            customScanResults[i].supports_ht = supports_ht;
            customScanResults[i].supports_vht = supports_vht;
            customScanResults[i].supports_wmm = supports_wmm;
            customScanResults[i].security_mode = security_mode;
            customScanResults[i].rsn_ie_len = rsn_ie_len;
            memcpy(customScanResults[i].rsn_ie, rsn_ie, sizeof(rsn_ie));
            customScanResults[i].ht_cap_info = ht_cap_info;
            customScanResults[i].ht_ampdu_params = ht_ampdu_params;
            customScanResults[i].ht_mcs0 = ht_mcs0;
            customScanResults[i].ht_mcs1 = ht_mcs1;
            customScanResults[i].ht_primary_channel = ht_primary_channel;
            customScanResults[i].ht_operation_info = ht_operation_info;
            customScanResults[i].vht_cap_info = vht_cap_info;
            customScanResults[i].vht_rx_mcs_map = vht_rx_mcs_map;
            customScanResults[i].vht_rx_highest = vht_rx_highest;
            customScanResults[i].vht_tx_mcs_map = vht_tx_mcs_map;
            customScanResults[i].vht_tx_highest = vht_tx_highest;
            customScanResults[i].vht_channel_width = vht_channel_width;
            customScanResults[i].vht_center_segment0 = vht_center_segment0;
            customScanResults[i].vht_center_segment1 = vht_center_segment1;
            return;
        }
    }

    int resultIndex = customScanResultsCount;
    bool replacing = false;
    if (resultIndex >= 32) {
        uint64_t oldest = UINT64_MAX;
        resultIndex = -1;
        for (int i = 0; i < customScanResultsCount; i++) {
            if (connState == CONN_STATE_CONNECTED &&
                memcmp(customScanResults[i].bssid, currentBssid, 6) == 0)
                continue;
            if (customScanResults[i].last_seen_time < oldest) {
                oldest = customScanResults[i].last_seen_time;
                resultIndex = i;
            }
        }
        replacing = resultIndex >= 0;
    }

    if (resultIndex >= 0 && resultIndex < 32) {
        RTL8822CScanResult& res = customScanResults[resultIndex];
        strlcpy(res.ssid, ssid, sizeof(res.ssid));
        memcpy(res.bssid, bssid, 6);
        res.channel = channel;
        res.is_secure = is_secure;
        res.rssi = signalDbm >= -120 && signalDbm <= 0 ? signalDbm : -127;
        res.supports_cck = supports_cck;
        res.supports_ht = supports_ht;
        res.supports_vht = supports_vht;
        res.supports_wmm = supports_wmm;
        res.security_mode = security_mode;
        res.rsn_ie_len = rsn_ie_len;
        memcpy(res.rsn_ie, rsn_ie, sizeof(rsn_ie));
        res.ht_cap_info = ht_cap_info;
        res.ht_ampdu_params = ht_ampdu_params;
        res.ht_mcs0 = ht_mcs0;
        res.ht_mcs1 = ht_mcs1;
        res.ht_primary_channel = ht_primary_channel;
        res.ht_operation_info = ht_operation_info;
        res.vht_cap_info = vht_cap_info;
        res.vht_rx_mcs_map = vht_rx_mcs_map;
        res.vht_rx_highest = vht_rx_highest;
        res.vht_tx_mcs_map = vht_tx_mcs_map;
        res.vht_tx_highest = vht_tx_highest;
        res.vht_channel_width = vht_channel_width;
        res.vht_center_segment0 = vht_center_segment0;
        res.vht_center_segment1 = vht_center_segment1;
        clock_get_uptime(&res.last_seen_time);

        char parseDb[256];
        snprintf(parseDb, sizeof(parseDb), "added: SSID='%s' BSSID=%02x:%02x:%02x:%02x:%02x:%02x ch=%d signal=%d secure=%d ht=%d vht=%d wmm=%d cap=%04x ampdu=%02x mcs=%02x/%02x op=%02x vhtmap=%04x/%04x vhtop=%u/%u/%u count=%d",
                 ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], channel,
                 res.rssi, is_secure,
                 supports_ht ? 1 : 0, supports_vht ? 1 : 0, supports_wmm ? 1 : 0,
                 ht_cap_info, ht_ampdu_params, ht_mcs0, ht_mcs1, ht_operation_info,
                 vht_rx_mcs_map, vht_tx_mcs_map, vht_channel_width,
                 vht_center_segment0, vht_center_segment1,
                 replacing ? customScanResultsCount : customScanResultsCount + 1);
        RTW_DEBUG_PROPERTY("Debug_Scan_Parse", parseDb);

        if (!replacing) customScanResultsCount++;
    } else {
        RTW_DEBUG_PROPERTY("Debug_Scan_Parse", "list full; no replaceable entry");
    }
}

void RealtekRTL8822C::configureWmmEdca(const UInt8* value, UInt32 len) {
    if (!value || len < 24 || value[0] != 0x00 || value[1] != 0x50 ||
        value[2] != 0xf2 || value[3] != 0x02 || value[4] != 0x01 ||
        value[5] != 0x01) return;
    const UInt32 acRegister[4] = { 0x0508, 0x050c, 0x0504, 0x0500 }; // BE/BK/VI/VO
    char db[256];
    // 5 GHz OFDM always uses the 9-us slot, regardless of the Short Slot bit
    // in the Association Response capability field (that bit is meaningful
    // for 2.4 GHz ERP). Linux receives this as bss_conf.use_short_slot=true.
    bool shortSlot = targetChannel > 14 || useShortSlot;
    UInt8 slotTime = shortSlot ? 9 : 20;
    UInt8 sifs = targetChannel > 14 ? 16 : 10;
    int pos = snprintf(db, sizeof(db), "qos_info=%02x band=%s slot=%u sifs=%u",
                       value[6], targetChannel > 14 ? "5g" : "2g",
                       slotTime, sifs);
    for (UInt32 i = 0; i < 4; i++) {
        const UInt8* ac = value + 8 + i * 4;
        UInt8 aci = (ac[0] >> 5) & 0x03;
        UInt8 aifsn = ac[0] & 0x0f;
        UInt8 ecwMin = ac[1] & 0x0f;
        UInt8 ecwMax = (ac[1] >> 4) & 0x0f;
        UInt16 txop = (UInt16)((UInt16)ac[2] | ((UInt16)ac[3] << 8));
        // Exact Linux rtw_aifsn_to_aifs(): AIFS = SIFS + AIFSN * slot.
        UInt8 aifs = (UInt8)(sifs + aifsn * slotTime);
        UInt32 param = ((UInt32)txop << 16) | ((UInt32)ecwMax << 12) |
                       ((UInt32)ecwMin << 8) | aifs;
        write32(acRegister[aci], param);
        if (pos > 0 && pos < (int)sizeof(db) - 32)
            pos += snprintf(db + pos, sizeof(db) - (size_t)pos,
                            " ac%u=%08x", aci, (unsigned int)param);
    }
    RTW_DEBUG_PROPERTY("Debug_WMM_EDCA", db);
}

void RealtekRTL8822C::handleMgmtFrame(const UInt8* frame, UInt32 len) {
    if (len < 24) return;

    UInt8 fc0 = frame[0];
    UInt8 subtype = (fc0 >> 4) & 0x0F;
    const UInt8* src_mac = frame + 10;

    if (subtype == 11) {
        debugAuthRxCount++;
        char auth_info[256];
        const UInt8* da = frame + 4;
        const UInt8* sa = frame + 10;
        const UInt8* bssid = frame + 16;
        snprintf(auth_info, sizeof(auth_info),
                 "RxAuth: DA=%02x:%02x:%02x:%02x:%02x:%02x SA=%02x:%02x:%02x:%02x:%02x:%02x BSSID=%02x:%02x:%02x:%02x:%02x:%02x len=%u state=%d",
                 da[0], da[1], da[2], da[3], da[4], da[5],
                 sa[0], sa[1], sa[2], sa[3], sa[4], sa[5],
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                 (unsigned int)len, (int)connState);
        RTW_DEBUG_PROPERTY("Debug_Auth_Rx_Detail", auth_info);
        RTW_DEBUG_LOG("RealtekRTL8822C: %s\n", auth_info);
    }

    bool from_target_ap = true;
    for (int i = 0; i < 6; i++) {
        if (targetBssid[i] != src_mac[i]) {
            from_target_ap = false;
            break;
        }
    }
    if (!from_target_ap) return;
    clock_get_uptime(&lastPeerRxTime);

    if ((subtype == 10 || subtype == 12) && len >= 26 &&
        connState != CONN_STATE_DISCONNECTED) {
        UInt16 reason = (UInt16)((UInt16)frame[24] | ((UInt16)frame[25] << 8));
        bool fourWayInProgress = targetSecurityMode == SECURITY_WPA2 &&
                                 !portAuthorized &&
                                 (wpaState == WPA_STATE_WAIT_M1 ||
                                  wpaState == WPA_STATE_WAIT_M3);
        char peerLeave[96];
        snprintf(peerLeave, sizeof(peerLeave), "subtype=%s reason=%u",
                 subtype == 12 ? "deauth" : "disassoc", reason);
        RTW_DEBUG_PROPERTY("Debug_Peer_Disconnect", peerLeave);
        traceEvent(subtype == 12 ? "link:peer-deauth" : "link:peer-disassoc");
        disconnectFromNetwork(subtype == 12 ? "peer-deauth" : "peer-disassoc", false);
        if (fourWayInProgress && reason == 15) {
            setProperty("WiFiStatus", "WPAHandshakeRejected");
            setProperty("DriverStatus", "AP ended the WPA2 four-way handshake (reason 15); verify the password and retry connect");
            traceEvent("wpa:peer-four-way-timeout");
        } else {
            setProperty("WiFiStatus", subtype == 12 ? "Deauthenticated" : "Disassociated");
        }
        return;
    }

    // Block Ack negotiation telemetry. Keep aggregation disabled until a
    // successful ADDBA response is observed and validated.
    if (subtype == 13 && len >= 32 && frame[24] == 3) {
        UInt8 action = frame[25];
        if (action == 0 && len >= 33) { // AP asks us to accept RX aggregation
            UInt16 param = (UInt16)((UInt16)frame[27] | ((UInt16)frame[28] << 8));
            UInt8 rxTid = (param >> 2) & 0x0f;
            UInt16 rxBufferSize = (param >> 6) & 0x03ff;
            UInt16 rxTimeout = (UInt16)((UInt16)frame[29] | ((UInt16)frame[30] << 8));
            UInt16 startSeq = (UInt16)(((UInt16)frame[31] | ((UInt16)frame[32] << 8)) >> 4);
            UInt16 negotiatedWindow = rxBufferSize ? rxBufferSize : RX_REORDER_MAX_WINDOW;
            if (negotiatedWindow > RX_REORDER_MAX_WINDOW) negotiatedWindow = RX_REORDER_MAX_WINDOW;
            bool retry = rxTid < RX_BA_TID_COUNT && rxBa[rxTid].active &&
                         rxBa[rxTid].dialogToken == frame[26] &&
                         rxBa[rxTid].windowSize == negotiatedWindow &&
                         rxBa[rxTid].timeoutTu == rxTimeout;
            bool accepted = startRxBaSession(rxTid, frame[26], startSeq,
                                             negotiatedWindow, rxTimeout);
            bool responded = sendAddBaResponse(frame[26], rxTid, accepted ? 0 : 37,
                                               negotiatedWindow, rxTimeout, retry);
            if (!responded && accepted) stopRxBaSession(rxTid, false);
            RTW_DEBUG_LOG("RealtekRTL8822C: RX ADDBA token=%u tid=%u start=%u buf=%u timeout=%u accepted=%d response=%d\n",
                  frame[26], rxTid, startSeq, negotiatedWindow, rxTimeout,
                  accepted ? 1 : 0, responded ? 1 : 0);
        } else if (action == 1 && len >= 33) { // response to our TX ADDBA
            baResponses++;
            UInt8 token = frame[26];
            UInt16 status = (UInt16)((UInt16)frame[27] | ((UInt16)frame[28] << 8));
            UInt16 param = (UInt16)((UInt16)frame[29] | ((UInt16)frame[30] << 8));
            UInt8 responseTid = (param >> 2) & 0x0f;
            UInt16 responseBufferSize = (param >> 6) & 0x03ff;
            baEstablished = status == 0 && token == baDialogToken && responseTid == baTid;
            if (baEstablished) {
                baBufferSize = responseBufferSize;
                baAttempts = 0;
                traceEvent("ba:tx-operational");
                char linkCapability[448];
                snprintf(linkCapability, sizeof(linkCapability),
                         "ap_ht=%d ap_vht=%d ap_wmm=%d ht_cap=%04x ampdu_param=%02x mcs=%02x/%02x ht_primary=%u ht_op=%02x vht_cap=%08x vht_rxmap=%04x vht_txmap=%04x vht_op=%u/%u/%u assoc=%s center=%d bw=%u ra_mask=%08x rate_id=%u ampdu=1 tid=%u buf=%u",
                         targetSupportsHt ? 1 : 0, targetSupportsVht ? 1 : 0,
                         targetSupportsWmm ? 1 : 0, targetHtCapInfo,
                         targetHtAmpduParams, targetHtMcs0, targetHtMcs1,
                         targetHtPrimaryChannel, targetHtOperationInfo,
                         (unsigned int)targetVhtCapInfo, targetVhtRxMcsMap,
                         targetVhtTxMcsMap, targetVhtChannelWidth,
                         targetVhtCenterSegment0, targetVhtCenterSegment1,
                         vhtNegotiated ? (targetBandwidth == 2 ? "vht80" :
                                          (targetBandwidth == 1 ? "vht40" : "vht20")) :
                         (htNegotiated ? (targetBandwidth == 1 ? "ht40" : "ht20") :
                          (targetChannel > 14 ? "legacy-g" : "legacy-bg")),
                         targetCenterChannel,
                         targetBandwidth == 2 ? 80U : (targetBandwidth == 1 ? 40U : 20U),
                         (unsigned int)negotiatedRaMask, negotiatedRateId, baTid, baBufferSize);
                RTW_DEBUG_PROPERTY("Debug_Link_Capability", linkCapability);
            }
            RTW_DEBUG_LOG("RealtekRTL8822C: ADDBA Response token=%u status=%u tid=%u buf=%u established=%d\n",
                  token, status, responseTid, responseBufferSize, baEstablished ? 1 : 0);
        } else if (action == 2 && len >= 30) { // DELBA
            UInt16 param = (UInt16)((UInt16)frame[26] | ((UInt16)frame[27] << 8));
            UInt8 tid = (param >> 12) & 0x0f;
            bool peerIsOriginator = (param & (1U << 11)) != 0;
            if (peerIsOriginator) {
                stopRxBaSession(tid, true);
                publishRxBaState();
                traceEvent("ba:peer-delba-rx");
            } else if (tid == baTid) {
                baEstablished = false;
                baAttempts = 0;
                traceEvent("ba:peer-delba-tx");
            }
        }
        char baDb[192];
        snprintf(baDb, sizeof(baDb), "established=%d requests=%u responses=%u attempts=%u token=%u tid=%u buf=%u timeout=%u agg=%d",
                 baEstablished ? 1 : 0, (unsigned int)baRequests, (unsigned int)baResponses,
                 (unsigned int)baAttempts, baDialogToken, baTid, baBufferSize, baTimeout,
                 baEstablished ? 1 : 0);
        RTW_DEBUG_PROPERTY("Debug_BA_State", baDb);
    }

    if (subtype == 11 && connState == CONN_STATE_CONNECTING_AUTH) {
        if (len < 30) return;
        UInt16 seq = (UInt16)((UInt16)frame[26] | ((UInt16)frame[27] << 8));
        UInt16 status = (UInt16)((UInt16)frame[28] | ((UInt16)frame[29] << 8));

        RTW_DEBUG_LOG("RealtekRTL8822C: Received Auth Response, seq=%d status=%d\n", seq, status);
        if (seq == 2 && status == 0) {
            if (!sendAssocRequest()) {
                RTW_ERROR_LOG("RealtekRTL8822C: Failed to send Assoc Request!\n");
                disconnectFromNetwork("assoc-queue-failed", false);
                setProperty("WiFiStatus", "AssocFailed");
            }
        } else {
            RTW_ERROR_LOG("RealtekRTL8822C: Auth Failed! status=%d\n", status);
            disconnectFromNetwork("auth-rejected", false);
            setProperty("WiFiStatus", "AuthFailed");
        }
    } else if (subtype == 1 && connState == CONN_STATE_CONNECTING_ASSOC) {
        if (len < 30) return;
        UInt16 status = (UInt16)((UInt16)frame[26] | ((UInt16)frame[27] << 8));

        RTW_DEBUG_LOG("RealtekRTL8822C: Received Assoc Response, status=%d\n", status);
        if (status == 0) {
            UInt16 capability = (UInt16)((UInt16)frame[24] | ((UInt16)frame[25] << 8));
            useShortSlot = targetChannel > 14 ||
                           (capability & (1U << 10)) != 0;
            bool responseHt = false;
            bool responseVht = false;
            bool responseWmm = false;
            UInt16 responseHtCap = 0;
            UInt8 responseAmpdu = 0;
            UInt8 responseMcs0 = 0;
            UInt8 responseMcs1 = 0;
            UInt8 responseHtPrimary = 0;
            UInt8 responseHtOperation = 0;
            UInt32 responseVhtCap = 0;
            UInt16 responseVhtRxMcsMap = 0xffff;
            UInt16 responseVhtRxHighest = 0;
            UInt16 responseVhtTxMcsMap = 0xffff;
            UInt16 responseVhtTxHighest = 0;
            UInt8 responseVhtWidth = 0;
            UInt8 responseVhtCenter0 = 0;
            UInt8 responseVhtCenter1 = 0;
            UInt32 ieOffset = 30;
            while (ieOffset + 2 <= len) {
                UInt8 id = frame[ieOffset];
                UInt8 ieLen = frame[ieOffset + 1];
                if (ieOffset + 2 + ieLen > len) break;
                if (id == 45 && ieLen >= 19) {
                    const UInt8* value = frame + ieOffset + 2;
                    responseHt = true;
                    responseHtCap = (UInt16)((UInt16)value[0] | ((UInt16)value[1] << 8));
                    responseAmpdu = value[2];
                    responseMcs0 = value[3];
                    responseMcs1 = value[4];
                } else if (id == 61 && ieLen >= 2) {
                    const UInt8* value = frame + ieOffset + 2;
                    responseHtPrimary = value[0];
                    responseHtOperation = value[1];
                } else if (id == 191 && ieLen >= 12) {
                    const UInt8* value = frame + ieOffset + 2;
                    responseVht = true;
                    responseVhtCap = (UInt32)value[0] | ((UInt32)value[1] << 8) |
                                     ((UInt32)value[2] << 16) | ((UInt32)value[3] << 24);
                    responseVhtRxMcsMap = (UInt16)((UInt16)value[4] | ((UInt16)value[5] << 8));
                    responseVhtRxHighest = (UInt16)((UInt16)value[6] | ((UInt16)value[7] << 8));
                    responseVhtTxMcsMap = (UInt16)((UInt16)value[8] | ((UInt16)value[9] << 8));
                    responseVhtTxHighest = (UInt16)((UInt16)value[10] | ((UInt16)value[11] << 8));
                } else if (id == 192 && ieLen >= 5) {
                    const UInt8* value = frame + ieOffset + 2;
                    responseVhtWidth = value[0];
                    responseVhtCenter0 = value[1];
                    responseVhtCenter1 = value[2];
                } else if (id == 221 && ieLen >= 7) {
                    const UInt8* value = frame + ieOffset + 2;
                    if (value[0] == 0x00 && value[1] == 0x50 &&
                        value[2] == 0xf2 && value[3] == 0x02 &&
                        (value[4] == 0x00 || value[4] == 0x01) &&
                        value[5] == 0x01) {
                        responseWmm = true;
                        if (value[4] == 0x01) configureWmmEdca(value, ieLen);
                    }
                }
                ieOffset += 2 + ieLen;
            }

            bool bandwidthResponseOk = true;
            if (targetBandwidth == 2) {
                bandwidthResponseOk = responseVht && responseVhtWidth == 1 &&
                                      responseVhtCenter0 == targetCenterChannel;
            } else if (targetBandwidth == 1) {
                UInt8 secondary = responseHtOperation & 0x03;
                int responseCenter = responseHtPrimary;
                if (secondary == 1) responseCenter += 2;
                else if (secondary == 3) responseCenter -= 2;
                else bandwidthResponseOk = false;
                bandwidthResponseOk = bandwidthResponseOk &&
                                      responseCenter == targetCenterChannel;
            }
            if (!bandwidthResponseOk) {
                traceEvent("assoc:bandwidth-mismatch");
                disconnectFromNetwork("assoc-bandwidth-mismatch", false);
                setProperty("WiFiStatus", "BandwidthMismatch");
                setProperty("DriverStatus", "AP Association Response changed the negotiated channel width/center");
                return;
            }
            if (responseHtPrimary != 0) {
                targetHtPrimaryChannel = responseHtPrimary;
                targetHtOperationInfo = responseHtOperation;
            }
            if (responseVht) {
                targetVhtChannelWidth = responseVhtWidth;
                targetVhtCenterSegment0 = responseVhtCenter0;
                targetVhtCenterSegment1 = responseVhtCenter1;
            }

            if (htNegotiated && (!responseHt || !responseWmm)) {
                htNegotiated = false;
                vhtNegotiated = false;
                negotiatedRaMask = targetChannel > 14 ? 0x00000ff0 : 0x00000fff;
                negotiatedRateId = targetChannel > 14 ? 7 : 6;
                traceEvent("assoc:peer-downgraded-legacy");
            } else if (htNegotiated) {
                targetHtCapInfo = responseHtCap;
                targetHtAmpduParams = responseAmpdu;
                targetHtMcs0 &= responseMcs0;
                targetHtMcs1 &= responseMcs1;
                if (targetHtMcs0 == 0) {
                    htNegotiated = false;
                    negotiatedRaMask = targetChannel > 14 ? 0x00000ff0 : 0x00000fff;
                    negotiatedRateId = targetChannel > 14 ? 7 : 6;
                } else {
                    negotiatedRaMask = (targetChannel > 14 ? 0x00000030U : 0x00000015U) |
                                       ((UInt32)targetHtMcs0 << 12);
                }
                if (htNegotiated && targetHtMcs1 != 0) {
                    negotiatedRaMask |= (UInt32)targetHtMcs1 << 20;
                    negotiatedRateId = targetChannel > 14 ? 4 : 2;
                } else if (htNegotiated) {
                    negotiatedRateId = targetChannel > 14 ? 5 : 3;
                }
                if (!htNegotiated) vhtNegotiated = false;
                if (vhtNegotiated && !responseVht) {
                    vhtNegotiated = false;
                    traceEvent("assoc:peer-downgraded-ht");
                } else if (vhtNegotiated) {
                    const UInt16 localVhtMcsMap = fiveGTwoStreamPowerValid ? 0xfffa : 0xfffe;
                    UInt32 vhtMask = 0;
                    for (UInt8 stream = 0; stream < 2; stream++) {
                        UInt8 peerCode = (UInt8)((responseVhtRxMcsMap >> (stream * 2)) & 0x03);
                        UInt8 localCode = (UInt8)((localVhtMcsMap >> (stream * 2)) & 0x03);
                        UInt8 code = (peerCode == 3 || localCode == 3) ? 3 :
                                     (peerCode < localCode ? peerCode : localCode);
                        UInt32 rates = code == 0 ? 0x0ffU :
                                       (code == 1 ? 0x1ffU :
                                       (code == 2 ? (targetBandwidth == 0 ? 0x1ffU : 0x3ffU) : 0));
                        vhtMask |= rates << (12 + stream * 10);
                    }
                    if ((vhtMask & 0x003ff000U) == 0) {
                        vhtNegotiated = false;
                        traceEvent("assoc:peer-vht-map-empty");
                    } else {
                        targetVhtCapInfo = responseVhtCap;
                        targetVhtRxMcsMap = responseVhtRxMcsMap;
                        targetVhtRxHighest = responseVhtRxHighest;
                        targetVhtTxMcsMap = responseVhtTxMcsMap;
                        targetVhtTxHighest = responseVhtTxHighest;
                        negotiatedRaMask = 0x00000010U | vhtMask;
                        negotiatedRateId = (vhtMask & 0xffc00000U) != 0 ? 9 : 10;
                    }
                }
            }
            char assocResponse[512];
            int assocPos = snprintf(assocResponse, sizeof(assocResponse),
                                    "status=0 aid=%u ht=%d vht=%d wmm=%d cap=%04x ampdu=%02x mcs=%02x/%02x htop=%u/%02x vhtcap=%08x vhtmap=%04x/%04x vhtop=%u/%u/%u negotiated=%s ies=",
                                    (unsigned int)((frame[28] | ((UInt16)frame[29] << 8)) & 0x3fff),
                                    responseHt ? 1 : 0, responseVht ? 1 : 0,
                                    responseWmm ? 1 : 0,
                                    responseHtCap, responseAmpdu,
                                    responseMcs0, responseMcs1,
                                    responseHtPrimary, responseHtOperation,
                                    (unsigned int)responseVhtCap,
                                    responseVhtRxMcsMap, responseVhtTxMcsMap,
                                    responseVhtWidth, responseVhtCenter0,
                                    responseVhtCenter1,
                                    vhtNegotiated ? (targetBandwidth == 2 ? "vht80" :
                                                     (targetBandwidth == 1 ? "vht40" : "vht20")) :
                                    (htNegotiated ? (targetBandwidth == 1 ? "ht40" : "ht20") :
                                     (targetChannel > 14 ? "legacy-g" : "legacy-bg")));
            for (UInt32 i = 30; i < len && assocPos > 0 &&
                 assocPos < (int)sizeof(assocResponse) - 3; i++) {
                assocPos += snprintf(assocResponse + assocPos,
                                     sizeof(assocResponse) - (size_t)assocPos,
                                     "%02x", frame[i]);
            }
            RTW_DEBUG_PROPERTY("Debug_Assoc_Response", assocResponse);

            // 1. Set AID and Link type (both net_type and AID register set)
            UInt16 aid = (UInt16)((UInt16)frame[28] | ((UInt16)frame[29] << 8));
            write32Mask(0x06A8, 0x7FF, aid & 0x7FF);
            write32Mask(0x0100, 0x30000, 2); // RTW_NET_MGD_LINKED = 2

            // 2. Match Linux station bring-up ordering: install the RA state
            // before reporting the MACID as associated to firmware.
            bool raSent = sendRaInfo(0, negotiatedRaMask, negotiatedRateId);
            bool mediaSent = raSent && sendMediaStatus(0, true);
            if (!raSent || !mediaSent) {
                traceEvent("assoc:firmware-link-state-failed");
                disconnectFromNetwork("firmware-link-state-failed", false);
                setProperty("WiFiStatus", "FirmwareLinkFailed");
                return;
            }
            char linkCapability[384];
            snprintf(linkCapability, sizeof(linkCapability),
                     "ap_ht=%d ap_vht=%d ap_wmm=%d ht_cap=%04x ampdu_param=%02x mcs=%02x/%02x ht_primary=%u ht_op=%02x vht_cap=%08x vht_rxmap=%04x vht_txmap=%04x vht_op=%u/%u/%u assoc=%s center=%d bw=%u primary_idx=%u ra_mask=%08x rate_id=%u ampdu=0",
                     targetSupportsHt ? 1 : 0, targetSupportsVht ? 1 : 0,
                     targetSupportsWmm ? 1 : 0,
                     targetHtCapInfo, targetHtAmpduParams, targetHtMcs0,
                     targetHtMcs1, targetHtPrimaryChannel, targetHtOperationInfo,
                     (unsigned int)targetVhtCapInfo, targetVhtRxMcsMap,
                     targetVhtTxMcsMap, targetVhtChannelWidth,
                     targetVhtCenterSegment0, targetVhtCenterSegment1,
                     vhtNegotiated ? (targetBandwidth == 2 ? "vht80" :
                                      (targetBandwidth == 1 ? "vht40" : "vht20")) :
                     (htNegotiated ? (targetBandwidth == 1 ? "ht40" : "ht20") :
                      (targetChannel > 14 ? "legacy-g" : "legacy-bg")),
                     targetCenterChannel,
                     targetBandwidth == 2 ? 80U : (targetBandwidth == 1 ? 40U : 20U),
                     targetPrimaryChannelIndex,
                     (unsigned int)negotiatedRaMask,
                     negotiatedRateId);
            RTW_DEBUG_PROPERTY("Debug_Link_Capability", linkCapability);

            connState = CONN_STATE_CONNECTED;
            memcpy(currentBssid, targetBssid, 6);
            portAuthorized = targetSecurityMode == SECURITY_OPEN;
            if (targetSecurityMode == SECURITY_WPA2) {
                wpaState = WPA_STATE_WAIT_M1;
                wpaRetries = 0;
                clock_get_uptime(&wpaStateStartTime);
            }
            clock_get_uptime(&lastPeerRxTime);
            UInt32 linkedRcr = read32(0x0608);
            linkedRcr &= ~1U;
            linkedRcr |= (1U << 1) | (1U << 2) | (1U << 3);
            linkedRcr |= (1U << 6) | (1U << 7);
            write32(0x0608, linkedRcr);
            OSSynchronizeIO();
            setProperty("WiFiStatus", portAuthorized ? "Connected" : "WPAHandshake");
            if (portAuthorized) setProperty("ConnectedSSID", targetSsid);
            if (portAuthorized)
                RTW_DEBUG_PROPERTY("Debug_Security_State", "mode=open port=authorized pairwise=none group=none cam=empty");
            else
                publishWpaState("association-complete");
            if (portAuthorized)
                activateNetworkDataPath("open-association-complete");
            else
                setLinkStatus(kIONetworkLinkValid);
            RTW_DEBUG_LOG("RealtekRTL8822C: Association Success! %s %s\n",
                  portAuthorized ? "Connected to" : "Starting WPA handshake with", targetSsid);
            if (htNegotiated && portAuthorized) {
                sendAddBaRequest();
            }
        } else {
            RTW_ERROR_LOG("RealtekRTL8822C: Association Failed! status=%d\n", status);
            char assocResponse[96];
            snprintf(assocResponse, sizeof(assocResponse), "status=%u negotiated=none", status);
            RTW_DEBUG_PROPERTY("Debug_Assoc_Response", assocResponse);
            disconnectFromNetwork("assoc-rejected", false);
            setProperty("WiFiStatus", "AssocFailed");
        }
    }
}

void RealtekRTL8822C::disconnectFromNetwork(const char* reason, bool sendDeauth) {
    if (scanInProgress)
        finishConnectedScan(true, reason ? reason : "disconnect", true);
    bool wasConnected = connState == CONN_STATE_CONNECTED;
    traceEvent(reason && strcmp(reason, "reconnect") == 0 ? "disconnect:reconnect" : "disconnect:user");

    IOOutputQueue* queue = getOutputQueue();
    if (queue) queue->stop();
    portAuthorized = false;
    resetRxBaSessions(false);

    // Quiesce ordinary data before management teardown. If the ring cannot
    // drain, use the already validated BEQ-only pointer recovery rather than
    // carrying stale descriptors into the next association.
    UInt32 disconnectOutstanding = 0;
    for (int i = 0; i < 1000; i++) {
        updateBeqCompletion();
        if (beqLock) {
            IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
            disconnectOutstanding = beqOutstanding;
            IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
        }
        if (disconnectOutstanding == 0) break;
        IODelay(100);
    }
    if ((read32(0x0210) & (1U << 13)) || disconnectOutstanding != 0) {
        recoverBeqPayloadOverflow();
    }

    if (wasConnected && baEstablished) sendDelBa();

    if (sendDeauth && wasConnected) {
        if (!waitForMgmtQueueEmpty(20000)) resetMgmtQueue("deauth-preflight-timeout");
        UInt8 frame[26];
        memset(frame, 0, sizeof(frame));
        frame[0] = 0xc0; // Deauthentication
        memcpy(frame + 4, currentBssid, 6);
        memcpy(frame + 10, macAddress, 6);
        memcpy(frame + 16, currentBssid, 6);
        frame[24] = 0x03; // STA is leaving
        frame[25] = 0x00;
        bool queued = transmitWifiFrame(frame, sizeof(frame), 18);
        if (queued && !waitForMgmtQueueEmpty(100000)) {
            resetMgmtQueue("deauth-completion-timeout");
        }
    }

    if (queue) {
        queue->flush();
    }

    connState = CONN_STATE_DISCONNECTED;
    bool mediaCleared = true;
    if (wasConnected) {
        mediaCleared = sendMediaStatus(0, false);
        if (!mediaCleared) {
            IODelay(1000);
            mediaCleared = sendMediaStatus(0, false);
        }
    }
    write32Mask(0x0100, 0x30000, 0);
    write32Mask(0x06A8, 0x7ff, 0);
    write32_clr(0x0608, (1U << 6) | (1U << 7));
    for (UInt32 i = 0; i < 6; i++) write8(0x0618U + i, 0);
    clearAllSecurityCam(reason ? reason : "disconnect");

    if (!waitForMgmtQueueEmpty(20000)) resetMgmtQueue("disconnect-final-timeout");

    if (beqLock) {
        IOInterruptState interruptState = IOSimpleLockLockDisableInterrupt(beqLock);
        updateBeqCompletionLocked();
        beqOutstanding = 0;
        beqWp = beqRp;
        beqLastHwRp = beqRp;
        beqQueueStalled = false;
        write16(0x03A8, (UInt16)beqWp);
        UInt32 txdmaStatus = read32(0x0210);
        if (txdmaStatus) write32(0x0210, txdmaStatus);
        OSSynchronizeIO();
        IOSimpleLockUnlockEnableInterrupt(beqLock, interruptState);
    }

    memset(currentBssid, 0, sizeof(currentBssid));
    memset(currentSsid, 0, sizeof(currentSsid));
    currentSsidLen = 0;
    memset(targetBssid, 0, sizeof(targetBssid));
    memset(targetSsid, 0, sizeof(targetSsid));
    targetChannel = 1;
    targetCenterChannel = 1;
    targetBandwidth = 0;
    targetPrimaryChannelIndex = 0;
    targetSupportsHt = false;
    targetSupportsVht = false;
    targetSupportsWmm = false;
    targetSecurityMode = SECURITY_OPEN;
    targetRsnIeLen = 0;
    memset(targetRsnIe, 0, sizeof(targetRsnIe));
    assocRsnIeLen = 0;
    memset(assocRsnIe, 0, sizeof(assocRsnIe));
    resetWpaState(true);
    targetHtCapInfo = 0;
    targetHtAmpduParams = 0;
    targetHtMcs0 = targetHtMcs1 = 0;
    targetHtPrimaryChannel = targetHtOperationInfo = 0;
    targetVhtCapInfo = 0;
    targetVhtRxMcsMap = targetVhtTxMcsMap = 0xffff;
    targetVhtRxHighest = targetVhtTxHighest = 0;
    targetVhtChannelWidth = 0;
    targetVhtCenterSegment0 = targetVhtCenterSegment1 = 0;
    htNegotiated = false;
    vhtNegotiated = false;
    negotiatedRaMask = 0x00000fff;
    negotiatedRateId = 6;
    authAttempts = 0;
    assocAttempts = 0;
    connStateStartTime = 0;
    lastPeerRxTime = 0;
    connectedSignalDbm = -127;
    publishedSignalDbm = -127;
    lastSignalPublishTime = 0;
    removeProperty("SignalStrength");
    debugMgmtShadowValid = false;
    debugDataReportPending = false;
    resetBaState();
    removeProperty("ConnectedSSID");
    setProperty("WiFiStatus", "Idle");
    RTW_DEBUG_PROPERTY("Debug_Security_State", "mode=none port=closed pairwise=none group=none cam=empty");
    setLinkStatus(kIONetworkLinkValid);

    char lifecycle[192];
    snprintf(lifecycle, sizeof(lifecycle), "reason=%s deauth=%d media_clear=%d mgmt=%08x beq=%08x rcr=%08x state=idle",
             reason ? reason : "unknown", sendDeauth ? 1 : 0, mediaCleared ? 1 : 0,
             (unsigned int)read32(0x03b0), (unsigned int)read32(0x03a8),
             (unsigned int)read32(0x0608));
    RTW_DEBUG_PROPERTY("Debug_Connection_Lifecycle", lifecycle);
}

bool RealtekRTL8822C::sendMediaStatus(UInt8 mac_id, bool connect) {
    UInt8 h2c[8];
    memset(h2c, 0, 8);
    UInt32 w0 = 0x01; // H2C_CMD_MEDIA_STATUS_RPT
    if (connect) {
        w0 |= (1U << 8);
    }
    w0 |= (UInt32)mac_id << 16;
    *(UInt32*)h2c = w0;
    *(UInt32*)(h2c + 4) = 0;

    RTW_DEBUG_LOG("RealtekRTL8822C: sendMediaStatus: connect=%d macid=%u w0=0x%08X\n", connect, mac_id, (unsigned int)w0);
    bool sent = sendH2CCommand(h2c);
    char mediaDb[96];
    snprintf(mediaDb, sizeof(mediaDb), "sent=%d connect=%d macid=%u w0=%08x",
             sent ? 1 : 0, connect ? 1 : 0, mac_id, (unsigned int)w0);
    RTW_DEBUG_PROPERTY("Debug_Media_State", mediaDb);
    return sent;
}

bool RealtekRTL8822C::sendRaInfo(UInt8 mac_id, UInt32 rate_mask, UInt8 rate_id) {
    UInt8 h2c[8];
    UInt32 w0 = 0x40; // H2C_CMD_RA_INFO
    w0 |= (UInt32)mac_id << 8;
    w0 |= ((UInt32)rate_id & 0x1FU) << 16;
    w0 |= (1U << 21); // init_ra_lvl = 1
    if (htNegotiated && targetBandwidth == 0 &&
        (targetHtCapInfo & (1U << 5))) {
        w0 |= (1U << 23); // SGI20
    } else if (htNegotiated && targetBandwidth == 1 &&
               (targetHtCapInfo & (1U << 6))) {
        w0 |= (1U << 23); // SGI40
    } else if (vhtNegotiated && targetBandwidth == 2 &&
               (targetVhtCapInfo & (1U << 5))) {
        w0 |= (1U << 23); // VHT SGI80
    }
    w0 |= ((UInt32)targetBandwidth & 0x03U) << 24;
    if (vhtNegotiated) w0 |= (1U << 28); // Linux SET_RA_INFO_VHT_EN=1
    w0 |= (1U << 30); // dis_pt = 1
    UInt32 w1 = rate_mask;
    *(UInt32*)h2c = w0;
    *(UInt32*)(h2c + 4) = w1;

    RTW_DEBUG_LOG("RealtekRTL8822C: sendRaInfo: macid=%u mask=0x%08X rate_id=%u w0=0x%08X\n", mac_id, (unsigned int)rate_mask, rate_id, (unsigned int)w0);
    bool sent = sendH2CCommand(h2c);
    char raDb[192];
    snprintf(raDb, sizeof(raDb), "sent=%d macid=%u mask=%08x rate_id=%u sgi=%d vht=%d bw=%u w0=%08x",
             sent ? 1 : 0, mac_id, (unsigned int)rate_mask, rate_id,
             (w0 & (1U << 23)) ? 1 : 0, vhtNegotiated ? 1 : 0,
             targetBandwidth == 2 ? 80U : (targetBandwidth == 1 ? 40U : 20U),
             (unsigned int)w0);
    RTW_DEBUG_PROPERTY("Debug_RA_State", raDb);
    return sent;
}

bool RealtekRTL8822C::sendAuthRequest() {
    clock_get_uptime(&connStateStartTime);
    authAttempts++;
    if (!waitForMgmtQueueEmpty(20000)) resetMgmtQueue("auth-preflight-timeout");
    RTW_DEBUG_PROPERTY("Debug_TXDMA_Status_PreAuth", (uint64_t)read32(0x0210), 32);
    write32(0x0210, (1U << 2)); // W1C BTI_PAGE_OVF immediately before MGMT TX
    OSSynchronizeIO();
    RTW_DEBUG_PROPERTY("Debug_TXDMA_Status_AfterClear", (uint64_t)read32(0x0210), 32);
    UInt8 frame[30];
    memset(frame, 0, sizeof(frame));

    frame[0] = 0xB0;
    frame[1] = 0x00;
    memcpy(frame + 4, targetBssid, 6);
    memcpy(frame + 10, macAddress, 6);
    memcpy(frame + 16, targetBssid, 6);

    frame[24] = 0x00; frame[25] = 0x00;
    frame[26] = 0x01; frame[27] = 0x00;
    frame[28] = 0x00; frame[29] = 0x00;

    RTW_DEBUG_LOG("RealtekRTL8822C: Sending Auth Request to AP %02x:%02x:%02x:%02x:%02x:%02x\n",
          targetBssid[0], targetBssid[1], targetBssid[2], targetBssid[3], targetBssid[4], targetBssid[5]);

    // Set BSSID target register before Auth
    for (UInt32 i = 0; i < 6; i++) {
        write8(0x0618U + i, targetBssid[i]);
    }
    // Set NET_TYPE to RTW_NET_NO_LINK (0) before Association
    write32Mask(0x0100, 0x30000, 0);

    connState = CONN_STATE_CONNECTING_AUTH;
    bool queued = transmitWifiFrame(frame, 30, 18); // Use QSEL=18 (MGMTQ)
    traceEvent(queued ? "auth:tx-queued" : "auth:tx-queue-failed");
    return queued;
}

bool RealtekRTL8822C::sendAssocRequest() {
    clock_get_uptime(&connStateStartTime);
    assocAttempts++;
    if (!waitForMgmtQueueEmpty(20000)) resetMgmtQueue("assoc-preflight-timeout");
    UInt8 frame[256];
    memset(frame, 0, sizeof(frame));

    frame[0] = 0x00;
    frame[1] = 0x00;
    memcpy(frame + 4, targetBssid, 6);
    memcpy(frame + 10, macAddress, 6);
    memcpy(frame + 16, targetBssid, 6);

    UInt16 associationCapability = 0x0401; // ESS + short slot time
    if (targetSecurityMode != SECURITY_OPEN) associationCapability |= 1U << 4; // Privacy
    frame[24] = (UInt8)associationCapability;
    frame[25] = (UInt8)(associationCapability >> 8);
    frame[26] = 0x03; frame[27] = 0x00;

    UInt32 offset = 28;
    frame[offset] = 0;
    size_t ssidLength = strnlen(targetSsid, 32);
    UInt8 ssid_len = (UInt8)ssidLength;
    frame[offset + 1] = ssid_len;
    memcpy(frame + offset + 2, targetSsid, ssid_len);
    offset += 2 + ssid_len;

    if (targetSupportsCck) {
        frame[offset] = 1;
        frame[offset + 1] = 8;
        frame[offset + 2] = 0x82;
        frame[offset + 3] = 0x84;
        frame[offset + 4] = 0x8B;
        frame[offset + 5] = 0x96;
        frame[offset + 6] = 0x0C;
        frame[offset + 7] = 0x12;
        frame[offset + 8] = 0x18;
        frame[offset + 9] = 0x24;
        offset += 10;

        frame[offset] = 50;
        frame[offset + 1] = 4;
        frame[offset + 2] = 0x30;
        frame[offset + 3] = 0x48;
        frame[offset + 4] = 0x60;
        frame[offset + 5] = 0x6C;
        offset += 6;
    } else {
        frame[offset] = 1;
        frame[offset + 1] = 8;
        frame[offset + 2] = 0x8C;
        frame[offset + 3] = 0x12;
        frame[offset + 4] = 0x98;
        frame[offset + 5] = 0x24;
        frame[offset + 6] = 0xB0;
        frame[offset + 7] = 0x48;
        frame[offset + 8] = 0x60;
        frame[offset + 9] = 0x6C;
        offset += 10;
    }

    if (targetSecurityMode == SECURITY_WPA2) {
        if (assocRsnIeLen == 0 || offset + assocRsnIeLen > sizeof(frame)) {
            traceEvent("assoc:missing-selected-rsn");
            return false;
        }
        memcpy(frame + offset, assocRsnIe, assocRsnIeLen);
        offset += assocRsnIeLen;
    }

    // Advertise the local RTL8822CE HT capability for the already selected
    // 20/40 MHz operation. The AP's MCS bytes are used only to form the
    // negotiated RA intersection; an Association Request must not copy peer
    // capabilities or include the AP-only HT Operation element.
    bool is5GHzLink = targetChannel > 14;
    UInt32 raMask = is5GHzLink ? 0x00000FF0U : 0x00000FFFU;
    UInt8 rateId = is5GHzLink ? 7 : 6;
    bool useHt = targetSupportsHt && (targetHtMcs0 != 0);
    vhtNegotiated = false;
    if (useHt) {
        const UInt8 localMcs0 = 0xff;
        const UInt8 localMcs1 = is5GHzLink && !fiveGTwoStreamPowerValid ? 0x00 : 0xff;
        UInt8 negotiatedMcs0 = targetHtMcs0 & localMcs0;
        UInt8 negotiatedMcs1 = targetHtMcs1 & localMcs1;

        // 802.11e QoS Capability. OpenBSD net80211 emits this for a QoS AP
        // before the HT Capability element. WMM is still emitted below for
        // every HT request, matching its HT association path.
        if (targetSupportsWmm) {
            frame[offset++] = 46;
            frame[offset++] = 1;
            frame[offset++] = 0; // QoS Info: no U-APSD requested
        }

        frame[offset++] = 45; frame[offset++] = 26;
        // Linux RTL8822C HT capabilities. Wider modes additionally advertise
        // 20/40 support, SGI40 and DSSS/CCK-in-40 as Linux does.
        UInt16 localHtCap = targetBandwidth >= 1 ? 0x19e3 : 0x09a1;
        frame[offset++] = (UInt8)localHtCap;
        frame[offset++] = (UInt8)(localHtCap >> 8);
        // Linux rtw8822c: max 64K AMPDU exponent and density 2.
        frame[offset++] = 0x0b;
        const UInt8 localMcsSet[16] = {
            localMcs0, localMcs1, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x2c, 0x01, 0x01, 0x00, 0x00, 0x00
        }; // RX mask, RX highest=300 Mbps, TX MCS set defined
        for (int i = 0; i < 16; i++) frame[offset++] = localMcsSet[i];
        frame[offset++] = 0; frame[offset++] = 0;
        for (int i = 0; i < 4; i++) frame[offset++] = 0;
        frame[offset++] = 0;

        // Wi-Fi Alliance WMM Information IE. Some 11n APs deliberately omit
        // HT from the Association Response unless the station advertises WMM.
        frame[offset++] = 221;
        frame[offset++] = 7;
        frame[offset++] = 0x00;
        frame[offset++] = 0x50;
        frame[offset++] = 0xf2;
        frame[offset++] = 0x02; // WMM OUI type
        frame[offset++] = 0x00; // Information subtype
        frame[offset++] = 0x01; // WMM version
        frame[offset++] = 0x00; // QoS info: no U-APSD requested

        // VHT is enabled on the selected 5 GHz HT20/HT40/VHT20/VHT40/VHT80
        // WMM path. The
        // VHT Operation element belongs to the AP; a station sends only its
        // local VHT Capability in the Association Request.
        bool useVht = is5GHzLink && targetSupportsVht && targetSupportsWmm;
        UInt32 vhtMask = 0;
        if (useVht) {
            const UInt16 localVhtMcsMap = fiveGTwoStreamPowerValid ? 0xfffa : 0xfffe;
            const UInt16 localVhtHighest = fiveGTwoStreamPowerValid ? 780 : 390;
            // Match Linux RTL8822C capabilities; the operational bandwidth
            // was already selected from the AP's HT/VHT Operation elements.
            const UInt32 localVhtCap = 0x03d071b2U;
            frame[offset++] = 191;
            frame[offset++] = 12;
            frame[offset++] = (UInt8)localVhtCap;
            frame[offset++] = (UInt8)(localVhtCap >> 8);
            frame[offset++] = (UInt8)(localVhtCap >> 16);
            frame[offset++] = (UInt8)(localVhtCap >> 24);
            frame[offset++] = (UInt8)localVhtMcsMap;
            frame[offset++] = (UInt8)(localVhtMcsMap >> 8);
            frame[offset++] = (UInt8)localVhtHighest;
            frame[offset++] = (UInt8)(localVhtHighest >> 8);
            frame[offset++] = (UInt8)localVhtMcsMap;
            frame[offset++] = (UInt8)(localVhtMcsMap >> 8);
            frame[offset++] = (UInt8)localVhtHighest;
            frame[offset++] = (UInt8)(localVhtHighest >> 8);

            for (UInt8 stream = 0; stream < 2; stream++) {
                UInt8 peerCode = (UInt8)((targetVhtRxMcsMap >> (stream * 2)) & 0x03);
                UInt8 localCode = (UInt8)((localVhtMcsMap >> (stream * 2)) & 0x03);
                UInt8 code = (peerCode == 3 || localCode == 3) ? 3 :
                             (peerCode < localCode ? peerCode : localCode);
                UInt32 rates = code == 0 ? 0x0ffU :
                               (code == 1 ? 0x1ffU :
                               (code == 2 ? (targetBandwidth == 0 ? 0x1ffU : 0x3ffU) : 0));
                vhtMask |= rates << (12 + stream * 10);
            }
            if ((vhtMask & 0x003ff000U) != 0) {
                vhtNegotiated = true;
                raMask = 0x00000010U | vhtMask;
                rateId = (vhtMask & 0xffc00000U) != 0 ? 9 : 10;
            }
        }

        // Linux RA mask layout: CCK/OFDM bits plus HT MCS1SS/2SS at 12/20.
        if (!vhtNegotiated) {
            raMask = (is5GHzLink ? 0x00000030U : 0x00000015U) |
                     ((UInt32)negotiatedMcs0 << 12);
            if (negotiatedMcs1 != 0) {
                raMask |= ((UInt32)negotiatedMcs1 << 20);
                rateId = is5GHzLink ? 4 : 2;
            } else {
                rateId = is5GHzLink ? 5 : 3;
            }
        }
        if (is5GHzLink) targetHtMcs1 = negotiatedMcs1;
        htNegotiated = true;
    } else {
        htNegotiated = false;
    }
    negotiatedRaMask = raMask;
    negotiatedRateId = rateId;

    char assocDb[512];
    int pos = snprintf(assocDb, sizeof(assocDb), "len=%u ht=%d vht=%d wmm=%d ra=%08x rateid=%u ies=",
                       (unsigned int)offset, htNegotiated ? 1 : 0,
                       vhtNegotiated ? 1 : 0, targetSupportsWmm ? 1 : 0,
                       (unsigned int)negotiatedRaMask, negotiatedRateId);
    for (UInt32 i = 28; i < offset && pos < (int)sizeof(assocDb) - 3; i++)
        pos += snprintf(assocDb + pos, sizeof(assocDb) - (size_t)pos,
                        "%02x", frame[i]);
    RTW_DEBUG_PROPERTY("Debug_Assoc_Request", assocDb);

    RTW_DEBUG_LOG("RealtekRTL8822C: Sending Association Request to AP %02x:%02x:%02x:%02x:%02x:%02x\n",
          targetBssid[0], targetBssid[1], targetBssid[2], targetBssid[3], targetBssid[4], targetBssid[5]);

    connState = CONN_STATE_CONNECTING_ASSOC;
    bool queued = transmitWifiFrame(frame, offset, 18); // Use QSEL=18 (MGMTQ)
    traceEvent(queued ? "assoc:tx-queued" : "assoc:tx-queue-failed");
    return queued;
}

bool RealtekRTL8822C::waitForMgmtQueueEmpty(UInt32 timeoutUs) {
    UInt32 waited = 0;
    while (waited <= timeoutUs) {
        UInt32 idx = read32(0x03b0);
        UInt32 hwRp = (idx >> 16) & 0x0fff;
        hwRp %= 128;
        mgmtRp = hwRp;
        if (hwRp == mgmtWp) return true;
        IODelay(20);
        waited += 20;
    }
    return false;
}

void RealtekRTL8822C::resetMgmtQueue(const char* reason) {
    UInt32 before = read32(0x03b0);
    UInt8 savedPause = read8(0x0522);
    write8(0x0522, savedPause | (1U << 6));
    OSSynchronizeIO();

    // RTL8822C HALMAC: BIT_CLR_MGQ_HOST_IDX is bit 1 and
    // BIT_CLR_MGQ_HW_IDX is bit 17 in REG_BD_RWPTR_CLR.
    const UInt32 resetMask = (1U << 17) | (1U << 1);
    write32(0x039c, resetMask);
    OSSynchronizeIO();
    mgmtWp = 0;
    mgmtRp = 0;
    write16(0x03b0, 0);
    write8(0x0522, savedPause);
    OSSynchronizeIO();

    char db[192];
    snprintf(db, sizeof(db), "reason=%s reset=%08x idx=%08x->%08x wp=0 rp=0",
             reason ? reason : "unknown", (unsigned int)resetMask,
             (unsigned int)before, (unsigned int)read32(0x03b0));
    RTW_DEBUG_PROPERTY("Debug_MGMT_Recovery", db);
    traceEvent("mgmt:queue-reset");
}

void RealtekRTL8822C::resetBaState() {
    baEstablished = false;
    baDialogToken = 0;
    baTid = 0;
    baBufferSize = 0;
    baTimeout = 0;
    baRequests = 0;
    baResponses = 0;
    baAttempts = 0;
    baStateStartTime = 0;
    RTW_DEBUG_PROPERTY("Debug_BA_State", "established=0 requests=0 responses=0 attempts=0 token=0 tid=0 buf=0 timeout=0 agg=0");
}

bool RealtekRTL8822C::sendAddBaRequest() {
    if (connState != CONN_STATE_CONNECTED || !htNegotiated) return false;
    if (!waitForMgmtQueueEmpty(20000)) {
        resetMgmtQueue("addba-preflight-timeout");
    }

    UInt8 frame[33];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xd0; // Action
    memcpy(frame + 4, currentBssid, 6);
    memcpy(frame + 10, macAddress, 6);
    memcpy(frame + 16, currentBssid, 6);
    frame[24] = 3; // Block Ack category
    frame[25] = 0; // ADDBA Request
    baDialogToken++;
    if (baDialogToken == 0) baDialogToken = 1;
    frame[26] = baDialogToken;
    baTid = 0;
    baBufferSize = 64;
    baTimeout = 0;
    UInt16 param = (UInt16)((1U << 1) | ((UInt32)baTid << 2) |
                            ((UInt32)baBufferSize << 6));
    frame[27] = (UInt8)param;
    frame[28] = (UInt8)(param >> 8);
    frame[29] = 0;
    frame[30] = 0;
    UInt16 startSeq = (UInt16)((dataSeqNum & 0x0fff) << 4);
    frame[31] = (UInt8)startSeq;
    frame[32] = (UInt8)(startSeq >> 8);

    bool queued = transmitWifiFrame(frame, sizeof(frame), 18);
    baAttempts++;
    clock_get_uptime(&baStateStartTime);
    if (!queued) {
        char failedDb[160];
        snprintf(failedDb, sizeof(failedDb),
                 "established=0 requests=%u responses=%u attempts=%u token=%u tid=0 agg=0 reason=queue-failed",
                 (unsigned int)baRequests, (unsigned int)baResponses,
                 (unsigned int)baAttempts, baDialogToken);
        RTW_DEBUG_PROPERTY("Debug_BA_State", failedDb);
        traceEvent("ba:addba-queue-failed");
        return false;
    }
    baRequests++;
    char db[192];
    snprintf(db, sizeof(db), "established=0 requests=%u responses=%u attempts=%u token=%u tid=%u buf=%u timeout=0 agg=0",
             (unsigned int)baRequests, (unsigned int)baResponses, (unsigned int)baAttempts,
             baDialogToken, baTid, baBufferSize);
    RTW_DEBUG_PROPERTY("Debug_BA_State", db);
    traceEvent("ba:addba-requested");
    return true;
}

bool RealtekRTL8822C::sendAddBaResponse(UInt8 token, UInt8 tid, UInt16 status,
                                    UInt16 bufferSize, UInt16 timeoutTu,
                                    bool retry) {
    if (connState != CONN_STATE_CONNECTED) return false;
    UInt8 frame[33];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xd0;
    memcpy(frame + 4, currentBssid, 6);
    memcpy(frame + 10, macAddress, 6);
    memcpy(frame + 16, currentBssid, 6);
    frame[24] = 3;
    frame[25] = 1; // ADDBA Response
    frame[26] = token;
    frame[27] = (UInt8)status;
    frame[28] = (UInt8)(status >> 8);
    if (bufferSize == 0 || bufferSize > RX_REORDER_MAX_WINDOW)
        bufferSize = RX_REORDER_MAX_WINDOW;
    UInt16 param = (UInt16)((1U << 1) | ((UInt32)tid << 2) | ((UInt32)bufferSize << 6));
    frame[29] = (UInt8)param;
    frame[30] = (UInt8)(param >> 8);
    frame[31] = (UInt8)timeoutTu;
    frame[32] = (UInt8)(timeoutTu >> 8);
    bool queued = transmitWifiFrame(frame, sizeof(frame), 18);
    traceEvent(queued ? (status == 0 ?
                        (retry ? "ba:rx-retry-acked" : "ba:rx-operational") :
                        "ba:rx-addba-refused") : "ba:rx-response-failed");
    return queued;
}

bool RealtekRTL8822C::sendDelBa() {
    if (!baEstablished || connState != CONN_STATE_CONNECTED) return true;
    if (!waitForMgmtQueueEmpty(20000)) resetMgmtQueue("delba-preflight-timeout");

    UInt8 frame[30];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xd0;
    memcpy(frame + 4, currentBssid, 6);
    memcpy(frame + 10, macAddress, 6);
    memcpy(frame + 16, currentBssid, 6);
    frame[24] = 3;
    frame[25] = 2; // DELBA
    UInt16 param = (UInt16)((1U << 11) | ((UInt32)baTid << 12)); // originator
    frame[26] = (UInt8)param;
    frame[27] = (UInt8)(param >> 8);
    frame[28] = 37; // requested from peer; session is being torn down
    frame[29] = 0;
    bool queued = transmitWifiFrame(frame, sizeof(frame), 18);
    if (queued) waitForMgmtQueueEmpty(100000);
    baEstablished = false;
    traceEvent(queued ? "ba:delba-queued" : "ba:delba-failed");
    return queued;
}

bool RealtekRTL8822C::checkLteCoexReady() {
    for (int cnt = 0; cnt < 1000; cnt++) {
        if (read32Mask(0x1700, 1U << 29) == 1) {
            return true;
        }
        IODelay(10);
    }
    return false;
}

UInt32 RealtekRTL8822C::readLteCoexReg(UInt16 offset) {
    if (!checkLteCoexReady()) {
        return 0;
    }
    write32(0x1700, 0x800F0000 | offset);
    return read32(0x1708);
}

bool RealtekRTL8822C::writeLteCoexReg(UInt16 offset, UInt32 mask, UInt32 value) {
    if (!checkLteCoexReady()) {
        return false;
    }
    UInt32 tmp = readLteCoexReg(offset);
    int shift = 0;
    if (mask != 0) {
        while (shift < 32 && !((mask >> shift) & 1)) {
            shift++;
        }
        tmp = (tmp & ~mask) | ((value << shift) & mask);
    }

    write32(0x1704, tmp);
    write32(0x1700, 0xC00F0000 | offset);
    return true;
}

void RealtekRTL8822C::initCoexWifiOnly() {
    RTW_DEBUG_LOG("RealtekRTL8822C: Configuring Coexistence for WLAN-only mode...\n");
    // Match RTL8822C PTA hardware init before forcing WLAN-only grants.
    write8(0x04C6, (UInt8)((read8(0x04C6) & ~0x30U) | 0x10U));
    write16(0x0762, read16(0x0762) | 0x1000U);

    // 1. Force WLAN owner: REG_SYS_SDIO_CTRL + 3 (0x73) set BIT(26) >> 24 = 0x04
    write8(0x0073, read8(0x0073) | 0x04);

    // 2. Set GNT_BT to SW_LOW (0x1) and GNT_WL to SW_HIGH (0x3) in indirect reg 0x38
    writeLteCoexReg(0x38, 0x3000, 0x3); // GNT_WL_hi
    writeLteCoexReg(0x38, 0x0300, 0x3); // GNT_WL_lo
    writeLteCoexReg(0x38, 0xc000, 0x1); // GNT_BT_hi
    writeLteCoexReg(0x38, 0x0c00, 0x1); // GNT_BT_lo

    // 3. Write score board: Active and Onoff true, and enable interrupt (0x8003 to 0xAA)
    write16(0x00AA, 0x8003);

    RTW_DEBUG_LOG("RealtekRTL8822C: Coex WLAN-only config done. 0x73=0x%02X 0xAA=0x%04X Ind38=0x%08X\n",
          read8(0x0073), read16(0x00AA), (unsigned int)readLteCoexReg(0x38));
}
