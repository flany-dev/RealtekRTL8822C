# RealtekRTL8822C v0.0.2

Version 0.0.2 adds a native macOS menu bar application and completes a focused
stability pass over the driver control, scan, transmit, receive, and lifecycle
paths.

## Downloads

- `RealtekRTL8822C-0.0.2-Release.zip` — the normal, log-free kext.
- `RealtekRTL8822C-0.0.2-Debug.zip` — the diagnostic kext with `Debug_*`
  IORegistry state and driver logging.
- `RealtekRTL8822CMenu-0.0.2.zip` — the Release menu bar application.

The two driver archives contain only `RealtekRTL8822C.kext`. The application
archive contains only `RealtekRTL8822CMenu.app`. The application does not
install or modify the kext or OpenCore configuration.

## Highlights

- Native menu bar scanning, connection, disconnection, Wi-Fi on/off, signal
  display, and Launch at Login control.
- Secure WPA2 password prompting and per-SSID storage in the user Keychain.
- Non-root application and CLI commands through a narrow local-user
  `IOUserClient`; no privileged helper or generic hardware access is exposed.
- Cached asynchronous connected scans across 2.4 and 5 GHz with bounded menu
  updates and explicit completion, cancellation, and timeout handling.
- Strict Debug/Release separation. Release contains no diagnostic kernel
  logging, `Debug_*` IORegistry properties, diagnostic CLI keys, or Debug UI.
- Hardened output retry classification and scan/control error handling so
  permanent failures cannot poison the output queue.
- Hardware-confirmed reconnect, interface control, sustained bidirectional
  traffic, and sleep/wake operation.

## Known issue

On the reference system, the built-in touchpad can briefly lag during network
scanning or connection. This issue is known and remains open in v0.0.2.

## Compatibility

- Realtek RTL8822CE PCIe, PCI ID `10ec:c822`, on x86_64 macOS 15 with
  OpenCore.
- AppleVTD/IOMMU is optional for the driver. The reference system was validated
  without IOMMU and with DMA protection enabled; platform policy should follow
  the configuration that is stable for the complete machine.
- Transmission is restricted to the documented FCC/US non-DFS 5 GHz channels
  and valid board-specific EFUSE power data. Users remain responsible for
  local regulatory compliance.
- WPA3/SAE, Enterprise authentication, required PMF, DFS/CAC, Apple wireless
  services, Apple Silicon, and other Realtek devices are not supported.

## Upgrade

Replace the previous kext in the bootloader configuration with the selected
v0.0.2 kext, copy `RealtekRTL8822CMenu.app` to `/Applications` if desired, and
reboot. Keep a known-bootable EFI backup. Deployment is manual; the project
contains no installer.

The distributed binaries are ad-hoc signed for local execution and are not
notarized by Apple.
