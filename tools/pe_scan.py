#!/usr/bin/env python3
"""PE reconnaissance for Bully.exe (and other binaries we touch).

Usage:
  python tools/pe_scan.py sections <exe>
  python tools/pe_scan.py imports  <exe>
  python tools/pe_scan.py strings  <exe> <pattern> [<pattern> ...]
  python tools/pe_scan.py ctx      <exe> <offset-hex>

Dependencies: pefile (pip install pefile)
"""
import re
import sys


def load(path):
    import pefile
    return pefile.PE(path)


def cmd_sections(path):
    pe = load(path)
    print(f"EP: {hex(pe.OPTIONAL_HEADER.AddressOfEntryPoint)}  "
          f"ImageBase: {hex(pe.OPTIONAL_HEADER.ImageBase)}")
    print(f"Characteristics: {hex(pe.FILE_HEADER.Characteristics)}")
    for s in pe.sections:
        print(f"{s.Name.decode().rstrip(chr(0)):10s} "
              f"VA={hex(s.VirtualAddress):10s} VSize={hex(s.Misc_VirtualSize):9s} "
              f"RawSize={hex(s.SizeOfRawData):9s} Entropy={s.get_entropy():.2f}")


def cmd_imports(path):
    pe = load(path)
    mods = sorted(set(e.dll.decode() for e in pe.DIRECTORY_ENTRY_IMPORT))
    print("\n".join(mods))


def cmd_strings(path, patterns):
    data = open(path, "rb").read()
    for p in patterns:
        pat = p.encode() if isinstance(p, str) else p
        hits = [m.start() for m in re.finditer(re.escape(pat), data)]
        first = hex(hits[0]) if hits else "-"
        print(f"{pat.decode(errors='replace'):20s} {len(hits):4d} hits, first at {first}")


def cmd_ctx(path, offset_str):
    data = open(path, "rb").read()
    off = int(offset_str, 16)
    blob = data[off:off + 256]
    print(blob)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    cmd, path, *rest = sys.argv[1], sys.argv[2], sys.argv[3:]
    if cmd == "sections":
        cmd_sections(path)
    elif cmd == "imports":
        cmd_imports(path)
    elif cmd == "strings":
        cmd_strings(path, rest)
    elif cmd == "ctx":
        cmd_ctx(path, rest[0])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
