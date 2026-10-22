#pragma once

// Track 43: Wanted (Trouble) system - gameplay rules + runtime state.
//
// The trouble meter itself is the 0-5 head counter already living on
// PlayerController::wanted (Track 20); this header holds everything that
// does not belong to a single entity: the shared tuning constants, the
// crime catalogue, and the system state the Engine drives per frame
// (HUD flash timer, the bust fade-out sequence, hide/leave-area decay
// accumulation, the curfew throttle and the per-crime re-trigger cooldowns).
//
// Every illegal act funnels through Engine::AddWanted, which clamps the
// level, stamps the HUD flash, plays the whistle cue the first time the
// player goes hot, and throttles repeats so one fight cannot be worth five
// heads. Escape is symmetric: hide (B) decays one head every 10 s, being
// clear and alone decays one head every 20 s, and the wardrobe (V) resets
// instantly when nobody is chasing.

namespace OpenBully {

// One illegal act. Doubles as the index of WantedSystem::lastCrimeTime, so
// adding a new crime is one enum value plus one hook into Engine::AddWanted.
enum class WantedCrime : unsigned char {
    Fighting = 0,       // punching someone in front of authority -> +1
    AttackAuthority,    // hitting a prefect or cop              -> +2
    Vandalism,          // throwing at / breaking property        -> +1
    Trespass,           // inside a restricted area               -> +1
    Curfew,             // out after 23:00                        -> +1
    Count
};

struct WantedSystem {
    // --- tuning -------------------------------------------------------------
    static constexpr int   kMaxHeads           = 5;     // 0 = clean, 5 = max heat
    static constexpr int   kCopSpawnAtHeads    = 4;     // 4-5 heads: cops spawn
    static constexpr int   kCopSpawnCount      = 2;
    static constexpr float kHideDecayInterval  = 10.0f; // hidden: 1 head per 10 s
    static constexpr float kLeaveDecayInterval = 20.0f; // clear + alone: 1 per 20 s
    static constexpr float kAuthorityClearDist = 30.0f; // "left the area" radius
    static constexpr float kGrabRange          = 2.2f;  // bust when within reach
    static constexpr float kCopSpawnDistance   = 20.0f; // cops walk in from here
    static constexpr float kBustFadeSeconds    = 0.8f;  // one half of the screen cut
    static constexpr float kCurfewStartMin     = 23.0f * 60.0f; // 23:00
    static constexpr float kCurfewEndMin       = 7.0f * 60.0f;  // until 07:00
    static constexpr float kHiddenBreakDist    = 3.0f;  // hidden still seen at this range
    static constexpr float kCrimeMinGap        = 3.0f;  // s between the same crime firing
    static constexpr float kAuthorityHuntRange = 18.0f; // sight radius while wanted > 0
    static constexpr int   kMoneyLossPercent   = 50;    // bust = lose 50% of the cash

    WantedSystem()
    {
        // First-crime guarantee: start each category far enough in the past
        // that kCrimeMinGap can never throttle the initial charge.
        for (double& t : lastCrimeTime) t = -1e9;
    }

    // --- runtime ------------------------------------------------------------
    float  flashTimer    = 0.0f;   // HUD red-flash seconds remaining
    float  bustFade      = 0.0f;   // 0..1 screen black-out (0 = nothing to draw)
    int    bustPhase     = 0;      // 0 = none, 1 = fading out, 2 = fading in
    float  bustTimer     = 0.0f;   // counts up through fade-out, down through fade-in
    bool   hidden        = false;  // tucked in a locker / trash can
    float  hideTimer     = 0.0f;   // accumulation toward the next decay head
    float  safetyTimer   = 0.0f;   // accumulation while clear and alone
    bool   trespassInside = false; // latch: one head per restricted-zone entry
    bool   curfewCharged = false;  // this night's curfew head already added
    bool   chased        = false;  // last frame any authority was engaged on Jimmy
    // Engine time of the last hit for each crime category; initialized far
    // in the past so the FIRST crime always charges (a stale 0 would get
    // throttled against kCrimeMinGap during the first 3 s of play).
    double lastCrimeTime[WantedCrime::Count] = {};
};

} // namespace OpenBully
