# Track 17: Animation System — Complete

Phase 5, Track 17 (150 min budget). Skeletal animation for Jimmy and NPCs.

## What was built

### 1. `.agr` animation loader (`src/engine/formats/agr_loader.{h,cpp}`)

Bully does NOT use standard Gamebryo `.KF` files — its skeletal animation
ships as `.agr` groups (verified in `bully-re/docs/agr.md`, byte-verified
against all 550 retail groups). The loader is a C++17 port of the reference
decoder (bully-re `src/anim/Agr.cpp`):

- All six key encodings (types 999-1004); ped clips are type 1002 (8-byte
  packed quaternion keys).
- Per-bone rotation tracks (36 bones per ped) via the previous-key chain walk.
- Position tracks for Root (pelvis bob) and ARROW (ground travel).
- Zero-norm keys (63 animations carry them on arm bones) skipped.
- Slerp/lerp evaluation, `AgrTravel` for ground-speed matching.

Animation data source: `Anim/C_Player.agr` (idles) + `F_*.agr` walk-cycle
groups extracted from `Stream/World.img` (`tools/img_unpack.py --filter
"F_*.agr"`). All 141 clips across 11 groups decode (100% rate); walk clip
selected by ARROW ground travel (~1.4 m/cycle).

### 2. Bone hierarchy from NIF (`nif_loader` extension)

- `NiSkinInstance` / `NiSkinData` blocks parsed: skeleton root, bone node
  refs, per-bone bind transforms, per-vertex weights (verified layout,
  docs/nif.md).
- `blockToNode` mapping: block indices != nodes[] positions (non-node blocks
  interleave); all hierarchy refs remapped post-parse.
- `NifNode.isShape` flag: bone DFS skips NiTriShape leaves so the 36-bone
  track order matches the rig exactly (verified on Player_Mascot + PLAYER).
- Skeleton DFS re-roots at `Dummy` (docs/agr.md track order).

### 3. AnimationController (`src/animation/animation_controller.{h,cpp}`)

- Skeleton build from the NIF skin root; bind-pose model matrices.
- Clip library (`AddClip`/`Play`/`Stop`), loop + one-shot, time tracking,
  normalized time, play/pause state.
- Crossfade blending (default 0.2 s, per-bone slerp + pelvis lerp).
- Per-frame evaluation: rotation replaces node rotation (Root/ARROW position
  tracks applied), model-space composition, skin matrices.
- Per-shape bind palettes (`AddShapeBindPacked`/`ShapePalette`): each skinned
  NiTriShape carries its own NiSkinInstance (Jimmy: 2-bone head + 25-bone
  body pieces), so palettes are computed per submesh at draw time.
- Walk-speed matching: playback rate = entity speed / authored ground speed
  (no foot skating).

### 4. GPU vertex skinning

- `SkinnedMeshVertex` + `SkinnedMesh` + `SkinnedMeshFromNif` (mesh.h/cpp):
  4 bone indices + 4 weights per vertex, rigid sub-shapes auto-bound to
  their nearest skeleton ancestor bone, weights normalized.
- `vs_mesh3d_skinned.sc`: `u_bones[64]` uniform palette, weighted mat4 sum.
- Renderer: skinned vertex layout (Uint8 indices + Float weights),
  `UploadSkinnedMeshGpu`, `DrawMesh3DSubSkinned` (per-submesh palette
  upload), graceful bind-pose fallback when the skinned program is missing.
- CMake compiles the new shader for d3d11/opengl/vulkan like the others.

### 5. Engine integration

- `LoadAnimationRig()` decodes all `.agr` groups once (data/extracted/anim).
- `CreateRig`/`CloneRig`: skeleton + clips + skinned GPU mesh; per-entity
  controller clones share GPU buffers.
- `AnimationComponent` (entity.h): rig index + playback state.
- Jimmy: skinned spawn path (PLAYER.nif primary, mascot fallback); walk when
  WASD moves (grounded, not sliding), idle otherwise.
- NPCs: 18 spawned with per-instance skinned rigs (64-bone CS_ models
  included), walk while wandering, idle when standing/facing.
- Verification hooks: `OPENBULLY_AUTO_WALK=1` (holds Forward headless),
  `OPENBULLY_SHOT_EVERY=<sec>` (recurring captures `<name>_N.tga`).

## Verification

- `build\Release\anim_test.exe data/extracted/anim data/extracted/world` —
  ALL PASS:
  - 19/19 F_Jocks animations decoded, 3 walks found
  - skeleton 36 bones (exact documented rig match)
  - skinned mesh 1768 verts / 10 submeshes with weights
  - pose moves after 0.5 s of walk (max |M-I| = 2.0)
  - crossfade walk->idle OK
- Palette integrity probe: 2 full walk cycles, every matrix finite,
  |entry| <= 100, no NaN/inf — no exploding vertices.
- Engine run: 141 clips loaded, Jimmy SKINNED (36 bones), 18 NPCs skinned
  (46/6/64-bone rigs), no errors; screenshot captures show per-frame pose
  change under auto-walk.
- Full build passes (openbully + anim_test + shaders, Release).

## Known limitations

- Rest pose vs bind pose: Player_Mascot's head bone carries a bind offset
  (same documented case as Zoe in bully-re/docs/nif.md). PLAYER.nif (the
  full Jimmy body, verified in bully-re tests/test_skin) is the primary
  model; the mascot remains as fallback.
- Normals are skinned with the rotation 3x3 (no inverse-transpose); fine
  for near-rigid bone transforms.
- Single walk/idle clip pair for all characters (faction variety = later
  work; the clip library already supports arbitrary named clips).
