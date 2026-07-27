# rtl8822cctl

`rtl8822cctl` sends commands to the loaded RealtekRTL8822C service. The Debug
build prints operational state plus the driver's `Debug_*` registry properties.
The Release build prints only ordinary user-facing state and contains no
`Debug_*` report keys.

```text
rtl8822cctl version
rtl8822cctl report
rtl8822cctl scan
rtl8822cctl connect <ssid> [password|--ask-password]
rtl8822cctl disconnect
```

Commands that change driver state normally require root privileges. A failure
such as `0xe00002c7` usually means the process lacks permission to update the
driver service.

`rtl8822cctl scan` is synchronous while disconnected. While connected, the driver
uses a bounded asynchronous off-channel sweep and the utility waits for the
home channel to be restored through the operational `ConnectedScanState`
property. In Debug, a clean result is also mirrored as
`Debug_Connected_Scan: ... active=0 result=complete restored=1`. Do not treat
new SSIDs alone as success: the existing WPA/BA session and ordinary traffic
must still work after the scan.

`rtl8822cctl connect <ssid>` automatically asks for a password without echoing it.
Press Enter on the empty prompt for an open network. The equivalent explicit
form `rtl8822cctl connect <ssid> --ask-password` remains available for compatibility.
Both forms avoid placing the credential in shell history or the process argument
list. A positional password remains available for non-interactive callers and
should not be used for ordinary interactive operation.

Before sharing `rtl8822cctl report`, redact SSIDs, BSSIDs, local IP addresses, and
other device identifiers. The report must never expose raw PMK/PTK/GTK bytes;
that is a release-blocking defect.

Release intentionally has no diagnostic revision property: absence of all
`Debug_*` fields is the expected user-facing behavior, not a reporting failure.
