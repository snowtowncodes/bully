#pragma once

// Track 49: Boss fights.
//
// BossComponent marks an NPC entity as a faction leader (Gary, Russell,
// Johnny, Derby, Ted) and carries the enhanced-combat state that
// Engine::UpdateBossEntity consumes on top of the regular Track 27 AI:
//
//   Phases   health-band FSM — Phase 1 (100-66%), Phase 2 (66-33%),
//            Phase 3 (33-0%, "desperate"). Each phase raises the attack
//            cadence (enrageMult) and unlocks special attacks; the 2nd and
//            3rd transitions fire a dodge QTE (success: 0 damage taken,
//            failure: shockwave damage).
//   Specials every boss has 2-3 unique moves (charge, stink bombs,
//            reinforcements, sand/perfume spray, ground slam, grab, flurry)
//            selected from its definition table, phase-gated.
//   QTE      one live prompt at a time (qteState Waiting); a random key
//            within a shrinking window. Success damages the boss, failure
//            damages the player. State + result flash are HUD-visible.
//
// The component is plain data (no SDL / engine types — the scancode is an
// int) so the entity layer stays dependency-free, matching the other
// components.

#include <cstdint>

namespace OpenBully {

// Boss roster. Index order doubles as the definition-table index.
enum class BossType : uint8_t {
    Gary    = 0,
    Russell = 1,
    Johnny  = 2,
    Derby   = 3,
    Ted     = 4,
    Count   = 5,
};

// Special attacks (per-boss subsets defined in boss.cpp).
enum class BossSpecial : uint8_t {
    None          = 0,
    Charge        = 1,  // windup then dash; contact damage (dodgeable)
    StinkBomb     = 2,  // lobs a zone at the player (ticking area damage)
    Reinforce     = 3,  // calls 2 cronies that chase the player
    SandThrow     = 4,  // ranged spray (small damage, blinds: stamina burn)
    GroundSlam    = 5,  // AoE around the boss (radius damage)
    Grab          = 6,  // close-range grapple attempt -> QTE break free
    Flurry        = 7,  // 3 rapid heavy swings
};

// Lifecycle of one QTE prompt.
enum class QteState : uint8_t {
    None    = 0,
    Waiting,    // prompt live: press qteKey before qteTimer expires
    Success,    // result flash (resultTimer)
    Failed,
};

// Health-band phases (values are the phase numbers shown on the HUD).
enum class BossPhase : uint8_t {
    One   = 1,   // 100-66%: basic pattern
    Two   = 2,   // 66-33%: faster attacks, specials unlock
    Three = 3,   // 33-0%: desperate/enraged, all specials, finisher
};

// Tuning for one boss type. All numbers gameplay-grade (seconds / HP).
struct BossDefinition {
    const char*  name;                  // boss bar headline
    const char*  title;                 // boss bar subtitle
    int          maxHealth;             // boss HP pool
    float        walkSpeed;             // u/s chase speed (AI moveSpeed)
    float        attackCooldown;        // phase-1 melee cadence (s)
    float        specialCooldown;       // base gap between specials (s)
    float        specialRange;          // max trigger distance (u)
    float        chargeDamage;          // charge/slam contact damage
    float        chargeSpeed;           // dash speed (u/s)
    BossSpecial  specials[3];           // move list (None = unused slot)
    // First phase that unlocks specials[1] / specials[2] (0 = never).
    uint8_t      unlockSecondPhase;
    uint8_t      unlockThirdPhase;
};

// One special's live state (windup -> active -> recover).
struct BossSpecialRun {
    BossSpecial kind    = BossSpecial::None;
    uint8_t     stage   = 0;      // 0 idle, 1 windup, 2 active, 3 recover
    float       timer   = 0.0f;   // countdown of the current stage
    bool        fired   = false;  // payload applied (charge hit / zone / etc.)
    int         flurryLeft = 0;   // Flurry: swings still queued
};

// Per-boss-entity state. Attached alongside AIComponent on the boss NPC.
struct BossComponent {
    BossType  type      = BossType::Russell;
    BossPhase phase     = BossPhase::One;
    int       maxHealth = 300;         // mirrors the definition (HUD scale)

    // --- special attack state ----------------------------------------------
    BossSpecialRun special;               // active move (None = free)
    float specialTimer  = 4.0f;          // countdown to the next special
    float specialCd     = 7.0f;          // current cooldown (phase-scaled)
    uint8_t specialSlot = 0;             // round-robin index into the table

    // --- engagement / behavior ---------------------------------------------
    bool  engaged      = false;          // sighted the player at least once
    float enrageMult   = 1.0f;           // melee cooldown multiplier per phase
    float finisherTimer = 0.0f;          // Phase-3 finisher telegraph window

    // --- QTE -----------------------------------------------------------------
    QteState qteState    = QteState::None;
    int      qteKey      = 0;            // SDL scancode value (plain int)
    float    qteWindow   = 1.4f;         // total prompt window (s)
    float    qteTimer    = 0.0f;         // remaining window (s)
    float    qteCooldown = 0.0f;         // gap before the next prompt may fire
    int      qteCount    = 0;            // prompts raised this fight
    int      qteWins     = 0;            // prompts won this fight

    // Result flash (HUD): result state + remaining flash time.
    float    resultTimer = 0.0f;
    // Phase-transition flash (HUD): >0 while "PHASE II/III" glows.
    float    phaseFlash  = 0.0f;
};

// Definition table lookup (out-of-line in boss.cpp).
const BossDefinition& GetBossDefinition(BossType type);
const char*           BossTypeName(BossType type);
const char*           BossSpecialName(BossSpecial s);
// 'W'/'A'/'S'/'D'/' ' for the QTE prompt glyph (0 = unknown scancode).
char                  BossQteKeyChar(int sdlScancode);

} // namespace OpenBully
