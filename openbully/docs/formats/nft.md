# NFT (Gamebryo texture) format — OpenBully notes

Date: 2026-10-21 · Track 11 (texture system)

## Verdict: not DDS, not raw pixels — a Gamebryo NIF container

A `.nft` opens with the same text line as a `.nif`
(`Gamebryo File Format, Version 20.3.0.9\n`) and carries the same block
table. The difference is content: texture-only blocks, never geometry.
Confirmed by hex dump of `TXD\fonts.nft` (loose file, 328,246 bytes) and by
parsing extracted World.img textures.

Block types seen in `.nft` files:

| Type | Role |
|---|---|
| `NiPixelData` | pixel format header + mipmap table + raw pixels |
| `NiSourceTexture` | ties a source file name (`.tga`) to a NiPixelData ref |
| `NiPalette` | 256×RGBA palette (PAL8 textures) |
| `NiStringExtraData` / `NiIntegerExtraData` | export tool metadata |
| `NiSourceCubeMap` | same layout as NiSourceTexture (external, names an .nft) |

## NiPixelData layout (byte-verified by bully-re on all 35,635 retail blocks)

```
u32  pixelFormat      0 RGB8 | 1 RGBA8 | 2 PAL8 | 4 DXT1 | 6 DXT5 (5=DXT3 never ships)
u8   bitsPerPixel
i32  rendererHint     -1
u32  extraData
u8   flags
u32  tiling
u8   srgb
4 x { u32 type; u32 convention; u8 bitsPerChannel; u8 isSigned; }
i32  palette          NiPalette block ref, -1 if none
u32  numMipmaps
u32  bytesPerPixel
numMipmaps x { u32 width; u32 height; u32 offset; }
u32  numPixels        bytes of one face, all mips
u32  numFaces
u8   pixels[numPixels * numFaces]
```

Block size is exactly `79 + 12*numMipmaps + numPixels*numFaces`.

DXT payloads stay little-endian even in the 131 big-endian CS_* files
(only NIF fields are swapped).

## Texture dictionary model (the load-time gotcha)

NIF shapes reference a base texture by NAME (NiTexturingProperty →
NiSourceTexture → stripped `.tga` base name). The pixels live in a shared
dictionary `.nft` that is usually NOT the model's own name: only ~2,450 of
5,720 models have a same-named `.nft`. Examples measured in this repo:

- `lowrider_sd1_d` (3_06CarBarr01) lives in `bu_carBarricade.nft`
- `p_ipoor_1950fridge_d` (1950Fridge) lives in `ppoor.nft`
- `sc29_flight_d` appears in 9 dictionaries (first alphabetical wins)

OpenBully therefore builds a name→NFT index at startup
(`Engine::BuildTextureIndex`, via `ListNftTextureNames`): one header-only
scan of every `.nft`, mapping 16,434 texture names across the 4,461
extracted dictionaries. `Engine::AcquireTexture` resolves
name → dictionary → `LoadNftFile(path, wantName)`, which decodes the
matching `NiSourceTexture`→`NiPixelData` (not just the first block).

## OpenBully loader (`src/engine/formats/nft_loader.h/cpp`)

- `LoadNftFile(path, wantName)` → `NftTexture` (RGBA8, mip 0 only)
- Formats decoded: RGB8, RGBA8 (channel order from the NiPixelFormat
  channel types), PAL8 (+NiPalette), DXT1, DXT5
- `ListNftTextureNames(path)` → all source names, no pixel decode
- `MakePlaceholderTexture()` → 4×4 magenta/black checker (proves sampling)
- Renderer uploads via `bgfx::createTexture2D(RGBA8)`; sampler
  `s_matBase` bound in `DrawMesh3DRange` per submesh draw.

## Verification (2026-10-21)

- 4,461/4,469 extracted NFTs (8 are the known-corrupt CS_* files documented
  in bully-re/docs/nft.md)
- Runtime: 32/32 meshes, 182 submeshes → 180 textured + 2 placeholder,
  0 texture failures (full 32-mesh scene)
- Single-mesh runs (`OPENBULLY_MESH=ASY_ABlock.nif`): 18/18 submeshes
  textured; screenshot `docs/asylum_shot.png` shows the facade with window
  texel patterns (no magenta placeholder anywhere)
- Regression: nif_test passes on cube.nif + first 200 world NIFs with the
  texture-reference parsing active

## Engine-side fixes that fell out of verification

- **CMakeLists**: the shader-stamp custom commands were collected into
  `OPENBULLY_SHADER_TARGETS` but no `add_custom_target` consumed them, so a
  re-configure silently stopped compiling shaders and the app ran on stale
  binaries. Fixed: `openbully_shaders` target + `add_dependencies(openbully ...)`.
- **Engine bounds**: camera fit used ALL vertices; NIFs can carry orphan
  vertices far outside the drawn surface (3_06CarBarr01: ±540-unit orphans
  vs a 4-unit mesh), dragging the auto-fit camera off the geometry. Fixed:
  bounds now walk `mesh.indices` only.

## Debug env hooks (this track unless noted)

| Var | Effect |
|---|---|
| `OPENBULLY_MESH=<file.nif>` | load only that mesh (tight camera fit) |
| `OPENBULLY_NOCULL=1` | disable backface culling (view flat geometry from behind) |
| `OPENBULLY_DEBUG_CAM=1` | identity view/proj + 0.002 scale (bypass camera) |
| `OPENBULLY_STATS=1` | bgfx STATS overlay (draw calls, submitted tris) |
| `OPENBULLY_SHOT=<path>` | bgfx native TGA screenshot ~1s in (Track 12) |
| `OPENBULLY_DEMO_ONLY`, `OPENBULLY_MESH_CAP`, `OPENBULLY_CAM_ZOOM`, `OPENBULLY_NO_TEXTURES`, `OPENBULLY_CAM_DEMO` | Track 10/12 hooks |
