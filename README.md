# RealtekRTL8822C

RealtekRTL8822C is an x86_64 macOS PCIe driver for the Realtek RTL8822CE
(`10ec:c822`). Version `0.0.2` is the current release.

The driver is implemented as an `IOEthernetController` with a native menu bar
application, `RealtekRTL8822CMenu`, and the companion command-line utility
`rtl8822cctl`. It does not replace Apple's native Wi-Fi framework or provide
Apple wireless services.

## Supported configuration

The reference configuration is an RTL8822CE cut D system running macOS 15
through OpenCore on the legacy PCI interrupt path. AppleVTD/IOMMU is not a
driver requirement: the reference machine has been validated without IOMMU and
with DMA protection enabled. IOMMU and DMA-protection policy is platform- and
bootloader-specific; use the configuration that is stable for the rest of the
machine.

Hardware-confirmed functionality includes:

- scanning on 2.4 GHz channels 1-11 and guarded non-DFS 5 GHz channels;
- open and WPA2-Personal/CCMP association;
- DHCP, DNS, and sustained bidirectional IP traffic;
- HT and VHT operation, WMM, TX/RX Block Ack, aggregation, and RX reorder;
- 20 MHz and VHT80 links, including lower and upper 5 GHz operation;
- disconnect, reconnect, AP switching, interface down/up, and sleep/wake
  recovery;
- connected off-channel scanning with restoration of the active link;
- separate diagnostic Debug and log-free Release profiles.

The final Release profile has been tested on the reference system through
normal traffic, sustained load, reconnect, and sleep/wake without a known
driver failure. Sustained bidirectional traffic remained stable in the final
v0.0.2 run. This remains a single-system matrix, not a claim of compatibility
with every RTL8822CE laptop or firmware configuration.

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
centered on hardware-confirmed 20 MHz and VHT80 links. Connected scanning was
validated on VHT80; additional AP, bandwidth, and 2.4 GHz connected-scan
coverage is welcome as post-release compatibility evidence.

Known issue: on the reference system, the built-in touchpad can briefly lag
during network scanning or connection. This issue is known and remains open in
the current driver.

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

- `build/Debug/RealtekRTL8822C.kext`, `build/Debug/rtl8822cctl`, and
  `build/Debug/RealtekRTL8822CMenu.app`;
- `build/Release/RealtekRTL8822C.kext`, `build/Release/rtl8822cctl`,
  `build/Release/RealtekRTL8822CMenu.app`, and
  `build/Release/RealtekRTL8822C.dSYM`.

Debug publishes `Debug_*` IORegistry diagnostics, enables diagnostic kernel
logging, and adds a **Debug Info** window to the menu app. Release compiles out
those properties, logging, diagnostic report keys, and the Debug UI. Automated
binary checks enforce this boundary.

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

- `build/package/RealtekRTL8822C-0.0.2-Release.zip`;
- `build/package/RealtekRTL8822C-0.0.2-Debug.zip`;
- `build/package/RealtekRTL8822CMenu-0.0.2.zip`.

Each of those two archives contains only `RealtekRTL8822C.kext`. The Release
asset is intended for ordinary use; Debug is intended for diagnostic reports.
The application archive contains only `RealtekRTL8822CMenu.app` and is a normal
v0.0.2 release download alongside the two kext archives.
`make package-local` creates the complete locally named test archive. None of
these commands installs anything.

## Runtime control

Launch `RealtekRTL8822CMenu.app` for menu bar scanning, connection,
disconnect, and Wi-Fi on/off control. WPA2 credentials are stored in the user
Keychain. Driver commands use a narrow local-user `IOUserClient` and do not
require `sudo` with the v0.0.2 kext.

Run `rtl8822cctl help` for the command list. Interactive `connect` prompts for a
password without echoing it or placing it in shell history. See the
[menu app reference](docs/MENU_APP.md),
[command reference](docs/RTL8822CCTL.md) and the repeatable
[hardware acceptance procedure](docs/HARDWARE_ACCEPTANCE.md).

## Project documents

- [Roadmap](ROADMAP.md)
- [Changelog](CHANGELOG.md)
- [v0.0.2 release notes](RELEASE_NOTES.md)
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
