# RealtekRTL8822C Roadmap

This roadmap describes public release scope. Detailed development history and
machine-specific diagnostic evidence are intentionally excluded.

## v0.0.1 - complete

- RTL8822CE `10ec:c822` firmware, EFUSE, calibration, PCI/DMA, interrupt, and
  lifecycle initialization.
- Open and WPA2-Personal/CCMP operation on 2.4 GHz and guarded non-DFS 5 GHz.
- HT/VHT, WMM, TX/RX Block Ack, aggregation, RX reorder, sustained traffic,
  connected scanning, reconnect, interface lifecycle, and sleep/wake.
- Strict Debug/Release separation, host models, sanitizers, provenance checks,
  reproducible builds, and public release packaging.

## v0.0.2 - complete

- Native macOS menu bar application and signal-aware status icon.
- Non-root scan, connect, disconnect, and Wi-Fi on/off controls through a
  narrow versioned `IOUserClient`.
- WPA2 Keychain integration and Launch at Login.
- Responsive connected scanning, stable sustained traffic, and Release-profile
  lifecycle validation.

## v0.0.3 - complete

Version 0.0.3 focuses on scan/connection stability and compatibility:

- BSSID-preserving structured scan snapshots with visible and hidden BSSes kept
  distinct across bands and security configurations.
- One asynchronous scan engine for connected and disconnected operation,
  bounded active probes, passive DFS discovery, and generation-based menu
  updates outside the main UI path.
- Manual hidden-network entry and directed discovery.
- Machine-readable connection attempts, queuing behind scans, cancellation,
  precise failure handling, and complete WPA state cleanup between retries.
- Corrected-password recovery, explicit replacement/forget actions, and
  Keychain persistence only after a successful WPA2 handshake.
- Deduplicated native connection, disconnection, and failure notifications.
- Explicit receive-only/DFS channel presentation instead of unsafe association
  attempts.
- macOS 12 deployment targets for the menu application and CLI. The Debug kext
  also has an experimental macOS 12 deployment target for compatibility testing;
  this has not been validated on macOS 12-14 and is not a support claim. Release
  remains targeted at the hardware-confirmed macOS 15 environment.
- Exact RTL8822CE `10ec:c822` matching without subsystem-vendor, laptop-model,
  or PCI-revision restrictions.
- Debug and log-free Release hardware validation, including scan, WPA2,
  sustained traffic, interface control, and sleep/wake.

## v0.0.4 - released

Version 0.0.4 expands hardware and system compatibility without changing the
established channel policy:

- persist a bounded initialization result, stage, PCI identity, silicon cut,
  RF-path topology, RFE option, and failure reason on the PCI provider when the
  controller cannot finish `start()`;
- expose a deeper Debug-only startup trace, PCI/chip snapshot, and selected
  EFUSE/RFE evidence without relying on unavailable kernel logs;
- distinguish unsupported hardware, an unloaded kext, a failed initialization,
  and a ready controller in the CLI and menu application;
- remove subsystem, PCI revision, 2T2R, cut-D-only, and reference-board PCIe
  policy assumptions from eligibility and initialization paths;
- follow Linux RTL8822C handling for cut D+, 1T1R/2T2R topology, RFE 5, RFE 6,
  and restoration of host PCIe policy;
- retain a start-only, bounded Release report and compile deeper diagnostics out
  of Release so the normal packet path has no additional reporting cost;
- retain macOS 12 deployment targets for Debug kext and userspace, with macOS
  12-14 support remaining experimental until separate hardware runs pass, and
  use the pinned backward-compatible MacKernelSDK kmod startup objects;

The reference macOS 15 cut-D system completed the release acceptance matrix.
macOS 12-14 and additional RTL8822CE boards remain experimental until each
configuration completes the same matrix independently.

## Later work

- Implement DFS regulatory-domain selection, CAC, radar detection, evacuation,
  and guarded transmit support as a separate evidence-gated milestone.
- Expand the RTL8822CE board, AP, macOS, bandwidth, interrupt, and sleep/wake
  matrix.
- Add WPA3/SAE, Enterprise authentication, and PMF in separate milestones.
- Continue replacing register literals with named definitions tied to the
  pinned Linux rtw88 reference.
- Evaluate native Apple wireless integration as a separate project phase.

## Release rule

A capability is supported only when implementation, automated checks, hardware
evidence, documentation, and limitations agree. Unverified behavior remains
experimental until that evidence exists.
