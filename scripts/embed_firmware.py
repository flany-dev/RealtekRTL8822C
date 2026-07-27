#!/usr/bin/env python3
"""Generate the deterministic in-kext RTL8822C firmware header."""

import argparse
import hashlib
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--sha256", required=True)
    args = parser.parse_args()

    data = args.input.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != args.sha256.lower():
        raise SystemExit(
            f"firmware SHA-256 mismatch: expected {args.sha256}, got {digest}"
        )

    lines = [
        "/* Generated file. Do not edit. */",
        f"/* Source SHA-256: {digest} */",
        "/* Firmware licensing is documented in THIRD_PARTY_NOTICES.md. */",
        "#ifndef RTW8822C_FW_GENERATED_H",
        "#define RTW8822C_FW_GENERATED_H",
        "",
        "static const unsigned char rtw8822c_fw[] = {",
    ]
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines.extend(
        [
            "};",
            f"static const unsigned int rtw8822c_fw_len = {len(data)};",
            "",
            "#endif",
            "",
        ]
    )
    content = "\n".join(lines)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

