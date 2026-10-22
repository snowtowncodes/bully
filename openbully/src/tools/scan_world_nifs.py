#!/usr/bin/env python3
"""Scan extracted World.img NIFs: vertex/triangle counts + plausibility.

Writes openbully/data/world_nif_inventory.txt sorted by vertex count
(descending) and prints a summary. Uses the nif_test.exe harness output.
"""
import os
import subprocess
import sys

WORLD_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "..", "..", "data", "extracted", "world")
TEST_EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "..", "build", "Release", "nif_test.exe")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "..", "..", "data", "world_nif_inventory.txt")


def main() -> int:
    nifs = sorted(f for f in os.listdir(WORLD_DIR) if f.lower().endswith(".nif"))
    print(f"scanning {len(nifs)} NIFs...")
    rows = []  # (name, verts, tris, status)
    for i, name in enumerate(nifs):
        path = os.path.join(WORLD_DIR, name)
        try:
            p = subprocess.run([TEST_EXE, path], capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired:
            rows.append((name, -1, -1, "timeout"))
            continue
        verts = tris = -1
        status = "fail"
        for line in (p.stdout or "").splitlines():
            line = line.strip().rstrip(",")
            if line.startswith("vertices "):
                parts = [p.strip(",") for p in line.split()]
                verts, tris = int(parts[1]), int(parts[3])
                status = "ok"
        rows.append((name, verts, tris, status))
        if (i + 1) % 500 == 0:
            print(f"  {i + 1}/{len(nifs)}")

    rows.sort(key=lambda r: (-(r[1] if r[1] >= 0 else 0), r[0]))
    ok_rows = [r for r in rows if r[3] == "ok"]
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(f"# World.img NIF inventory - {len(rows)} NIFs, {len(ok_rows)} parse OK\n")
        f.write("# name | vertices | triangles | status\n")
        for name, v, t, s in rows:
            f.write(f"{name} | {v} | {t} | {s}\n")

    big = [r for r in ok_rows if r[1] >= 100]
    print(f"parse OK: {len(ok_rows)}, failed: {len(rows) - len(ok_rows)}")
    print(f"loadable (>=100 verts): {len(big)}")
    print(f"total verts: {sum(r[1] for r in ok_rows)}, total tris: {sum(r[2] for r in ok_rows)}")
    print(f"inventory written: {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
