# v0.0.5 Hardware Acceptance

The reference RTL8822CE cut D system completed the v0.0.5 functional run under
macOS 14 Sonoma and macOS 15 with both Debug and Release profiles. Validation
includes the menu application, non-root controls, traffic, structured scanning,
hidden-network discovery, reconnect, interface lifecycle, and sleep/wake. This
document is the repeatable qualification procedure for v0.0.5.

Run this procedure on the exact Debug and Release artifacts intended for the
release. Record artifact hashes, macOS version, OpenCore version, PCI ID, chip
revision, IOMMU state, access-point model, channel, and bandwidth.

IOMMU state is recorded for compatibility evidence, not as a pass/fail
requirement. The reference system operates without IOMMU and with DMA
protection enabled. Use the firmware and OpenCore policy that is stable for the
complete hardware configuration.

Use Debug for internal state validation. Repeat the user-facing scenarios with
Release to confirm normal operation without diagnostic output.

## 1. Boot and initialization

After deploying the selected artifact manually:

```sh
export RTL8822CCTL="/path/to/rtl8822cctl"
"$RTL8822CCTL" version
"$RTL8822CCTL" report
```

Debug acceptance:

- version and build configuration match the selected artifact;
- `DriverBuildTargetMacOS` reports `12.0` for Debug and `15.5` for Release;
- power and interface state are active;
- firmware, EFUSE, DMA, PHY, DACK, IQK, and TXGAPK complete without failure;
- `RTL8822CRXPacketPoolStatus` reports `ready` or
  `ready-individual-fallback`; Debug records the bounded request size, ready
  count, allocator result, and smallest successful batch;
- BEQ and MGMTQ have their reviewed descriptor counts while BKQ, VIQ, and VOQ
  remain disabled because the current data path has no producer for them;
- TXDMA and TX error status are zero;
- interrupt masks are `000004fd/00000a00/00010000`.

Complete at least ten cold boots. Every boot must scan and connect without a
firmware, calibration, DMA, or interface failure.

For every qualification run, record:

```sh
"$RTL8822CCTL" availability
"$RTL8822CCTL" report
ioreg -c IOPCIDevice -r -k vendor-id
```

If initialization fails, `availability` must explicitly report that failure,
and `report` must still contain `RTL8822CStartResult`, the last stage, PCI and
subsystem identity, chip cut/topology when BAR access succeeded, and the stable
failure reason. Debug must additionally contain `RTL8822CDebugStartTrace` and
the deepest PCI/chip/EFUSE snapshot reached. A generic "kext not loaded" result
for this case blocks the release.

Also record `RTL8822CPCIELinkSpeed`, `RTL8822CPCIELinkWidth`,
`RTL8822CPCIPhyConfig`, and `RTL8822CPCIeLinkConfig`. The PHY/link values may
report `no-matching-parameters` or `nonfatal-warning`; these describe the
optional compatibility layer and are not startup success criteria. A fatal
start failure must still be reported through `RTL8822CStartResult` and its
stable failure stage.

The Debug macOS 12 deployment target is an experimental compatibility-testing
option. It links the pinned backward-compatible MacKernelSDK kmod startup
objects. Both Debug and Release are runtime-confirmed on the reference hardware
under macOS 14 and macOS 15. macOS 12 and macOS 13 remain experimental until
the complete procedure is confirmed there.

## 2. Scan and channel coverage

Verify discovery and non-zero RSSI on:

- one 2.4 GHz access point;
- one lower-band 5 GHz access point on channels 36-48;
- one upper-band 5 GHz access point on channels 149-165 where locally legal.

Use `rtl8822cctl bss` to verify that same-name BSSes remain distinct by BSSID,
hidden entries retain their channel and age, and later Probe Responses can
upgrade a hidden entry without a subsequent beacon erasing its SSID. Confirm
that the scan plan reaches channels 12/13 and 52-144 passively. DFS and other
receive-only rows must remain visible but unavailable for connection.

Validate 20 MHz, 40 MHz, and 80 MHz operation on controlled access points.
Channel 165 must remain 20 MHz. Do not test or enable DFS transmission; that
requires a later independent regulatory, CAC, radar, and evacuation milestone.

Reject any result where the selected primary channel, center channel,
bandwidth, RF state, or transmit-power table does not match the access point.

## 3. Connection and security lifecycle

Test a router and an Android hotspot using `RealtekRTL8822CMenu.app`:

1. Open the menu and confirm that a cached list appears immediately.
2. Verify that a background scan starts only when the previous scan is at
   least fifteen seconds old, and that the list updates once at completion.
3. Select the intended SSID and connect using the secure password prompt.
4. Confirm that the credential is reused from Keychain on reconnect.
5. Verify WPA2/CCMP completion, DHCP, DNS, download, and upload.
6. Disconnect and confirm the link becomes idle.
7. Reconnect without rebooting.
8. Switch directly to the other access point and repeat.
9. Enter a wrong password and verify that a cancellable replacement prompt
   appears without saving the failed credential.
10. Retry with the correct password and confirm that it is saved only after the
    successful WPA2 handshake.
11. Replace and forget the password through **Manage WPA2 Password...**.
12. Start a connection during a scan, confirm it queues, then repeat and cancel
    it during discovery, authentication, association, and WPA negotiation.
13. Use **Join Other Network...** to discover and connect to a controlled hidden
    SSID on a supported transmit channel.
14. Confirm bounded notifications for connection, disconnection, and actionable
    failure, including a queued connection.

Repeat the connection through `rtl8822cctl connect` as a CLI fallback. Neither
the application nor v0.0.5 CLI commands should require `sudo`.

Reject stale credentials, stale CAM entries, replay failures, permanent queue
stalls, TXDMA faults, or an inability to reconnect.

## 4. Connected scan

While sustained WPA2 traffic is active:

```sh
"$RTL8822CCTL" scan
"$RTL8822CCTL" report
```

Run once on 2.4 GHz and once on a VHT80 5 GHz link. Debug must show a completed
connected scan with the original primary channel, center channel, bandwidth,
primary index, receive filter, WPA state, and Block Ack sessions restored.
Traffic must resume and all rings must drain without recovery or DMA errors.

## 5. Sustained receive and system responsiveness

Generate a large inbound TCP transfer for at least fifteen minutes, followed by
a bidirectional test. During the run:

- move windows and continuously scroll;
- hold and drag using the built-in touchpad;
- repeat the same actions with a USB mouse;
- monitor throughput, CPU usage, packet loss, and UI latency.

Debug acceptance for the bounded RX poll:

- the RX packet reserve records no packet-path miss; a transient batch failure
  is acceptable only when the bounded fallback replenishes the reserve and the
  final pool remains healthy;
- the reported `max_batch` does not exceed the configured `budget`, and the
  final budget/delay values match the reviewed release source;
- sustained downlink must not exceed 254 outstanding BEQ descriptors; the
  RX-side TX-service counter must advance and a stalled queue must restart as
  soon as hardware RP frees the reviewed threshold;
- Debug must report the independent basic output queue with the reviewed
  non-zero capacity;
  `kIOReturnOutputStall` must be consumed as queue backpressure and must not
  surface to `networkQuality` or other applications as error 0x102;
- a stable AP RX Block Ack agreement must not be repeatedly stopped by ADDBA
  retries with the same dialog token and parameters; retries may increase their
  own counter but must preserve the live reorder window;
- `HIMR0=0x000004fd`; RTL8822C must not enable the RDU interrupt. A latched RDU
  status observed together with another enabled cause is diagnostic only and
  must not independently re-enter the ISR;
- `input=immediate`; interface input-queue flushing must not occur while
  hardware interrupts are masked;
- protected RX uses an immutable CCMP payload view; no per-packet CCMP
  allocation, DMA-buffer compaction, or plaintext staging copy is permitted;
- `poll_avg_us` and `poll_max_us` remain bounded and are recorded with the
  throughput result;
- polling becomes inactive after traffic stops;
- RX, management, and data rings finish synchronized;
- no RX overflow, TXDMA fault, permanent output stall, or recovery loop occurs.

Release must remain responsive under the same load and provide comparable
throughput without kernel log output or `Debug_*` IORegistry properties.
Record several consecutive upload results: a single peak is insufficient.
Record the environment and comparison client rather than treating one machine's
measured rate as a universal minimum speed.

## 6. Interface and power lifecycle

While connected:

1. Bring the BSD interface down and back up.
2. Confirm the driver remains idle until an explicit reconnect.
3. Reconnect and verify ordinary protected traffic.
4. Sleep for at least thirty seconds.
5. Wake, reconnect explicitly, and verify DHCP, DNS, download, and upload.

Repeat at least ten sleep/wake cycles. Reject lost `IFF_RUNNING`, failed packet
filter restoration, stale keys, disabled PCI bus mastering after successful
resume, non-zero TXDMA status, or loss of USB/network functionality attributable
to the driver.

## 7. Release profile

With the Release kext, CLI, and menu application installed:

- `report` contains only operational user-facing fields;
- the driver IORegistry service contains no `Debug_*` properties;
- the kext emits no RealtekRTL8822C kernel log messages;
- scan, connect, traffic, connected scan, disconnect, reconnect, and sleep/wake
  work exactly as in the Debug functional run;
- the application contains no **Debug Info** item and continues to handle
  automatic/manual scans, structured BSS results, queued/cancelled connections,
  hidden-network discovery, notifications, Keychain credentials, Wi-Fi on/off,
  and errors.

## 8. Shutdown and final evidence

Shut down and reboot while connected. There must be no raw C2H/CCX console
dump, panic, or inability to scan and reconnect on the next boot.

Preserve:

- Debug reports after boot, each connection, connected scan, load, and wake;
- Release status output and IORegistry/log checks;
- menu application scan, credential, on/off, and reconnect results;
- throughput and responsiveness measurements;
- artifact hashes and the completed environment matrix.

Any unexplained timeout, DMA error, security failure, panic, permanent stall,
or reproducible system-latency regression blocks the release.
