#pragma once

// Track 46: stunt trick system — scoring, combos, achievements.
//
// State machine layered ON TOP of Track 32 (physics) + Track 33 (feel):
//   - the feel layer owns trick POSES (FeelState wheelie/stoppie angles);
//   - this module owns trick SCORING: which trick is active, how long it
//     has run, points accrued, the chain multiplier, pending/banked score,
//     per-type career stats and the achievement table.
//
// Inputs arrive once per frame from Engine::UpdateTrickScoring as a plain
// TrickInputs snapshot; StepTricks advances the state machine and reports
// edge events (started / landed / banked / comboLost / achievement) in
// TrickEvents for the Engine to log / pop up / play sounds on.
//
// Standalone by design (no glm, no engine deps) so a headless probe can
// verify scoring without a window.

#include <cstdint>

namespace OpenBully {

// --- Trick types --------------------------------------------------------------
enum class TrickType : int {
    None     = -1,
    Wheelie  = 0,   // W hold + S tap on the bike (or Shift + W, Track 33 path):
                    // rear wheel lifts, front stays up.
    Stoppie  = 1,   // hold S (brake) while rolling forward: front dives,
                    // rear lifts (brake-dive endo).
    BunnyHop = 2,   // tap Space while rolling: upward impulse, scored on the
                    // airtime until the wheels re-contact.
    Grind    = 3,   // rail grinding — RESERVED. The demo level has no rail
                    // collision data to trace yet; the type exists so saves/
                    // stats/UI keep a stable slot (spec: sparks on grind
                    // "if implemented" — deferred, needs rail geometry).
    Count
};

const char* TrickName(TrickType t);   // "WHEELIE", "STOPPIE", "BUNNY HOP", "GRIND"

// --- Scoring tunables -----------------------------------------------------------
inline constexpr float kTrickWheeliePts  = 60.0f;  // points per second held
inline constexpr float kTrickStoppiePts  = 50.0f;  // points per second held
inline constexpr float kTrickHopBasePts  = 30.0f;  // flat reward for hopping
inline constexpr float kTrickHopAirPts   = 40.0f;  // + per second airborne
inline constexpr float kTrickMinTime     = 0.40f;  // s: shortest scored trick
inline constexpr float kTrickComboWindow = 5.0f;   // s: chain window after a land
inline constexpr int   kTrickComboMax    = 8;      // multiplier cap
// Track 46: bunny-hop launch impulse (u/s upward velocity added on Space tap).
inline constexpr float kBunnyHopImpulse  = 4.5f;

// --- Per-type career stats --------------------------------------------------------
struct TrickTypeStats {
    int   landed      = 0;    // tricks completed (>= kTrickMinTime)
    float longestTime = 0.0f; // best single duration (s)
    int   totalPoints = 0;    // scored points (multiplier applied) for this type
};

// --- Achievements -----------------------------------------------------------------
// Plain-data rows checked by StepTricks after every land / bank / combo bump.
// A row unlocks when ALL of its non-zero goal fields are met.
struct TrickAchievement {
    const char* name;
    const char* desc;
    TrickType type;      // per-type landing count goal (None = any trick)
    int       count;     // 0 = not count-bound
    int       comboMult; // reach this chain multiplier (0 = not bound)
    int       score;     // career score >= this (0 = not bound)
    bool      unlocked;
};

// Shared table (mutable: `unlocked` flips inside StepTricks).
TrickAchievement* GetTrickAchievements();
int               TrickAchievementCount();

// --- Runtime state -----------------------------------------------------------------
struct TrickSystem {
    // Live trick (None while idle).
    TrickType active       = TrickType::None;
    float     activeTime   = 0.0f; // s since the trick started
    float     activePoints = 0.0f; // raw points accrued (multiplier applied at land)

    // Combo state.
    int   comboMult = 1;       // multiplier the CURRENT/next landing uses
    int   pending   = 0;       // landed points awaiting the bank (window expiry)
    float window    = 0.0f;    // s remaining in the chain window (0 = closed)
    int   bestMult  = 1;       // career-high multiplier reached

    // Career totals (session-persistent; ResetSession keeps these).
    int totalScore = 0;
    TrickTypeStats perType[int(TrickType::Count)];

    // HUD popup state (Engine::Render draws it melee-popup style).
    char  popupText[64] = {};
    float popupTimer    = 0.0f;

    // Mount cleanup: clear the live trick + combo, keep career totals.
    void ResetSession();
    // Dismount cleanup: end the live trick, bank pending into the total.
    void BankAndReset();
    // Crash cleanup: drop pending, reset the multiplier (bail rule).
    void Bail();
};

// Per-frame input snapshot (Engine fills from feel/state/input edges).
struct TrickInputs {
    bool mounted  = true;
    bool wheelie  = false;  // wheelie pose held this frame (feel layer)
    bool stoppie  = false;  // stoppie pose held this frame (feel layer)
    bool hopStart = false;  // Space edge while rolling+grounded this frame
    bool grounded = false;
    bool airborne = false;
    bool crash    = false;  // crash/eject edge -> bail
};

// Edge events for one step (Engine consumes; all false on silent frames).
struct TrickEvents {
    bool        started      = false;
    TrickType   startedType  = TrickType::None;
    bool        landed       = false;
    TrickType   landedType   = TrickType::None;
    int         landedPoints = 0;  // multiplier already applied
    int         landedMult   = 1;
    float       landedTime   = 0.0f;
    bool        banked       = false;
    int         bankedPoints = 0;
    bool        comboLost    = false;
    int         lostPoints   = 0;
    int         achievement  = -1; // index into the achievement table, -1 = none
};

// Advance the trick state machine by dt (clamped to 0.1 s). Safe to call with
// mounted=false every frame — tricks simply cannot start.
void StepTricks(TrickSystem& ts, const TrickInputs& in, float dt, TrickEvents& out);

} // namespace OpenBully
