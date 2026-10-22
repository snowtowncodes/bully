# Track 28: NPC Reaction System

Phase 7+ completion of the crowd AI: hit reactions, witness reactions, help
calls, crowd behavior, authority (prefect) response and KO/respawn handling.
The system is wired through the existing Track 27 NPC AI state machine
(`Alert/Chase/Attack/Flee/Return`) and the Track 23 melee combat path, so
NPC-vs-NPC and NPC-vs-player reactions share one steering + attack codepath.

## Event queue

`src/entities/ai_events.h` defines the abstract event queue:

```cpp
enum class AIEventType : uint8_t { Hit, WitnessedFight, HelpCall, NpcDefeated };
struct AIEvent { AIEventType type; EntityID sourceEntity; EntityID targetEntity;
                 glm::vec3 position; double time; };
```

Combat and projectile damage emit events; `Engine::UpdateNpcs` drains the queue
once per frame (`ProcessAIEvents`), then scans for new witnesses
(`ScanFightWitnesses`). Follow-up events (help calls) queue for the next frame,
which bounds per-frame work regardless of crowd size.

## Reaction table

| Event | Source | Reaction |
|---|---|---|
| `HIT` | attacker | victim fights back (`CHASE` the attacker) — or flees if health < 50% and a 50% chance rolls flee; always emits `HELP_CALL` |
| `HELP_CALL` | victim | same-model NPCs within 20 u chase the attacker |
| `WITNESSED_FIGHT` | aggressor | **prefect**: 0.4-0.8 s `ALERT` then `CHASE` + `ATTACK` the aggressor. **civilian**: 4-6 u crowd slot if the crowd has room, else keeps wandering |
| `NPC_DEFEATED` | attacker | log + mission hook (`NotifyNpcDefeated`) |

Witness detection is continuous, not event-only: any hot fight spot within 15 u
with line of sight from a roaming NPC (or one already standing in an idle
player-alert) emits `WITNESSED_FIGHT`. "Hot" = a hit landed within the last 10 s.
Fight spots are registered by `RegisterFight` on every hit and pruned 20 s after
going cold.

## States added

`AIState::Watch` and `AIState::Ko` were added to `AIComponent`
(`src/entities/npc.h`) with `AIRole { Civilian, Prefect }`. The Track 27
engaged states were made target-aware: `targetEntity` (an event-assigned
attacker/aggressor) takes priority, and without one the target is the player as
before. Flee now runs away from `threatPos`/target. Watch NPCs disperse through
`RETURN` 10 s after the last hit; KO'd NPCs fall over via an eased `rotation.x`
roll (`kFallTime` 0.5 s), lie down, and respawn at their spawn point with full
health after `kRespawnDelay` 10 s.

## Prefects

`data/npc_schedules.json` already carries a schedule named `prefect`, so
`SpawnScheduledNpc` designates scheduled spawns on a schedule whose name contains
`prefect` as authorities (and, on the legacy unscheduled path, the first two
spawned NPCs). Prefects get `sightRadius = 14.0` and `AIRole::Prefect`; they are
tracked in `Engine::m_prefectIds`. When a prefect's punch drops the player to 0
health, `Busted()` logs `BUSTED`, restores full health, bumps wanted +1 and
teleports the player back to spawn (`m_jimmySpawn`).

## Hit reaction animation

`C_Player.agr` has no named flinch clip available — `MAINPED.HXD` (the name
dictionary for the ped animation group) is not extracted, so clip indices cannot
be resolved by name. The hit reaction is therefore the existing melee stun freeze
plus knockback, plus a short procedural recoil lean (`rotation.x`, 0.35 s). KO
is the "fall over via rotation" fallback from the spec, not a KO animation.

## Wiring

- `ResolveMeleeHit` (player punch) → `DamageNpc(best, m_jimmyId, dmg, pos, stunTime)`; knockback stays in the caller.
- `WeaponTryAttack` (melee weapons) and `UpdateProjectiles` → `DamageNpc(...)` instead of raw `health.Damage` + destroy; NPCs are no longer destroyed on death, they KO and respawn.
- `ResolveNpcMeleeHit` (NPC punch lands on the player) → `Busted()` when a prefect kills the player.
- `DamageNpc` registers the fight spot, plays the flinch + stun, queues `HIT`, and calls `KnockOutNpc` at 0 health (which also fires the mission hook).

## Verification (2026-10-22, Windows Release)

`cmake --build build --config Release` — passes.

`OPENBULLY_MELEE_DEMO=1` 130 s run (scripted Jimmy walks into the crowd and
chains punches; log `track28_run8.log`):

```
[INFO] Melee: atk_light_punch hit NPC idx=40 for 10 (health 90/100, combo 1)
[INFO] NPC AI: entity 40 fights back against idx=32     <- HIT -> CHASE (fight back)
[INFO] NPC AI: entity 40 CHASE -> ATTACK (dist 1.4)     <- NPC melee on the player
[INFO] NPC AI: prefect 34 saw the fight - moving in on idx=32   <- authority response
[INFO] NPC AI: entity 43 flees (health 40/100)          <- low-health flee branch
[INFO] NPC idx=42 KO'd by idx=32 - down for 10s         <- KO at 0 health
[INFO] NPC idx=42 respawned at spawn point (health 100/100)
[INFO] BUSTED                                           <- prefect defeated the player
```

Counts across the run: 38 fight-backs, 14 flees, 10 KOs, 2 respawns,
1 BUSTED, 2 prefects designated, watch census peaking at 9-10 of the crowd,
169 ALERT -> WATCH transitions, zero error-log lines.

Screenshots (OPENBULLY_SHOT_EVERY=6): `docs/track28_crowd.png` - the fight
spot with surrounding NPCs converged/engaged; `docs/track28_ko_fall.png` -
KO'd NPCs lying on the ground (fall-over rotation). The melee-demo camera is a
tight 3.2 u follow, so the crowd frames are close-up; the census numbers above
are the authoritative crowd verification.
