#pragma once

// AIComponent (Phase 5, Track 16).
//
// Marks an entity as an NPC and carries the simple state machine used by
// Engine::UpdateNpcs:
//   IDLE       — stand still for a moment, then pick a wander target
//   WANDER     — walk toward targetPos; on arrival go back to IDLE
//   FACE_PLAYER— player within faceRadius: turn to look, resume when far
//
// Movement is authored in the engine (raycast ground snap + sphere wall
// resolve reuse the collision world), so this component is plain data.

#include <glm/glm.hpp>

namespace OpenBully {

enum class AIState : uint8_t {
    Idle = 0,
    Wander,
    FacePlayer,
};

struct AIComponent {
    AIState  state       = AIState::Idle;
    float    timer       = 0.0f;    // seconds remaining in IDLE (or accumulated)
    glm::vec3 targetPos{0.0f};      // WANDER destination (y = ground)
    bool     hasTarget   = false;
    glm::vec3 homePos{0.0f};        // spawn point; wander targets stay near it

    // Tuning (per-NPC so the crowd can be varied later).
    float moveSpeed    = 1.5f;      // u/s walking
    float wanderRadius = 6.0f;      // max distance from homePos
    float idleTimeMin  = 1.0f;      // IDLE duration range (random in-between)
    float idleTimeMax  = 4.0f;
    float faceRadius   = 5.0f;      // player-detection radius (task spec)
    float turnSpeed    = 6.0f;      // rad/s toward facing target
    float facingYaw    = 0.0f;      // current body yaw (radians, 0 = +Z)
};

} // namespace OpenBully
