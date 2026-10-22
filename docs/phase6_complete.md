# Phase 6 Complete: Missions, UI, Audio

Phase 6 integrated three tracks — the Mission System (Track 20), the HUD/UI
renderer (Track 20), and the Audio system (Track 21) — on top of the Phase 5
foundation. All three run together in one Release build; a scripted headless
verification drive walked the full Chapter-1 demo mission chain end to end,
and a 3-minute soak at ~240 fps stayed error-free.

## What shipped

### Mission System (Track 20) — `src/mission/`

- `MissionManager` with hand-authored JSON missions
  (`data/missions/demo_missions.json`; the retail game uses Lua 5.0 `.lur`
  bytecode, deliberately not parsed).
- Objective types: `GO_TO_LOCATION`, `TALK_TO_NPC`, `COLLECT_ITEM` (proximity
  completion), `DEFEAT_NPC` / `CUSTOM` (external completion, plus an
  `auto_complete_on_reach` flag for hand-authored demo beats).
- World-space **trigger zones** start missions when Jimmy enters them;
  `start_after` chains missions Bully-style (Chapter-1 flow:
  Welcome to Bullworth → This Is Your School → Defend Yourself).
- All mission/zone positions are expressed relative to runtime reference
  points (Jimmy's spawn, the anchor mesh), so content stays aligned with
  wherever the level loads.

### HUD/UI (Track 20) — `src/ui/` + `vs_ui.sc` / `fs_ui.sc`

- `UIRenderer` on bgfx **view 10**: screen-space ortho, alpha blended, drawn
  after all world views. Single-quad submits through transient buffers.
- Widgets: **health bar** (responds to H / HUD-demo damage), **wanted stars**
  (J/K debug keys, 0–5), **clock**, **money**, and a **minimap** with player
  anchor + up to 64 nearby-NPC dots.
- Built-in 5x7 bitmap font atlas; 1280x720 reference layout scales with the
  backbuffer.
- `Engine::UpdateHUD` refreshes a plain-data `HUDState` snapshot each frame;
  the UI layer never touches entities or SDL.

### Audio (Track 21) — `src/audio/`

- SDL2 audio device (WASAPI, 44.1 kHz / 16-bit / stereo) + software mixer
  (32 voices, constant-power pan, master volume). SDL2_mixer deliberately
  not vendored — real Bully audio lives in XACT banks we don't decode yet,
  so all SFX are procedural PCM: footsteps (noise burst + thump), UI click,
  and a seamless looping ambient-wind bed.
- 3D positioning (inverse-square attenuation + listener-relative pan) with
  the listener updated from the camera every frame.
- **M toggles mute** (master gain to 0 at mix time; unmute restores).
- `audio_test.exe` harness: **9/9 checks pass** (device open, procedural
  buffers, footstep cadence throttle ~0.35 s, mute state machine, 3D
  near/far attenuation, double-shutdown safety).

## Integration bugs found and fixed during this phase

1. **JSON parser rejected legal whitespace** (`src/mission/mission.cpp`).
   `JsonParser::ParseObject` did not skip whitespace before a key, so any
   `{"a": 1, "b": 2}`-style file failed at the first space after a comma.
   The single-line demo_missions.json tripped exactly this
   (`JSON parse failed ... byte 589`), leaving the mission system idle.
   Fix: `SkipWs()` before key parsing.
2. **Demo mission data pushed objectives off the rooftop.** Offsets such as
   `(0,0,5)` placed targets at z=10.1 while the ASY_ABlock roof ends at
   z=8.1; the scripted drive sprinted off the edge and plummeted
   (y: 465 → −2225). All six targets were re-authored inside the roof
   bounds with ≥1.5 u edge margin (verified programmatically).
3. **Mission demo drive fought the follow camera.** Mouse-delta yaw steering
   lost a per-frame tug-of-war with `UpdateJimmyCamera`'s own `SetLookAt`
   re-aim; at spawn the yaw was still ~0 (+Z) when Forward was first held,
   which is what launched Jimmy off the +Z roof edge. The drive now aims the
   camera directly (`SetLookAt` toward the objective each frame, current
   pitch preserved) — drive and follow-cam agree, Jimmy turns in place then
   walks.
4. **Mission chain never fired from the root mission.** The activation gate
   required the just-completed mission to *itself* have a non-empty
   `start_after`, so `ch1_this_is_your_school` (which waits on the root
   `ch1_welcome`) never started. Fix: gate on `next.startAfter == m.id` only.
5. **`auto_complete_on_reach` was never honored.** `ReachedObjective`
   unconditionally returned false for `DEFEAT_NPC` / `CUSTOM`, so mission 3
   (Defend Yourself, demo-scripted beats) stalled at 1/2 forever even with
   the player standing on the target. Fix: those types check proximity when
   `completeOnReach` is set, and still return false otherwise.

## Verification evidence (`phase6_final.log`)

- Missions: 3 registered, 2 trigger zones; full chain observed:
  - `MISSION ACTIVATED: 'Welcome to Bullworth'` (trigger `tz_spawn` entered)
  - `OBJECTIVE COMPLETE` × 2 → `MISSION COMPLETE: 'Welcome to Bullworth'`
  - `MISSION ACTIVATED: 'This Is Your School' — chained after 'ch1_welcome'`
  - subsequent objectives advance and complete on proximity
- Objective **beacon column** rendered at the active target while a mission
  is active (emissive pulse).
- Audio: device opened (`44100 Hz, 2 channels (wasapi)`), ambient wind loop
  started on level load, `footstep #N` logged every 8th step while walking;
  M mute handled in `Engine::Update` (`Audio: muted (master volume 0)`).
- HUD: `HUD demo: wanted -> 3`, then −10 health per second, mirroring on the
  health bar/stars/clock/minimap drawn by view 10 (`UIRenderer initialized
  (view 10, font atlas 384x8)`).
- Screenshot: `docs/phase6_verify.png` (1280x720), captured in-engine via
  `OPENBULLY_SHOT=phase6_verify` + `OPENBULLY_SHOT_DELAY=45` mid-mission.
- **Stability**: 3-minute soak (exceeds the 2-minute requirement) at
  ~238–240 fps steady, no `[ERROR]` lines, no log flooding, Jimmy grounded
  (`onGround=1`) throughout. Preserved as
  `openbully/docs/phase6_stability_log.txt`.

## Known issues / limitations

- Audio is procedural placeholder; XACT bank decoding (real Bully SFX) is
  future work (see `docs/track21_audio.md`).
- The mission demo JSON is authored for the ASY_ABlock spawn; the
  relative-to-spawn scheme keeps it aligned, but targets assume rooftop
  bounds (documented in the JSON `_comment`).
- MSB8065 shader-stamp warnings persist from Phase 5 (cosmetic, in-code
  note already flagged for the build-system cleanup).
- Wanted-level decay, mission failure conditions, and reward/cutscene hooks
  are stubs reserved for Phase 7.
