# RTL8822C Firmware Inputs

`rtw8822c_fw.bin` is an unmodified copy of the official linux-firmware file:

<https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/rtw88/rtw8822c_fw.bin>

Expected SHA-256:

```text
3deecb31210986d98cdbfb000391e08d602a6eee4ffc883969faa2b907ab03ba
```

The official `WHENCE` records that the firmware was supplied by Realtek
engineer Yan-Hsuan Chuang and marks it redistributable under
`LICENCE.rtlwifi_firmware.txt`. The build embeds the unmodified bytes into a
generated header under `build/<Configuration>/generated/`; that header is not
a source file and must not be edited or committed.

The licence also contains a separate limited patent grant whose operating
system combination is restricted to OSI-approved licences. Redistribution is
permitted under the conditions above, but this project does not claim that the
patent grant covers use with macOS.

`rtw8822c_tables.h` is BSD-3-Clause register/table data derived from the Linux
rtw88 reference commit named in its header. Regenerate it from a checkout of
that commit with:

```sh
python3 scripts/generate_rtl8822c_tables.py \
  ../rtw88/rtw8822c_table.c firmware/rtw8822c_tables.h
make tables-check
```

It is driver data, not part of the proprietary firmware blob.
