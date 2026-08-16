# rtl8822cctl

`rtl8822cctl` sends commands to the loaded RealtekRTL8822C service. The Debug
build prints operational state plus the driver's `Debug_*` registry properties.
The Release build prints only ordinary user-facing state and contains no
`Debug_*` report keys.

```text
rtl8822cctl version
rtl8822cctl availability
rtl8822cctl status
rtl8822cctl report
rtl8822cctl scan
rtl8822cctl bss
rtl8822cctl connect <ssid> [password|--ask-password]
rtl8822cctl cancel-connect
rtl8822cctl disconnect
rtl8822cctl on
rtl8822cctl off
```

With the v0.0.4 kext, commands use the versioned RTL8822C local-user
client and do not require root privileges. The user client accepts only status,
structured scan snapshots, scan, connect, cancellation, disconnect, and
interface-state commands; it exposes no DMA memory or register access. The
`availability` distinguishes a ready driver, compatible PCI hardware without a
loaded service, a recorded initialization failure, and unsupported hardware.
After a failed `start()`, `report` reads the bounded postmortem from the
surviving PCI provider even though no controller service exists. A command failure
against an older kext normally means the required user-client protocol is
unavailable.

`rtl8822cctl off` disables the driver Wi-Fi interface and disconnects the
current network without changing EFI or unloading the kext. `rtl8822cctl on`
enables it again; select a network afterward to reconnect.

`rtl8822cctl scan` starts the same bounded asynchronous channel state machine
whether the interface is connected or disconnected, then waits for its terminal
state. A connected scan returns to the home channel between excursions and
restores the primary channel, center channel, bandwidth, receive filter, WPA
state, and Block Ack sessions. Active probes are restricted to supported
transmit channels; DFS and other receive-only channels are visited passively.
Do not treat new SSIDs alone as success: ordinary traffic must still work after
a connected scan.

`rtl8822cctl bss` prints the structured protocol-v2 BSS snapshot as JSON. Each
entry includes BSSID, SSID/hidden state, security, channel, bandwidth, RSSI,
age, and whether the current policy permits connection. This is the preferred
way to inspect same-name, hidden, DFS, and cross-band results.

`rtl8822cctl connect <ssid>` automatically asks for a password without echoing it.
Press Enter on the empty prompt for an open network. The equivalent explicit
form `rtl8822cctl connect <ssid> --ask-password` remains available for compatibility.
Both forms avoid placing the credential in shell history or the process argument
list. A positional password remains available for non-interactive callers and
should not be used for ordinary interactive operation.

`rtl8822cctl cancel-connect` cancels a pending request during queued discovery,
authentication, association, or WPA negotiation. The ordinary report exposes a
machine-readable connection-attempt ID, phase, result, and failure reason.

`report` also includes the build target and start result/stage, PCI identity,
silicon cut, RF-path count, RFE option, and failure reason. Debug additionally
includes a stage trace and selected PCI, chip, EFUSE, and RFE evidence. The
Debug kext reports the experimental macOS 12 target used for compatibility
testing and links the pinned MacKernelSDK startup objects. This has not been
validated on macOS 12-14 and does not establish support for those systems.
Release retains the hardware-confirmed macOS 15.5 target.

Before sharing `rtl8822cctl report`, redact SSIDs, BSSIDs, local IP addresses, and
other device identifiers. The report must never expose raw PMK/PTK/GTK bytes;
that is a release-blocking defect.

Release intentionally has no diagnostic revision or `RTL8822CDebug*` property:
absence of those deep fields is expected, not a reporting failure. Its bounded
startup properties are written only during initialization and do not affect the
packet path.

The v0.0.4 Debug driver archive includes the matching Debug `rtl8822cctl` so an
external tester can run `rtl8822cctl availability` and `rtl8822cctl report`
without building the utility locally.
