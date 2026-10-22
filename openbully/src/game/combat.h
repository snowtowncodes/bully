#pragma once

// CombatComponent (Phase 6, Track 24: grappling system).
//
// CombatState covers the grapple flow between two characters:
//   Grabbing     - this entity is the ATTACKER holding a grapple
//   Grappled     - this entity is the TARGET being held
//   BreakingFree - this entity is the target mashing out (short exit state)
//   None         - no combat interaction (normal gameplay)
//
// The pairing itself lives in the Engine (one grapple at a time in the demo
// level: Engine::m_grappleTarget); this component is plain per-entity data
// so the entity layer stays dependency-free.
//
// Grapple moves (attacker input, Track 24):
//   HeadlockStrike (U) 8 dmg, KneeStrike (I) 12 dmg - grapple continues
//   Throw (O) 20 dmg - ends the grapple, target staggers out
//
// Break free (target): button mashing. Human targets mash any attack key /
// Space (edge-detected presses); NPC targets accumulate simulated presses on
// a timer. mashCount >= mashRequired -> BreakingFree -> None.

#include <cstdint>

namespace OpenBully {

enum class CombatState : uint8_t {
    None = 0,
    Grabbing,       // attacker: holding the grapple
    Grappled,       // target: being held
    BreakingFree,   // target: mash threshold reached, exiting
};

enum class GrappleMove : uint8_t {
    None = 0,
    HeadlockStrike,
    KneeStrike,
    Throw,
};

struct CombatComponent {
    CombatState state      = CombatState::None;
    float stateTimer       = 0.0f;   // seconds in the current state
    GrappleMove move       = GrappleMove::None;
    float moveCooldown     = 0.0f;   // attacker: time until the next move
    int   health           = 50;     // combat vitality (NPC; Jimmy uses PlayerController::health)
    // Break-free mashing (target side).
    int   mashCount        = 0;
    int   mashRequired     = 10;
    float npcMashTimer     = 0.0f;   // simulated mash cadence for NPC targets
    float npcMashInterval  = 0.45f;  // one simulated press every 0.45 s
};

// Attack tuning shared by Engine::GrappleAttack.
struct GrappleMoveInfo {
    int    damage;
    float  cooldown;
    bool   endsGrapple;
    const char* name;
};

} // namespace OpenBully
