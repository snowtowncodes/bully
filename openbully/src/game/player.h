#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include "weapon.h" // Track 26: InventorySlot (parallel edit added slots[])

// PlayerController component (Phase 4, Track 15).
//
// Marks an entity as the player-controlled character and carries the
// movement tuning. The input snapshot itself lives in Input (polled once per
// frame by the Engine); this component only stores per-player state, so the
// entity layer stays free of SDL types.
//
// Jimmy's update (Engine::UpdateJimmy) consumes it:
//   - WASD moves the CHARACTER in camera-yaw space (never the camera
//     directly; the camera follows via smooth lerp),
//   - movement speed 4 u/s horizontally while on the ground,
//   - no horizontal control while airborne (gravity only).

namespace OpenBully {

// Track 20: player vitals. Kept in the player component so the HUD, gameplay
// code and debug keys all read/write one source of truth.
struct Health {
    int current = 100;
    int max     = 100;

    void Damage(int amount) {
        if (amount <= 0) return;
        current -= amount;
        if (current < 0) current = 0;
    }
    void Heal(int amount) {
        if (amount <= 0) return;
        current += amount;
        if (current > max) current = max;
    }
    float Fraction() const { return max > 0 ? float(current) / float(max) : 0.0f; }
};

// Track 25: stamina pool. Drains on dodge rolls (20), held blocks (5/s)
// and attacks (10); regenerates at `regenRate` whenever the player is not
// spending (block hold counts as spending, so it never nets positive).
struct Stamina {
    float current    = 100.0f;
    float max        = 100.0f;
    float regenRate  = 20.0f;   // units/second when not spending

    // Attempts to spend `amount`. False (and no change) when short — actions
    // must check the return instead of assuming.
    bool Spend(float amount) {
        if (amount <= 0.0f) return true;
        if (current < amount) return false;
        current -= amount;
        return true;
    }
    // Passive regeneration. Clamped to [0, max].
    void Regen(float dt) {
        current = std::min(max, current + regenRate * dt);
    }
    float Fraction() const { return max > 0.0f ? current / max : 0.0f; }
};

// Track 25: defensive state machine driven by Engine::UpdateJimmyCombat.
// Named DefenseState (not CombatState) because game/combat.h (Track 24
// grappling) already claims that name. Covering the defensive flow:
//   Normal  — free movement.
//   Dodge   — roll in a locked direction; brief invincibility, no attacks.
//   Block   — RMB held; 50% damage reduction, rooted in place.
//   Counter — brief window after a well-timed block; next attack is boosted.
enum class DefenseState : int { Normal = 0, Dodge = 1, Block = 2, Counter = 3 };

struct PlayerController {
    float moveSpeed     = 4.0f;   // horizontal units/sec (task spec)
    float turnSpeed     = 12.0f;  // rad/s toward the movement direction
    float groundSnapMax = 0.6f;   // max downward step snapped per frame (u)
    float radius        = 0.5f;   // collision sphere radius (task spec)
    float height        = 1.8f;   // standing height above the feet origin

    // Runtime state (updated per frame by Engine::UpdateJimmy).
    float facingYaw   = 0.0f;     // current body yaw (radians, 0 = +Z)
    bool  facingValid = false;    // false until the first move command

    // Track 20: vitals + wanted level (HUD state; debug keys mutate these).
    Health health;                // 100/100 by default
    int    wanted = 0;            // 0..5 wanted stars
    int    money  = 0;            // cash on hand (display only for now)

    // --- Track 25: dodge / block / stamina ---------------------------------
    Stamina     stamina;
    DefenseState state = DefenseState::Normal;
    float stateTimer        = 0.0f;  // counts down while in Dodge/Counter
    float invincibleTimer   = 0.0f;  // i-frames (0.5 s from dodge start)
    float dodgeCooldown     = 0.0f;  // prevents hold-shift roll chaining
    glm::vec3 dodgeDir{0.0f, 0.0f, 0.0f}; // locked roll direction (unit, XZ)
    float dodgeRollSpin     = 0.0f;  // accumulated yaw spin (radians)
    bool  counterArmed      = false; // counter window hit: next attack boosted
    float blockStartTime    = -1e9f; // m_time when the current block began
    float attackCooldown    = 0.0f;  // punch rate limit

    // --- Track 26: weapon inventory ----------------------------------------
    // Fixed 5 slots (selected with keys 1-5). An empty slot keeps
    // type == Unarmed; equippedSlot == -1 means the unarmed punch is used.
    InventorySlot slots[5];
    int           equippedSlot = -1;
};

} // namespace OpenBully
