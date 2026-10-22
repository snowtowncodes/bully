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
