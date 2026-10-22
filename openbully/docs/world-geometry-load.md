# World.img Level Geometry — Load Report

Date: 2026-10-21 · Task: "Load World.img level geometry once decryption is available"

## Decryption status

The decryption agent rewrote all 5,720 extracted NIFs in place under
`openbully/data/extracted/world/` (the decrypt function was **not** added to
`src/tools/extract_img.py` — the task's expected signal never appeared; the
signal that landed was clean `Gamebryo File Format` magic in the files).
Every NIF now parses as version `0x14030009`.

## Inventory (see `data/world_nif_inventory.txt` for all 5,720)

- Parse OK: **5,698** / 5,720 (22 failures are non-geometry/edge cases)
- Loadable (≥100 verts): **4,275**
- Total geometry: **5,637,564 verts / 4,417,510 triangles**

Top meshes by vertex count:

| NIF | verts | tris |
|---|---|---|
| ifreaksEntrance.nif | 65,882 | 88,410 |
| sc_HalloweenSkel.nif | 51,823 | 38,808 |
| DL_CA1b_mountains.nif | 31,883 | 36,716 |
| Com_shelves.nif | 31,284 | 15,271 |
| tallfirs.nif | 25,923 | 19,942 |
| BU1p_cliff01.nif | 19,750 | 16,986 |
| JO_Room06.nif | 17,705 | 12,815 |
| inAsylMtns.nif | 16,956 | 27,528 |
| RI2p_crosstrees05.nif | 16,232 | 12,156 |
| SC1b_walls_B.nif | 14,251 | 7,086 |

## Engine changes (Phase 3)

- `renderer.h/.cpp`: `GpuMesh` multi-mesh upload (`UploadMeshGpu`,
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
- Run: 32/32 meshes loaded, 0 failed, 243 fps (`Direct3D 11`)
- Screenshot: `docs/geometry_shot.bmp` (+ `.png`) — level geometry visible
  (walkable rings, terrain slabs, walls) with depth-tested shading.

## Known limitations (next steps)

- First-32-alphabetical cap: full streaming/culling lands later.
- Identity model transforms: per-node NiNode hierarchy transforms are not
  applied yet, so meshes sit in local space (fine for these single-node
  walkables, wrong for assembled scenes).
- No textures/materials yet — flat shaded.
