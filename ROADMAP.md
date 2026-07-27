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

Creating the Git repository, publishing it, and tagging `v0.0.1` are owner
release operations and are intentionally not performed by the build system.

## Known v0.0.1 boundaries

- One RTL8822CE cut D system is the reference hardware matrix.
- Transmission is restricted to the documented FCC/US non-DFS channel set.
- HT40/VHT40 is implemented but has less hardware coverage than 20 MHz and
  VHT80 operation.
- Native Apple wireless services and security modes beyond WPA2-Personal/CCMP
  are outside the release scope.

These are documented compatibility boundaries, not open v0.0.1 release
blockers.

## Post-v0.0.1

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
