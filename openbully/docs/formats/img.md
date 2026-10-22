# Bully SE: IMG Archive Format

Bully: Scholarship Edition (PC) stores most of its game assets in **IMG v1**
archives — the same archive family used by GTA III and Vice City. The PC port
of Bully was built on the **Gamebryo (NetImmerse)** engine rather than
RenderWare, so *the contents* of these archives are NIF/NFT (Gamebryo) files,
not DFF/TXD. Only the container format is shared with classic GTA.

## Variants found in a Bully SE install

| Archive | Location | Format | Entries | Contents |
|---|---|---|---|---|
| `World.img/.dir` | `Stream/` | IMG v1 | 11,980 | streamed world assets: `.nif`, `.nft`, `.agr`, `.lip`, `.col`, `.cat`, `.ipb`, `.lur` (~7.8 GiB) |
| `ide.img/.dir` | `Objects/` | IMG v1 | 77 | `.idb` object-definition tables |
| `Act.img/.dir` | `Act/` | IMG v1 | 479 | `.cat` files |
| `Cuts.img/.dir` | `Cuts/` | IMG v1 | 863 | cutscene assets: `.mxd`, `.agr`, `.hxd`, `.dat`, `.inf`, `.sub` |
| `Scripts.img` | `Scripts/` | **not IMG** | — | concatenated Lua 5.1 bytecode chunks (magic `1B 4C 75 61`), no directory |
| `<Language>.img` | `Config/Text/` | — | — | localized text resources |

Only the four `.img`/`.dir` pairs are IMG archives. `Scripts.img` is a plain
byte stream of Lua chunks and should not be parsed with an IMG reader.

## IMG v1 layout

An archive is a pair of files with the same basename:

### `.dir` — directory (no header)

Repeated until end of file; entry count = `filesize / 32`:

```c
struct DirEntry {          // 32 bytes, little-endian
    uint32_t offset;       // start of data, in 2048-byte sectors
    uint32_t size;         // data length, in 2048-byte sectors (padded)
    char     name[24];     // filename, null-padded (e.g. "Algie1.lur")
};
```

- **Sector size is 2048 bytes.** Byte offset = `offset * 2048`; byte size =
  `size * 2048`. The format mirrors CD-ROM sectors: every stored file starts
  on a sector boundary and its allocation is padded to a whole sector.
- The **exact unpadded file size is not recoverable** from the archive alone;
  consumers must tolerate trailing padding (usually zero bytes).
- Directory entries appear in the same order as the data they describe, with
  no gaps between files — the game may coalesce adjacent reads.
- Offsets in `World.dir` are relative to the start of `World.img` (the `.img`
  holds only raw data, no header of its own).

### RESOLVED: `World.img` on this machine is NOT the PC archive (2026-10-21, Track 3)

The 7.8 GiB `World.img` was investigated byte-by-byte while wiring the NIF
loader (Track 3). Findings:

- The region covered by `World.dir` (~1.9 GiB) does NOT contain the indexed
  entries: most sampled entry offsets read as zero-filled, and the bytes that
  are present do not match any NIF structure (no `Gamebryo File Format` string
  anywhere in the first 200 MiB, nor anywhere in the file).
- The file is an **Xbox 360 XGD2 DVD disc image**: ISO9660 PVD at sector 16
  (`XGD2DVD_NTSC`), `MICROSOFT*XBOX*MEDIA` XDVDFS volume descriptor at sector
  129,856, an XDVDFS directory tree (root `data`, `$SystemUpdate`,
  `default.xex`), and MPEG pack headers (`00 00 01 BA`) near the file tail.
  File mtime 1996-12-24 and Stream folder mtime 2026-10-10 indicate it was
  swapped in after the original analysis (bully-re/docs/journal.md, 2026-09-22,
  describes a readable 1.9 GiB World.img with 11,980 entries).
- Consequence: streamed `.nif`/`.nft` assets are **not extractable from this
  install** until the PC `World.img` is restored (Steam "verify integrity",
  or re-extract from the retail MSI — the bully-re journal documents the
  MSI-cab extraction path used on 2026-09-21).
- The NIF loader (Track 3) is validated against the loose retail files that
  ship outside the archive: `Graphics/cube.nif`, `Models/*.nif`,
  `Models/SkyDome/*.nif` — same version (0x14030009), same block layout,
  including both endianness variants.

Historical note (superseded by the above): the directory region was earlier
believed to be self-consistent; sampling during Track 3 disproved that.

### `.img` — data

No header. Concatenation of the stored files at their directory offsets.

## Version history (Rockstar 3D-era context)

| Version | Games | Directory |
|---|---|---|
| v1 | GTA III, VC, **Bully SE (PC)** | separate `.dir` file |
| v2 | GTA SA | embedded at start of `.img`, `"VER2"` magic |
| v3 | GTA IV | embedded, `R*N.` magic, optional AES-128-ECB header encryption |

## Notes for the OpenBully pipeline

- **Reading**: parse `.dir` with `struct.unpack("<II24s")`, then `pread` from
  the `.img`. Safe and fully random-access; the 7.8 GiB `World.img` never
  needs to be loaded into memory.
- **Contents are Gamebryo, not RenderWare**:
  - `.nif` — NetImmerse/Gamebryo meshes. Bully SE PC uses NIF version
    14.0.3.0 (`14030009`). Some shipped files are big-endian on PC.
  - `.nft` — textures; identical container to `.nif` (contains
    `NiSourceTexture`/`NiPixelData` blocks), just a different extension.
    Pixel formats seen: RGB8, RGBA8, DXT1, DXT3, DXT5_ALT, PAL8.
  - `.col` — collision; uses `COL3` magic but with a Bully-specific layout
    that differs from GTA SA's.
  - Other extensions (`.agr` animations, `.lip` lip sync, `.cat`, `.ipb`,
    `.lur`, `.idb`) are Bully-specific and undocumented upstream.
- **Extracting textures**: rename `.nft` → `.nif` to open in NifSkope/pyffi.
- Existing community tooling: `bullyfury` (Python, PyPI — IMG + IDB/IPB/NIF
  parsing), IMGEditor, gta-img (Rust), MultiEx/GTA modding tools.

## References

- GTAMods wiki, "IMG archive": https://gtamods.com/wiki/IMG_archive
- Xentax wiki, "GTA Series IMG DIR"
- Bully Modding Documentation (simonbestia): models are `.nif` on 360/PC
- `bullyfury` on PyPI — Python toolkit for these exact formats
