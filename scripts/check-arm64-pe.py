#!/usr/bin/env python3
"""Fail if Windows EXE/DLL PE images in a staging directory are not ARM64.

Usage: python scripts/check-arm64-pe.py dist/Sunshine-Foundation-Windows-ARM64
Does not execute the files or check whether third-party drivers are signed.
"""
import argparse
import struct
from pathlib import Path

ARM64 = 0xAA64


def machine(path: Path):
    with path.open('rb') as f:
        head = f.read(64)
        if len(head) < 64 or head[:2] != b'MZ':
            return None
        offset = struct.unpack_from('<I', head, 0x3C)[0]
        f.seek(offset)
        header = f.read(6)
        if len(header) < 6 or header[:4] != b'PE\x00\x00':
            return None
        return struct.unpack_from('<H', header, 4)[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('folder', type=Path)
    args = parser.parse_args()
    if not args.folder.is_dir():
        raise SystemExit('Not a directory')
    files = sorted(p for p in args.folder.rglob('*') if p.is_file() and p.suffix.lower() in ('.dll','.exe'))
    if not files:
        raise SystemExit('No PE binaries found')
    fails = []
    for p in files:
        m = machine(p)
        label = 'ARM64' if m == ARM64 else ('not PE' if m is None else f'machine 0x{m:04x}')
        print(f'{p.relative_to(args.folder)}: {label}')
        if m != ARM64:
            fails.append(str(p))
    if fails:
        raise SystemExit(f'FAILED: {len(fails)} non-ARM64 binaries present')
    print(f'PASS: {len(files)} Windows EXE/DLL binaries are ARM64')


if __name__ == '__main__':
    main()
