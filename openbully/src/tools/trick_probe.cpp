// Track 46: headless trick-scoring probe (no GPU, no SDL, no engine).
// Verifies the StepTricks state machine end to end:
//   - duration scoring (longer wheelie = more points)
//   - shortest-trick filter (tap < kTrickMinTime scores nothing)
//   - combo chaining (x2, x3 inside the 5 s window; reset outside)
//   - bank on window expiry
//   - bail: crash forfeits pending + resets multiplier
//   - achievement unlock (First Flair on the first land)
// Mirrors the tools/vehicle_test.cpp conventions.

#include "game/tricks.h"

#include <cstdio>

using namespace OpenBully;

static int g_checks = 0, g_failed = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++g_checks;                                                         \
        if (cond) {                                                         \
            std::printf("[PASS] %s\n", msg);                                \
        } else {                                                            \
            ++g_failed;                                                     \
            std::printf("[FAIL] %s\n", msg);                                \
        }                                                                   \
    } while (0)

static TrickInputs GroundedInputs() {
    TrickInputs in;
    in.mounted = in.grounded = true;
    in.airborne = false;
    return in;
}

int main() {
    TrickSystem ts;
    TrickInputs in = GroundedInputs();
    TrickEvents ev;

    // --- 1. wheelie scoring: 1.2 s hold lands ~72 pts, longer hold lands more
    bool sawStart = false;
    TrickType startType = TrickType::None;
    in.wheelie = true;
    for (int i = 0; i < 72; ++i) { // 1.2 s at 60 Hz
        ev = TrickEvents{};
        StepTricks(ts, in, 1.0f / 60.0f, ev);
        if (ev.started) { sawStart = true; startType = ev.startedType; }
    }
    CHECK(sawStart && startType == TrickType::Wheelie,
          "wheelie start event fired");
    in.wheelie = false;
    ev = TrickEvents{};
    StepTricks(ts, in, 1.0f / 60.0f, ev); // release frame -> land
    CHECK(ev.landed && ev.landedType == TrickType::Wheelie, "wheelie landed");
    const int wheelieShortPts = ev.landedPoints;
    CHECK(wheelieShortPts >= int(60.0f * 1.0f), // >= 1.0 s worth (accrual starts
          "1.2s wheelie scores ~duration x 60/s");        // a frame late)
    CHECK(ts.comboMult == 2, "combo bumped to x2 after first land");
    CHECK(ts.pending == wheelieShortPts, "landed points are pending");
    CHECK(GetTrickAchievements()[0].unlocked, "First Flair unlocked");

    // --- 2. tap filter: a 0.1 s "trick" scores nothing
    const int pendingBefore = ts.pending;
    in.wheelie = true;
    for (int i = 0; i < 6; ++i) { ev = TrickEvents{}; StepTricks(ts, in, 1.0f / 60.0f, ev); }
    in.wheelie = false;
    ev = TrickEvents{};
    StepTricks(ts, in, 1.0f / 60.0f, ev);
    CHECK(!ev.landed, "0.1s tap does not land (min-time filter)");
    CHECK(ts.pending == pendingBefore, "tap left pending unchanged");

    // --- 3. chained stoppie inside the window scores at x2
    in.stoppie = true;
    for (int i = 0; i < 60; ++i) { ev = TrickEvents{}; StepTricks(ts, in, 1.0f / 60.0f, ev); }
    in.stoppie = false;
    ev = TrickEvents{};
    StepTricks(ts, in, 1.0f / 60.0f, ev);
    CHECK(ev.landed && ev.landedType == TrickType::Stoppie, "stoppie landed");
    CHECK(ev.landedMult == 2, "chained stoppie scored at x2");
    const int stoppiePts = ev.landedPoints;
    CHECK(stoppiePts == int(ev.landedTime * 50.0f) * 2 ||
              stoppiePts == (int(ev.landedTime * 50.0f) + 1) * 2,
          "stoppie points = raw x multiplier");
    CHECK(ts.comboMult == 3, "combo bumped to x3 after second land");

    // --- 4. window expiry banks the chain
    in.stoppie = false;
    bool sawBank = false;
    int  bankedPts = 0;
    for (int i = 0; i < 61 * 6; ++i) { // 6 s idle -> window (5 s) expires
        ev = TrickEvents{};
        StepTricks(ts, in, 1.0f / 60.0f, ev);
        if (ev.banked) { sawBank = true; bankedPts = ev.bankedPoints; }
    }
    CHECK(sawBank, "chain banked on window expiry");
    CHECK(bankedPts == pendingBefore + stoppiePts,
          "banked amount = wheelie + stoppie pending");
    CHECK(ts.totalScore == bankedPts, "bank moved pending into total");
    CHECK(ts.pending == 0 && ts.comboMult == 1, "combo closed after bank");

    // --- 5. combo reset when the window lapses between tricks
    in.wheelie = true;
    for (int i = 0; i < 72; ++i) { ev = TrickEvents{}; StepTricks(ts, in, 1.0f / 60.0f, ev); }
    in.wheelie = false;
    ev = TrickEvents{}; StepTricks(ts, in, 1.0f / 60.0f, ev); // land (x1 again)
    CHECK(ts.comboMult == 2, "fresh combo after window lapse");
    for (int i = 0; i < 61 * 6; ++i) { ev = TrickEvents{}; StepTricks(ts, in, 1.0f / 60.0f, ev); }

    // --- 6. bail: crash forfeits pending + resets multiplier
    in.wheelie = true;
    for (int i = 0; i < 72; ++i) { ev = TrickEvents{}; StepTricks(ts, in, 1.0f / 60.0f, ev); }
    in.wheelie = false;
    ev = TrickEvents{}; StepTricks(ts, in, 1.0f / 60.0f, ev); // land -> pending > 0
    const int pendingAtRisk = ts.pending;
    CHECK(pendingAtRisk > 0, "pending score at risk before bail");
    in.crash = true;
    ev = TrickEvents{};
    StepTricks(ts, in, 1.0f / 60.0f, ev);
    CHECK(ev.comboLost && ev.lostPoints == pendingAtRisk, "bail loses pending");
    CHECK(ts.pending == 0 && ts.comboMult == 1 && ts.active == TrickType::None,
          "bail resets combo state");
    in.crash = false;

    // --- 7. bunny hop: base + airtime, scored at the multiplier in effect
    in.hopStart = true;
    bool sawHopStart = false;
    ev = TrickEvents{};
    StepTricks(ts, in, 1.0f / 60.0f, ev);
    sawHopStart = ev.started;
    CHECK(sawHopStart && ev.startedType == TrickType::BunnyHop, "hop started");
    in.hopStart = false;
    in.airborne = true;
    in.grounded = false;
    for (int i = 0; i < 54; ++i) { ev = TrickEvents{}; StepTricks(ts, in, 1.0f / 60.0f, ev); } // 0.9 s air
    in.airborne = false;
    in.grounded = true;
    ev = TrickEvents{};
    StepTricks(ts, in, 1.0f / 60.0f, ev);
    CHECK(ev.landed && ev.landedType == TrickType::BunnyHop, "hop landed on wheels-down");
    CHECK(ev.landedPoints >= int(kTrickHopBasePts), "hop scores at least the base");

    // --- 8. stability: 2 minutes of mixed input never produces NaN/negatives
    for (int i = 0; i < 60 * 120; ++i) {
        in.wheelie = (i % 180) < 90;
        in.stoppie = (i % 180) >= 90 && (i % 300) < 150;
        in.hopStart = (i % 240) == 0;
        in.airborne = (i % 97) < 30;
        in.grounded = !in.airborne;
        in.crash = (i % 601) == 600;
        ev = TrickEvents{};
        StepTricks(ts, in, 1.0f / 60.0f, ev);
        if (ts.pending < 0 || ts.totalScore < 0 || ts.activeTime < 0.0f ||
            ts.comboMult < 1 || ts.comboMult > kTrickComboMax) {
            CHECK(false, "2-min stability: state went out of range");
            break;
        }
        in.crash = false;
    }
    CHECK(true, "2-min stability: trick state stayed in range");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
