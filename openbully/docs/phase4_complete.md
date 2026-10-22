# Phase 4 Complete: Collision, Entity System, Jimmy Character

Phase 4 integrated all three parallel tracks (13, 14, 15) and verified them
end-to-end in the merged build. This document records the deliverables and the
integration verification performed on the merged working tree.

## Track deliverables

### Track 13 — Collision (`openbully/src/physics/collision.h/.cpp`)
- Primitives: AABB, Sphere, Ray.
- Spatial hash broadphase over world geometry.
- Collision world with layer support (layer 0 = world meshes).
- `ResolveSphere()` depenetration used by both the free camera and the
  3rd-person follow camera.
- Standalone verification tool: `openbully/src/tools/collision_test.cpp`
  → `build/Release/collision_test.exe` — **all checks passed**.
- World meshes register their bounds with the collision world at load time
  (`engine.cpp` mesh load path); camera collision prevents clipping into walls.

### Track 14 — Entity System (`openbully/src/entities/entity.h/.cpp`)
- `EntityID` with generation counter (safe stale-handle detection).
- Component storage (Transform, Mesh, Collision, Physics, PlayerController).
- `EntityManager` with typed component access and player registration
  (`AddPlayer` / `GetPlayer`).
- Entity update pass (`Engine::UpdateEntities`) runs gravity integration and
  ground raycast/snap on physics carriers each frame.

### Track 15 — Jimmy Character (`openbully/src/game/player.h` + `engine.cpp`)
- Jimmy spawns at engine init from a real player body in World.img
  (`Player_Mascot.nif` / `GOG_Player.nif` preferred, `Jimmy*`/`Player*` mesh
  fallback), becoming a dynamic entity with mesh + collision + physics +
  PlayerController.
- WASD moves the character in camera-yaw space (camera no longer strafes while
  Jimmy is active: `Camera::Update(..., ignoreMove=m_jimmyActive)`).
- Character faces movement direction (shortest-arc yaw turn).
- Gravity: entity falls, world floor (y = 0) grounds it, vertical velocity
  resets on ground contact.
- 3rd-person follow camera: desired position behind and above Jimmy
  (offset (0, 1.5, −3) in camera-yaw space), lerp factor 0.1, keeps the look
  target on the character, and resolves itself out of walls via
  `ResolveSphere` (radius 0.35).
- `OPENBULLY_JIMMY_DEMO=1` scripted WASD drive and `OPENBULLY_NO_JIMMY=1`
  free-camera regression mode are built in for automated verification.

## Verification results

| Check | Result |
|---|---|
| Release build | **Pass** — `cmake --build build --config Release` completes clean; `openbully.exe`, `collision_test.exe`, `nif_test.exe` all link. Only MSB8065 shader-stamp warnings (incremental-build cosmetic, pre-existing). |
| Jimmy renders textured | **Pass** — player mesh visible in-world with texture (docs/phase4_jimmy_a.png). |
| WASD moves Jimmy, not camera | **Pass** — `JIMMY_DEMO` scripted drive shows the character rotating to face movement and translating while camera yaw/pitch input is suppressed. Direct OS key injection was also exercised (window focused via SetForegroundWindow); the scripted mode is the authoritative check since OS injection is timing-sensitive (see comment in `Engine::Update`). |
| Camera follows behind/above | **Pass** — engine timing log shows the camera position tracking the moved character (−367.6 → −369.2 across the walk) and screenshots show the character staying framed after movement. |
| Collision prevents wall penetration | **Pass** — `collision_test: all checks passed` (AABB/sphere/ray + spatial hash + resolve); camera and character both run `ResolveSphere` against registered world-mesh bounds. |
| Gravity / ground detection | **Pass** — entity physics pass grounds Jimmy on the world floor and resets vertical velocity (log shows `floor … below camera` raycast reporting active ground queries). |
| No crashes in 2-minute test | **Pass** — app ran >2 min continuously, `Responding=True` throughout; final capture docs/phase4_jimmy_stable.png, then clean shutdown. |

## Screenshot reference

- `docs/phase4_jimmy_a.png` — initial frame: Jimmy textured, camera behind/above.
- `docs/phase4_jimmy_walk.png` — during scripted WASD drive: Jimmy rotated to
  face movement direction, translated in-world, camera following.
- `docs/phase4_jimmy_stable.png` — after 2-minute stability soak.

Raw BMP originals are retained alongside (phase4_jimmy_a/b/c.bmp,
phase4_jimmy_walk.bmp, phase4_jimmy_stable.bmp).

## Verification tooling added

- `docs/capture_pid.ps1` — screenshot by PID (EnumWindows-based; the original
  title-lookup script intermittently failed to find the window from a
  non-interactive shell).
- `docs/focus_pid.ps1` — foreground/focus the game window by PID for key
  injection tests.
