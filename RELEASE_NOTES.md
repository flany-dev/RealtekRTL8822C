# RealtekRTL8822C v0.0.3

Version 0.0.3 is a scan, connection, and compatibility release for the
RTL8822CE macOS driver and its menu bar application.

## Downloads

- `RealtekRTL8822C-0.0.3-Release.zip` - the normal, log-free kext.
- `RealtekRTL8822C-0.0.3-Debug.zip` - the diagnostic kext with driver logging
  and `Debug_*` IORegistry state.
- `RealtekRTL8822CMenu-0.0.3.zip` - the Release menu bar application.

The two driver archives contain only `RealtekRTL8822C.kext`. The application
archive contains only `RealtekRTL8822CMenu.app`. Deployment remains manual; the
project does not install or modify OpenCore.

## Highlights

- Structured BSSID-keyed scan results preserve hidden BSSes, same-name networks
  on different bands, security, channel, bandwidth, RSSI, and result age.
- Connected and disconnected scans share one asynchronous state machine.
  Supported channels use bounded Probe Requests while channels 12/13 and DFS
  channels remain passive receive-only scan targets.
- **Join Other Network...** performs directed discovery for manually entered
  visible or hidden SSIDs.
- Connect requests can wait behind an active scan and can be cancelled during
  discovery, authentication, association, or WPA negotiation.
- Structured attempt IDs, phases, results, and failure codes replace decisions
  based on diagnostic strings.
- Wrong WPA2 credentials lead to a cancellable replacement prompt. Explicit
  password replacement and forgetting are available from the menu, and a new
  password is saved only after successful WPA2 authorization.
- Connection, disconnection, and actionable failure notifications are native,
  bounded, and deduplicated.
- Scan snapshots are fetched only when their generation changes and decoded
  outside the main UI path. Cached networks remain immediately available when
  the menu opens.
- DFS and other receive-only networks remain visible but are clearly disabled;
  the driver shares the same channel policy and cannot transmit on them.
- WPA replay, nonce, PTK/GTK, and remembered EAPOL state are fully reset between
  attempts, allowing reliable recovery after an incorrect password.

## Compatibility

- Realtek RTL8822CE PCIe, PCI ID `10ec:c822`, on x86_64 macOS with OpenCore.
  Matching does not depend on subsystem vendor/device, laptop model, or PCI
  revision.
- The reference system is an RTL8822CE cut D machine running macOS 15. Debug
  and Release were validated through scanning, WPA2, reconnect, sustained
  traffic, interface control, and sleep/wake.
- The menu application and CLI are built with a macOS 12 deployment target.
- The Debug kext is also built with an experimental macOS 12 deployment target
  to permit compatibility testing. It has only been validated on the current
  macOS 15 reference system. The local newer SDK supplies newer kmod startup
  objects, so this does not establish macOS 12-14 support.
- The Release kext remains targeted at macOS 15.5.
- AppleVTD/IOMMU is optional for this driver. The reference system was validated
  without IOMMU and with DMA protection enabled; platform policy should follow
  the configuration that is stable for the complete machine.

## Functional boundaries

- Transmission is restricted to channels 1-11, 36/40/44/48, and
  149/153/157/161/165 with valid board-specific EFUSE power data. DFS channels
  are discovered passively but require regulatory, CAC, and radar support before
  transmission can be enabled.
- WPA1/TKIP, WPA3/SAE, Enterprise authentication, required PMF, Apple wireless
  services, Apple Silicon, and Realtek devices other than RTL8822CE
  `10ec:c822` are outside this release.
- Public development binaries are ad-hoc signed and are not notarized by Apple.

## Upgrade

Replace the previous kext in the bootloader configuration with the selected
v0.0.3 kext, copy `RealtekRTL8822CMenu.app` to `/Applications` if desired, and
reboot. Keep a known-bootable EFI backup.
