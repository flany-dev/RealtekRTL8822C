# RealtekRTL8822C

RealtekRTL8822C is an x86_64 macOS PCIe driver for the Realtek RTL8822CE
(`10ec:c822`). Version `0.0.5` is the current release.

The driver is implemented as an `IOEthernetController` with a native menu bar
application, `RealtekRTL8822CMenu`, and the companion command-line utility
`rtl8822cctl`. It does not replace Apple's native Wi-Fi framework or provide
Apple wireless services.

## Supported configuration

The reference configuration is an RTL8822CE cut D system running through
OpenCore on the legacy PCI interrupt path. Both Debug and Release profiles are
runtime-confirmed on this same reference hardware under macOS 14 Sonoma and
macOS 15. AppleVTD/IOMMU is not a driver requirement: the reference machine has
been validated without IOMMU and with DMA protection enabled. IOMMU and
DMA-protection policy is platform- and bootloader-specific; use the
configuration that is stable for the rest of the machine.

Hardware-confirmed functionality includes:

- structured active/passive scanning across 2.4 GHz and 5 GHz, with unsupported
  transmit channels shown explicitly as receive-only;
- open and WPA2-Personal/CCMP association;
- DHCP, DNS, and sustained bidirectional IP traffic;
- HT and VHT operation, WMM, TX/RX Block Ack, aggregation, and RX reorder;
- 20 MHz and VHT80 links, including lower and upper 5 GHz operation;
- disconnect, reconnect, AP switching, interface down/up, and sleep/wake
  recovery;
- connected off-channel scanning with restoration of the active link;
- hidden-network directed discovery, queued/cancellable connection attempts,
  credential recovery, and native link notifications;
- separate diagnostic Debug and log-free Release profiles.

The final v0.0.5 Debug and Release profiles have been tested on the reference
system under macOS 14 and macOS 15 through scanning, WPA2 traffic, sustained
load, reconnect, and lifecycle operation without finding a driver failure.
This remains a reference-system confirmation, not a universal compatibility
claim for every RTL8822CE configuration or macOS version.

## Channel policy and limitations

5 GHz transmission is limited to FCC/US non-DFS channels 36/40/44/48 and
149/153/157/161/165 and requires valid board-specific EFUSE power data. The
driver does not derive a regulatory domain from macOS; users are responsible
for operating only on channels legal in their location.

The following are not supported:

- DFS/CAC and 5 GHz transmission outside the guarded channel set;
- WPA1/TKIP, WPA3/SAE, Enterprise authentication, and required PMF;
- Apple's own Wi-Fi menu integration, IO80211 integration, AirDrop, and AWDL;
- Apple Silicon or Realtek devices other than RTL8822CE `10ec:c822`;
- automatic installation or modification of an OpenCore configuration.

HT40/VHT40 selection is implemented, but the published reference matrix is
centered on hardware-confirmed 20 MHz and VHT80 links. Unvalidated AP, macOS,
and bandwidth combinations remain experimental.

## Build

Requirements:

- macOS Command Line Tools with a macOS 15.5 or newer SDK;
- an x86_64 target;
- Acidanthera MacKernelSDK at `../MacKernelSDK`, or a custom location supplied
  as `MAC_KERNEL_SDK=/path/to/MacKernelSDK`;
- the pinned `firmware/rtw8822c_fw.bin` input included in this repository.

```sh
make debug
make release
make test
```

Artifacts are written only under `build/`:

- `build/Debug/RealtekRTL8822C.kext` and `build/Debug/rtl8822cctl`;
- `build/Release/RealtekRTL8822C.kext`, `build/Release/rtl8822cctl`,
  and `build/Release/RealtekRTL8822C.dSYM`;
- `build/app/RealtekRTL8822CMenu.app` (the single runtime-profile-aware app).

Debug publishes `Debug_*` operational state plus deep `RTL8822CDebug*` startup
diagnostics. The single distributed menu app discovers the kext profile at
runtime: it shows **Debug Info** for a loaded Debug kext and **Fail Info…** for
a failed Debug start, reading the latter from the PCI provider without a
controller service. Release retains only a bounded start-time compatibility
result, so both diagnostic buttons stay hidden for a Release kext. Neither
profile relies on `IOLog`, and no v0.0.5
compatibility property is updated from the packet path. Automated binary checks
enforce this boundary.

The application and CLI use a macOS 12 deployment target. The Debug kext also
uses an experimental macOS 12 deployment target for compatibility testing and
links the pinned MacKernelSDK startup objects instead of newer SDK 15.5 kmod
objects. Both profiles are runtime-confirmed on macOS 14 and macOS 15 on the
reference hardware. macOS 12 and macOS 13 remain experimental. The Release
kext retains its macOS 15.5 build target despite the confirmed macOS 14 runtime
result.

Deployment is deliberately outside the build system. Copy the selected kext
and utility manually to the locations used by your boot configuration. Keep a
known-bootable EFI backup before replacing a kernel extension.

## Verification and packaging

```sh
make release-check
```

The release gate builds both profiles, treats warnings as errors, runs protocol,
WPA, lifecycle, channel, queue, and RX models, executes ASan/UBSan tests,
verifies firmware and Linux table provenance, checks the public documentation
surface, confirms reproducible Release builds, and creates a checksummed archive
under `build/package/`.

`make package` creates the complete maintainer archive, two kext-only GitHub
Release assets, and the menu application asset:

- `build/package/RealtekRTL8822C-0.0.5-Release.zip`;
- `build/package/RealtekRTL8822C-0.0.5-Debug.zip`;
- `build/package/RealtekRTL8822CMenu-0.0.5.zip`.

The Release driver archive contains only `RealtekRTL8822C.kext`. The Debug
archive contains the Debug kext and matching Debug `rtl8822cctl`, so a failed
startup report can be collected without compiling the project. The application
archive contains only `RealtekRTL8822CMenu.app` and is a normal v0.0.5 release
download alongside the two driver archives.
`make package-local` creates the complete locally named test archive. None of
these commands installs anything.

## Runtime control

Launch `RealtekRTL8822CMenu.app` for menu bar scanning, connection,
disconnect, and Wi-Fi on/off control. WPA2 credentials are stored in the user
Keychain. Driver commands use a narrow local-user `IOUserClient` and do not
require `sudo` with the v0.0.5 kext.

If initialization fails, the app and `rtl8822cctl availability` report that
state separately from an unloaded kext. `rtl8822cctl report` then reads the
postmortem directly from the surviving `IOPCIDevice`; the Debug build includes
the deeper startup trace and selected EFUSE/RFE evidence.

Run `rtl8822cctl help` for the command list. Interactive `connect` prompts for a
password without echoing it or placing it in shell history. See the
[menu app reference](docs/MENU_APP.md),
[command reference](docs/RTL8822CCTL.md) and the repeatable
[hardware acceptance procedure](docs/HARDWARE_ACCEPTANCE.md).

## Project documents

- [Roadmap](ROADMAP.md)
- [Changelog](CHANGELOG.md)
- [v0.0.5 release notes](RELEASE_NOTES.md)
- [Contributing](CONTRIBUTING.md)
- [Security policy](SECURITY.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)
- [Firmware provenance](firmware/README.md)

## Licence

Project code is BSD-3-Clause. Linux rtw88-derived material retains Realtek's
copyright and uses the BSD-3-Clause option of its upstream dual licence. The
unmodified RTL8822C firmware is redistributed under the included Realtek
firmware licence. See [Third-party notices](THIRD_PARTY_NOTICES.md) for exact
provenance and the firmware patent-grant limitation relevant to macOS use.
