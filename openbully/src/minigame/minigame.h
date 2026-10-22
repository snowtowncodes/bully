#pragma once

// Minigame framework (Track 44).
//
// A reusable full-screen minigame system class sessions (Track 41+) plug
// into: the class system calls MinigameManager::StartMinigame(type) when an
// activity starts, registers SetOnResult(...) once, and treats the finished
// MinigameResult (pass/fail + 0..100 score) as the grade. The Engine owns
// the manager and gates gameplay input while an activity is on screen.
//
// Lifecycle inside the manager (Phases):
//   StartMinigame -> INTRO   title + instructions screen (SPACE begins)
//                 -> PLAYING the game consumes dedicated raw-key input
//                 -> RESULT  PASS/FAILED banner (R retry / E continue)
//                 -> Inactive
// ESC forfeits during INTRO/PLAYING (result still fires with forfeited=true)
// and confirms the RESULT screen. The Engine keeps journal-ESC priority by
// passing allowEsc=false on frames where the journal consumed the key.
//
// Input separation: minigames read *raw scancodes* through the same Input
// snapshot (edge API), so keys typed in a minigame never leak into gameplay
// consumers the frame the manager is active (Engine also strip-freezes all
// logical buttons + mouse deltas while active). Headless verification uses
// OPENBULLY_MINIGAME_DEMO, which injects synthetic raw presses through
// Input::SetRawKey and drives this exact input path.
//
// Progress (optional): per-type win count + best score persist to
// <base>/saves/minigames.json (LoadProgress at Init, auto-save per result).

#include "../game/input.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace OpenBully {

class UIRenderer; // src/ui/ui.h (Engine owns one; passed in Update/Draw)

namespace Minigames {

// --- Activity types ---------------------------------------------------------
// The six class minigame slots. Three ship with this track; the rest are
// reserved (CreateMinigame returns nullptr until implemented).
enum class MinigameType : int {
    WordScramble = 0,   // English: unscramble the word (typed, 30 s)
    MathQuiz,           // Math: 5 questions, 4 choices each (10 s/question)
    ButtonMash,         // Gym: hit 60 SPACE presses before the 10 s runs out
    Rhythm,             // reserved (future: press to the beat)
    TargetPractice,     // reserved (future: aim + fire)
    MemoryMatch,        // reserved (future: tile pairs)
    Count
};

const char* MinigameTypeName(MinigameType t);   // "WORD SCRAMBLE", ...
// Menu/hint subject tag ("ENGLISH", "MATH", "GYM", ...).
const char* MinigameTypeSubject(MinigameType t);

// --- Result handed to the class system --------------------------------------
struct MinigameResult {
    MinigameType type  = MinigameType::WordScramble;
    bool   completed   = false;  // natural end (vs ESC forfeit)
    bool   passed      = false;  // success threshold met
    int    score       = 0;     // class-facing grade, 0..100
    float  timeTaken   = 0.0f;  // seconds spent in the PLAYING phase
    bool   forfeited   = false; // quit with ESC
};

// --- Input isolation boundary -------------------------------------------------
// Per-frame edge snapshot of the keys a minigame is allowed to consume.
// The Engine fills this right after Input::Poll while an attempt is active
// (so demo-injected keys are included), then clears the shared scancodes
// for the rest of the frame. Gameplay systems that reuse those keys --
// inventory 1-5, reload R, journal J, grapple G/U/I/O, save F5/F9, the
// arrows, the schedule toggle C -- therefore cannot fire while a minigame
// is on screen, and the minigame still sees every press. This is the
// whole-game key set, not a copy of Input.
struct MinigameInput {
    bool escape    = false;   // forfeit (INTRO/PLAYING) / continue (RESULT)
    bool space     = false;   // begin (INTRO), mash (Gym)
    bool backspace = false;   // erase (English)
    bool enter     = false;   // submit (Math), continue (RESULT)
    bool up        = false;   // cursor (Math)
    bool down      = false;   // cursor (Math)
    bool retry     = false;   // R: new attempt (RESULT)
    bool cont      = false;   // E: continue to gameplay (RESULT)
    bool letter[26] = {};     // A..Z, edge
    bool digit[4]   = {};     // 1..4, edge

    // Copies the minigame key set from a live Input snapshot.
    void CaptureFrom(const Input& in);
    // First pressed letter, or 0.
    char TypedLetter() const;
    // Pressed choice digit 1..4, or 0.
    int  DigitPressed() const;
};

// --- Base interface ----------------------------------------------------------
// One instance per attempt; Init() re-randomizes (seed != 0 pins the RNG).
class IMinigame {
public:
    virtual ~IMinigame() = default;

    virtual MinigameType GetType() const = 0;
    virtual const char*  GetTitle() const = 0;
    virtual const char*  GetInstructions() const = 0;

    // (Re)start a fresh attempt with the given RNG seed.
    virtual void Init(uint32_t seed) = 0;
    // Per-frame gameplay update (only runs in the PLAYING phase).
    virtual void Update(float dt, const MinigameInput& in) = 0;
    // Draws the game content inside the panel rect (Manager draws the
    // panel/frame/timer chrome around it). All coordinates are UI pixels.
    virtual void Draw(UIRenderer& ui, float px, float py, float pw, float ph,
                      float scale) = 0;

    virtual bool IsComplete() const = 0;
    virtual MinigameResult GetResult() const = 0;
    // One-line summary for the RESULT banner ("SOLVED  MISTAKES 2" etc).
    virtual std::string GetResultLine() const = 0;

    // --- scripted verification hooks (OPENBULLY_MINIGAME_DEMO only) ---------
    // Scancode a perfect player would press for scripted step `step`
    // (manager's per-attempt press counter), or 0 = nothing. Default: none.
    virtual int DemoKey(int step) const { (void)step; return 0; }
    // Ask the game to play a failing attempt (MathQuiz answers wrong).
    virtual void SetDemoFailRun(bool fail) { (void)fail; }
};

// Factory: nullptr for reserved/not-yet-implemented types.
std::unique_ptr<IMinigame> CreateMinigame(MinigameType type);

// --- Manager ------------------------------------------------------------------
// Owns the active attempt, phase transitions, retry/forfeit flow, the result
// callback and the optional progress file. The Engine calls StartMinigame on
// behalf of the class system, then Update + Draw once per frame.
class MinigameManager {
public:
    // Class-system hook: fires exactly once per finished attempt (pass, fail
    // or forfeit). Rewards/grades belong in the callee.
    using ResultCallback = std::function<void(const MinigameResult&)>;

    void SetOnResult(ResultCallback cb) { m_onResult = std::move(cb); }

    // Class-system entry point: opens the full-screen overlay. Returns false
    // when an attempt is already active or the type is not implemented.
    bool StartMinigame(MinigameType type, uint32_t seed = 0);

    bool        IsActive() const { return m_phase != Phase::Inactive; }
    MinigameType ActiveType() const { return m_type; }
    // Attempts since the last non-retry StartMinigame (retries included).
    int         Attempts() const { return m_attempts; }

    // Per-frame phase machine. `allowEsc` = this frame's ESC belongs to the
    // minigame (Engine clears the quit latch for the same edge).
    void Update(float dt, const MinigameInput& in, bool allowEsc);
    // Overlay chrome + active game content (screen-space UI pixels).
    void Draw(UIRenderer& ui, float screenW, float screenH);

    const MinigameResult& LastResult() const { return m_lastResult; }

    // --- progress persistence (<base>/saves/minigames.json) -----------------
    void LoadProgress(const std::string& baseDir);   // Engine::Init
    void SaveProgress(const std::string& baseDir);   // auto after each result
    int  Wins(MinigameType t) const      { return InRange(t) ? m_wins[int(t)]  : 0; }
    int  BestScore(MinigameType t) const { return InRange(t) ? m_best[int(t)] : 0; }

    // --- OPENBULLY_MINIGAME_DEMO scripted driver -----------------------------
    // Replays the full framework story through the real input path: scramble
    // win -> quiz fail + R retry -> quiz pass -> mash ESC-forfeit -> mash
    // pass. Injects raw presses via Input::SetRawKey (one frame each).
    void DriveDemo(double now, Input* input, const std::string& baseDir);

private:
    enum class Phase { Inactive, Intro, Playing, Result };
    // Demo sequence steps (OPENBULLY_MINIGAME_DEMO driver only).
    enum class DemoStep {
        WaitStart = 0,            // t>=1.0: StartMinigame(WordScramble)
        AwaitIntro,               // press SPACE on the instructions screen
        PlayingAuto,              // feed DemoKey while PLAYING; watch RESULT
        AwaitFailRetry,           // quiz failed: press R for a fresh attempt
        AwaitContinue,            // press E to leave the RESULT banner
        WaitNextActivity,         // idle gap, then queue the next activity
        AwaitIntroMashForfeit,    // mash #1 intro: SPACE then ESC mid-play
        PlayingMashForfeit,       // mash #1: ESC fires the forfeit path
        Done,
    };

    void StartAttempt(MinigameType type, uint32_t seed); // fresh instance
    void FinishAttempt(bool forfeited);                  // result + callbacks

    bool InRange(MinigameType t) const {
        return int(t) >= 0 && int(t) < int(MinigameType::Count);
    }

    std::unique_ptr<IMinigame> m_game;
    MinigameType m_type       = MinigameType::WordScramble;
    Phase        m_phase      = Phase::Inactive;
    float        m_phaseTimer = 0.0f;     // seconds in the current phase
    float        m_fade       = 0.0f;     // overlay fade-in 0..1
    uint32_t     m_seed       = 0;        // current attempt seed
    uint32_t     m_seedCounter= 1;        // fresh seeds on retry
    MinigameResult m_lastResult;
    ResultCallback m_onResult;
    int          m_attempts   = 0;         // resets on StartMinigame
    int          m_wins[int(MinigameType::Count)] = {};
    int          m_best[int(MinigameType::Count)] = {};
    std::string  m_progressDir;            // cached base dir for auto-save
    bool         m_progressDirty = false;

    // --- demo bot state -------------------------------------------------------
    DemoStep m_demoStep     = DemoStep::WaitStart;
    double   m_demoNextAt   = 0.0;   // gate for scripted presses/starts
    double   m_demoForfeitAt= 0.0;   // mash-forfeit ESC moment
    bool     m_demoKeyHeld  = false; // press/release alternation
    int      m_demoPressCount = 0;   // per-attempt scripted press counter
    int      m_demoBump     = 0;     // which demo activity is queued next
    bool     m_demoFailArmed= false; // next attempt plays to fail (quiz)
    bool     m_demoExternal = false; // driving an attempt a class started
    bool     m_demoLogged   = false; // final "demo complete" log guard
};

} // namespace Minigames
} // namespace OpenBully
