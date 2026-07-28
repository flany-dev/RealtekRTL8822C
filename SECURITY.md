# Security Policy

## Supported version

Security fixes target the current `0.0.2` release line. Older development
artifacts and locally modified builds are not supported.

## Reporting

Do not post Wi-Fi passwords, PMKs, PTKs, GTKs, full EAPOL key frames, private
MAC addresses, or unredacted system logs in a public issue. Provide the minimum
reproduction and redact SSIDs/BSSIDs unless they are essential.

Report suspected key-handling, memory-safety, DMA, or privilege issues through
the repository's private GitHub Security Advisory interface. Do not open a
public issue before a coordinated fix is available.

## Operational risk

This is an experimental third-party kernel extension. A defect can panic the
kernel, corrupt DMA-visible memory, or prevent networking after wake. Test with
a recoverable OpenCore configuration and a known-good EFI backup.
