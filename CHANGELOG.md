# Changelog

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

- Reference hardware: RTL8822CE cut D on x86_64 macOS 15 with OpenCore and
  AppleVTD/IOMMU enabled.
- 5 GHz TX is restricted to channels 36/40/44/48 and 149/153/157/161/165 with
  valid board EFUSE power data. DFS is not supported.
- WPA1/TKIP, WPA3/SAE, Enterprise authentication, PMF, Apple wireless services,
  Apple Silicon, and other Realtek chips are outside v0.0.1 scope.
