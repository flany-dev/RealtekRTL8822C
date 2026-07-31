#include <cassert>
#include <cstring>

struct Lifecycle {
    bool suspended = false;
    bool interfaceEnabled = true;
    bool interfaceEnabledBeforeSleep = true;
    bool connected = true;
    bool queueRunning = true;
    bool interruptsEnabled = true;
    bool busMasterEnabled = true;
    bool camEmpty = false;
    bool bsdUp = true;
    unsigned h2cSequence = 0;
    unsigned h2cLastBox = 0;
    unsigned h2cWritePointer = 0;
    bool selectedMedium = true;
    bool linkActive = false;
    unsigned linkPublishOrder = 0;
    unsigned queueStartOrder = 0;
    unsigned orderCounter = 0;
    bool multicastSupported = true;
    bool controllerEnabled = true;
    bool running = true;

    void disconnect() {
        queueRunning = false;
        connected = false;
        camEmpty = true;
    }

    void disableInterface() {
        interfaceEnabled = false;
        queueRunning = false;
        if (connected) disconnect();
        else camEmpty = true;
    }

    void enableInterface() {
        if (suspended) interfaceEnabledBeforeSleep = true;
        else interfaceEnabled = true;
    }

    void reconcileBsdUp() {
        if (!suspended && !interfaceEnabled && bsdUp) enableInterface();
    }

    void suspend() {
        interfaceEnabledBeforeSleep = interfaceEnabled;
        interfaceEnabled = false;
        if (connected) disconnect();
        camEmpty = true;
        queueRunning = false;
        interruptsEnabled = false;
        busMasterEnabled = false;
        suspended = true;
        controllerEnabled = false;
        running = false;
    }

    void resume(bool success) {
        busMasterEnabled = true;
        if (!success) {
            interruptsEnabled = false;
            busMasterEnabled = false;
            interfaceEnabled = false;
            suspended = true;
            camEmpty = true;
            return;
        }
        suspended = false;
        interfaceEnabled = interfaceEnabledBeforeSleep;
        connected = false;
        queueRunning = false;
        camEmpty = true;
        interruptsEnabled = true;
    }

    bool restoreEthernetController() {
        controllerEnabled = true;
        if (!multicastSupported) {
            controllerEnabled = false;
            running = false;
            return false;
        }
        running = true;
        return true;
    }

    void firmwareDownloaded() {
        h2cSequence = 0;
        h2cLastBox = 0;
        h2cWritePointer = 0;
    }

    void activateDataPath() {
        queueRunning = true;
        queueStartOrder = ++orderCounter;
        assert(selectedMedium);
        linkActive = true;
        linkPublishOrder = ++orderCounter;
    }
};

struct PartialStart {
    bool superStarted = true;
    bool interruptsEnabled = false;
    bool busMasterEnabled = true;
    unsigned preparedMappings = 0;
    unsigned releasedMappings = 0;
    bool releasedWhileBusMastering = false;

    void allocateMapping() { preparedMappings++; }

    void fail() {
        interruptsEnabled = false;
        busMasterEnabled = false;
        while (releasedMappings < preparedMappings) {
            if (busMasterEnabled) releasedWhileBusMastering = true;
            releasedMappings++;
        }
        superStarted = false;
    }
};

struct LinkEvents {
    unsigned generation = 0;
    const char* type = "none";

    void publish(const char* nextType) {
        generation++;
        if (generation == 0) generation = 1;
        type = nextType;
    }

    void disconnect(const char* reason, bool authorized) {
        if (!authorized) return;
        bool local = reason &&
            (std::strcmp(reason, "user") == 0 ||
             std::strcmp(reason, "reconnect") == 0 ||
             std::strcmp(reason, "interface-disabled") == 0 ||
             std::strcmp(reason, "system-sleep") == 0 ||
             std::strcmp(reason, "connection-cancelled") == 0);
        publish(local ? "local-disconnect" : "disconnected");
    }
};

int main() {
    Lifecycle interfaceCycle;
    interfaceCycle.disableInterface();
    assert(!interfaceCycle.interfaceEnabled);
    assert(!interfaceCycle.connected);
    assert(!interfaceCycle.queueRunning);
    assert(interfaceCycle.camEmpty);
    interfaceCycle.enableInterface();
    assert(interfaceCycle.interfaceEnabled);
    assert(!interfaceCycle.connected);
    assert(!interfaceCycle.queueRunning);

    Lifecycle sleepCycle;
    sleepCycle.suspend();
    assert(sleepCycle.suspended);
    assert(!sleepCycle.interfaceEnabled);
    assert(!sleepCycle.interruptsEnabled);
    assert(!sleepCycle.busMasterEnabled);
    assert(sleepCycle.camEmpty);
    sleepCycle.h2cSequence = 37;
    sleepCycle.h2cLastBox = 3;
    sleepCycle.h2cWritePointer = 19;
    sleepCycle.firmwareDownloaded();
    assert(sleepCycle.h2cSequence == 0);
    assert(sleepCycle.h2cLastBox == 0);
    assert(sleepCycle.h2cWritePointer == 0);
    sleepCycle.resume(true);
    assert(!sleepCycle.suspended);
    assert(sleepCycle.interfaceEnabled);
    assert(!sleepCycle.connected);
    assert(!sleepCycle.queueRunning);
    assert(sleepCycle.interruptsEnabled);
    assert(sleepCycle.busMasterEnabled);
    assert(sleepCycle.camEmpty);
    assert(sleepCycle.restoreEthernetController());
    assert(sleepCycle.controllerEnabled);
    assert(sleepCycle.running);
    sleepCycle.activateDataPath();
    assert(sleepCycle.queueRunning);
    assert(sleepCycle.linkActive);
    assert(sleepCycle.queueStartOrder < sleepCycle.linkPublishOrder);

    Lifecycle missingFilterRestore;
    missingFilterRestore.suspend();
    missingFilterRestore.resume(true);
    missingFilterRestore.multicastSupported = false;
    assert(!missingFilterRestore.restoreEthernetController());
    assert(!missingFilterRestore.controllerEnabled);
    assert(!missingFilterRestore.running);

    Lifecycle failedResume;
    failedResume.suspend();
    failedResume.resume(false);
    assert(failedResume.suspended);
    assert(!failedResume.interfaceEnabled);
    assert(!failedResume.interruptsEnabled);
    assert(!failedResume.busMasterEnabled);
    assert(failedResume.camEmpty);

    Lifecycle retainedBsdUp;
    retainedBsdUp.disableInterface();
    assert(!retainedBsdUp.interfaceEnabled);
    retainedBsdUp.bsdUp = true;
    retainedBsdUp.reconcileBsdUp();
    assert(retainedBsdUp.interfaceEnabled);
    assert(!retainedBsdUp.connected);
    assert(!retainedBsdUp.queueRunning);

    // Every allocation boundary in start() must be safe to unwind. The PCI
    // function loses bus mastering before the first DMA mapping is released.
    for (unsigned failurePoint = 0; failurePoint <= 14; failurePoint++) {
        PartialStart start;
        for (unsigned i = 0; i < failurePoint; i++) start.allocateMapping();
        start.fail();
        assert(!start.superStarted);
        assert(!start.interruptsEnabled);
        assert(!start.busMasterEnabled);
        assert(start.releasedMappings == start.preparedMappings);
        assert(!start.releasedWhileBusMastering);
    }

    LinkEvents events;
    events.publish("connected");
    assert(events.generation == 1 && std::strcmp(events.type, "connected") == 0);
    events.disconnect("user", true);
    assert(events.generation == 2 &&
           std::strcmp(events.type, "local-disconnect") == 0);
    events.publish("connected");
    events.disconnect("peer-deauth", true);
    assert(events.generation == 4 &&
           std::strcmp(events.type, "disconnected") == 0);
    events.disconnect("timeout", false);
    assert(events.generation == 4);
    return 0;
}
