#pragma once

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
};

} // namespace OpenBully
