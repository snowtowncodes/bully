#!/usr/bin/env python3
"""List (and optionally extract) the contents of a Bully: Scholarship Edition IMG v1 archive.

Bully SE (PC) uses the classic Rockstar "IMG v1" archive layout, same as GTA III/VC:
  - `<name>.dir`: directory of 32-byte entries (no header), repeated until EOF:
      u32 offset   (in 2048-byte sectors)
      u32 size     (in 2048-byte sectors)
      char[24]     filename, null-padded
  - `<name>.img`: raw concatenated file data, no header, sector-aligned.

Read-only by design: this tool never writes to the game files.

Usage:
  python extract_img.py <archive>              # list entries
  python extract_img.py <archive> -v           # list with offsets/sizes
  python extract_img.py <archive> -x OUTDIR    # extract all entries to OUTDIR
  python extract_img.py <archive> -x OUTDIR --filter nif
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

SECTOR = 2048
DIR_ENTRY = struct.Struct("<I I 24s")


@dataclass(frozen=True)
class ImgEntry:
    name: str
    offset: int  # sector units
    size: int    # sector units (padded)

    @property
    def byte_offset(self) -> int:
        return self.offset * SECTOR

    @property
    def byte_size(self) -> int:
        return self.size * SECTOR


class ImgArchive:
    def __init__(self, archive: Path):
        self.archive = archive
        self.dirfile = archive.with_suffix(".dir")
        self.entries: list[ImgEntry] = []

    def read_directory(self) -> None:
        if not self.dirfile.exists():
            # IMG v2 (GTA SA "VER2") stores entries inside the .img; detect it.
            with self.archive.open("rb") as f:
                magic = f.read(4)
            if magic == b"VER2":
                raise NotImplementedError(
                    f"{self.archive.name}: IMG v2 (VER2) not supported; Bully SE uses v1"
                )
            raise FileNotFoundError(
                f"Directory file not found: {self.dirfile} "
                f"(Bully SE uses split .img/.dir pairs)"
            )
        data = self.dirfile.read_bytes()
        if len(data) % DIR_ENTRY.size:
            print(
                f"warning: {self.dirfile.name} size {len(data)} not a multiple of "
                f"{DIR_ENTRY.size}; trailing {len(data) % DIR_ENTRY.size} bytes ignored",
                file=sys.stderr,
            )
        self.entries = []
        for i in range(len(data) // DIR_ENTRY.size):
            off, size, raw = DIR_ENTRY.unpack_from(data, i * DIR_ENTRY.size)
            name = raw.split(b"\x00", 1)[0].decode("latin-1")
            self.entries.append(ImgEntry(name, off, size))

    def read_entry(self, entry: ImgEntry) -> bytes:
        with self.archive.open("rb") as f:
            f.seek(entry.byte_offset)
            return f.read(entry.byte_size)

    def extract(self, outdir: Path, filter_ext: str | None = None) -> int:
        outdir.mkdir(parents=True, exist_ok=True)
        n = 0
        for e in self.entries:
            if filter_ext and not e.name.lower().endswith("." + filter_ext.lower().lstrip(".")):
                continue
            # Security: reject path traversal attempts
            if ".." in e.name or e.name.startswith("/") or e.name.startswith("\\"):
                raise ValueError(f"Unsafe entry name: {e.name}")
            out_path = (outdir / e.name).resolve()
            if not out_path.is_relative_to(outdir.resolve()):
                raise ValueError(f"Path traversal attempt: {e.name}")
            data = self.read_entry(e)
            out_path.write_bytes(data)
            n += 1
        return n


def human(n: int) -> str:
    value = float(n)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if value < 1024:
            return f"{value:.1f} {unit}" if unit != "B" else f"{n} B"
        value /= 1024
    return f"{value:.1f} TiB"


def main() -> int:
    ap = argparse.ArgumentParser(description="List/extract Bully SE IMG v1 archive")
    ap.add_argument("archive", type=Path, help="path to the .img file")
    ap.add_argument("-v", "--verbose", action="store_true", help="show offsets and sizes")
    ap.add_argument("-x", "--extract", type=Path, metavar="DIR", help="extract entries to DIR")
    ap.add_argument("--filter", metavar="EXT", help="only extract entries ending in .EXT")
    args = ap.parse_args()

    arc = ImgArchive(args.archive)
    try:
        arc.read_directory()
    except (FileNotFoundError, NotImplementedError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    total = sum(e.byte_size for e in arc.entries)
    print(f"{args.archive.name}: {len(arc.entries)} entries, {human(total)} data "
          f"(sector size {SECTOR})")
    if args.verbose:
        for e in arc.entries:
            print(f"  {e.offset:>12} {e.size:>8}  {e.byte_size:>12}  {e.name}")
    else:
        for e in arc.entries:
            print(f"  {e.name}")

    if args.extract:
        n = arc.extract(args.extract, args.filter)
        print(f"extracted {n} files to {args.extract}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
