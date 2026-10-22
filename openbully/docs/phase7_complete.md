# Phase 7 Complete: Combat, Grappling, Dodge/Block, Weapons

**Date:** 2026-10-22
**Status:** All 4 tracks verified in Release, combined-session and 2-minute stability runs clean.

## Scope

Phase 7 integrates four combat tracks onto the Phase 4–6 foundation
(entities, Jimmy, NPCs, missions/HUD/audio):

| Track | System | Files |
|-------|--------|-------|
| 23 | Combat core (melee attacks, hit detection, combos) | `src/combat/combat.{h,cpp}`, engine hooks |
| 24 | Grappling (3 moves, break-free) | `src/game/combat.h`, engine hooks |
| 25 | Dodge / Block / Counter (i-frames, stamina) | `src/game/player.h`, engine hooks |
| 26 | Weapons (pickup, equip, melee/ranged, durability) | `src/game/weapon.h`, engine hooks |

All engine integration lives in `openbully/src/engine/engine.{h,cpp}`
(~5,000 lines region: `UpdateJimmyCombat`, `UpdateMeleeCombat`,
`HandleCombatInput`, `UpdateGrapple`, `UpdateWeapons`, `UpdateProjectiles`).

## Known-issue resolution (verified)

- **`CombatState` name collision (Tracks 23 vs 24):** resolved by
  namespacing. Track 23's melee state machine lives in
  `OpenBully::Melee::CombatState` (`src/combat/combat.h`); Track 24's
  grapple flow keeps `OpenBully::CombatState` (`src/game/combat.h`).
  Both compile and coexist — the combined run exercises both in the same
  session.
- **`DrawText` macro leak:** full Release build is clean; no
  `DrawText`-related errors or warnings (the only build warnings are the
  pre-existing MSB8065 shader-stamp notices from the bgfx shader step).

## Build

```
cmake --build build --config Release
# -> build\Release\openbully.exe  (no C++ errors or warnings)
```

## Runtime verification (scripted demos)

Each demo is a headless scripted run; logs in `openbully/phase7_*.log`,
runner scripts in `openbully/scripts/phase7_run.ps1` /
`phase7_combined.ps1`.

### OPENBULLY_MELEE_DEMO=1 — melee attacks + combos
Attacks land on NPCs with per-attack damage/range; the
Light→Light→Heavy 3-hit combo applies the finisher multiplier:
- `atk_light_punch hit NPC idx=46 for 10 (combo 1)` … `(combo 2)`
- `atk_heavy_punch hit NPC idx=46 for 40 FINISHER (combo 3)`
- Combo continues across targets; a miss resets it (`whiffed (nothing in the 1.6 sphere)`).

### OPENBULLY_GRAPPLE_DEMO=1 — grapple sequence
- `Grapple: started (npc #46, mash threshold 10)`
- `Grapple headlock strike: 8 damage -> NPC health 42`
- `Grapple knee strike: 12 damage -> NPC health 30`
- `Grapple: NPC broke free after 10 presses` → `Grapple: ended (target broke free)`
(Sequence completes end-to-end including the break-free path.)

### OPENBULLY_COMBAT_DEMO=1 (paired with OPENBULLY_HUD_DEMO=1) — dodge/block/counter
- `Block: entered (50% damage reduction)` → `Hit BLOCKED (HUD demo): 10 -> 5 damage`
- `COUNTER! well-timed block -> next attack x4.0 (window 0.6s)`
- `Dodge roll: dir (0.00, 1.00), i-frames 0.5s (stamina 80/100)`
- `Hit DODGED (HUD demo): 0 damage (i-frames 0.44s left)` — i-frames verified
- `COUNTER ATTACK! damage 40 (stamina 90/100)` — counter consumed
- Stamina drain + regen traced once per second in the log.

### OPENBULLY_WEAPON_DEMO=1 — pickup / fire
- 4 pickups spawn from World.img NIFs: `Weapon pickup 'Bat' / 'GarbageLid' / 'Yardstick' / 'Slingshot'`
- `Weapon demo: picked up Slingshot (slot 0)`
- `Projectile hit NPC idx=36 for 12 (health 88/100)` — ranged fire + hit
- Durability/ammo tracked (`Slingshot durability 40`, `Slingshot: out of ammo`).

## Combined session (all systems coexisting)

`scripts/phase7_combined.ps1` runs with all five demo env vars set for
2 minutes. Highlights proving coexistence:
- Melee combo + grapple on the same NPC; block→`COUNTER ATTACK with Slingshot!`
  → projectile for 48 (counter ×4 applied to the ranged weapon).
- Dodge roll with i-frames while a melee combo is in flight.
- Two NPCs defeated during the run (`NPC idx=46 defeated`, `NPC idx=36 defeated`).
- Zero `[ERROR]`/assert lines in the full 2-minute log.

## Stability

2-minute run: `Frame timing: 4.17 ms avg (240.0 fps)` at start **and**
end — no drift, no leaks observed, no errors. (~28,800 frames.)

## Screenshot

`docs/phase7_combat.png` (from `phase7_combined.tga`, 1280×720): 3rd-person
Jimmy mid-session with the grapple pose-freeze fallback, weapon pickups
placed in the yard, health/stamina HUD bars and vitals widgets top-right,
minimap bottom-right.

## How to reproduce

```powershell
cmake --build openbully\build --config Release
powershell -File openbully\scripts\phase7_run.ps1       # 4 demos + screenshots
powershell -File openbully\scripts\phase7_combined.ps1  # combined + 2-min stability
```
