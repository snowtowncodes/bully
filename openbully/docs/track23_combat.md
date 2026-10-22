# Track 23: Combat System Core — Complete

Phase 6, Track 23 (150 min budget). Melee combat for Jimmy: attack animations
from World.img, hit detection, damage/knockback/stun, combos, input.

## What was built

### 1. Combat core (`src/combat/combat.{h,cpp}`)

- `Melee::CombatState`: IDLE / ATTACKING / STUNNED / KNOCKED_DOWN.
  (Namespace `Melee` avoids the Track 24 grapple `CombatState` in
  `game/combat.h`.)
- `Melee::AttackType`: LIGHT_PUNCH / HEAVY_PUNCH / LIGHT_KICK / HEAVY_KICK.
- `Melee::AttackData`: damage, range, effective duration, hit-frame window
  (normalized animation progress), knockback impulse, stun time.
- `Melee::CombatComponent`: state, attack type, timers, combo counter,
  damage dealt/taken, `RegisterHit`/`ResetCombo`.
- Combo: Light → Light → Heavy = 3-hit chain, the heavy lands at ×2 damage
  as a FINISHER; the combo resets on a whiff or after 2 s of idle.

### 2. Attack animations from World.img (asset pipeline)

- `Anim/C_Player.agr` (extracted from `Stream/World.img`, the player
  animation group) is decoded once by `Engine::LoadCombatAnimations()` —
  439/439 animations.
- The `.agr` carries no names; the four attack clips are picked by index
  through the MAINPED.HXD dictionary (`docs/hxd.md`):
  `#34 PUNCH_S_4` (light punch), `#93 HEAVY_PUNCH_2` (heavy punch),
  `#163 LIGHT_KICK` (light kick), `#152 POW_KICK_3` (heavy kick).
  The mapping is verified: the group's record sizes sum to the file size
  exactly (1,327,168 bytes).
- `Engine::CreateRig` registers the clips on every skinned rig
  (`atk_light_punch` etc.), so Jimmy AND NPCs can play them.

### 3. Hit detection + damage (`Engine::UpdateMeleeCombat`)

- During ATTACKING, the normalized clip progress is checked against the
  attack's hit-frame window; the sphere check fires ONCE per attack.
- Sphere center = attacker + forward·(range/2), radius = range/2; candidates
  are NPCs whose world AABB the sphere touches (nearest wins).
- On hit: target Health (Track 20/26 vitals) drops, target enters STUNNED
  (or KNOCKED_DOWN at 0 HP), knockback impulse away from the attacker
  (decays exponentially), combo increments; damage popup on the HUD.
- Stunned/knocked-down NPCs skip their AI tick (they reel and stay down).

### 4. Input

- LMB (Button::Attack) → light punch; RMB (Button::Block press-edge) →
  heavy punch; Q → light kick; E → heavy kick (raw scancode edge).
- Attacks are blocked while dodging (Track 25) or with a weapon equipped
  (Track 26 owns LMB then — the two systems coexist by design).

### 5. HUD feedback

- Top-center damage popup: "HIT -10 COMBO x4" / "FINISHER -40 ...".
- Bottom-left combo meter + 2 s combo-window bar while a chain is live.

## Verification

- Full build passes (Release, openbully + shaders).
- `OPENBULLY_MELEE_DEMO=1 OPENBULLY_SHOT=track23_cam OPENBULLY_SHOT_DELAY=24
  OPENBULLY_SHOT_EVERY=1 OPENBULLY_NPC_COUNT=6` — scripted run:
  - "Combat anims: 439 decoded ... (expect 439)" + 4 attack clips bound.
  - Log shows light→light→heavy chains: `hit NPC idx=33 for 10 (combo 1)`,
    `... (combo 2)`, `40 FINISHER (combo 3)`, whiffs resetting the combo,
    NPCs reaching 0/100 and KNOCKED_DOWN.
  - Screenshot `track23_cam_4.png`: damage popup + combo counter on screen
    during a fight.

## Known limitations

- The demo anchor (ASY_ABlock) has no collision beneath its roof shell —
  committed Track 21 runs show the same free-fall. The melee demo drives
  Jimmy's position directly and holds him at the target's plane to stay on
  the script; normal input gameplay is unaffected.
- Kick clips registered for all rigs but the scripted demo only punches;
  Q/E exercise the same path (`StartMeleeAttack(LightKick/HeavyKick)`).
- Damage numbers are static text (no floating 3D projections yet).
