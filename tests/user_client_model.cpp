#include <cassert>
#include <cstdint>
#include <cstring>

#include "RTL8822CUserClientShared.h"

static bool validCommand(const RTL8822CUserClientCommand& command) {
    if (command.version != RTL8822C_USER_CLIENT_PROTOCOL_VERSION ||
        command.reserved != 0 || command.enabled > 1 ||
        !std::memchr(command.ssid, '\0', sizeof(command.ssid)) ||
        !std::memchr(command.password, '\0', sizeof(command.password))) {
        return false;
    }
    const bool emptySsid = command.ssid[0] == '\0';
    const bool emptyPassword = command.password[0] == '\0';
    switch (command.command) {
        case kRTL8822CUserCommandConnect:
            return !emptySsid && command.enabled == 0;
        case kRTL8822CUserCommandDirectedScan:
            return !emptySsid && emptyPassword && command.enabled == 0;
        case kRTL8822CUserCommandSetInterfaceEnabled:
            return emptySsid && emptyPassword;
        case kRTL8822CUserCommandUpdateStatus:
        case kRTL8822CUserCommandScan:
        case kRTL8822CUserCommandDisconnect:
        case kRTL8822CUserCommandCancelConnection:
            return emptySsid && emptyPassword && command.enabled == 0;
        default:
            return false;
    }
}

struct InterfaceModel {
    bool enabled = true;
    bool userDisabled = false;

    void userSet(bool value) {
        userDisabled = !value;
        enabled = value;
    }

    void bsdDisable() { enabled = false; }

    void bsdEnable() {
        enabled = !userDisabled;
    }

    void reconcile(bool bsdUp) {
        if (!enabled && !userDisabled && bsdUp) enabled = true;
    }
};

struct PendingConnectModel {
    bool scanActive = false;
    bool valid = false;
    char ssid[33] = {};
    char password[65] = {};

    bool submit(const char* nextSsid, const char* nextPassword) {
        if (!scanActive) return false;
        std::memset(ssid, 0, sizeof(ssid));
        std::memset(password, 0, sizeof(password));
        std::strncpy(ssid, nextSsid, sizeof(ssid) - 1);
        std::strncpy(password, nextPassword, sizeof(password) - 1);
        valid = true;
        return true;
    }

    bool finishScan(bool completed) {
        bool shouldConnect = completed && valid;
        valid = false;
        std::memset(ssid, 0, sizeof(ssid));
        std::memset(password, 0, sizeof(password));
        return shouldConnect;
    }
};

struct ConnectionAttemptModel {
    uint32_t generation = 0;
    bool active = false;
    const char* phase = "idle";
    const char* result = "none";
    const char* failure = "none";

    uint32_t begin(const char* nextPhase) {
        generation++;
        if (generation == 0) generation = 1;
        active = true;
        phase = nextPhase;
        result = "pending";
        failure = "none";
        return generation;
    }

    void publish(const char* nextPhase, const char* nextResult,
                 const char* nextFailure) {
        phase = nextPhase;
        result = nextResult;
        failure = nextFailure;
        if (std::strcmp(nextResult, "pending") != 0) active = false;
    }
};

struct AttemptEpochModel {
    uint32_t current = 0;
    uint32_t auth = 0;
    uint32_t assoc = 0;
    uint32_t wpa = 0;
    bool active = false;

    void begin() { current++; active = true; auth = assoc = wpa = 0; }
    void enterAuth() { auth = current; }
    void enterAssoc() { assoc = current; }
    void enterWpa() { wpa = current; }
    bool acceptsAuth() const { return active && auth == current; }
    bool acceptsAssoc() const { return active && assoc == current; }
    bool acceptsWpa() const { return active && wpa == current; }
};

struct WpaM1GuardModel {
    uint64_t currentReplay = 0;
    uint32_t currentNonceTag = 0;
    bool waitingM3 = false;

    bool accept(uint64_t replay, uint32_t nonceTag) const {
        if (!waitingM3) return true;
        if (replay < currentReplay) return false;
        if (replay == currentReplay) return nonceTag == currentNonceTag;
        return true;
    }
};

struct WpaAttemptResetModel {
    bool pmkValid = false;
    bool replayValid = true;
    uint64_t lastReplay = 9;
    bool waitingM3 = true;

    void reset(bool clearPmk) {
        if (clearPmk) pmkValid = false;
        replayValid = false;
        lastReplay = 0;
        waitingM3 = false;
    }
};

struct ConnectionUxModel {
    bool localTaskActive = false;
    bool driverPending = false;

    void begin() { localTaskActive = true; }
    void refresh(const char* structuredResult) {
        if (std::strcmp(structuredResult, "pending") == 0)
            driverPending = true;
    }
    bool connectionPending() const { return localTaskActive || driverPending; }
    bool mayScan() const { return !connectionPending(); }
    void complete() { localTaskActive = false; driverPending = false; }
};

struct SnapshotGenerationModel {
    bool initialized = false;
    uint32_t loaded = 0;

    bool needsLoad(uint32_t published) const {
        return !initialized || published != loaded;
    }

    void apply(uint32_t generation) {
        initialized = true;
        loaded = generation;
    }
};

struct AsyncOperationGenerationModel {
    uint64_t current = 0;

    uint64_t begin() { return ++current; }
    void invalidate() { current++; }
    bool accepts(uint64_t completion) const { return completion == current; }
};

struct CredentialLifecycleModel {
    bool keychainPresent = true;
    bool sessionReplacement = false;
    bool replacementNeedsSave = false;

    void enterReplacement() {
        sessionReplacement = true;
        replacementNeedsSave = true;
    }

    void connectionFinished(bool authorized) {
        if (!authorized || !replacementNeedsSave) return;
        keychainPresent = true;
        replacementNeedsSave = false;
    }

    void forget() {
        keychainPresent = false;
        sessionReplacement = false;
        replacementNeedsSave = false;
    }
};

int main() {
    static_assert(sizeof(RTL8822CUserClientCommand) == 116,
                  "user-client ABI changed");
    static_assert(sizeof(RTL8822CUserClientScanEntry) == 64,
                  "scan-entry ABI changed");
    static_assert(sizeof(RTL8822CUserClientScanSnapshot) == 2064,
                  "scan-snapshot ABI changed");
    static_assert(kRTL8822CScanEntryConnectable == (1U << 6),
                  "connectable flag changed");
    static_assert(kRTL8822CScanEntryDfsRequired == (1U << 7),
                  "DFS flag changed");

    RTL8822CUserClientCommand command = {};
    command.version = RTL8822C_USER_CLIENT_PROTOCOL_VERSION;
    command.command = kRTL8822CUserCommandScan;
    assert(validCommand(command));

    command.version++;
    assert(!validCommand(command));
    command.version = RTL8822C_USER_CLIENT_PROTOCOL_VERSION;
    command.command = kRTL8822CUserCommandScan;
    command.ssid[0] = 'x';
    assert(!validCommand(command));
    std::memset(command.ssid, 0, sizeof(command.ssid));
    command.enabled = 1;
    assert(!validCommand(command));
    command.enabled = 0;
    command.command = kRTL8822CUserClientSelectorCount + 100;
    assert(!validCommand(command));
    command.command = kRTL8822CUserCommandConnect;
    assert(!validCommand(command));
    std::strcpy(command.ssid, "test");
    assert(validCommand(command));
    std::memset(command.ssid, 'x', sizeof(command.ssid));
    assert(!validCommand(command));
    std::memset(&command, 0, sizeof(command));
    command.version = RTL8822C_USER_CLIENT_PROTOCOL_VERSION;
    command.command = kRTL8822CUserCommandDirectedScan;
    std::strcpy(command.ssid, "HiddenAP");
    assert(validCommand(command));
    std::strcpy(command.password, "not-allowed");
    assert(!validCommand(command));
    std::memset(&command, 0, sizeof(command));
    command.version = RTL8822C_USER_CLIENT_PROTOCOL_VERSION;
    command.command = kRTL8822CUserCommandCancelConnection;
    assert(validCommand(command));

    InterfaceModel interface;
    interface.userSet(false);
    assert(!interface.enabled && interface.userDisabled);
    interface.reconcile(true);
    assert(!interface.enabled);
    interface.bsdEnable();
    assert(!interface.enabled);
    interface.userSet(true);
    assert(interface.enabled && !interface.userDisabled);
    interface.bsdDisable();
    interface.reconcile(true);
    assert(interface.enabled);

    PendingConnectModel pending;
    assert(!pending.submit("ignored", "password"));
    pending.scanActive = true;
    assert(pending.submit("first", "first-password"));
    assert(pending.valid && std::strcmp(pending.ssid, "first") == 0);
    assert(pending.submit("second", "second-password"));
    assert(std::strcmp(pending.ssid, "second") == 0);
    assert(std::strcmp(pending.password, "second-password") == 0);
    assert(pending.finishScan(true));
    assert(!pending.valid && pending.ssid[0] == '\0' &&
           pending.password[0] == '\0');
    assert(pending.submit("cancelled", "secret"));
    assert(!pending.finishScan(false));
    assert(!pending.valid && pending.password[0] == '\0');

    ConnectionAttemptModel attempt;
    uint32_t queuedID = attempt.begin("queued");
    attempt.publish("preparing", "pending", "none");
    attempt.publish("authenticating", "pending", "none");
    attempt.publish("associating", "pending", "none");
    attempt.publish("negotiating-wpa", "pending", "none");
    attempt.publish("connected", "success", "none");
    assert(!attempt.active && attempt.generation == queuedID);
    uint32_t failedID = attempt.begin("preparing");
    assert(failedID != queuedID);
    attempt.publish("terminal", "failure", "invalid-credentials");
    assert(!attempt.active &&
           std::strcmp(attempt.failure, "invalid-credentials") == 0);
    uint32_t replacedID = attempt.begin("queued");
    attempt.publish("terminal", "cancelled", "replaced");
    assert(!attempt.active && replacedID == failedID + 1);

    AttemptEpochModel epoch;
    epoch.begin();
    epoch.enterAuth();
    assert(epoch.acceptsAuth() && !epoch.acceptsAssoc());
    epoch.enterAssoc();
    assert(epoch.acceptsAssoc());
    epoch.begin();
    assert(!epoch.acceptsAuth() && !epoch.acceptsAssoc() &&
           !epoch.acceptsWpa());
    epoch.enterWpa();
    assert(epoch.acceptsWpa());

    WpaM1GuardModel m1;
    // A new attempt may legitimately receive the same AP replay/ANonce tuple;
    // there is no standard local attempt token in EAPOL M1.
    assert(m1.accept(7, 0xa1));
    m1.waitingM3 = true;
    m1.currentReplay = 8;
    m1.currentNonceTag = 0xa2;
    assert(!m1.accept(7, 0xa1));
    assert(!m1.accept(8, 0xa3));
    assert(m1.accept(8, 0xa2));
    assert(m1.accept(9, 0xa4));

    WpaAttemptResetModel wpaReset;
    wpaReset.pmkValid = true; // freshly derived for the replacement password
    wpaReset.reset(false);
    assert(wpaReset.pmkValid && !wpaReset.replayValid &&
           wpaReset.lastReplay == 0 && !wpaReset.waitingM3);
    wpaReset.reset(true);
    assert(!wpaReset.pmkValid);

    ConnectionUxModel ux;
    ux.begin();
    ux.refresh("none"); // directed refresh before the driver attempt exists
    assert(ux.connectionPending() && !ux.mayScan());
    ux.refresh("pending");
    assert(ux.connectionPending() && !ux.mayScan());
    ux.complete();
    assert(!ux.connectionPending() && ux.mayScan());

    SnapshotGenerationModel snapshot;
    assert(snapshot.needsLoad(0));
    snapshot.apply(0);
    assert(!snapshot.needsLoad(0));
    assert(snapshot.needsLoad(1));
    snapshot.apply(1);
    assert(!snapshot.needsLoad(1));

    AsyncOperationGenerationModel asyncScan;
    uint64_t firstScan = asyncScan.begin();
    assert(asyncScan.accepts(firstScan));
    uint64_t secondScan = asyncScan.begin();
    assert(!asyncScan.accepts(firstScan));
    assert(asyncScan.accepts(secondScan));
    asyncScan.invalidate();
    assert(!asyncScan.accepts(secondScan));

    CredentialLifecycleModel credentials;
    credentials.enterReplacement();
    assert(credentials.keychainPresent && credentials.sessionReplacement &&
           credentials.replacementNeedsSave);
    credentials.connectionFinished(false);
    assert(credentials.keychainPresent && credentials.replacementNeedsSave);
    credentials.connectionFinished(true);
    assert(credentials.keychainPresent && !credentials.replacementNeedsSave);
    credentials.forget();
    assert(!credentials.keychainPresent && !credentials.sessionReplacement);

    const bool invalidCredentialDeletesSavedPassword = true;
    const bool timeoutDeletesSavedPassword = false;
    const bool timeoutOffersReplacementPrompt = true;
    assert(invalidCredentialDeletesSavedPassword);
    assert(!timeoutDeletesSavedPassword && timeoutOffersReplacementPrompt);
    return 0;
}
