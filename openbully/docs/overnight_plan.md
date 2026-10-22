# Overnight Autonomous Build Plan

**Start time:** 2026-10-22 03:05 UTC
**Target:** Phases 4-6 complete by morning

## Active Phase 4 Tracks

1. **Track 13: Collision System** (ses_e9e6ee5f7ffe2F5ZFh2F4KxpLI)
   - AABB, sphere collision
   - Raycast system
   - Spatial partitioning
   - Camera collision with level
   - Budget: 90 minutes

2. **Track 14: Entity System** (ses_e9e6ee549ffeMTGOaqZ6IzuNFl)
   - Entity handle (ID + generation)
   - Component storage (Transform, Mesh, Collision, Physics)
   - Entity manager
   - Update loop
   - Budget: 90 minutes

3. **Track 15: Jimmy Character** (ses_e9e6ee44affeJ3L2vuQks41NYn)
   - Waits for Track 13 + 14
   - Load Jimmy mesh from World.img
   - 3rd-person camera follow
   - WASD movement
   - Ground physics
   - Budget: 120 minutes

4. **Phase 4 Integration** (ses_e9e6ee425ffe74LFnNFbEa1l7i)
   - Waits for Track 13 + 14 + 15
   - Merge all changes
   - Full build test
   - 2-minute smoke test
   - Screenshot Jimmy in-world
   - Write phase4_complete.md
   - Commit
   - Budget: 60 minutes

5. **Phase 5 Prep: Asset Catalog** (ses_e9e6e1a53ffeXnB1VV4NkexDFk)
   - Catalog NPCs, animations, scripts, audio, UI assets
   - Create docs/asset_catalog.md
   - Budget: 60 minutes

## Phase 5 Plan (Launch after Phase 4 Integration complete)

### Track 16: NPC System
**Budget:** 120 minutes

**Requirements:**
- Load 5-10 NPC models from World.img (pedestrians, students, teachers)
- Spawn system (position, model selection)
- Basic AI states: stand, wander, face direction
- NPC entity components (Transform, Mesh, Collision, AI)
- Render all NPCs in world

**Verification:**
- 10+ NPCs visible in level
- Some standing, some wandering
- No crashes
- Screenshot showing multiple NPCs

### Track 17: Animation System
**Budget:** 150 minutes

**Requirements:**
- Load Gamebryo .KF/.KFM animation files
- Animation controller (play, blend, loop)
- Bone hierarchy from NIF
- Skinned mesh rendering (vertex skinning)
- Apply animations to Jimmy and NPCs (walk/idle cycles)

**Implementation:**
- Parse NIF node hierarchy (bone transforms)
- Parse KF keyframe data (rotation, translation, scale)
- Interpolate keyframes per frame
- Update bone matrices
- Pass to vertex shader for skinning
- Wire into entity update: play animation based on movement state

**Verification:**
- Jimmy walks when moving (legs animate)
- Jimmy idles when stopped
- NPCs play walk animations
- Screenshot showing animated character

### Track 18: Physics Improvements
**Budget:** 90 minutes

**Requirements:**
- Proper ground collision (character stands on terrain)
- Stair climbing (detect steps, adjust height)
- Slope walking (adjust velocity based on normal)
- Falling physics (gravity until ground contact)
- Collision response (slide along walls, no sinking)

**Implementation:**
- Ray-cast downward from character position
- If hit ground within step height (0.5u): adjust Y position to ground
- If on slope: apply gravity component parallel to surface
- Stair detection: if vertical obstacle < max-step (0.3u), boost Y
- Wall collision: project movement onto surface tangent

**Verification:**
- Jimmy walks on uneven terrain
- Jimmy climbs stairs
- Jimmy doesn't sink through floor
- Jimmy slides along walls
- Screenshot showing Jimmy on terrain/stairs

### Phase 5 Integration
**Budget:** 60 minutes

- Merge Track 16 + 17 + 18
- Full build
- Smoke test: walk around with NPCs, animations, physics
- Screenshot
- Write docs/phase5_complete.md
- Commit

## Phase 6 Plan (Launch after Phase 5 Integration complete)

### Track 19: Mission System
**Budget:** 180 minutes

**Requirements:**
- Load mission scripts from game files
- Trigger zones (enter area → start mission)
- Mission objectives (go to X, talk to Y, defeat Z)
- Mission state machine
- UI mission indicator (arrow/waypoint)

**Implementation:**
- Parse mission script format (likely Lua or custom)
- Define mission data structure (objectives, triggers, rewards)
- Add trigger zones to level (AABB regions)
- Check player position against zones each frame
- Activate mission on zone entry
- Track objective state
- Render objective marker in world

**Verification:**
- Walk into zone → mission starts
- Objective marker appears
- Complete objective → mission completes
- Screenshot showing mission in progress

### Track 20: UI System
**Budget:** 120 minutes

**Requirements:**
- HUD overlay (health bar, wanted level, time)
- Minimap (top-corner radar showing nearby area)
- Basic font rendering
- UI textures from World.img
- Health system (damage, heal)

**Implementation:**
- Extract HUD textures (health icons, minimap border, etc.)
- Load font texture atlas
- Render UI as screen-space quads (bgfx view layer)
- Add health component to Jimmy
- Damage test: press H key → decrease health, update bar
- Minimap: top-down orthographic render of nearby area

**Verification:**
- Health bar renders
- Minimap shows player position
- Damage decreases bar
- Screenshot showing HUD

### Track 21: Audio System
**Budget:** 150 minutes

**Requirements:**
- Load audio files (music, SFX, dialogue)
- Audio playback (start, stop, loop)
- 3D audio positioning (distance attenuation, panning)
- Background music
- Footstep SFX on movement

**Implementation:**
- Parse Bully audio format (or extract to .wav/.ogg)
- Integrate audio library (SDL_mixer or OpenAL)
- Play background music on level load
- 3D audio: calculate distance/direction from listener (camera)
- Play footstep SFX when Jimmy moves (sync with animation)
- Add audio source component to entities

**Verification:**
- Music plays on launch
- Footsteps audible when walking
- Audio attenuates with distance
- No audio when stopped

### Phase 6 Integration
**Budget:** 60 minutes

- Merge Track 19 + 20 + 21
- Full build
- Smoke test: mission, HUD, audio all working
- Screenshot + short video clip
- Write docs/phase6_complete.md
- Commit

## Total Estimated Time

- Phase 4: ~6 hours (360 minutes)
- Phase 5: ~7 hours (420 minutes)
- Phase 6: ~8.5 hours (510 minutes)
- **Total:** ~21.5 hours

**Realistic overnight target:** Phase 4 complete + Phase 5 started

## Success Criteria (Morning Delivery)

**Minimum (Phase 4 complete):**
- Collision working
- Entity system working
- Jimmy spawns, controlled with WASD
- Camera follows Jimmy
- Jimmy collides with level
- docs/phase4_complete.md exists
- Build passes
- Screenshot shows Jimmy in-world

**Stretch (Phase 5 partial):**
- NPCs spawned
- Basic animations playing
- Improved physics

**Best case (Phase 5 complete):**
- All Phase 5 tracks integrated
- Jimmy and NPCs animate
- Physics polished
- docs/phase5_complete.md exists

## Monitoring Strategy

Since task_status is unavailable on this host, orchestrator will:
1. Wait for completion notifications from background agents
2. Launch next phase when dependencies clear
3. Handle any stuck tracks if reported
4. Continue work until morning or all phases complete

## Autonomous Decision Authorization

Agents are authorized to:
- Make implementation decisions within scope
- Choose reasonable defaults for unspecified parameters
- Skip optional nice-to-haves if running over budget
- Report blockers but continue with workarounds
- Write placeholder stubs if full implementation blocks integration

Agents must NOT:
- Change project architecture without approval
- Skip verification steps
- Merge broken code
- Skip documentation

## Communication Protocol

Agents report completion via system notifications. Orchestrator checks notifications and launches next phase. No polling loops or active monitoring required.

## Morning Handoff

User wakes up to:
1. Summary of completed phases
2. Screenshot/video evidence
3. Build verification status
4. Any blockers or issues encountered
5. Next recommended steps
