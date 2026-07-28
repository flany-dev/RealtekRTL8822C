# Third-Party Notices

This file records the provenance and redistribution status of code and binary
components used by RealtekRTL8822C v0.0.2.

## Linux rtw88

- Upstream project: Linux rtw88 wireless driver
- Development reference: <https://github.com/lwfinger/rtw88>
- Pinned reference commit: `a56bcd26e770257612a0803249cbd4095fc6feca`
- Upstream copyright: Copyright (c) 2018-2019 Realtek Corporation
- Upstream licence: `GPL-2.0 OR BSD-3-Clause`
- Selected licence for this project: BSD-3-Clause
- Local derived material: RTL8822C register/table data, power sequences,
  PCI/DMA descriptor handling, firmware and RF calibration sequencing,
  security CAM handling, TX/RX behavior, and related constants in
  `src/RealtekRTL8822C.cpp`, `include/power_seq.h`, and
  `firmware/rtw8822c_tables.h`.

The original copyright and the selected SPDX identifier must remain attached
to generated or adapted Realtek material.

## MacKernelSDK

- Project: Acidanthera MacKernelSDK
- Upstream: <https://github.com/acidanthera/MacKernelSDK>
- Pinned local commit: `05094e5e88cec7caedbfb35e8449ed0db94bf95b`
- Licence: Apple Public Source License 2.0 and per-file upstream notices
- Role: build-time headers and compatibility support
- Redistribution decision: use as a pinned external dependency/submodule and
  retain its own licence; do not absorb it into the RealtekRTL8822C licence.

## RTL8822C Firmware

- Official distribution: Linux firmware repository
  <https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/rtw88/rtw8822c_fw.bin>
- Provenance recorded by official `WHENCE`: sent to Larry Finger by Realtek
  engineer Yan-Hsuan Chuang.
- Local input: `firmware/rtw8822c_fw.bin`
- SHA-256:
  `3deecb31210986d98cdbfb000391e08d602a6eee4ffc883969faa2b907ab03ba`
- Exact-match verification: the official linux-firmware file has the same
  size (202600 bytes) and SHA-256 as the local input.
- Embedded representation: generated at build time as
  `build/<Configuration>/generated/rtw8822c_fw.h`.
- Licence: Realtek binary firmware redistribution licence, reproduced in
  `firmware/LICENCE.rtlwifi_firmware.txt`.
- Licence SHA-256:
  `a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e`
- Redistribution status: permitted in unmodified binary form when the Realtek
  copyright notice, conditions, and disclaimer accompany the distribution.
- Patent notice: the separate limited patent grant in the firmware licence
  covers the firmware alone, or its combination with an operating system
  licensed under an OSI-approved licence. This project makes no representation
  that the patent grant extends to use with macOS.

The build verifies both pinned hashes. Public source and binary distributions
must include `firmware/LICENCE.rtlwifi_firmware.txt`; the firmware must not be
modified, reverse engineered, decompiled, or disassembled.
