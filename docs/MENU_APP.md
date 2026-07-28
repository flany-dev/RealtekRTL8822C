# RealtekRTL8822CMenu

`RealtekRTL8822CMenu.app` is the v0.0.2 native macOS menu bar frontend for the
RealtekRTL8822C driver. It provides a familiar Wi-Fi control surface without
claiming integration with Apple's private Wi-Fi framework.

## Features

- menu bar icon reflecting enabled, connecting, connected, disconnected, and
  unavailable states, with four connected-signal levels;
- current SSID and driver status;
- available network list sorted by signal strength;
- open and WPA2-Personal network connection;
- password prompt and per-SSID credential storage in the macOS user Keychain;
- connected and disconnected scans;
- disconnect and Wi-Fi on/off controls;
- native **Launch at Login** control managed by macOS `SMAppService`;
- Debug-only diagnostic window with refresh and copy actions.

The app has no Dock icon and remains available through the menu bar. Quit it
from the final menu item.

The connected-signal indicator uses RSSI already attached to received Wi-Fi
frames. It does not run pings, throughput tests, active probes, or periodic
full diagnostic reports. The driver publishes a new value only when the
smoothed signal changes materially, at most once every three seconds.

Connected scanning is passive and preserves the active link by returning to
its home channel after each short off-channel visit. Results observed during
the previous five minutes are retained, so a single missed beacon does not make
an entire network disappear. Opening the menu shows the cached list immediately
and starts a background scan only when the previous scan attempt is at least
15 seconds old. **Search for Networks** remains available for an explicit
refresh. Ordinary status refreshes update the menu bar icon without rebuilding
a menu while it is being used.

New scan results are accumulated by the driver and replace the visible list
exactly once per completed sweep, including while the menu remains open. During
that pass the refresh item changes to **Searching…** and cannot submit a second
command. The app deliberately does not rebuild the menu for every received
beacon: doing that would add IORegistry and AppKit work to the input-sensitive
path. The 15-second cooldown is measured from successful scan completion; a
failed attempt is also temporarily rate-limited to avoid retry storms.

Scan channel switches follow the Linux RTL8822C ordering and use the scan
timer's dwell instead of a 20-ms busy wait. Scan-only diagnostic publication is
also suppressed. These constraints keep scanning off the interactive hot path;
they do not reduce the existing passive observation interval.

Cached display entries are not blindly trusted for association: selecting a
network not observed during the last ten seconds forces a fresh sweep. When the
32-entry cache is full, a newly observed BSS replaces the oldest entry other
than the currently connected AP.

## Driver requirement

The app requires the matching v0.0.2 or newer kext. It communicates through a
fixed-size, versioned `IOUserClient` available only to the active local user.
No privileged helper, `sudo`, shell command, arbitrary registry-property write,
DMA mapping, or register-control method is exposed.

Turning Wi-Fi off sets an explicit driver latch. The existing BSD interface may
remain visible, but scan and connection requests are rejected and the network
data path remains disabled until Wi-Fi is turned on again.

## Password handling

WPA2 passwords are entered through `NSSecureTextField` and stored as generic
password items in the current user's Keychain. A saved password is reused on
the next selection of the same SSID. If connection fails, the app removes that
saved item only when the failure indicates invalid WPA credentials. Scan,
channel, or missing-SSID failures preserve it. Credentials are also cached in
memory for the current app session to avoid repeated Keychain requests.

The driver supports WPA2-Personal/CCMP and open networks. WPA1/TKIP, WPA3/SAE,
Enterprise authentication, and required PMF remain unsupported.

## Debug and Release

The Debug app adds **Debug Info…**, which displays the ordinary operational
properties plus the kext's `Debug_*` diagnostics. The Release app omits this
menu item and does not embed the diagnostic property names. Binary checks
enforce the separation.

## Build output

```text
build/Debug/RealtekRTL8822CMenu.app
build/Release/RealtekRTL8822CMenu.app
```

The v0.0.2 GitHub Release asset `RealtekRTL8822CMenu-0.0.2.zip` contains only
the Release application bundle. Install the matching Release or Debug kext
separately; the standalone application archive does not contain a driver.

There is no install target. Copy the selected app manually to `/Applications`
or another user-selected location after installing the matching kext through
the user's existing bootloader workflow. Public development builds are ad-hoc
signed rather than Apple-notarized; on first launch, use Finder's **Open**
confirmation if Gatekeeper asks for approval.
