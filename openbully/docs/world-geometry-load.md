# World.img Level Geometry — Load Report

Date: 2026-10-21 · Task: "Load World.img level geometry once decryption is available"

## Archive provenance (resolved)

The original `World.img` was a wrong file (an Xbox 360 XGD2 DVD dump), which
is why its NIFs looked "encrypted". The user replaced it with the real PC
archive — no decryption was ever needed. All 5,720 NIFs extract as plaintext
Gamebryo and parse as version `0x14030009`.
`src/tools/extract_img.py` — the task's expected signal never appeared; the
signal that landed was clean `Gamebryo File Format` magic in the files).
Every NIF now parses as version `0x14030009`.

## Inventory (see `data/world_nif_inventory.txt` for all 5,720)

- Parse OK: **5,698** / 5,720 (22 failures are non-geometry/edge cases)
- Loadable (≥100 verts): **4,275**
- Total geometry: **5,637,564 verts / 4,417,510 triangles**

Top meshes by vertex count:
## Engine changes (Phase 3)

- `renderer.h/.cpp`: `GpuMesh` multi-mesh upload (`UploadMeshGpu`,
  `DestroyGpuMesh`), 3D draw path (`DrawMesh3D`) with MVP matrix, depth
  buffer, CW culling (Gamebryo LH), camera state (`SetCamera`), separate
  view-1 pipeline + `u_mvp` uniform.
- `engine.h/.cpp`: `LoadWorldGeometry(dir, max)` — scans a directory,
  parses NIFs (`LoadNifFile`), filters ≥100 verts, uploads, computes bounds,
  fits an orbit camera. `Render()` draws all world meshes; the Phase-2 test
  triangle remains as a 2D overlay for the regression baseline.
  Verified demo meshes (JimmysRoom01, 3_06CarBarr01, 1950Fridge) load first;
  `OPENBULLY_DEMO_ONLY=1` restricts the scene to the trio.
- `nif_loader.h/.cpp`: full NiNode hierarchy decode — per-node translation,
  row-major rotation, scale, children; parent links derived post-pass;
  `NifWorldTransform()` composes child-to-root (column-major 4x4, bgfx
  convention). Engine bakes world transforms into vertices/normals at load.
- `vs_mesh3d.sc` / `fs_mesh3d.sc`: MVP transform + fake sun lighting + UV
  tint. Registered in `CMakeLists.txt` for d3d11/opengl/vulkan
  (`varying.def.sc` gained `a_normal : NORMAL`).
- `scan_world_nifs.py` / `capture_by_pid.ps1` / `capture_printwindow.ps1`:
  inventory + screenshot tooling.
  `DestroyGpuMesh`), 3D draw path (`DrawMesh3D`) with MVP matrix, depth
  buffer, CW culling (Gamebryo LH), camera state (`SetCamera`), separate
  view-1 pipeline + `u_mvp` uniform.
- `engine.h/.cpp`: `LoadWorldGeometry(dir, max)` — scans a directory,
  parses NIFs (`LoadNifFile`), filters ≥100 verts, uploads, computes bounds,
  fits an orbit camera. `Render()` draws all world meshes; the Phase-2 test
  triangle remains as a 2D overlay for the regression baseline.
- `vs_mesh3d.sc` / `fs_mesh3d.sc`: MVP transform + fake sun lighting + UV
  tint. Registered in `CMakeLists.txt` for d3d11/opengl/vulkan
  (`varying.def.sc` gained `a_normal : NORMAL`).
- `scan_world_nifs.py` / `capture_by_pid.ps1` / `capture_printwindow.ps1`:
  inventory + screenshot tooling.

## Verification

- Release build: **pass**
- Full scene run: 32/32 meshes loaded, 0 failed, ~240 fps (`Direct3D 11`)
- Demo-only run: 3/3 verified meshes (JimmysRoom01 694 verts,
  3_06CarBarr01 1220, 1950Fridge 456), ~235 fps, transforms applied
- Screenshot: `docs/geometry_shot.png` — demo trio (room interior, barrier
  posts, props) rendered transform-positioned with depth-tested shading.

## Known limitations (next steps)

- First-32 cap: full streaming/culling lands later.
- Skinned meshes: skinning is skipped; skinned geometries render at their
  bind-pose node transform (may float if the bind pose is detached).
- No textures/materials yet — flat shaded.

## Track 10 — Input & Camera (2026-10-22)

- `src/game/input.h/.cpp`: `Input::Poll()` snapshots `SDL_GetKeyboardState`
  (W/A/S/D, Space, LCtrl, LShift, Escape) and `SDL_GetRelativeMouseState`
  deltas; relative mouse mode set once at startup; `SDL_QUIT` drained
  defensively. Logical `Button` enum keeps SDL scancodes out of game code.
- `src/game/camera.h/.cpp`: free-fly FPS camera (glm, LH/Y-up matching
  Gamebryo geometry). Yaw/pitch from mouse (0.002 rad/px, pitch clamped
  ~89°), horizontal WASD on the yaw plane, Space/LCtrl vertical, LShift
  ×4 sprint, 5 units/sec base. `GetViewMatrix()` builds `glm::lookAtLH`.
- Renderer: `SetCameraView()` + `GetProjection()` replace eye/target/up —
  engine supplies the view matrix; projection (60° LH, ZO for D3D,
  0.1/20000) is built in `Renderer::Init`. Engine composes
  `mvp = proj * view * model` per mesh.
- Spawn: union bounds of loaded meshes; camera sits 0.9·radius+15 outside
  the −Z face at +0.1·radius height, aimed at the bounds center. Fallback
  (no geometry): (0, 5, −10) looking at origin.
- `Engine::Update()` polls input + integrates camera; `Render()` uploads
  the view matrix and builds MVPs. `Engine::QuitRequested()` (Escape) is
  checked by the main loop.
- `OPENBULLY_CAM_DEMO=1`: scripted flythrough (forward → sprint → mouse
  pan → strafe → rise) driving the same `Camera::Update` path for
  automated verification — OS key injection is unreliable from harnesses.

### Verification (demo flythrough, 240 fps vsync)

- Position log: forward −Z at 5 u/s → ×4 sprint; yaw pan 0.00→0.38 rad;
  strafe ΔX ≈ +4.7; rise ΔY ≈ +3.5 — all deltas match speed × time.
- Frames `docs/camera_seq_a/b/c.bmp`: geometry cluster moves right →
  center and scales up as the camera advances; view matrix + projection +
  world geometry + Track 11 textures render together.
- Mouse look and movement verified in-process (demo drives the identical
  code path); ESC quit polls the same snapshot — live keypress injection
  was not available from the test harness.
- Build: Release pass. Run: 32/32 meshes, ~240 fps, D3D11.

## Track 11 — NFT Texture System (2026-10-21)

- `src/engine/formats/nft_loader.h/.cpp`: `.nft` = Gamebryo NIF container
  (not DDS). Decodes RGB8 / RGBA8 / PAL8 / DXT1 / DXT5 to RGBA8 (mip 0);
  `LoadNftFile(path, wantName)` decodes a specific texture inside a
  dictionary NFT; `ListNftTextureNames` powers the index. Format notes in
  `docs/formats/nft.md`.
- Texture dictionary: NIFs name a base texture; pixels live in shared
  dictionaries (only ~2,450/5,720 models have a same-named .nft).
  `Engine::BuildTextureIndex` scans all 4,461 extracted .nft files once
  (16,434 names), `AcquireTexture` resolves name → dictionary → GPU
  texture with negative caching.
- NIF loader: shapes now decode `NiTexturingProperty` → base
  `NiSourceTexture` and populate `NifGeometryData::textureName`;
  `MeshSubMesh::textureName` carries it through the mesh contract.
- Renderer: `LoadTexture` / `UploadTexture` / `BindTexture` /
  `DestroyTexture` + `GpuTexture`; sampler `s_matBase` bound per submesh
  draw in `DrawMesh3DRange`; magenta/black placeholder proves sampling for
  unresolvable names. `DrawMesh3DSub` draws an index range.
- Engine: per-submesh draw loop with texture binding (no NFT I/O in the
  frame loop — textures resolve at load time into `WorldMesh::SubRange`).
- Engine fixes from verification: camera-fit bounds now walk
  `mesh.indices` (orphan vertices up to ±540 units sat far outside the
  drawn 4-unit surface and dragged the old camera off-target); CMake
  shader compilation landed in an `openbully_shaders` target (the stamps
  existed but nothing depended on them — a re-configure silently stopped
  refreshing shader binaries, which cost the day's debugging).
- Verification: 4,461/4,469 NFTs extracted (8 known-corrupt per
  bully-re/docs/nft.md); full scene 32/32 meshes, 180 textured + 2
  placeholder submeshes, 0 texture failures; single-mesh
  `OPENBULLY_MESH=ASY_ABlock.nif` → 18/18 textured, facade window texels
  visible in `docs/asylum_shot.png`; `docs/formats/nft.md` records hex-dump
  evidence and the dictionary-resolution measurements. Regression:
  nif_test passes cube.nif + first 200 world NIFs with texture parsing
  active; new env hooks `OPENBULLY_MESH`, `OPENBULLY_NOCULL`,
  `OPENBULLY_DEBUG_CAM`, `OPENBULLY_STATS` documented in nft.md.
