# RealtekRTL8822C v0.0.5

Version 0.0.5 is a compatibility, startup-allocation, and Release hot-path
cleanup release for the RTL8822CE macOS driver and menu application. Debug and
Release are runtime-confirmed on the reference RTL8822CE cut D system under
macOS 14 Sonoma and macOS 15.

## Downloads

- `RealtekRTL8822C-0.0.5-Release.zip` - the normal kext with bounded startup
  status and no deep diagnostics or kernel logging.
- `RealtekRTL8822C-0.0.5-Debug.zip` - the diagnostic kext and matching Debug
  CLI with detailed IORegistry startup and allocator evidence.
- `RealtekRTL8822CMenu-0.0.5.zip` - the single profile-aware menu application.

The Release archive contains only `RealtekRTL8822C.kext`. The Debug archive
also contains `rtl8822cctl` so a failed initialization report can be collected
without building the project. The application archive contains only
`RealtekRTL8822CMenu.app`. Deployment remains manual; the project does not
install or modify OpenCore.

## Startup compatibility

- The initial RX mbuf reserve is built in bounded batches of at most 128
  packets instead of one nonblocking 512-packet request.
- If a list allocation is rejected, Debug records the allocator error and the
  driver retries with 64, 32, or 16 packets, followed by a bounded individual
  allocation fallback.
- Startup requires a useful reserve rather than the full 512 packets. The
  independent replenisher fills the remaining capacity after initialization.
- RX pool status remains on the PCI provider when initialization fails, so
  **Fail Info...** and `rtl8822cctl report` expose the last request, errno,
  ready count, smallest successful batch, and fallback state.
- The reference Sonoma load confirmed four successful 128-packet startup
  batches. Sustained traffic also exercised the individual fallback after one
  transient list-allocation failure without a pool miss or packet-path
  allocation failure.

## DMA and queue cleanup

- Removed BKQ, VIQ, and VOQ descriptor and payload arenas. The implemented data
  path submits ordinary traffic through BEQ and management traffic through
  MGMTQ; the unused queues consumed about 0.75 MiB of physically contiguous
  DMA memory and added eighteen unnecessary startup failure points.
- Resume now restores only the active H2C, BEQ, MGMTQ, beacon, and RX rings.
- The raw 802.11 transmitter is restricted to MGMTQ and rejects invalid queue
  selectors or oversized frames instead of permitting an unaccounted BEQ
  submission.

## Release performance boundary

- CCX packet requests, raw RX descriptor formatting, management register
  snapshots, and sampled TX reports are compiled out of Release.
- Release no longer requests diagnostic firmware TX reports or parses their
  C2H responses on the receive path.
- Debug retains the complete diagnostic surface. Release retains only bounded
  startup and operational properties and does not link `IOLog`.

## Runtime validation

The reference hardware completed Debug and Release runs on both macOS 14
Sonoma and macOS 15. Confirmed behavior includes:

- cold initialization, firmware download, EFUSE, DACK, IQK, and PCI/DMA setup;
- active/passive scanning, WPA2 association, DHCP/DNS, and bidirectional data;
- HT/VHT operation, TX/RX Block Ack, aggregation, and connected scanning;
- RX pool replenishment under sustained traffic with zero pool misses;
- bounded RX polling, clean DMA/TX status, and drained output queues;
- disconnect, reconnect, interface lifecycle, and normal menu application use.

The Debug kext and userspace retain a macOS 12 deployment target. macOS 12 and
macOS 13 remain experimental. Release retains its macOS 15.5 build target; its
operation on macOS 14 is nevertheless runtime-confirmed on the reference
hardware.

## Unchanged boundaries

- Transmission remains restricted to channels 1-11, 36/40/44/48, and
  149/153/157/161/165 with valid EFUSE power data.
- DFS/CAC/radar handling, WPA3/SAE, Enterprise authentication, required PMF,
  Apple wireless services, Apple Silicon, and non-RTL8822CE devices remain out
  of scope.
- Public development binaries are ad-hoc signed and are not notarized.

## Upgrade

Replace the previous kext with the selected v0.0.5 artifact and reboot. Keep a
known-bootable EFI backup. Use Debug when collecting initialization or runtime
diagnostics; use Release for normal operation.
