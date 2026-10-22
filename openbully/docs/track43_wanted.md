# Track 43: Wanted System (Trouble Meter)

Status: **complete** (builds; gameplay flow verified end-to-end headless; see
"Known tree issues" at the bottom for two pre-existing crashes that surface
around this system in Debug builds).

## Overview

The wanted meter is a 0-5 head counter on `PlayerController::wanted` (the
HUD widget existed since Track 20). Track 43 adds the gameplay around it:
crimes that charge it, authority response at 1-5 heads, cops at 4-5, the
bust sequence, and three escape paths.

Files:

| File | Role |
| --- | --- |
| `src/game/wanted.h` | Tuning constants, `WantedCrime` catalogue, runtime state (flash, bust cut, decay timers, curfew latch). |
| `src/engine/engine.cpp` (`AddWanted`, `PlayerViolence`, `VandalismCrime`, `AnyAuthorityWitnessed/Near/Chasing`, `UpdateWanted`, `SetupTrespassZones`, `SpawnCops`, `DespawnCops`, `ServiceCopRequests`, `BeginBust`, `FinishBust`, `DriveWantedDemo`) | The system itself. |
| `src/ui/ui.{h,cpp}` | HUDState additions (flash/cops/hidden/bustFade/busted) + head-icon rendering, screen-edge red flash, COPS! banner, HIDDEN indicator, bust fade-to-black with BUSTED!. |
| `src/audio/audio.{h,cpp}` | `SoundId::Whistle` - procedural two-tone warble (C7<->E7, 0.55 s), the "spotted" cue. |
| `src/mission/{mission.h,mission.cpp}` | `AVOID_BUST` objective: survive `duration` seconds; `NotifyBust()` fails such missions when the player is caught. |
| `data/missions/demo_missions.json` | "Trouble on the Roof" chained mission exercising AVOID_BUST. |

## Crime table (task spec -> implementation)

| Action | Charge | Hook |
| --- | --- | --- |
| Fighting in front of authority | +1 | `DamageNpc` -> `PlayerViolence`: +1 when any prefect-role NPC is within 18 u with LOS (LOS waived inside 6 u). |
| Attacking authority (prefect/cop) | +2 | `PlayerViolence` when the victim carries `AIRole::Prefect` (cops reuse the role). |
| Vandalism (breaking windows / property) | +1 | `UpdateProjectiles`: a player-owned damaging rock hitting the world = `VandalismCrime` (same witness rule). |
| Trespassing (restricted area) | +1 | Two roof-edge zones built relative to `m_jimmySpawn` (outside the demo mission walk targets); one charge per entry (latched). |
| Curfew (out after 23:00) | +1 | One charge per night, `m_clockMinutes >= 23:00 or < 07:00`; hidden players skip it. |

`AddWanted` is the single gate: per-crime 3 s re-trigger throttle (one brawl
is one "fighting" head; the gap re-arms so prolonged trouble escalates),
clamps at 5, stamps the 1.2 s HUD flash, and plays the whistle on the
0 -> 1 climb.

## Authority response

- **1-5 heads**: any prefect-role NPC that spots the player (18 u + LOS)
  switches to CHASE with the player as target (Track 28 states reused);
  Track 27/28 melee path does the rest. Prefects that reach grab range
  (2.2 u) trigger the bust.
- **4-5 heads**: two cops spawn 20 u away (prefect-role AI, 24 u sight,
  2.6 u/s), immediately chasing; below 4 heads they despawn.
- Cop spawn/despawn is **deferred to the top of the next frame**
  (`ServiceCopRequests`): entity creation reallocates the rig store, which
  must not happen mid-frame while other systems hold references.
- **Hidden** players cannot be spotted (`hiddenBlind` in the Track 27 sight
  check) and already-engaged authorities lose them normally (loseTimer ->
  RETURN).
- **Post-bust grace**: 6 s of i-frames after a bust also suppress re-aggro,
  so the schedule-prefect camping the spawn cannot instantly re-bust.

## Bust sequence

`BeginBust` (phase 1): 0.8 s fade to black + whistle.
`FinishBust` (at full black, phase 2 fades back in over 0.8 s):
- wanted -> 0, health restored, 50% of money lost,
- respawn at the spawn point (ground-snapped), camera re-anchored,
- cops despawned (deferred), prefects released to Idle, fight records +
  AI event queue cleared, hide/curfew state reset,
- `MissionManager::NotifyBust()` fails any active AVOID_BUST mission.

The legacy `Busted()` (Track 28 stub) remains as the health-KO path and is
unchanged.

## Escape mechanics (task spec)

| Path | Rule |
| --- | --- |
| Hide (B - locker/trash can stand-in) | 1 head decays every 10 s; cannot be spotted at range (authorities within 3 u still notice). |
| Leave the area | No authority within 30 u: 1 head decays every 20 s. |
| Wardrobe change (V) | Instant wanted reset - refused while hidden or while any authority is actively chasing. |

## HUD

- 5 head icons (pixel-art faces), filled = heat; gold normally, red during
  the flash, dim outlines when clean; eyes on filled heads.
- Screen-edge red border flash for 1.2 s on every climb.
- `COPS!` label under the heads while cops are live.
- `HIDDEN - HEAT DECAYS` indicator while hiding.
- Bust: full-screen fade to black with `BUSTED!`, fade back in.

## Save/load

The meter itself persists (`SaveData.wanted` existed since Track 36).
Cops are runtime pursuers: destroyed and system state (`WantedSystem`,
trespass zone cache, pending cop requests) reset on load; `DestroyDynamicEntities`
also clears cops.

## Missions ("don't get busted")

`AVOID_BUST` objective type: `"duration"` seconds of survival; completing
by timer, failing the whole mission via `NotifyBust()` if caught. Demo
mission `ch1_trouble` ("Trouble on the Roof") chains after
`ch1_defend_yourself`.

## Verification

`scripts/track43_run.ps1` + `OPENBULLY_WANTED_DEMO=1` drive the whole flow
headless (timeline in `DriveWantedDemo`): assault (+2) -> prefect chase ->
BUST (fade/penalty/respawn/reset) -> trespass (+1) -> hide -> decay to 0 ->
vandalism/assault/trespass (+1+2+1 = 4) -> cops spawn -> second bust.

Verified in logs (`track43_run.log`):
- `WANTED: DEMO: punched a prefect -> 2 heads (assault on authority)`
- `WANTED: authority 34 spotted Jimmy (wanted 2) - giving chase`
- `BUSTED! caught by authority - fading out` / `BUSTED: wanted 2 -> 0, money -0` /
  `BUSTED: back in control (outside the school)`
- post-bust re-charge: `entered a restricted area -> 1 head (trespassing)`
- screenshot: `docs/track43_wanted_hud.png` (1280x720)
- a 120 s no-demo soak (`track43_stab.log`, 382 log lines, no errors)

## Known tree issues (pre-existing, not Track 43)

1. **Debug-bgfx screenshot crash**: with `OPENBULLY_SHOT` set, Debug builds
   int3 inside `bx::debugPrintf` at the capture (bgfx readback debug
   assert). Same offset appears in today's crash log from earlier tracks'
   runs. Release captures fine - the verification runs the demo on the
   Release binary for the screenshot.
2. **Jimmy ground-snap fall**: when a chasing NPC arrives at the rooftop
   spawn, Jimmy occasionally drops through the roof (`onGround=0`,
   ~-30 u/s) until the kill-plane resets him. Survives on Release; the bust
   now lands (grab range 2.2 u) before the fall gets going in the demo.
3. The parallel `AVOID_BUST`/cutscene/minigame work was mid-flight during
   this track; `main.cpp` currently includes `platform/crash_report.h`
   which is missing, breaking the Release configuration until that session
   lands its file.
