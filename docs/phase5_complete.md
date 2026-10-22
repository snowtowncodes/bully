# Phase 5 Complete: NPCs, Animation, Physics

Phase 5 delivered three tracks on top of the Phase 4 foundation (collision,
entity system, Jimmy character). All three tracks are integrated into a single
running scene: Jimmy plus 18 animated NPCs on the loaded world geometry with
the Track 18 character controller active.

## Track 16: NPC System

- 18 NPCs spawn around the player anchor, built from **16 distinct character
  models** (`CS_*.nif`: BALC1-3, BWSign, B_Bald, B_Bhat1-6, B_Boots1-3 —
  "NPC models: 16 loaded, 0 failed (from 5720 scanned)").
- Skinned rigs per model: 46-bone, 6-bone (props/signs), and 64-bone variants.
- 3-state AI (`src/entities/npc.h`):
  - **IDLE** — stand, occasionally retarget.
  - **WANDER** — pick a nearby target, walk to it (playback of the walk clip,
    ground-following movement).
  - **FACE_PLAYER** — turn toward Jimmy when he is within range.
- Runtime log confirms all three states cycling every frame
  (`NPC AI: idle=N wander=N face=N`), with per-NPC position/target sampling.
- `OPENBULLY_NPC_COUNT=<n>` hook exists for reproducible verification runs.
- Verification artifacts: `docs/track16_npcs_demo.png`,
  `docs/track16_stability_log.txt` (multi-minute stability soak: ~238 fps
  steady, no errors, no leaked logs, states transitioning continuously).

## Track 17: Animation System

Full report: `openbully/docs/track17_animation.md`. Summary:

- `.agr` animation loader — C++17 port of the reference decoder; **141 clips
  across 11 `F_*.agr` groups decode at 100%** (walk clip selected by ARROW
  ground travel ~1.4 m/cycle, idle clip #0).
- Bone hierarchy from NIF skin instances; 36-bone rig order matches the
  documented rig exactly (verified on Player_Mascot + PLAYER).
- `AnimationController`: clip library, loop/one-shot, crossfade blending
  (0.2 s), per-shape bind palettes, walk-speed matching (no foot skating).
- **GPU vertex skinning**: `vs_mesh3d_skinned.sc` with `u_bones[64]` palette,
  4 indices + 4 weights per vertex, per-submesh palette upload.
- Walk/idle animations on Jimmy and all NPCs (walk while moving, idle while
  standing/facing).
- Verification: `anim_test.exe` all pass (19/19 Jocks clips, skeleton match,
  pose moves, crossfade OK); palette integrity probe — every matrix finite
  over 2 full walk cycles.

## Track 18: Physics Improvements

Character-controller pipeline in `Engine::UpdateJimmy`
(`openbully/src/engine/engine.cpp`, documented in-code):

1. **Gravity / falling** — `v.y += g*dt` clamped to max fall speed; slight
   air control while airborne.
2. **Slope following** — move direction projected onto the ground plane
   (`n x up`) so walking follows the surface angle.
3. **Stair step-up** — horizontal probe finds blocking boxes; obstacles whose
   top is within step height raise the position (stair climbing).
4. **Ground alignment** — down raycast with snap window; resets fall velocity
   on contact; steep normals trigger slope slide; high landing speed logs
   fall damage.
5. **Wall sliding** — sphere resolve against normals; incoming velocity
   projected onto the wall tangent so Jimmy slides along walls, not into them.

Runtime log evidence: `Jimmy: (-735.36, 464.92, 5.06) onGround=1` held steady
across the entire soak (ground alignment), while the `OPENBULLY_JIMMY_DEMO`
scripted run exercised airborne descent (`onGround=0` with falling velocity)
and re-contact. Physics-visualization artifact from the physics track:
`openbully/phase4_physics_track18.png`.

## Integration verification (this phase)

1. **Release build**: full rebuild passes clean —
   `cmake --build openbully/build --config Release` → exit 0; only
   pre-existing MSB8065 shader-stamp warnings (incremental-build quirk, not a
   compile error). Outputs: `build/Release/openbully.exe` (+ anim_test,
   nif_test, collision_test).
2. **Runtime**: `openbully.exe` (Release) ran with the full scene:
   - World geometry + textures loaded, collision world registered.
   - 141 AGR clips decoded, rig ready (walk=76, idle=0).
   - Jimmy spawned SKINNED (36 bones, walk/idle bound), grounded.
   - 18 NPCs spawned from 16 models, all skinned (46/6/64-bone rigs).
   - All three AI states observed cycling; frame timing ~4.2 ms (~238 fps),
     ~125 fps during init-heavy first frames. No errors in the log.
3. **Screenshot**: `docs/phase5_verify.png` (1280x720) captured in-engine
   via `OPENBULLY_SHOT=phase5_verify` + `OPENBULLY_SHOT_DELAY=6` during a
   live run with Jimmy grounded and NPCs in mixed IDLE/WANDER/FACE states.

## Known issues / limitations

- Only a single walk/idle clip pair is used for all characters; faction-
  specific walk variety is future work (the clip library already supports
  arbitrary named clips).
- Normals are skinned with the rotation 3x3 (no inverse-transpose) — fine for
  near-rigid bone transforms, may show artifacts under extreme scale/shear.
- Rest-vs-bind pose quirk on Player_Mascot's head bone (documented in
  track17_animation.md); PLAYER.nif is the primary model, mascot is fallback.
- MSB8065 shader-stamp warnings during MSBuild incremental builds (stamps are
  written to `c:\` roots); cosmetic but should be fixed by making shader-rule
  outputs land inside the build dir.
- NPC AI has no collision-aware navigation: NPCs follow flat-target wander
  and rely on the same ground snap, but do not path around walls (they slide
  along them via the shared collision resolve).
- `face` state counts stay low (0-4) in the log because NPCs only face the
  player within a narrow radius — expected, but worth widening for gameplay
  feel in Phase 6.

## Phase 6 readiness

Solid base for the next phase:

- Entity system + handles proven at scale (32 static + 18 dynamic characters,
  handle gen/recycle verified in-log).
- Skinned animation pipeline supports arbitrary clips — combat/gesture
  animations can be added without new infrastructure.
- Character controller has the primitives (ground probe, step-up, wall slide)
  needed for gameplay movement (sprinting, jumping, throwing).
- Suggested Phase 6 focus: interaction/combat between Jimmy and NPCs
  (hit reactions using the clip library), camera gameplay mode (follow-cam
  instead of free camera), NPC-to-NPC awareness, and fixing the shader-stamp
  build warning.
