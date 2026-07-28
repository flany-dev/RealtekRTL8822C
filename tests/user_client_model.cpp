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
        case kRTL8822CUserCommandSetInterfaceEnabled:
            return emptySsid && emptyPassword;
        case kRTL8822CUserCommandUpdateStatus:
        case kRTL8822CUserCommandScan:
        case kRTL8822CUserCommandDisconnect:
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

int main() {
    static_assert(sizeof(RTL8822CUserClientCommand) == 116,
                  "user-client ABI changed");

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
    return 0;
}
