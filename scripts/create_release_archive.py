#!/usr/bin/env python3
"""Create a deterministic release ZIP without AppleDouble metadata."""

from __future__ import annotations

import argparse
import pathlib
import stat
import zipfile


FIXED_TIME = (2026, 1, 1, 0, 0, 0)


def add_directory(archive: zipfile.ZipFile, name: str) -> None:
    info = zipfile.ZipInfo(name.rstrip("/") + "/", FIXED_TIME)
    info.create_system = 3
    info.external_attr = (stat.S_IFDIR | 0o755) << 16
    archive.writestr(info, b"")


def add_file(archive: zipfile.ZipFile, path: pathlib.Path, name: str) -> None:
    info = zipfile.ZipInfo(name, FIXED_TIME)
    info.create_system = 3
    mode = 0o755 if path.stat().st_mode & stat.S_IXUSR else 0o644
    info.external_attr = (stat.S_IFREG | mode) << 16
    info.compress_type = zipfile.ZIP_DEFLATED
    archive.writestr(info, path.read_bytes(), compresslevel=9)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    source = args.source.resolve()
    if not source.is_dir():
        parser.error(f"source directory does not exist: {source}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    root = source.name
    with zipfile.ZipFile(args.output, "w") as archive:
        add_directory(archive, root)
        for path in sorted(source.rglob("*"), key=lambda item: item.as_posix()):
            relative = pathlib.PurePosixPath(root, *path.relative_to(source).parts)
            if path.is_dir():
                add_directory(archive, relative.as_posix())
            elif path.is_file():
                add_file(archive, path, relative.as_posix())
            else:
                raise ValueError(f"unsupported release entry: {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
