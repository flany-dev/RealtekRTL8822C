# Changelog

## 0.0.3 - 2026-07-31

### Scan and network discovery

- Added a fixed-size structured BSS snapshot keyed by BSSID. Hidden BSSes,
  same-name networks on different bands, security, channel, bandwidth, RSSI,
  and age are preserved without parsing formatted report text.
- Unified connected and disconnected scanning under one asynchronous
  timer-driven state machine with bounded active probes, passive DFS discovery,
  cancellation, restoration, and generation-based publication.
- Added directed discovery and **Join Other Network...** for manually entered
  SSIDs, including hidden Open and WPA2 networks.
- Added explicit connectable and DFS-required capability flags. Receive-only
  networks remain visible but cannot reach the TX path.

### Connection and credential lifecycle

- Added structured connection attempt IDs, phases, terminal results, failure
  codes, scan-time queuing, and cancellation across discovery, authentication,
  association, and WPA negotiation.
- Hardened stale attempt handling while retaining standards-compliant
  Auth/Association and EAPOL retry behavior.
- Restored complete WPA teardown between attempts, including replay, nonce,
  PTK/GTK, CAM, and remembered EAPOL state.
- Added cancellable wrong-password recovery, explicit password replacement and
  forgetting, and persistence only after successful WPA authorization.
- Added deduplicated native connection, disconnection, and failure banners.

### Application responsiveness and compatibility

- Moved structured scan snapshot retrieval and decoding off the main UI path,
  skipped unchanged generations, and guarded asynchronous completions against
  stale operations.
- Added persistent disabled presentation for hidden, DFS, and other receive-only
  BSS rows.
- Added clear loaded-driver, compatible-hardware/unloaded-kext, and unsupported
  hardware states.
- Set the application and CLI deployment target to macOS 12. Debug kext builds
  now use an experimental macOS 12 deployment target while Release remains at
  macOS 15.5. Older macOS kext operation is not yet runtime-confirmed.
- Added compatibility checks for deployment targets, weak-linked macOS 13 APIs,
  and exact `10ec:c822` matching without subsystem/revision restrictions.

### Validation and packaging

- Runtime-confirmed Debug and Release scanning, Open/WPA2 connection,
  credential recovery, notifications, sustained traffic, interface control,
  and sleep/wake on the reference system.
- Extended host models, profile checks, reproducibility gates, and the existing
  three-asset release packaging for v0.0.3.

## 0.0.2 - 2026-07-29

### Menu bar application

- Added the native `RealtekRTL8822CMenu.app` status item with operational
  state, Wi-Fi on/off control, scanning, available-network selection,
  disconnect, signal display, and status-aware menu bar icons.
- Added the project application icon and a lightweight four-level menu bar
  signal indicator sourced from received-frame RSSI. Connecting and scanning
  use an intentionally empty Wi-Fi glyph until the link is confirmed.
- Connected scans now retain recently observed BSS entries for five minutes
  instead of replacing the whole list from one short passive sweep. This avoids
  intermittent cross-band omissions without multiplying channel switches.
  Connection commands still require that the selected entry was seen during
  the last ten seconds, and a full cache evicts its oldest non-current entry.
- Menu opening shows cached results immediately and starts a background scan
  only when the previous attempt is at least 15 seconds old. Scan results are
  applied once after the sweep, even if the menu remains open, instead of
  rebuilding it for every received network. The scan item visibly changes to
  **Searching…** and rejects duplicate commands. Periodic status refreshes
  update the status icon without rebuilding an open menu.
- Removed the scan-time 20-ms channel-switch busy wait that is absent from the
  Linux RTL8822C path. Equivalent passive observation time is provided by the
  asynchronous dwell timer, and scan-only channel/TX-power diagnostics are
  suppressed.
- Reduced menu-app Keychain traffic: RSSI publication is rate-limited, session
  credentials are reused, and non-authentication failures no longer delete a
  valid saved password.
- Added a native macOS **Launch at Login** toggle without a helper daemon.
- Added secure WPA2 password prompting and per-SSID storage in the macOS user
  Keychain. Open networks connect without a credential prompt.
- Added a Debug-only diagnostic window with refresh and copy controls. The
  Release app contains neither the Debug menu item nor `Debug_*` report keys.

### Driver control boundary

- Added a fixed-size, versioned `IOUserClient` command protocol for status
  refresh, scan, connect, disconnect, and interface power state.
- Restricted the user client to the active local user and limited it to the
  explicit Wi-Fi command set; it exposes no memory mappings or arbitrary
  register access.
- Added an explicit user-disabled interface latch so BSD `IFF_UP` reconciliation
  cannot silently turn Wi-Fi back on after the menu app disables it.
- Updated `rtl8822cctl` to use the same non-root command path and added `on` and
  `off` commands.

### Stability and performance

- Preserved the Linux RTL8822C PCI queue stop/wake thresholds while separating
  temporary BEQ pressure from permanent TX preparation errors. Only real ring
  pressure is retried; invalid or no-longer-valid packets cannot poison the
  independent output queue.
- Removed the obsolete fabricated-BSSID fallback from the TX path and tightened
  command-specific user-client validation.
- Propagated scan programming failures, cancellation, and timeout states to the
  CLI and menu application instead of reporting an accepted asynchronous
  command as completed.
- Runtime-confirmed the Release candidate through scanning, WPA2 connection,
  reconnect, sustained bidirectional traffic, interface control, and
  sleep/wake with stable traffic delivery.
- Confirmed that AppleVTD/IOMMU is optional for this driver on the reference
  system. Operation was validated without IOMMU and with DMA protection
  enabled; platform policy remains user-specific.

### Build and packaging

- Added reproducible Debug and Release app bundles under `build/`, ad-hoc
  signing for local execution, app profile-separation checks, and an app-only
  GitHub Release archive. The v0.0.2 release assets comprise Release and Debug
  kext archives plus the standalone menu application archive.

## 0.0.1 - 2026-07-28

First public release for Realtek RTL8822CE `10ec:c822` on x86_64 macOS 15.

### Wireless functionality

- Added 2.4 GHz and guarded FCC/US non-DFS 5 GHz scanning and association.
- Added open and WPA2-Personal/CCMP networks with an in-driver four-way and
  group-key handshake, replay protection, CAM programming, and secure key
  teardown.
- Added HT/VHT negotiation, 20/40/80 MHz channel programming, WMM EDCA,
  firmware rate adaptation, TX/RX Block Ack, aggregation, BAR handling, and RX
  reorder.
- Added connected off-channel scanning with restoration of the home channel,
  bandwidth, filters, security state, and traffic service windows.
- Added explicit disconnect/reconnect, AP switching, stale credential removal,
  and interactive hidden password input in `rtl8822cctl`.

### Data path and performance

- Corrected RTL8822C PCI interrupt masks and implemented bounded RX polling.
- Moved output queue service off the serialized RX work loop and retained
  stalled packets in an independent bounded host queue.
- Added idempotent RX ADDBA retry handling and Linux-compatible queue stop/wake
  thresholds.
- Removed protected-frame allocation/copy from the RX hot path, added a batched
  receive packet reserve, and mapped bulk DMA payload arenas as coherent
  cacheable memory while retaining uncached hardware rings.
- Corrected 5 GHz short-slot WMM timing. Sustained traffic on the reference
  system remains responsive and performs comparably to the comparison client.

### Lifecycle and safety

- Added staged cold initialization, suspend/resume hardware restoration,
  H2C-generation reset, packet-filter restoration, and safe rollback with PCI
  bus mastering disabled before DMA-visible memory is released.
- Hardened scan, RSN, EAPOL, CCMP, descriptor, firmware, and partial-start input
  validation.
- Added deterministic key and CAM cleanup across disconnect, interface disable,
  sleep, resume failure, and shutdown.

### Build and release engineering

- Renamed the public project, kext, IOClass, bundle identifier, utility, and
  package to the RTL8822C-specific `RealtekRTL8822C` scope.
- Added strict Debug and Release profiles. Release contains no `IOLog`
  dependency, `Debug_*` IORegistry properties, or diagnostic CLI report keys.
- Added warnings-as-errors builds, host protocol models, WPA cryptographic
  vectors, ASan/UBSan checks, Linux table provenance verification, deterministic
  firmware embedding, reproducible binaries, and reproducible release archives.
- Added formal public documentation, macOS x86_64 CI, issue templates,
  contribution and security policies, firmware provenance, and complete
  third-party notices.

### Supported release boundary

- Reference hardware: RTL8822CE cut D on x86_64 macOS 15 with OpenCore. IOMMU
  state is not a driver requirement and should be selected for platform
  compatibility.
- 5 GHz TX is restricted to channels 36/40/44/48 and 149/153/157/161/165 with
  valid board EFUSE power data. DFS is not supported.
- WPA1/TKIP, WPA3/SAE, Enterprise authentication, PMF, Apple wireless services,
  Apple Silicon, and other Realtek chips are outside v0.0.1 scope.
