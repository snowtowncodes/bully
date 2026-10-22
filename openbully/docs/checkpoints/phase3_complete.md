# Phase 3 Checkpoint — Input & Camera / Textures / Lighting

**Date**: 2026-10-21 (integration window 19:18–21:20)
**Status**: ⚠️ PARTIAL — all three tracks delivered code that compiles and runs; one
cross-track regression (3D draws do not reach the screen) is unresolved. See
"Open defect" below.

## Delivered by each track (merged on master working tree)

### Track 10 — Input & Camera (`src/game/`)
- `input.h/.cpp`: per-frame keyboard/mouse snapshot (`SDL_GetKeyboardState`,
  `SDL_GetRelativeMouseState`), logical `Button` enum (W/A/S/D, Space, LCtrl,
  LShift), Escape quit, relative mouse mode at startup.
- `camera.h/.cpp`: free-fly FPS camera (glm, LH/Y-up matching Gamebryo);
  mouse look 0.002 rad/px with pitch clamp, horizontal WASD, Space/LCtrl
  vertical, Shift ×4 sprint, 5 u/s base; `GetViewMatrix()` via `glm::lookAtLH`.
- Renderer gained `SetCameraView()` + `GetProjection()`; engine composes
  `mvp = proj * view * model` per mesh. Spawn from geometry bounds with
  `OPENBULLY_CAM_ZOOM` scaling hook.
- `OPENBULLY_CAM_DEMO=1`: scripted flythrough driving the same
  `Camera::Update` path for automated verification.
- New files wired into CMakeLists (`input.cpp`, `camera.cpp`); vendored
  `external/glm` (1.0.1 headers, `glm_headers` INTERFACE target).

### Track 11 — Texture Loading, NFT (`src/engine/formats/nft_loader.*`)
- NFT = Gamebryo NIF container with texture blocks; loader parses header,
  block table, NiPixelData (layout byte-verified on all 35,635 retail
  blocks), NiPalette, NiSourceTexture naming.
- Decoders: RGB8/RGBA8 (with channel-order from NiPixelFormat), PAL8,
  DXT1/BC1, DXT5/BC3 (software decode to RGBA8, mip 0).
- 4,461 `.nft` dictionaries extracted to `data/extracted/textures` (16,434
  texture names indexed by `Engine::BuildTextureIndex`).
- Renderer: `LoadTexture` / `UploadTexture` / `BindTexture` /
  `DestroyTexture`; `s_matBase` sampler; magenta/black 4×4 placeholder.
- Engine: per-submesh texture resolution at load (NIF NiTexturingProperty →
  NiSourceTexture name → `.nft` dictionary), cached in `m_textureCache`;
  `Render()` binds per sub-mesh (`DrawMesh3DSub`).
- Result on the demo trio: **15/15 sub-meshes textured, 0 placeholder**;
  full 32-mesh run: **180 textured / 2 placeholder, 29 dictionaries cached,
  0 failures**.

### Track 12 — Lighting
- `fs_mesh3d.sc`: per-fragment Lambert + ambient (uniforms `u_lightDir`,
  `u_lightColor`, `u_ambientColor` packed as vec4), samples `s_matBase`
  multiplied by vertex tint.
- `vs_mesh3d.sc`: passes view-space normal (`u_view`) and UVs through.
- `Renderer::SetLight()`; engine sets a directional sun (to-light
  (−0.3, 0.8, 0.5), white, 0.2 ambient floor).
- `RequestScreenshot()`/`OPENBULLY_SHOT` verification hook (see defect D2);
  `OPENBULLY_NOCULL`, `OPENBULLY_STATS`, `OPENBULLY_MESH`,
  `OPENBULLY_MESH_CAP` debug hooks.

## Integration work performed (this session)
- Watched for track completion (quiet-period heuristic; tracks finalized
  ~20:08–20:15).
- Merged: CMakeLists now builds all track files + glm + shader aggregate
  target; `openbully_shaders` dependency added.
- Fixed spawn framing: anchor camera on vertex-heaviest mesh instead of
  union bounds of scattered level meshes (union puts a 780-unit span in a
  few pixels).
- Build: `cmake --build build --config Release` — **PASS** (openbully.exe,
  nif_test.exe, all 4 shaders × 3 backends).
- Runtime: 32/32 world meshes load, 0 failed; ~240 fps D3D11 (RTX 4070
  SUPER); **stability: 130 s continuous run with scripted camera, zero
  crashes** (5/5 stable post-rebuild runs of 20–35 s as well).

## Verification results
| Criterion | Result |
|---|---|
| Build (Release) | ✅ PASS |
| Run without crash (≥2 min) | ✅ PASS (130 s + 5 shorter runs) |
| Camera movement (WASD + mouse) | ✅ PASS — scripted demo drives identical `Camera::Update` path; position/yaw deltas match speed × time in logs (`phase3_final.log`) |
| Meshes textured / placeholder | ✅ PASS — 15/15 demo, 180/182 full (2 placeholder) |
| Lighting visible (geometry not flat gray) | ❌ **BLOCKED** by D1 |

## Open defects

### D1 (blocker): 3D draws never rasterize — screen shows only the view-1 clear color
- Symptom: window content is uniformly the view-1 clear color `0x00404C`
  (verified via PrintWindow capture and bgfx TGA dump); identical result
  with: new camera, identity view/proj (`OPENBULLY_DEBUG_CAM`), backface
  culling disabled, camera outside mesh bounds.
- bgfx debug stats (`OPENBULLY_STATS`) show **30 draws / 4,360 indices
  submitted, GPU ~6.9 ms** — draws ARE submitted and the GPU reports work,
  yet zero fragments land.
- Ruled out: NaN vertex data (nif_test clean), vertex layout drift
  (static_assert + unchanged since the working 19:24 build), viewport
  (BackbufferRatio::Equal), program validity (createProgram succeeds),
  buffer creation, frame()/present order, camera math (fails even with
  identity matrices), culling, spawn position.
- Regression window: last visually confirmed render 19:52
  (`docs/camera_seq_*.bmp`, tiny but real cluster); every capture after the
  Track 11/12 renderer/shader rewrite (19:53–20:08) is empty — including
  the tracks' own `texture_shot*`/`dbgcam_shot`/`textured_final` PNGs
  (empty teal) and `lighting_shaded_geometry.tga` (uniform clear).
- Prime suspects, in order: (a) new `vs_mesh3d`/`fs_mesh3d` pair compiled
  by shaderc for `s_5_0` — varying/uniform mismatch or sampler declaration
  producing a silently-degenerate program; (b) interaction of the new
  per-draw uniforms (`u_lightDir` etc., declared but maybe packed/
  aliased unexpectedly) with the D3D11 uniformReflection; (c) MSB8065
  CMake stamp bug (`OPENBULLY_BINARY_DIR` empty → stamps written to
  `c:\`) forcing shader recompiles with a stale/odd shaderc invocation
  state.
- Fastest bisect: revert ONLY the shader pair to the 19:24 committed
  versions (keep new renderer.cpp — unused uniforms/samplers are benign;
  this was prepared and is one `git show HEAD:...` away) and rebuild; if
  the scene reappears, rewrite the Track 12 shader against shaderc's
  D3D11 quirks.

### D2 (minor): `OPENBULLY_SHOT` TGA dump captures only the clear color
- `bgfx::requestScreenShot` in `DrawMesh3DRange` produces a uniform
  `0x00404C` TGA regardless of scene (Track 12's own
  `lighting_shaded_geometry.tga` is identical — this path never worked).
- Also does not fire in triangle-only mode (hook lives inside
  `DrawMesh3DRange`). Replace with a D3D11 backbuffer readback or
  window-capture (the only path that ever produced a correct image).

### D3 (minor): CMake shader stamp paths broken
- `OPENBULLY_BINARY_DIR` resolves empty inside the custom-command OUTPUT
  (`c:\stamp_*`), so MSB8065 fires and shaderc re-runs every build.
- Fix: stamp files should be created by the command itself (`-E touch`)
  inside the binary dir, or use `cmake -E make_directory` + explicit
  BYPRODUCTS in the binary dir.

### D4 (minor): anchor-mesh bounds inflated by orphan indexed vertices
- `3_06CarBarr01`: indexed bounds span ±540 units vs a ~4-unit visible
  mesh; ASY_ABlock radius 450. Anchor selection by vertex count can pick
  these; consider percentile-based bounds or a max-extent sanity filter.

## Current state
- Working: build, load pipeline (NIF + NFT + texture index/resolution),
  input/camera logic, frame loop, stability, per-submesh draw submission,
  textures bound (verified via logs), lighting uniforms set.
- Not working: visible 3D output (D1) — so "lighting visible" cannot be
  confirmed; screenshot automation (D2).
- Next steps: bisect D1 via the prepared shader revert; then re-run this
  checkpoint's verification table; then the screenshot for the record
  (manual `drive_camera.ps1` + window capture from an interactive session
  is the proven path).

## Evidence files
- `build/phase3_final.log` — 130 s stability run, camera deltas, texture counts
- `docs/camera_seq_a/b/c.bmp` (19:52, last good window captures),
  `docs/phase3_stats.png` (bgfx stats: draws submitted, GPU busy),
  `docs/phase3_pw.png` / `docs/phase3_dc.png` (clear-color-only window)
- `docs/phase3_final.png` — final TGA (uniform clear; documents D2)
