# RealtekRTL8822C v0.0.4

Version 0.0.4 is a hardware-compatibility and self-diagnostics release for the
RTL8822CE macOS driver and menu application. The final Debug and Release builds
passed runtime qualification on the reference macOS 15 system. macOS 12-14
kext operation remains experimental pending separate hardware evidence.

## Downloads

- `RealtekRTL8822C-0.0.4-Release.zip` - the normal kext with bounded startup
  status and no deep diagnostics or kernel logging.
- `RealtekRTL8822C-0.0.4-Debug.zip` - the diagnostic kext and matching Debug
  CLI with detailed IORegistry startup evidence.
- `RealtekRTL8822CMenu-0.0.4.zip` - the Release menu bar application.

The Release driver archive contains only `RealtekRTL8822C.kext`; the Debug
archive also contains `rtl8822cctl` for collecting the fallback report. The
application archive contains only `RealtekRTL8822CMenu.app`. Deployment remains
manual; the project does not install or modify OpenCore.

## Hardware compatibility

- Matching remains exact to RTL8822CE PCI ID `10ec:c822`, without subsystem,
  laptop-model, ACPI path, or PCI-revision restrictions.
- Silicon cut and 1T1R/2T2R topology are detected from hardware. RF table,
  calibration, power, HT/VHT capability, and firmware RF-path decisions no
  longer assume the reference 2T2R board.
- Linux RTL8822C parity was added for the cut-D+ EMAC clock selection, RFE 5
  PCI analog path, automatic REFCLK calibration, and RFE 6 CLKREQ behavior
  across sleep/wake.
- Added a bounded local PCI PHY compatibility layer based on the gen1/gen2
  concept: negotiated PCIe generation/width and cut selection are exposed,
  optional DBI/MDIO table transactions are non-fatal, and the current 8822C
  tables remain sentinel-only until a confirmed board parameter is available.
- The exact PCI personality now uses a 5000 probe score and no redundant
  `IONameMatch`, reducing binding dependence on ACPI/provider naming.
- The driver no longer forces PCIe Relaxed Ordering or No Snoop. Its temporary
  ASPM change is restored on failure and stop, preserving platform PCI policy.
- EFUSE RFE options 0 through 6 are recognized. Erased or unsupported values
  stop before transmit initialization and leave an explicit diagnostic result.

## Failed-start diagnostics

- Every initialization phase publishes a stable stage and result on the PCI
  provider. A failed `IOService::start()` therefore leaves a postmortem even
  after the controller instance is removed.
- Release retains only bounded start-time fields: build target, PCI/subsystem
  identity, revision, chip cut, RF path count, RFE option, stage, and failure.
- Debug additionally records the stage trace, PCI configuration snapshot, chip
  topology, selected EFUSE/trims summary, and RFE 6 CLKREQ evidence.
- `rtl8822cctl availability` and the menu app now distinguish unsupported
  hardware, an unloaded kext, failed initialization, and a ready controller.
- `rtl8822cctl report` can read the surviving PCI provider when no controller
  service exists. The app shows **Fail Info…** with deep details only for a
  failed Debug kext; a failed Release kext offers the bounded
  **Copy Compatibility Report** action.
- The same application shows **Debug Info…** for a running Debug kext by
  reading the controller profile and merging its PCI-provider startup trace.
- Neither profile links `IOLog`. Compatibility diagnostics are never updated
  from the RX/TX path, so Release traffic performance is unaffected.

## macOS compatibility

- The application and CLI retain a macOS 12 deployment target and guard their
  macOS 13-only `SMAppService` use.
- The Debug kext retains an experimental macOS 12 deployment target. The build
  verifies its load command and source surface and links the pinned MacKernelSDK
  kmod startup objects rather than the macOS 15.5 SDK copies.
- Release remains targeted at macOS 15.5. macOS 12-14 kext operation is not yet
  a support claim; macOS 14 is the first required secondary-system validation.

## Unchanged boundaries

- Transmission remains restricted to channels 1-11, 36/40/44/48, and
  149/153/157/161/165 with valid board EFUSE power data.
- DFS/CAC/radar handling, WPA3/SAE, Enterprise authentication, required PMF,
  Apple wireless services, Apple Silicon, and other Realtek devices remain out
  of scope.
- Public development binaries are ad-hoc signed and are not notarized.

## Upgrade

Replace the previous kext with the selected v0.0.4 artifact and reboot. Keep a
known-bootable EFI backup. On a new board, use Debug first and preserve the
compatibility report before attempting ordinary network validation.
