// Track 46: stunt trick system implementation. See tricks.h.

#include "game/tricks.h"

#include <algorithm>
#include <cstdio>

namespace OpenBully {

// ---------------------------------------------------------------------------
// Names + achievements table
// ---------------------------------------------------------------------------

const char* TrickName(TrickType t) {
    switch (t) {
        case TrickType::Wheelie:  return "WHEELIE";
        case TrickType::Stoppie:  return "STOPPIE";
        case TrickType::BunnyHop: return "BUNNY HOP";
        case TrickType::Grind:    return "GRIND";
        default:                  return "TRICK";
    }
}

static TrickAchievement s_achievements[] = {
    // name              desc                                        type                   count  combo  score  unlocked
    {"First Flair",      "Land your first trick",                    TrickType::None,         1,    0,     0,     false},
    {"Wheelie Master",   "Land 100 wheelies",                        TrickType::Wheelie,    100,    0,     0,     false},
    {"Hop Wizard",       "Land 25 bunny hops",                       TrickType::BunnyHop,    25,    0,     0,     false},
    {"Combo King",       "Reach a x5 trick combo",                   TrickType::None,         0,    5,     0,     false},
    {"Style Icon",       "Earn 5,000 style points",                  TrickType::None,         0,    0,  5000,     false},
};

TrickAchievement* GetTrickAchievements() { return s_achievements; }
int               TrickAchievementCount() {
    return int(sizeof(s_achievements) / sizeof(s_achievements[0]));
}

// ---------------------------------------------------------------------------
// TrickSystem helpers
// ---------------------------------------------------------------------------

void TrickSystem::ResetSession() {
    active       = TrickType::None;
    activeTime   = 0.0f;
    activePoints = 0.0f;
    comboMult    = 1;
    pending      = 0;
    window       = 0.0f;
    popupTimer   = 0.0f;
    popupText[0] = '\0';
}

void TrickSystem::BankAndReset() {
    // Dismount cleanup: what was earned stays earned.
    if (pending > 0) {
        totalScore += pending;
        pending = 0;
    }
    active       = TrickType::None;
    activeTime   = 0.0f;
    activePoints = 0.0f;
    comboMult    = 1;
    window       = 0.0f;
}

void TrickSystem::Bail() {
    // Crash rule: the un-banked chain is lost, multiplier resets.
    pending   = 0;
    comboMult = 1;
    window    = 0.0f;
    active       = TrickType::None;
    activeTime   = 0.0f;
    activePoints = 0.0f;
}

// ---------------------------------------------------------------------------
// Achievement check (after every state-changing event)
// ---------------------------------------------------------------------------

static int CheckAchievements(TrickSystem& ts) {
    int totalLanded = 0;
    for (const TrickTypeStats& st : ts.perType) totalLanded += st.landed;

    const int n = TrickAchievementCount();
    for (int i = 0; i < n; ++i) {
        TrickAchievement& a = s_achievements[i];
        if (a.unlocked) continue;
        if (a.count > 0) {
            const int have = a.type == TrickType::None
                                 ? totalLanded
                                 : ts.perType[int(a.type)].landed;
            if (have < a.count) continue;
        }
        if (a.comboMult > 0 && ts.bestMult < a.comboMult) continue;
        if (a.score > 0 && ts.totalScore < a.score) continue;
        a.unlocked = true;
        return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Main step
// ---------------------------------------------------------------------------

void StepTricks(TrickSystem& ts, const TrickInputs& in, float dt,
                TrickEvents& out) {
    dt = std::clamp(dt, 0.0f, 0.1f);
    if (ts.popupTimer > 0.0f) ts.popupTimer -= dt;

    // --- Bail (crash/eject): combo is lost, live trick discarded ------------
    if (in.crash) {
        const bool hadCombo = ts.pending > 0 || ts.comboMult > 1;
        const int  lost     = ts.pending;
        if (hadCombo) {
            out.comboLost = true;
            out.lostPoints = lost;
            std::snprintf(ts.popupText, sizeof(ts.popupText), "COMBO LOST");
            ts.popupTimer = 2.0f;
        }
        ts.Bail();
        return;
    }

    // --- Chain window tick: expiry banks pending and closes the combo -------
    if (ts.window > 0.0f) {
        ts.window -= dt;
        if (ts.window <= 0.0f) {
            ts.window = 0.0f;
            if (ts.pending > 0) {
                out.banked       = true;
                out.bankedPoints = ts.pending;
                ts.totalScore   += ts.pending;
                ts.pending       = 0;
            }
            ts.comboMult = 1;
        }
    }

    // --- Active trick: accrue, or land when the pose releases ----------------
    if (ts.active != TrickType::None) {
        bool holding = false;
        switch (ts.active) {
            case TrickType::Wheelie:  holding = in.wheelie; break;
            case TrickType::Stoppie:  holding = in.stoppie; break;
            case TrickType::BunnyHop:
                // Airborne is the "pose"; a short grace window absorbs the
                // 1-2 grounded frames between the impulse and liftoff.
                holding = in.airborne || ts.activeTime < 0.3f;
                break;
            default: holding = false; break;
        }

        if (holding) {
            ts.activeTime += dt;
            float rate = 0.0f;
            switch (ts.active) {
                case TrickType::Wheelie:  rate = kTrickWheeliePts; break;
                case TrickType::Stoppie:  rate = kTrickStoppiePts; break;
                case TrickType::BunnyHop: rate = kTrickHopAirPts;  break;
                default: break;
            }
            // Raw points accrue (float — int(rate*dt) would truncate the
            // 50/40 pts-per-second rates to zero at 60 Hz); the combo
            // multiplier applies at landing.
            ts.activePoints += rate * dt;
            // Live popup: trick name + growing score while the pose is held
            // (the Engine logs the start/land edges; this keeps the HUD alive
            // mid-trick so a screenshot during a wheelie shows the counter).
            std::snprintf(ts.popupText, sizeof(ts.popupText), "%s +%d",
                          TrickName(ts.active), int(ts.activePoints));
            ts.popupTimer = std::max(ts.popupTimer, 0.5f);
        } else {
            // Pose released: land (score) if long enough to count.
            if (ts.activeTime >= kTrickMinTime) {
                out.landed       = true;
                out.landedType   = ts.active;
                out.landedTime   = ts.activeTime;
                out.landedMult   = ts.comboMult;
                out.landedPoints = int(ts.activePoints) * ts.comboMult;
                ts.pending      += out.landedPoints;
                TrickTypeStats& st = ts.perType[int(ts.active)];
                st.landed += 1;
                st.longestTime = std::max(st.longestTime, ts.activeTime);
                st.totalPoints += out.landedPoints;
                // Chain: every land arms the NEXT trick with a higher
                // multiplier as long as it starts inside the window (the
                // window expiry below resets the multiplier to x1).
                ts.comboMult = std::min(ts.comboMult + 1, kTrickComboMax);
                ts.bestMult  = std::max(ts.bestMult, ts.comboMult);
                ts.window = kTrickComboWindow;
            }
            // Too short to score: silently discard (tap filters).
            ts.active       = TrickType::None;
            ts.activeTime   = 0.0f;
            ts.activePoints = 0.0f;
        }
    }

    // --- Starts (only when idle) ----------------------------------------------
    if (ts.active == TrickType::None && in.mounted) {
        // Priority: hop edge > wheelie pose > stoppie pose (the Engine
        // guarantees wheelie/stoppie are mutually exclusive: wheelie needs
        // throttle, stoppie needs brake-only).
        TrickType want = TrickType::None;
        if (in.hopStart && in.grounded)      want = TrickType::BunnyHop;
        else if (in.wheelie)                 want = TrickType::Wheelie;
        else if (in.stoppie)                 want = TrickType::Stoppie;

        if (want != TrickType::None) {
            ts.active       = want;
            ts.activeTime   = 0.0f;
            ts.activePoints = 0.0f;
            out.started     = true;
            out.startedType = want;
            // Bunny hop seeds the flat base reward; airtime adds on top.
            if (want == TrickType::BunnyHop)
                ts.activePoints = kTrickHopBasePts;
        }
    }

    // --- Achievements -----------------------------------------------------------
    out.achievement = CheckAchievements(ts);
}

} // namespace OpenBully
