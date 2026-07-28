# RealtekRTL8822C

RealtekRTL8822C is an x86_64 macOS PCIe driver for the Realtek RTL8822CE
(`10ec:c822`). Version `0.0.1` is the first public release.

The driver is implemented as an `IOEthernetController` with a companion command-line
utility, `rtl8822cctl`. It does not integrate with Apple's native Wi-Fi menu.

## Supported configuration

The v0.0.1 reference configuration is an RTL8822CE cut D system running macOS
15 through OpenCore with AppleVTD/IOMMU enabled and the legacy PCI interrupt
path.

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
driver failure. This is a single-system hardware matrix, not a claim of
compatibility with every RTL8822CE laptop or firmware configuration.

## Channel policy and limitations

5 GHz transmission is limited to FCC/US non-DFS channels 36/40/44/48 and
149/153/157/161/165 and requires valid board-specific EFUSE power data. The
driver does not derive a regulatory domain from macOS; users are responsible
for operating only on channels legal in their location.

The following are not supported in v0.0.1:

- DFS/CAC and 5 GHz transmission outside the guarded channel set;
- WPA1/TKIP, WPA3/SAE, Enterprise authentication, and required PMF;
- Apple's Wi-Fi menu, IO80211 integration, AirDrop, and AWDL;
- Apple Silicon or Realtek devices other than RTL8822CE `10ec:c822`;
- automatic installation or modification of an OpenCore configuration.

HT40/VHT40 selection is implemented, but the published reference matrix is
centered on hardware-confirmed 20 MHz and VHT80 links. Connected scanning was
validated on VHT80; additional AP, bandwidth, and 2.4 GHz connected-scan
coverage is welcome as post-release compatibility evidence.

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
- `build/Release/RealtekRTL8822C.kext`, `build/Release/rtl8822cctl`, and
  `build/Release/RealtekRTL8822C.dSYM`.

Debug publishes `Debug_*` IORegistry diagnostics and enables diagnostic kernel
logging. Release compiles out those properties and logging and exposes only
operational status. Automated binary checks enforce this boundary.

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

`make package` creates the complete maintainer archive plus two kext-only
GitHub Release assets:

- `build/package/RealtekRTL8822C-0.0.1-Release.zip`;
- `build/package/RealtekRTL8822C-0.0.1-Debug.zip`.

Each of those two archives contains only `RealtekRTL8822C.kext`. The Release
asset is intended for ordinary use; Debug is intended for diagnostic reports.
`make package-local` creates the complete locally named test archive. None of
these commands installs anything.

## Runtime control

Run `rtl8822cctl help` for the command list. Interactive `connect` prompts for a
password without echoing it or placing it in shell history. See the
[command reference](docs/RTL8822CCTL.md) and the repeatable
[hardware acceptance procedure](docs/HARDWARE_ACCEPTANCE.md).

## Project documents

- [Roadmap](ROADMAP.md)
- [Changelog](CHANGELOG.md)
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
