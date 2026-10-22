#pragma once

// Track 23: melee combat core.
//
// Lives in namespace OpenBully::Melee: Phase 6 Track 24 already owns
// OpenBully::CombatState / OpenBully::CombatComponent (game/combat.h, the
// grapple flow), so the melee state machine keeps its spec names inside a
// nested namespace instead of colliding with it.
//
//   CombatState  IDLE / ATTACKING / STUNNED / KNOCKED_DOWN
//   AttackType   LIGHT_PUNCH / HEAVY_PUNCH / LIGHT_KICK / HEAVY_KICK
//   AttackData   damage, range, duration, hit frames (normalized animation
//                progress window in which the hit check fires, once/attack)
//   CombatComponent  per-entity melee state: state, attack type, stun timer,
//                combo counter, damage dealt/taken
//
// Attack animations: decoded from Anim/C_Player.agr (World.img player
// animation group). The .agr does not name its animations — indices come
// from the MAINPED.HXD dictionary ("GROUP\NAME" records, docs/hxd.md),
// verified against the shipped file: the group's record sizes sum to the
// C_Player.agr size exactly (439 records, 1,327,168 bytes).
//   34  C_PLAYER\PUNCH_S_4      light punch  (1.033 s authored)
//   93  C_PLAYER\HEAVY_PUNCH_2  heavy punch  (1.167 s)
//   163 C_PLAYER\LIGHT_KICK     light kick   (0.667 s)
//   152 C_PLAYER\POW_KICK_3     heavy kick   (1.100 s)

#include <cstdint>

#include <glm/glm.hpp>

#include "../game/player.h" // Health (Track 20 vitals)

namespace OpenBully {
namespace Melee {

// Attack type count sentinel (array bounds).
constexpr int kAttackTypeCount = 4;

enum class CombatState : uint8_t {
    Idle = 0,
    Attacking,
    Stunned,
    KnockedDown,
};

enum class AttackType : uint8_t {
    LightPunch = 0,
    HeavyPunch,
    LightKick,
    HeavyKick,
};

// Gameplay tuning for one attack. `duration` is the EFFECTIVE gameplay
// duration (the engine plays the authored clip at speed = authored /
// duration). `hitWindowStart/End` are normalized animation progress bounds
// ("hit frames"): during that window the attack's sphere check fires, once.
struct AttackData {
    const char* clipName;      // clip name registered on every rig
    int         clipIndex;     // C_Player.agr animation index (HXD order)
    float       authoredDur;   // HXD gameplay duration of the clip (s)
    float       duration;      // effective attack duration (s)
    float       damage;        // base damage
    float       range;         // forward sphere-check distance (u)
    float       hitWindowStart;
    float       hitWindowEnd;
    float       knockback;     // impulse (u/s) applied to the target
    float       stunTime;      // target stun duration (s)
};

const AttackData& GetAttackData(AttackType type);
AttackType        GetAttackType(int index); // 0..kAttackTypeCount-1

// Light -> Light -> Heavy = 3-hit combo: the heavy finishes it for bonus
// damage. Exposed so the engine applies it and the HUD can show it.
float ComboMultiplier(int comboCount, AttackType attack);

// Per-entity melee state. Jimmy and NPCs both carry one.
struct CombatComponent {
    CombatState state        = CombatState::Idle;
    AttackType  attack       = AttackType::LightPunch;
    float       attackTimer  = 0.0f;  // counts down while Attacking (s)
    float       attackYaw    = 0.0f;  // attack direction (rad, 0 = +Z)
    float       stunTimer    = 0.0f;  // counts down while Stunned (s)
    float       downTimer    = 0.0f;  // KnockedDown hold (0 = stays down)
    glm::vec3   knockbackVel{0.0f};   // knockback impulse (decays, u/s)

    // Combo tracking (consecutive successful hits).
    int        comboCount      = 0;
    AttackType comboPrev[2]    = {AttackType::LightPunch, AttackType::LightPunch};
    float      comboIdleTimer  = 0.0f;  // > 2 s of no hit resets the combo

    // One hit check per attack (fires once inside the hit window).
    bool hitApplied = false;

    // Scoreboard.
    int damageDealt = 0;
    int damageTaken = 0;

    // Combo bookkeeping on a landed hit. Returns the damage multiplier
    // applied (1.0 or the 3-hit finisher bonus).
    float RegisterHit(AttackType type) {
        comboCount++;
        comboIdleTimer = 0.0f;
        const float mult = ComboMultiplier(comboCount, type);
        comboPrev[1] = comboPrev[0];
        comboPrev[0] = type;
        return mult;
    }
    // Reset on a miss or after 2 s of idle.
    void ResetCombo() {
        comboCount     = 0;
        comboIdleTimer = 0.0f;
        comboPrev[0]   = AttackType::LightPunch;
        comboPrev[1]   = AttackType::LightPunch;
    }
};

} // namespace Melee
} // namespace OpenBully
