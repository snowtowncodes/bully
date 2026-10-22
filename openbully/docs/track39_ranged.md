# Track 39: Ranged Combat

Slingshot projectile mechanics layered on the Track 26 weapon system.

## Projectile system

- `Engine::Projectile` (engine.h) gained `owner` (EntityID, AI attribution) and
  `knockback` (u/s impulse on an NPC hit) plus a `bounces` cap.
- Pool cap: `kMaxProjectiles = 50`; `SpawnProjectile` recycles the oldest live
  rock when full.
- Physics: linear motion with gravity (`p.velocity.y -= 9.8 * dt`), segment
  raycast from old → new position every frame against the collision world.

## Slingshot firing

- **RMB** with the slingshot equipped toggles **aim mode** (`m_aiming`):
  HUD crosshair + `ROCKS n/cap` readout (UI track-39 block in DrawHUD).
  RMB no longer blocks while the slingshot is out (UpdateJimmyCombat gates
  `blocking` on `!m_aiming`).
- **LMB** fires from Jimmy's chest toward the **camera aim direction** when
  aiming (yaw live, pitch clamped to `kAimPitchMin/Max = -0.25..0.45` so the
  3rd-person follow camera's downward view doesn't fire into the ground);
  body-facing otherwise.
- Ammo: the slingshot table row now carries 15 damage / 20 starting rocks
  (Track 39 spec: 15 dmg, 20 rocks per pickup).
- Reload: auto when firing on empty (`auto-reloaded to N rocks`) or manual
  **R** (`UpdateAimMode`), capped at `kAmmoCapacity = 30`.

## Projectile collision (UpdateProjectiles)

- World hit → shatter + `SpawnImpactFx` puff (4 short-lived debris boxes).
- NPC hit → `DamageNpc` (flinch + AI reaction event queue, Track 28 chain:
  hit → ALERT → CHASE), 15 damage, **headshot ×2** (head sphere 0.35u at
  1.55u), knockback impulse 3 u/s along the projectile's XZ velocity applied
  through `Melee::CombatComponent::knockbackVel`.
- Vehicle hit → elastic bounce (velocity reflected ×0.5, rock pushed out of
  the hull along the AABB normal; 3 bounces then the rock despawns — no
  infinite jitter).
- Track 38's garbage-lid block (Jimmy holding a lid shatters incoming rocks)
  is preserved ahead of the NPC sphere test.
- Impact FX / entity creation is deferred until after the ForEachAI loop:
  creating entities mid-iteration reallocates the component arrays and
  dangles the loop's references (this crashed once during verification).

## Ammo pickups

- `SpawnAmmoBoxes` drops 3 ground-snapped boxes around the player spawn
  (also respawned by `RespawnDynamicEntities` after F9 loads).
- Walk-over within 1.2u adds 20 rocks (cap 30) to the equipped/first
  slingshot slot; boxes carry no WeaponComponent, so the E-pickup path can
  never hand them out as a 0-durability slingshot (this bug was found and
  fixed during verification). Boxes bob/spin like weapon pickups.

## Verification

- `OPENBULLY_WEAPON_DEMO=1`: auto-pickup slingshot → aim mode (crosshair
  frames) → auto-fires every 0.9 s; if no NPC is within 60 u it spawns one
  box-NPC target. Run16 evidence: `target box NPC spawned`, two
  `Projectile hit NPC idx=70 for 15`, `entity 70 IDLE -> ALERT`.
- Screenshots: `track39_air_*.tga` (rock in flight), `docs/track39_ranged.png`.
- Stability: 125 s Release run, alive throughout, 0 errors
  (`track39_stab.log`).
- Both Debug and Release builds pass.

## Notes / known limits

- Slow-time 0.5x during aim was optional and not implemented.
- The demo's roof shell under the spawn has no collision (pre-existing);
  rocks fired in aim mode there arc into the void and expire by life — hit
  proof uses the demo's spawned target NPC.
- A parallel session was editing ui.h/ui.cpp journal code during this track;
  two merge collisions were reconciled (JournalMissionEntry::currentObjective
  member; duplicate ClampJournalScroll definition).
