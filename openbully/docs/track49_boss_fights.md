# Track 49: Boss Fights

Faction-leader boss fights: enhanced combat encounters with a phase system,
per-boss special attacks, quick-time events, a dedicated boss HUD, and a
victory/defeat loop tied into the mission + cutscene systems.

## What shipped

### Boss roster (`src/game/boss.h/.cpp`)
Five `BossDefinition` table entries — Gary (The Mastermind), Russell (The
Jock Titan), Johnny (Greaser Leader), Derby (Prep King), Ted (Jock Bruiser) —
each with HP, walk/charge speeds, attack + special cooldowns, and a 2-3 move
special table (charge / stink bomb / reinforcements / sand throw / ground
slam / grab / flurry) with phase-gated unlocks.

### BossComponent (`AddBoss`/`GetBoss` on EntityManager)
Plain-data component alongside AIComponent: phase, special run state
(windup -> active -> recover), QTE state/window/cooldown, enrage multiplier.
`Engine::SpawnBoss` builds the NPC with boss vitals; `UpdateNpcs` routes boss
entities into `Engine::UpdateBossEntity` (dedicated FSM — never flees, never
returns home; handles phases, specials, QTEs and its own melee progress).

### Phases
- **Phase 1** (100-66%): basic pattern, first special only.
- **Phase 2** (66-33%): 2nd special unlocks, cadence x0.72, special cd x0.75.
- **Phase 3** (33-0%): 3rd special, cadence x0.50, special cd x0.55, wider
  heavy-attack mix. Transitions fire a shockwave QTE (dodge it or take 8).

### Specials
Per-boss moves through a windup (0.55s telegraph) -> active -> recover
machine: Russell charge (dodgeable, 22 dmg), Derby stink bombs (ticking AoE
zones), Johnny reinforcements (up to 2 crony NPCs), Gary sand throw
(projectile) + ground slam (AoE + zone), grabs (break-free QTE) and flurries
(3 heavy swings through the normal NPC attack path). Zone/crony entity
creation is **deferred** (`FlushBossSpawns` after `UpdateNpcs`) — entity
creation inside the AI `ForEachAI` would reallocate the dense arrays and
dangle the loop's references.

### QTEs
One live prompt at a time: random key (W/A/S/D) in a shrinking window
(1.5s, 1.1s in phase 3). Success -> boss takes 14; failure -> player takes
12. Fires on phase transitions, boss grabs (break free) and player
block-counters (punish window).

### HUD (`src/ui/ui.h/.cpp`)
Top-center boss bar with name, title, phase thresholds and a phase indicator
("PHASE III - ENRAGED" past 33%); QTE prompt box with the key glyph and a
draining window bar; SUCCESS/TOO SLOW result flashes; center-screen banners
(engage / phase-up / victory / defeat). Data flows through the plain-data
`HUDState` snapshot like every other widget.

### Victory / defeat
- Boss KO -> `OnBossDefeated`: banner, the authored `boss_victory` cutscene
  (data/cutscenes/demo_cutscene.json, Track 48 playback), `boss_duel`
  mission completion, cronies scatter. A fallback camera orbit
  (`UpdateBossCutsceneCamera`) covers the no-JSON case.
- Player KO -> `OnPlayerDefeatedByBoss`: checkpoint respawn (full health +
  1.5s i-frames), boss full reset at its spawn (retry is beatable), zones +
  QTE cleared, retry counter tracked.
- Jimmy void guard (`Engine::UpdateJimmy`): falling >6u below the last
  ground level respawns at the checkpoint (a boss-retry when engaged) —
  the spawn rooftop has walkable-looking edges and falls used to poison the
  camera with NaNs.

### Integration notes
The boss duel is sanctioned: hitting the boss grants no respect loss and no
wanted heat (`DamageNpc` exemption), and a boss KO of the player routes to
the boss retry instead of the generic Busted arrest cut.

## Verification (`scripts/track49_run.ps1`, or env vars directly)

`OPENBULLY_BOSS_DEMO=1` drives a scripted headless fight (demo clock pauses
while a cutscene owns input; game clock pinned at noon so the 23:00 curfew
cannot BUST the run): walk to the boss -> engagement -> block+swing ->
scripted -70 shocks force phase II/III -> scripted player death (retry
path) -> chip damage to the KO -> victory cutscene + mission complete.
QTE prompts 1-2 are answered (success path), later ones ignored (failure
path). `OPENBULLY_BOSS_TYPE` selects the roster slot (0-4) or `none`/
`off` disables the boss; `OPENBULLY_SHOT`/`_EVERY` capture TGA screenshots
of the bar + QTE prompt.

Observed log chain (build2 Release, 2026-10-22): spawn -> ENGAGED ->
charge special -> PHASE II -> QTE SUCCESS -> retry path -> PHASE II again ->
QTE #2 -> GRAB + break-free QTE -> PHASE III (cd 0.75s) -> QTE FAILED ->
`BOSS VICTORY: RUSSELL down after 1 retries, 2/3 QTEs won` -> `boss_victory`
cutscene played + finished -> `MISSION COMPLETE: 'Yard Boss Duel'`.
Plain 135s Release stability run: survived. 44 TGA screenshots captured.

## Known issues (pre-existing, out of scope)
Debug (MSVC Debug config) builds exit with 0x80000003 ~9-11s into any run —
reproduces with `OPENBULLY_BOSS_TYPE=none` (no boss at all) and on this tree
with eight earlier tracks' uncommitted WIP, so it is not Track 49 code. The
crash reporter (`src/platform/crash_report.cpp`) is installed in main and
will log `CRASH: code ... at module+0x...` through the game logger when it
is eventually chased. Release builds are unaffected (135s+ verified).
