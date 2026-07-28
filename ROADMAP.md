# RealtekRTL8822C Roadmap

This roadmap describes public release scope. Detailed development history and
machine-specific diagnostic evidence are intentionally excluded from the
public repository.

## v0.0.1 — complete

The first-release engineering and documentation gates are complete:

- [x] RTL8822CE `10ec:c822` firmware, EFUSE, calibration, PCI/DMA, interrupt,
  and lifecycle initialization.
- [x] 2.4 GHz and guarded non-DFS 5 GHz scanning and association.
- [x] Open and WPA2-Personal/CCMP networks with fresh credential and key
  teardown semantics.
- [x] HT/VHT negotiation, WMM, TX/RX Block Ack, aggregation, RX reorder, and
  connected off-channel scanning.
- [x] DHCP and sustained bidirectional traffic without the former inbound-load
  UI latency or receive-copy bottleneck.
- [x] Disconnect/reconnect, AP switching, interface down/up, and sleep/wake
  recovery on the reference system.
- [x] Strict Debug/Release separation with no diagnostic logging or `Debug_*`
  IORegistry surface in Release.
- [x] Bounds and lifecycle hardening for scan, RSN, EAPOL, CCMP, DMA mappings,
  partial start, suspend, and resume failure paths.
- [x] Host protocol models, WPA vectors, ASan/UBSan, Linux table provenance,
  deterministic firmware embedding, reproducible builds, and reproducible
  release packaging.
- [x] Public documentation, contribution and security policies, firmware
  licence, third-party notices, issue templates, and macOS x86_64 CI.

The repository publication and `v0.0.1` tag were completed as explicit owner
release operations outside the build system.

## Known v0.0.1 boundaries

- One RTL8822CE cut D system is the reference hardware matrix.
- Transmission is restricted to the documented FCC/US non-DFS channel set.
- HT40/VHT40 is implemented but has less hardware coverage than 20 MHz and
  VHT80 operation.
- Native Apple wireless services and security modes beyond WPA2-Personal/CCMP
  are outside the release scope.

These are documented compatibility boundaries, not open v0.0.1 release
blockers.

## v0.0.2 — complete, publication pending

- [x] Native macOS menu bar application with Wi-Fi state and network list.
- [x] Scan, connect, disconnect, and Wi-Fi on/off actions.
- [x] WPA2 credential prompt and macOS Keychain storage.
- [x] Native Launch at Login control.
- [x] Versioned local-user `IOUserClient`; no `sudo` or privileged helper.
- [x] Explicit user-disabled latch resistant to BSD `IFF_UP` reconciliation.
- [x] Debug-only diagnostic window and log-free/diagnostic-free Release UI.
- [x] Debug/Release app builds and app-only release packaging.
- [x] Application icon, pending-link glyph, and lightweight RSSI levels.
- [x] Bounded connected-scan cache across 2.4 and 5 GHz.
- [x] Runtime validation of non-root commands, password flow, repeated on/off,
  reconnect, connected scan, and sleep/wake with the menu app running.
- [x] Stable sustained bidirectional traffic and final TX/error-path audit.
- [x] Final v0.0.2 release audit, reproducible packaging, and release assets.

Publishing the GitHub Release and tagging `v0.0.2` are owner operations and are
intentionally not performed by the build system or automated agents without
explicit approval.

## Post-v0.0.2

- Expand the RTL8822CE laptop, AP, macOS, bandwidth, and sleep/wake matrix.
- Add a trustworthy regulatory-domain source before broadening channel policy.
- Design DFS/CAC support only after regulatory handling is complete.
- Add WPA3/SAE, Enterprise authentication, and PMF in separate milestones.
- Replace remaining magic register values with named definitions tied to the
  pinned Linux rtw88 reference.
- Split the controller into smaller modules while preserving verified hardware
  and lifecycle ordering.
- Evaluate native Apple wireless integration as a separate project phase.

## Release rule

A capability is supported only when implementation, automated checks, hardware
evidence, documentation, and limitations agree. New unverified behavior remains
experimental until that evidence exists.
