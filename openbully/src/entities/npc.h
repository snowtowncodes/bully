#pragma once

// AIComponent (Phase 5, Track 16; Track 27 AI state machine; Track 28
// reaction states; Track 29 schedule hookups).
//
// Marks an entity as an NPC and carries the state machine used by
// Engine::UpdateNpcs:
//   IDLE        stand still for a moment, then patrol or pick a wander target
//   WANDER      walk toward targetPos; on arrival go back to IDLE
//   FACE_PLAYER — legacy soft-face state (player within faceRadius)
//   PATROL      Track 27: walk the patrolPoints loop in order
//   ALERT       noticed the player in sight (Track 27) or a fight (Track 28);
//               brief deciding pause, then act
//   CHASE       pursue the target (Track 27: the player; Track 28:
//               targetEntity); direct steering + sidestep when blocked
//   ATTACK      Track 27: in melee range; swing with a cooldown through the
//               Track 23 melee combat path
//   FLEE        run away (Track 27: low health; Track 28: threatPos)
//   RETURN      Track 27: lost the target; walk back to the patrol route /
//               home, then resume IDLE
//   WATCH       Track 28: crowd behavior — stand at a slot, face the fight
//   KO          Track 28: knocked out; fall over, respawn after a delay
//
// Movement is authored in the engine (raycast ground snap + sphere wall
// resolve reuse the collision world), so this component is plain data.

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

#include "entity_id.h"        // EntityID (own header: no entity.h cycle)

#include "../game/player.h" // Track 26: Health for NPC damage
#include "../game/faction.h" // Track 42: Faction enum (plain data, no cycles)

namespace OpenBully {

enum class AIState : uint8_t {
    Idle = 0,
    Wander,
    FacePlayer,
    // Track 27:
    Patrol,
    // Track 27/28: engaged states.
    Alert,
    Chase,
    Attack,
    Flee,
    Return,
    // Track 28: crowd reactions.
    Watch,
    Ko,
};

// Track 28: NPC social role. Prefects respond to witnessed fights by
// chasing + attacking the aggressor; civilians watch (or help).
enum class AIRole : uint8_t {
    Civilian = 0,
    Prefect,
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
    float faceRadius   = 5.0f;      // legacy soft-face radius (Track 16)
    float turnSpeed    = 6.0f;      // rad/s toward facing target
    float facingYaw    = 0.0f;      // current body yaw (radians, 0 = +Z)

    // Track 26: vitals so weapon attacks (melee + slingshot) can hurt NPCs.
    Health health;

    // --- Track 27: patrol ---------------------------------------------------
    // Waypoints walked in order, looping. Empty = no patrol (wander instead).
    std::vector<glm::vec3> patrolPoints;
    uint32_t patrolIndex = 0;

    // --- Track 27: perception + combat tuning -------------------------------
    float sightRadius    = 10.0f;   // player-detect radius (u, task spec)
    float attackRange    = 1.8f;    // melee-trigger distance (u, task spec)
    float attackCooldown = 1.5f;    // seconds between melee swings (task spec;
                                    // Track 28 CHASE uses the same rate limit)
    float chaseSpeedMult = 2.0f;    // chase/flee speed = moveSpeed * this
    float fleeHealthFrac = 0.2f;    // health fraction below which the NPC flees
    float alertDuration  = 1.0f;    // ALERT pause before committing to CHASE
    float loseSightTime  = 5.0f;    // seconds unseen before giving up (RETURN)
    float stuckTime      = 2.0f;    // seconds without progress before sidestep

    // --- Track 27: runtime state --------------------------------------------
    float alertTimer    = 0.0f;     // counts down while ALERT
    float attackCdTimer = 0.0f;     // counts down between NPC attacks
    float loseTimer     = 0.0f;     // accumulates while engaged + target unseen
    float stuckTimer    = 0.0f;     // accumulates while blocked (CHASE/RETURN)
    float sidestepTimer = 0.0f;     // active sidestep remaining (0 = none)
    float lastTargetDist = 0.0f;    // previous-frame target distance (stuck check)
    glm::vec3 lastPos{0.0f};        // position at the last stuck check
    glm::vec3 sidestepDir{0.0f};    // sidestep direction (unit, XZ)
    glm::vec3 fleeDir{0.0f};        // current flee direction (unit, XZ)

    // --- Track 28: reaction state -------------------------------------------
    AIRole   role          = AIRole::Civilian;

    // --- Track 42: faction membership ---------------------------------------
    // Assigned at spawn from the model index (Engine::FactionForModel);
    // AIRole::Prefect NPCs are Faction::Authority. Respect bands
    // (Engine::m_factions) gate perception + AI reactions.
    Faction  faction       = Faction::Townies;

    EntityID targetEntity{};        // CHASE: who we pursue/attack
    glm::vec3 threatPos{0.0f};      // FLEE: run away from here
    glm::vec3 fightPos{0.0f};       // WATCH/ALERT: fight location to face
    float    stateTimer   = 0.0f;   // generic countdown (alert/flee/chase)
    float    flinchTimer  = 0.0f;   // procedural hit-reaction lean (s)
    float    koTimer      = 0.0f;   // KO: seconds until respawn
    float    fallTimer    = 0.0f;   // KO: fall-over animation progress (s)
    float    watchTimer   = 0.0f;   // WATCH: refreshed while the fight is hot

    // --- Track 29: schedule hookups ------------------------------------------
    int  scheduleIndex = -1;    // index into Engine's schedule list (-1 = unscheduled)
    bool lockIdle      = false; // schedule pins this NPC in place (no wandering)
};

} // namespace OpenBully
