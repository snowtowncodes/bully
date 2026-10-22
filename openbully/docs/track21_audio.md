# Track 21: Audio System — Complete

**Status**: complete (procedural placeholder backend; XACT bank decoding
deferred). Build clean, all verification checks pass.

## Backend decision

`docs/asset_catalog.md` documents Bully's real audio as XACT-era banks:
`.bin` containers holding `.rsm` streams (Speech 1.93 GB / 40,400 streams,
Music 550 MB / 263, Ambients 174 MB / 143) plus 322 `.SEA` XACT sound banks.
Decoding those is a dedicated reverse-engineering track and was explicitly
out of scope here. **What ships in Track 21 is procedural placeholder audio,
not real game audio.**

- **SDL2_mixer: not vendored, deliberately.** The vendored SDL2-2.30.12 VC
  package has no mixer, and the procedural buffers + optional user .wav path
  need nothing SDL2_mixer provides (its value is compressed-format decode —
  irrelevant until XACT decoding lands). Instead the engine has a ~150-line
  software mixer on the base SDL2 audio callback. When real bank decode
  arrives, SDL2_mixer (or a real mixer) can be revisited; the `Sound`/mixer
  seam is where it plugs in.
- **Device**: `SDL_OpenAudioDevice`, 44.1 kHz / AUDIO_S16SYS / stereo
  requested (whatever the driver grants is logged; verified: wasapi grants
  exactly 44100 Hz, 2 channels, 1024-frame buffer).
- **Mixer**: up to 32 voices mixed additively on the SDL audio thread under
  a mutex; per-voice constant-power pan gains and a master gain (atomic,
  read lock-free in the callback).

## Implemented

| Deliverable | Where | Verified by |
|---|---|---|
| `AudioManager` Init/Shutdown/PlaySound/StopAll/SetMasterVolume | `src/audio/audio.h/.cpp` | `audio_test` harness: 9/9 checks |
| `Sound` struct (PCM buffer, length, sample rate) | `src/audio/audio.h` (device-format interleaved S16) | generated + mixed live |
| 3D positioning (distance inverse-square clamped + pan by relative X) | `PlaySound(id, vol, pos)`, listener = camera | near/far play checks; attenuation `ref²/(ref²+d²)` |
| Footsteps with internal ~0.35 s cooldown | `PlayFootstep(worldPos)` | harness: 200 rapid calls in 2 s → 6 accepted; in-game: `footstep #1` log on grounded walk |
| UI click (sine blip), footstep (filtered noise burst + thump), ambient wind (low-passed noise loop) | procedural generators | generated at device rate, lengths logged |
| User .wav extensibility path | `LoadUserWavs("data/audio")` — `footstep.wav`/`click.wav`/`wind.wav` override the procedural set, auto-converted to device format | loads 0 files when dir absent (normal), logs overrides when present |
| Init after window creation | `Engine::Init`, right after `Renderer::Init` | log order in run logs |
| Ambient loop on level load | `Engine::Init` after geometry load | `ambient wind loop started` in every run log |
| Debug key M → mute | `Button::Mute` (input.h/.cpp) → `ToggleMute()` in `Engine::Update` | live game: injected M key logged `muted (master volume 0)` then `unmuted` |
| Verification counter | `FootstepCount()`, logs #1 then every 8th | run logs |

## Deferred (explicit)

- **XACT bank decoding** (`.bin`/`.rsm`/`.SEA`) — all "real" Bully audio.
- Walk-cycle **phase-accurate** footstep sync (currently a fixed 0.35 s
  cadence timer while grounded+moving; the timer matches the cooldown spec).
- **NPC footsteps**, UI click wiring (the sound exists; no menu calls it yet),
  occlusion/reverb/EAX-style effects, music streaming, listener roll/zone
  ambience switching.

## Bug found & fixed during verification

`SpawnTestEntities`'s Track-14 self-test (`QuerySphere` → destroy first hit →
recycle) destroyed **the player** when Jimmy was the first layer-2 hit
(happens whenever the static-entity count is small, e.g.
`OPENBULLY_MESH_CAP=8`): Jimmy's handle went stale, silently disabling
movement/follow-cam/footsteps while the free camera kept the demo moving.
Now guarded to never target `m_jimmyId`
(`src/engine/engine.cpp`, `SpawnTestEntities`).

## Files

- `src/audio/audio.h` / `src/audio/audio.cpp` — new (AudioManager)
- `src/engine/engine.h/.cpp` — audio member, init/ambient/listener/mute
  integration, footstep trigger in `UpdateJimmy`, self-test player guard,
  out-of-line `Engine()` ctor (unique_ptr to fwd-declared AudioManager)
- `src/game/input.h/.cpp` — `Button::Mute` (SDL scancode M)
- `src/tools/audio_test.cpp` — new headless harness
- `CMakeLists.txt` — `audio.cpp` in openbully; `audio_test` target

## Reproducing verification

```powershell
cmake --build build --config Release --target openbully,audio_test
build\Release\audio_test.exe                 # 9/9 checks

# in-game (scripted walk; footstep + mute logs):
$env:OPENBULLY_JIMMY_DEMO = "1"
build\Release\openbully.exe
# press M after Jimmy spawns; watch for
#   [INFO] Audio: footstep #1 @ ...
#   [INFO] Audio: muted (master volume 0)
#   [INFO] Audio: unmuted
```

Run logs: `track21_run.log` / `track21_run2.log` / `track21_run3.log`
(root, gitignored pattern with the other phase logs).
