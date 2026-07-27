# Contributing

Changes must preserve the confirmed RTL8822C hardware order and cite either a
runtime report, a reproducible test, or the pinned Linux rtw88 reference.

Before submitting a change:

```sh
make clean
make test
```

Keep generated files, local SDK copies, binaries, logs, private reports, SSIDs,
BSSIDs, and credentials out of patches. Do not change RF, calibration, DMA,
descriptor, or WPA ordering without a focused explanation and corresponding
hardware validation plan.

New Linux-derived code must retain the upstream copyright and compatible SPDX
identifier. Update `THIRD_PARTY_NOTICES.md` when adding a dependency or copied
data.

