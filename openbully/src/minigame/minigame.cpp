// Minigame framework (Track 44) — implementation.
//
// Sample activities: WordScramble (English), MathQuiz (Math), ButtonMash
// (Gym). Rhythm / TargetPractice / MemoryMatch stay reserved until a track
// implements them (CreateMinigame logs + returns nullptr; StartMinigame
// refuses those types).
//
// Games draw through the shared UIRenderer primitives (view 10, screen-space
// UI pixels; same scale convention as DrawHUD: 1280x720 reference scaled by
// height). Games never touch SDL state or bgfx resources directly.

#include "minigame.h"

#include "../platform/logger.h"
#include "../ui/ui.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>

#if defined(_MSC_VER)
#include <filesystem>
#endif

namespace OpenBully {
namespace Minigames {

// --- Input isolation ---------------------------------------------------------

void MinigameInput::CaptureFrom(const Input& in) {
    // Order matches the fixed single-key fields.
    const SDL_Scancode keys[8] = {
        SDL_SCANCODE_ESCAPE, SDL_SCANCODE_SPACE, SDL_SCANCODE_BACKSPACE,
        SDL_SCANCODE_RETURN, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
        SDL_SCANCODE_R,      SDL_SCANCODE_E,
    };
    escape    = in.WasKeyPressed(keys[0]);
    space     = in.WasKeyPressed(keys[1]);
    backspace = in.WasKeyPressed(keys[2]);
    enter     = in.WasKeyPressed(keys[3]);
    up        = in.WasKeyPressed(keys[4]);
    down      = in.WasKeyPressed(keys[5]);
    retry     = in.WasKeyPressed(keys[6]);
    cont      = in.WasKeyPressed(keys[7]);
    for (int i = 0; i < 26; ++i)
        letter[i] = in.WasKeyPressed(SDL_Scancode(SDL_SCANCODE_A + i));
    for (int i = 0; i < 4; ++i)
        digit[i] = in.WasKeyPressed(SDL_Scancode(SDL_SCANCODE_1 + i));
}

char MinigameInput::TypedLetter() const {
    for (int i = 0; i < 26; ++i)
        if (letter[i]) return char('A' + i);
    return 0;
}

int MinigameInput::DigitPressed() const {
    for (int i = 0; i < 4; ++i)
        if (digit[i]) return i + 1;
    return 0;
}

namespace {

// Same packing as ui.cpp's local PackRGBA (Color0 = A<<24|B<<16|G<<8|R).
uint32_t Pack(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return uint32_t(a) << 24 | uint32_t(b) << 16 | uint32_t(g) << 8 | uint32_t(r);
}

const uint32_t kColDim      = Pack(0, 0, 0, 175);       // full-screen backdrop
const uint32_t kColPanel    = Pack(16, 18, 26, 235);    // panel body
const uint32_t kColBorder   = Pack(205, 195, 150, 200); // panel frame
const uint32_t kColHeader   = Pack(45, 40, 22, 235);    // title bar
const uint32_t kColTitle    = Pack(255, 220, 120, 255); // title text
const uint32_t kColText     = Pack(220, 220, 228, 245); // body text
const uint32_t kColSubtle   = Pack(160, 160, 170, 210); // hints
const uint32_t kColGood     = Pack(90, 220, 90, 255);
const uint32_t kColBad      = Pack(240, 80, 80, 255);
const uint32_t kColWarn     = Pack(255, 210, 60, 255);
const uint32_t kColAccent   = Pack(120, 200, 255, 255);
const uint32_t kColBarBg    = Pack(0, 0, 0, 180);
const uint32_t kColBarFrame = Pack(25, 25, 28, 200);
const uint32_t kColSlotBg   = Pack(30, 32, 40, 255);
const uint32_t kColSlotHi   = Pack(70, 100, 160, 255);
const uint32_t kColSlotGood = Pack(40, 120, 50, 255);
const uint32_t kColSlotBad  = Pack(140, 40, 40, 255);

uint32_t ScaleA(uint32_t rgba, float a) {
    const uint8_t alpha = uint8_t(float((rgba >> 24) & 0xFFu) * a);
    return (rgba & 0x00FFFFFFu) | (uint32_t(alpha) << 24);
}

// Centered text helper (advance width from the 5x7 font metrics).
void DrawTextCentered(UIRenderer& ui, float cx, float y,
                      const std::string& text, uint32_t rgba) {
    ui.DrawText(cx - ui.MeasureText(text) * 0.5f, y, text, rgba);
}

void DrawLineCentered(UIRenderer& ui, float cx, float y, const char* text,
                      uint32_t rgba) {
    DrawTextCentered(ui, cx, y, std::string(text), rgba);
}

// Panel frame: filled body + 2px border on all four sides.
void DrawPanelFrame(UIRenderer& ui, float px, float py, float pw, float ph,
                    uint32_t bodyRgba) {
    const float b = 2.0f;
    ui.DrawRect(px - b, py - b, pw + 2.0f * b, b, kColBorder);
    ui.DrawRect(px - b, py + ph, pw + 2.0f * b, b, kColBorder);
    ui.DrawRect(px - b, py, b, ph, kColBorder);
    ui.DrawRect(px + pw, py, b, ph, kColBorder);
    ui.DrawRect(px, py, pw, ph, bodyRgba);
}

// Generic countdown bar (red under 25% left).
void DrawTimerBar(UIRenderer& ui, float px, float py, float pw, float s,
                  float timeLeft, float timeTotal) {
    const float barH = 10.0f * s;
    const float frac = std::min(1.0f, std::max(0.0f, timeLeft / timeTotal));
    ui.DrawRect(px, py, pw, barH + 4.0f * s, kColBarFrame);
    ui.DrawRect(px + 1.0f * s, py + 1.0f * s, pw - 2.0f * s, barH + 2.0f * s,
                kColBarBg);
    const float w = std::max(0.0f, (pw - 4.0f * s) * frac);
    if (w > 0.0f)
        ui.DrawRect(px + 2.0f * s, py + 2.0f * s, w, barH,
                    frac > 0.25f ? kColAccent : kColBad);
}

// Horizontal progress (0..1) in a framed slot.
void DrawProgressBar(UIRenderer& ui, float x, float y, float w, float h,
                     float frac, uint32_t col) {
    frac = std::min(1.0f, std::max(0.0f, frac));
    ui.DrawRect(x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, kColBarFrame);
    ui.DrawRect(x, y, w, h, kColBarBg);
    if (frac > 0.0f)
        ui.DrawRect(x + 1.0f, y + 1.0f, std::max(0.0f, (w - 2.0f) * frac),
                    h - 2.0f, col);
}

SDL_Scancode LetterKey(char c) {
    c = char(std::toupper(c));
    return (c >= 'A' && c <= 'Z')
        ? SDL_Scancode(SDL_SCANCODE_A + (c - 'A')) : SDL_SCANCODE_UNKNOWN;
}

SDL_Scancode DigitKey(int n) { // n in 1..4
    return SDL_Scancode(SDL_SCANCODE_1 + (n - 1));
}

} // namespace

// ============================================================================
// Type metadata
// ============================================================================

const char* MinigameTypeName(MinigameType t) {
    switch (t) {
        case MinigameType::WordScramble:   return "WORD SCRAMBLE";
        case MinigameType::MathQuiz:       return "MATH QUIZ";
        case MinigameType::ButtonMash:     return "ENDURANCE MASH";
        case MinigameType::Rhythm:         return "RHYTHM";
        case MinigameType::TargetPractice: return "TARGET PRACTICE";
        case MinigameType::MemoryMatch:    return "MEMORY MATCH";
        default:                           return "MINIGAME";
    }
}

const char* MinigameTypeSubject(MinigameType t) {
    switch (t) {
        case MinigameType::WordScramble:   return "ENGLISH";
        case MinigameType::MathQuiz:       return "MATH";
        case MinigameType::ButtonMash:     return "GYM CLASS";
        case MinigameType::Rhythm:         return "MUSIC";
        case MinigameType::TargetPractice: return "SHOP";
        case MinigameType::MemoryMatch:    return "BIOLOGY";
        default:                           return "CLASS";
    }
}

// ============================================================================
// Word Scramble (English) — type the unscrambled word, 30 s timer.
// ============================================================================

class WordScrambleGame final : public IMinigame {
public:
    MinigameType GetType() const override { return MinigameType::WordScramble; }
    const char* GetTitle() const override { return "WORD SCRAMBLE"; }
    const char* GetInstructions() const override {
        return "THE LETTERS ARE SCRAMBLED. TYPE THE WORD!\n"
               "LETTERS TYPE - BACKSPACE ERASES - 30 SECONDS";
    }

    void Init(uint32_t seed) override {
        m_rng.seed(seed ? seed : 0x5A17u);
        m_word = kWords[m_rng() % kWordCount];
        m_scrambled = m_word;
        // Fisher-Yates until visibly different (guard against identity).
        for (int guard = 0; guard < 64; ++guard) {
            for (size_t i = m_scrambled.size() - 1; i > 0; --i) {
                const size_t j = m_rng() % (i + 1);
                std::swap(m_scrambled[i], m_scrambled[j]);
            }
            if (m_scrambled != m_word) break;
        }
        m_typed.clear();
        m_timeLeft   = kTimeLimit;
        m_elapsed    = 0.0f;
        m_mistakes   = 0;
        m_wrongFlash = 0.0f;
        m_complete   = false;
        m_result     = MinigameResult{};
        m_result.type = MinigameType::WordScramble;
    }

    void Update(float dt, const MinigameInput& in) override {
        if (m_complete) return;
        m_timeLeft -= dt;
        m_elapsed  += dt;
        if (m_wrongFlash > 0.0f) m_wrongFlash -= dt;

        // Typed letter + backspace, both from the isolated minigame snapshot.
        if (m_typed.size() < m_word.size()) {
            const char c = in.TypedLetter();
            if (c != 0) m_typed.push_back(c);
        }
        if (in.backspace && !m_typed.empty())
            m_typed.pop_back();

        // Full-length guess: compare, reset on a mismatch.
        if (m_typed.size() == m_word.size()) {
            if (m_typed == m_word) {
                m_complete         = true;
                m_result.completed = true;
                m_result.passed    = true;
                m_result.timeTaken = m_elapsed;
                // Grade: 55 base + speed bonus - 10 per mistake, 0..100.
                const float left = std::max(0.0f, m_timeLeft);
                m_result.score = std::min(100, std::max(0,
                    int(55.0f + 45.0f * (left / kTimeLimit)) - m_mistakes * 10));
                return;
            }
            m_typed.clear();
            ++m_mistakes;
            m_wrongFlash = 0.45f;
        }

        if (m_timeLeft <= 0.0f) {
            m_complete         = true;
            m_result.completed = true;
            m_result.passed    = false;
            m_result.timeTaken = m_elapsed;
            m_result.score     = 0;
        }
    }

    void Draw(UIRenderer& ui, float px, float py, float pw, float ph,
              float s) override {
        const float cx = px + pw * 0.5f;

        // Scrambled letters, one tile each.
        const float tileW = 40.0f * s;
        const float tileH = 52.0f * s;
        const float gap   = 6.0f * s;
        const float rowW  = (tileW + gap) * float(m_scrambled.size());
        float x = px + pw * 0.5f - rowW * 0.5f;
        const float y = py + 56.0f * s;
        for (size_t i = 0; i < m_scrambled.size(); ++i) {
            DrawPanelFrame(ui, x, y, tileW, tileH, kColSlotBg);
            DrawTextCentered(ui, x + tileW * 0.5f, y + tileH * 0.5f - kGlyphH,
                             std::string(1, m_scrambled[i]), kColWarn);
            x += tileW + gap;
        }

        // Player typing (typing cursor blinks until full length).
        std::string typed = m_typed;
        if (typed.size() < m_word.size() && std::fmod(m_elapsed, 0.8f) < 0.4f)
            typed += "_";
        const uint32_t typedCol = m_wrongFlash > 0.0f ? kColBad : kColText;
        DrawTextCentered(ui, cx, y + tileH + 44.0f * s, typed, typedCol);

        char mist[48];
        std::snprintf(mist, sizeof(mist), "MISTAKES %d", m_mistakes);
        DrawLineCentered(ui, cx, py + ph - 76.0f * s, mist, kColSubtle);

        char tl[16];
        std::snprintf(tl, sizeof(tl), "%.1fS", std::max(0.0f, m_timeLeft));
        DrawLineCentered(ui, cx, py + ph - 56.0f * s, tl,
                         m_timeLeft < 5.0f ? kColBad : kColSubtle);

        DrawTimerBar(ui, px + 20.0f * s, py + ph - 34.0f * s, pw - 40.0f * s,
                     s, m_timeLeft, kTimeLimit);
    }

    bool IsComplete() const override { return m_complete; }
    MinigameResult GetResult() const override { return m_result; }
    std::string GetResultLine() const override {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s!  MISTAKES %d",
                      m_result.passed ? "SOLVED" : "TIME UP", m_mistakes);
        return buf;
    }
    int DemoKey(int step) const override {
        // The perfect player types the target word one letter per step.
        if (step < 0 || step >= int(m_word.size())) return 0;
        return int(LetterKey(m_word[size_t(step)]));
    }

private:
    static constexpr float kTimeLimit = 30.0f;
    static constexpr float kGlyphH    = 7.0f; // UIRenderer::kGlyphHeight copy
    static constexpr const char* kWords[] = {
        "MISCHIEF", "PRINCIPAL", "HOMEWORK", "DETENTION", "GYMNASIUM",
        "CHALKBOARD", "EXPLOSION", "PENALTY", "SLINGSHOT", "CHEMISTRY",
        "BICYCLE", "CURFEW", "NEIGHBOR", "TRIANGLE", "ELEPHANT",
        "LIBRARY", "PREFECT",
    };
    static constexpr int kWordCount = int(sizeof(kWords) / sizeof(kWords[0]));

    std::mt19937 m_rng;
    std::string m_word, m_scrambled, m_typed;
    float m_timeLeft   = 0.0f;
    float m_elapsed    = 0.0f;
    float m_wrongFlash = 0.0f;
    int   m_mistakes   = 0;
    bool  m_complete   = false;
    MinigameResult m_result;
};

// ============================================================================
// Math Quiz (Math) — 5 questions, 4 choices, 10 s each; pass at 3 correct.
// ============================================================================

class MathQuizGame final : public IMinigame {
public:
    MinigameType GetType() const override { return MinigameType::MathQuiz; }
    const char* GetTitle() const override { return "MATH QUIZ"; }
    const char* GetInstructions() const override {
        return "5 QUESTIONS. PICK 1-4 (OR ARROWS + ENTER).\n"
               "10 SECONDS PER QUESTION. PASS AT 3 CORRECT!";
    }

    void Init(uint32_t seed) override {
        std::mt19937 rng(seed ? seed : 0xBEEFu);
        for (int i = 0; i < kQuestions; ++i)
            BuildQuestion(i, rng);
        m_idx        = 0;
        m_cursor     = 0;
        m_timeLeft   = kPerQuestion;
        m_elapsed    = 0.0f;
        m_correct    = 0;
        m_score      = 0;
        m_feedback   = 0.0f;
        m_feedbackGood = false;
        m_complete   = false;
        m_result     = MinigameResult{};
        m_result.type = MinigameType::MathQuiz;
    }

    void Update(float dt, const MinigameInput& in) override {
        if (m_complete) return;
        m_elapsed += dt;

        // Feedback pause: flash the verdict, then advance.
        if (m_feedback > 0.0f) {
            m_feedback -= dt;
            if (m_feedback <= 0.0f) NextQuestion();
            return;
        }

        m_timeLeft -= dt;
        if (m_timeLeft <= 0.0f) { // timeout counts as wrong
            Settle(false, -1);
            return;
        }

        // Direct picks 1-4.
        const int pick = in.DigitPressed();
        if (pick >= 1 && pick <= 4) {
            const int k = pick - 1;
            Settle(k == m_qs[m_idx].correctSlot, k);
            return;
        }
        // Arrows + ENTER selection (dialogue-style alternative).
        if (in.up)   m_cursor = (m_cursor + 3) % 4;
        if (in.down) m_cursor = (m_cursor + 1) % 4;
        if (in.enter) Settle(m_cursor == m_qs[m_idx].correctSlot, m_cursor);
    }

    void Draw(UIRenderer& ui, float px, float py, float pw, float ph,
              float s) override {
        const int shown   = std::min(m_idx, kQuestions - 1);
        const Question& q = m_qs[shown];
        const float cx    = px + pw * 0.5f;

        char prog[24];
        std::snprintf(prog, sizeof(prog), "QUESTION %d/5", shown + 1);
        DrawLineCentered(ui, cx, py + 10.0f * s, prog, kColSubtle);

        DrawTimerBar(ui, px + 20.0f * s, py + ph - 34.0f * s, pw - 40.0f * s,
                     s, m_timeLeft, kPerQuestion);

        // Equation, chunky.
        const std::string eq(q.text);
        ui.DrawText(cx - ui.MeasureText(eq) * 2.5f, py + 42.0f * s, eq,
                    kColText);

        // 2x2 choice grid.
        const float btnW = (pw - 70.0f * s) * 0.5f;
        const float btnH = 44.0f * s;
        const float gx   = px + (pw - (btnW * 2.0f + 18.0f * s)) * 0.5f;
        const float gy   = py + 96.0f * s;
        for (int k = 0; k < 4; ++k) {
            const float bx = gx + float(k % 2) * (btnW + 18.0f * s);
            const float by = gy + float(k / 2) * (btnH + 14.0f * s);
            uint32_t fill = kColSlotHi;
            if (m_feedback > 0.0f) {
                if (k == q.correctSlot)     fill = kColSlotGood;
                else if (k == q.chosen)     fill = kColSlotBad;
            }
            DrawPanelFrame(ui, bx, by, btnW, btnH, fill);
            char label[40];
            std::snprintf(label, sizeof(label), "%d. %d", k + 1, q.choices[k]);
            const bool cursor = m_cursor == k && m_feedback <= 0.0f;
            DrawLineCentered(ui, bx + btnW * 0.5f, by + btnH * 0.5f - 3.5f * s,
                             label, cursor ? kColWarn : kColText);
        }

        // Verdict flash + score rail.
        if (m_feedback > 0.0f) {
            DrawLineCentered(ui, cx, py + ph - 82.0f * s,
                             m_feedbackGood ? "CORRECT!" : "WRONG!",
                             m_feedbackGood ? kColGood : kColBad);
        }
        char rail[48];
        std::snprintf(rail, sizeof(rail), "CORRECT %d/5 - PASS AT 3", m_correct);
        DrawLineCentered(ui, cx, py + ph - 60.0f * s, rail, kColSubtle);
    }

    bool IsComplete() const override { return m_complete; }
    MinigameResult GetResult() const override { return m_result; }
    std::string GetResultLine() const override {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%d/5 CORRECT", m_correct);
        return buf;
    }
    int DemoKey(int step) const override {
        // Perfect run always picks the correct slot.
        if (step < 0 || step >= kQuestions) return 0;
        const int slot = m_demoFail
            ? (m_qs[step].correctSlot + 1) % 4 : m_qs[step].correctSlot;
        return int(DigitKey(slot + 1));
    }
    void SetDemoFailRun(bool fail) override { m_demoFail = fail; }

private:
    static constexpr int   kQuestions   = 5;
    static constexpr float kPerQuestion = 10.0f;
    struct Question {
        std::string text;
        int choices[4]  = {0, 0, 0, 0};
        int correct     = 0;       // answer value
        int correctSlot = 0;       // slot index of the answer
        int chosen      = -1;      // slot the player picked (-1 = timeout)
        bool right      = false;
    };

    void BuildQuestion(int i, std::mt19937& rng) {
        Question& q = m_qs[i];
        q.chosen = -1;
        q.right  = false;

        // Difficulty ramp: +, -, +/*, *, *.
        int a = 0, b = 0, ans = 0;
        char op = '+';
        switch (i) {
            case 0: a = int(rng() % 40) + 10; b = int(rng() % 40) + 10;
                    op = '+'; ans = a + b; break;
            case 1: a = int(rng() % 50) + 20; b = int(rng() % 40) + 10;
                    op = '-'; ans = a - b; break;
            case 2: if (rng() % 2) { a = int(rng() % 30) + 10;
                                     b = int(rng() % 30) + 10;
                                     op = '+'; ans = a + b; }
                    else         { a = int(rng() % 12) + 2;
                                   b = int(rng() % 12) + 2;
                                   op = '*'; ans = a * b; }
                    break;
            case 3: a = int(rng() % 12) + 2; b = int(rng() % 12) + 2;
                    op = '*'; ans = a * b; break;
            default: a = int(rng() % 15) + 5; b = int(rng() % 12) + 2;
                     op = '*'; ans = a * b; break;
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%d %c %d = ?", a, op, b);
        q.text    = text;
        q.correct = ans;

        // Three distinct near-miss distractors.
        const int cands[6] = { ans + 1, ans - 1, ans + 10, ans - 10,
                               a + b, std::max(0, ans - 2 * b) };
        int n = 0;
        for (int c : cands) {
            if (n >= 3) break;
            if (c < 0 || c == ans) continue;
            bool dup = false;
            for (int k = 0; k < n; ++k)
                if (q.choices[k] == c) { dup = true; break; }
            if (dup) continue;
            q.choices[n++] = c;
        }
        while (n < 3) { // pathological filler (cands collided)
            const int filler = ans + n + 2 + int(rng() % 3);
            bool dup = filler == ans;
            for (int k = 0; k < n && !dup; ++k) dup = q.choices[k] == filler;
            if (!dup) q.choices[n++] = filler;
        }
        // Place the answer in a random slot.
        const int slot = int(rng() % 4);
        int distIdx = 0;
        for (int k = 0; k < 4; ++k)
            q.choices[k] = (k == slot) ? ans : q.choices[distIdx++];
        q.correctSlot = slot;
    }

    void Settle(bool right, int chosen) {
        Question& q = m_qs[m_idx];
        q.right  = right;
        q.chosen = chosen;
        m_feedbackGood = right;
        if (right) { ++m_correct; m_score += 20; }
        m_feedback = 0.45f;
    }

    void NextQuestion() {
        ++m_idx;
        if (m_idx >= kQuestions) {
            m_complete         = true;
            m_result.completed = true;
            m_result.passed    = m_correct >= 3;
            m_result.timeTaken = m_elapsed;
            m_result.score     = m_score;
            return;
        }
        m_timeLeft = kPerQuestion;
        m_cursor   = 0;
    }

    Question m_qs[kQuestions];
    int   m_idx        = 0;
    int   m_cursor     = 0;
    int   m_correct    = 0;
    int   m_score      = 0;
    float m_timeLeft   = 0.0f;
    float m_elapsed    = 0.0f;
    float m_feedback   = 0.0f;
    bool  m_feedbackGood = false;
    bool  m_complete   = false;
    bool  m_demoFail   = false;
    MinigameResult m_result;
};

// ============================================================================
// Button Mash (Gym) — hit 60 SPACE presses before the 10 s timer runs out.
// ============================================================================

class ButtonMashGame final : public IMinigame {
public:
    MinigameType GetType() const override { return MinigameType::ButtonMash; }
    const char* GetTitle() const override { return "ENDURANCE MASH"; }
    const char* GetInstructions() const override {
        return "MASH SPACE! REACH THE TARGET PRESSES\nBEFORE THE TIMER RUNS OUT.";
    }

    void Init(uint32_t seed) override {
        (void)seed;
        m_presses  = 0;
        m_timeLeft = kTimeLimit;
        m_elapsed  = 0.0f;
        m_complete = false;
        m_pulse    = 0.0f;
        m_result   = MinigameResult{};
        m_result.type = MinigameType::ButtonMash;
    }

    void Update(float dt, const MinigameInput& in) override {
        if (m_complete) return;
        m_timeLeft -= dt;
        m_elapsed  += dt;
        m_pulse    += dt;
        if (in.space) ++m_presses;

        if (m_presses >= kTarget) {
            m_complete         = true;
            m_result.completed = true;
            m_result.passed    = true;
            m_result.timeTaken = m_elapsed;
            // Grade: time-left bonus for finishing the set early.
            const float left = std::max(0.0f, m_timeLeft);
            m_result.score = std::min(100, int(50.0f + 50.0f * (left / kTimeLimit)));
        } else if (m_timeLeft <= 0.0f) {
            m_complete         = true;
            m_result.completed = true;
            m_result.passed    = false;
            m_result.timeTaken = m_elapsed;
            m_result.score     = int(100.0f * float(m_presses) / float(kTarget));
        }
    }

    void Draw(UIRenderer& ui, float px, float py, float pw, float ph,
              float s) override {
        const float cx = px + pw * 0.5f;
        char cnt[40];
        std::snprintf(cnt, sizeof(cnt), "%d / %d", m_presses, kTarget);

        // Pulsing counter (breathes faster once presses land).
        const float pulse = 1.0f + (m_presses > 0 && !m_complete
            ? 0.10f * std::max(0.0f, std::sin(m_pulse * 9.0f)) : 0.0f);
        const float bigScale = 5.0f * s * pulse;
        const std::string cntStr(cnt);
        const float w = ui.MeasureText(cntStr) * bigScale;
        ui.DrawText(cx - w * 0.5f, py + 46.0f * s, cntStr,
                    m_complete ? kColGood : kColWarn);

        DrawProgressBar(ui, px + 60.0f * s, py + 130.0f * s, pw - 120.0f * s,
                        22.0f * s, float(m_presses) / float(kTarget),
                        m_complete ? kColGood : kColAccent);

        char rate[48];
        std::snprintf(rate, sizeof(rate), "RATE %.1f/S",
                      m_elapsed > 0.0f ? float(m_presses) / m_elapsed : 0.0f);
        DrawLineCentered(ui, cx, py + 190.0f * s, rate, kColSubtle);

        char tl[16];
        std::snprintf(tl, sizeof(tl), "%.1fS", std::max(0.0f, m_timeLeft));
        DrawLineCentered(ui, cx, py + 212.0f * s, tl,
                         m_timeLeft < 3.0f ? kColBad : kColSubtle);

        DrawTimerBar(ui, px + 20.0f * s, py + ph - 34.0f * s, pw - 40.0f * s,
                     s, m_timeLeft, kTimeLimit);
    }

    bool IsComplete() const override { return m_complete; }
    MinigameResult GetResult() const override { return m_result; }
    std::string GetResultLine() const override {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%d/%d PRESSES", m_presses, kTarget);
        return buf;
    }
    int DemoKey(int step) const override {
        (void)step;
        return (!m_complete && m_timeLeft > 0.0f) ? int(SDL_SCANCODE_SPACE) : 0;
    }

private:
    static constexpr float kTimeLimit = 10.0f;
    static constexpr int   kTarget    = 60;
    int   m_presses  = 0;
    float m_timeLeft = 0.0f;
    float m_elapsed  = 0.0f;
    float m_pulse    = 0.0f;
    bool  m_complete = false;
    MinigameResult m_result;
};

// --- Factory ----------------------------------------------------------------

std::unique_ptr<IMinigame> CreateMinigame(MinigameType type) {
    switch (type) {
        case MinigameType::WordScramble: return std::make_unique<WordScrambleGame>();
        case MinigameType::MathQuiz:     return std::make_unique<MathQuizGame>();
        case MinigameType::ButtonMash:   return std::make_unique<ButtonMashGame>();
        default:
            Logger::Warning("Minigame: %s is not implemented yet",
                            MinigameTypeName(type));
            return nullptr;
    }
}

// ============================================================================
// MinigameManager
// ============================================================================

bool MinigameManager::StartMinigame(MinigameType type, uint32_t seed) {
    if (!InRange(type)) return false;
    if (m_phase != Phase::Inactive) return false; // one at a time
    m_game = CreateMinigame(type);
    if (!m_game) return false;                    // reserved type
    m_type = type;
    m_attempts = 0;
    StartAttempt(type, seed);
    Logger::Info("Minigame: started %s (subject %s)",
                 MinigameTypeName(type), MinigameTypeSubject(type));
    return true;
}

void MinigameManager::StartAttempt(MinigameType type, uint32_t seed) {
    m_type = type;
    m_seed = seed ? seed : (m_seedCounter += 2654435761u);
    m_game = CreateMinigame(type);
    if (!m_game) { m_phase = Phase::Inactive; return; }
    m_game->SetDemoFailRun(m_demoFailArmed);
    m_demoFailArmed = false;
    m_game->Init(m_seed);
    m_phase      = Phase::Intro;
    m_phaseTimer = 0.0f;
    m_fade       = 0.0f;
    m_demoPressCount = 0;
    m_attempts  += 1;
}

void MinigameManager::FinishAttempt(bool forfeited) {
    if (m_game) {
        m_lastResult = m_game->GetResult();
        m_lastResult.forfeited = forfeited;
        m_lastResult.completed = !forfeited;
        if (forfeited) {
            m_lastResult.passed = false;
            m_lastResult.score  = 0;
        }
    } else {
        m_lastResult = MinigameResult{};
    }
    m_lastResult.type = m_type;
    if (m_lastResult.passed) {
        m_wins[int(m_type)] += 1;
        m_best[int(m_type)]  = std::max(m_best[int(m_type)],
                                        m_lastResult.score);
        m_progressDirty = true;
    }
    m_phase      = Phase::Result;
    m_phaseTimer = 0.0f;
    Logger::Info("Minigame: %s %s (score %d, %.1fs%s)",
                 MinigameTypeName(m_type),
                 m_lastResult.passed ? "PASSED" : "FAILED",
                 m_lastResult.score, m_lastResult.timeTaken,
                 forfeited ? ", forfeited" : "");
    if (m_onResult) m_onResult(m_lastResult);
    if (m_progressDirty && !m_progressDir.empty())
        SaveProgress(m_progressDir);
}

void MinigameManager::Update(float dt, const MinigameInput& in, bool allowEsc) {
    if (m_phase == Phase::Inactive) return;
    m_fade = std::min(1.0f, m_fade + dt * 3.0f);
    m_phaseTimer += dt;

    const bool esc = allowEsc && in.escape;
    if (esc && (m_phase == Phase::Intro || m_phase == Phase::Playing)) {
        FinishAttempt(/*forfeited=*/true);
        return;
    }

    switch (m_phase) {
        case Phase::Intro:
            if (in.space) {
                m_phase      = Phase::Playing;
                m_phaseTimer = 0.0f;
            }
            break;

        case Phase::Playing:
            if (m_game) {
                m_game->Update(dt, in);
                if (m_game->IsComplete()) FinishAttempt(false);
            } else {
                FinishAttempt(false);
            }
            break;

        case Phase::Result:
            if (m_phaseTimer < 0.30f) break; // input lockout after the banner
            if (in.retry) {
                // Retry: fresh attempt, same type, new seed.
                StartAttempt(m_type, 0);
            } else if (in.cont || in.enter || esc) {
                // Continue: close the overlay, hand control back to gameplay.
                m_phase      = Phase::Inactive;
                m_phaseTimer = 0.0f;
                m_fade       = 0.0f;
                m_game.reset();
            }
            break;

        default:
            break;
    }
}

void MinigameManager::Draw(UIRenderer& ui, float screenW, float screenH) {
    if (m_phase == Phase::Inactive || !m_game) return;
    if (!ui.IsInitialized()) return;

    const float W = screenW;
    const float H = screenH;
    const float s = std::min(1.5f, std::max(0.75f, H / 720.0f));
    const float fade = m_fade;

    // Full-screen dim backdrop.
    ui.DrawRect(0.0f, 0.0f, W, H, ScaleA(kColDim, fade));

    // Panel (760x500 reference, centered).
    const float pw = 760.0f * s;
    const float ph = 500.0f * s;
    const float px = 0.5f * (W - pw);
    const float py = 0.5f * (H - ph);
    DrawPanelFrame(ui, px, py, pw, ph, ScaleA(kColPanel, fade));
    const float padX = 26.0f * s;

    // Header bar: title + subject tag.
    const float headH = 34.0f * s;
    ui.DrawRect(px, py, pw, headH, ScaleA(kColHeader, fade));
    ui.DrawRect(px, py + headH - 1.0f, pw, 1.0f, ScaleA(kColBorder, fade));
    {
        const std::string title(m_game->GetTitle());
        ui.DrawText(px + padX, py + 12.0f * s, title, ScaleA(kColTitle, fade));
        const std::string subTag =
            std::string("[") + MinigameTypeSubject(m_type) + "]";
        ui.DrawText(px + pw - padX - ui.MeasureText(subTag), py + 12.0f * s,
                    subTag, ScaleA(kColAccent, fade));
    }

    // Footer hint bar.
    const float footH = 26.0f * s;
    ui.DrawRect(px, py + ph - footH, pw, footH, ScaleA(kColHeader, fade));
    const char* foot =
        m_phase == Phase::Intro   ? "SPACE: START      ESC: QUIT (FORFEIT)" :
        m_phase == Phase::Playing ? "ESC: FORFEIT" :
                                    "R: RETRY      E: CONTINUE";
    {
        const std::string footStr(foot);
        ui.DrawText(px + padX, py + ph - footH + 8.0f * s, footStr,
                    ScaleA(kColSubtle, fade));
        // Best-score ribbon (progress persistence deliverable).
        if (Wins(m_type) > 0 && m_phase != Phase::Result) {
            const std::string best = "BEST " + std::to_string(BestScore(m_type));
            ui.DrawText(px + pw - padX - ui.MeasureText(best),
                        py + ph - footH + 8.0f * s, best,
                        ScaleA(kColWarn, fade));
        }
    }

    const float contentY = py + headH + 8.0f * s;

    if (m_phase == Phase::Intro) {
        // Instructions screen: title + rules + episode record + prompt.
        DrawLineCentered(ui, 0.5f * W, contentY + 46.0f * s,
                         m_game->GetTitle(), ScaleA(kColWarn, fade));
        const std::string instr(m_game->GetInstructions());
        float ly = contentY + 108.0f * s;
        size_t start = 0;
        while (true) {
            const size_t nl = instr.find('\n', start);
            const std::string line = instr.substr(
                start, nl == std::string::npos ? std::string::npos
                                               : nl - start);
            DrawTextCentered(ui, 0.5f * W, ly, line, ScaleA(kColText, fade));
            ly += 24.0f * s;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        if (Wins(m_type) > 0) {
            char rec[48];
            std::snprintf(rec, sizeof(rec), "WINS %d      BEST %d",
                          Wins(m_type), BestScore(m_type));
            DrawLineCentered(ui, 0.5f * W, py + ph - footH - 40.0f * s, rec,
                             ScaleA(kColAccent, fade));
        }
        if (std::fmod(m_phaseTimer, 1.0f) < 0.6f) { // blinking prompt
            DrawLineCentered(ui, 0.5f * W, py + ph - footH - 72.0f * s,
                             "PRESS SPACE TO BEGIN", ScaleA(kColGood, fade));
        }
        return;
    }

    // Playing/result: the game renders inside the content area; a result
    // banner lands over it on completion.
    m_game->Draw(ui, px + padX, contentY + 6.0f * s, pw - 2.0f * padX,
                 ph - headH - footH - 14.0f * s, s);

    if (m_phase == Phase::Result) {
        const float bw = 460.0f * s;
        const float bh = 150.0f * s;
        const float bx = 0.5f * (W - bw);
        const float by = 0.5f * (H - bh) - 10.0f * s;
        ui.DrawRect(bx, by, bw, bh, ScaleA(Pack(10, 12, 16, 245), fade));
        DrawPanelFrame(ui, bx, by, bw, bh, Pack(0, 0, 0, 0));
        const uint32_t verdict = m_lastResult.forfeited ? kColWarn
                               : m_lastResult.passed    ? kColGood
                                                        : kColBad;
        const char* banner = m_lastResult.forfeited ? "FORFEITED"
                           : m_lastResult.passed    ? "PASSED!"
                                                    : "FAILED";
        DrawLineCentered(ui, 0.5f * W, by + 18.0f * s, banner,
                         ScaleA(verdict, fade));
        {
            const std::string line = m_game->GetResultLine();
            DrawTextCentered(ui, 0.5f * W, by + 56.0f * s, line,
                             ScaleA(kColText, fade));
        }
        char stats[80];
        std::snprintf(stats, sizeof(stats), "SCORE %d      TIME %.1fS",
                      m_lastResult.score, m_lastResult.timeTaken);
        DrawLineCentered(ui, 0.5f * W, by + 82.0f * s, stats,
                         ScaleA(kColSubtle, fade));
        if (m_phaseTimer > 0.30f && std::fmod(m_phaseTimer, 1.1f) < 0.75f) {
            DrawLineCentered(ui, 0.5f * W, by + bh - 30.0f * s,
                             "R: RETRY      E: CONTINUE",
                             ScaleA(kColAccent, fade));
        }
    }
}

// --- Progress persistence -----------------------------------------------------

void MinigameManager::LoadProgress(const std::string& baseDir) {
    m_progressDir = baseDir;
    char path[1024];
    std::snprintf(path, sizeof(path), "%ssaves/minigames.json",
                  baseDir.c_str());
    std::ifstream f(path);
    if (!f.good()) return; // first run: defaults stay zeroed
    const std::string json((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    // Tiny tolerant parser: {"wins":[...],"best":[...]}
    const int count = int(MinigameType::Count);
    auto readArray = [&](const char* key, int* out) {
        const size_t at = json.find(key);
        if (at == std::string::npos) return;
        const size_t open = json.find('[', at);
        if (open == std::string::npos) return;
        const size_t close = json.find(']', open);
        if (close == std::string::npos) return;
        size_t pos = open + 1;
        for (int i = 0; i < count && pos < close; ++i) {
            while (pos < close && (json[pos] == ' ' || json[pos] == ','))
                ++pos;
            if (pos >= close) break;
            out[i] = std::atoi(json.c_str() + pos);
            while (pos < close && json[pos] != ',' && json[pos] != ']') ++pos;
        }
    };
    readArray("\"wins\"", m_wins);
    readArray("\"best\"", m_best);
    Logger::Info("Minigame: progress loaded (%s)", path);
}

void MinigameManager::SaveProgress(const std::string& baseDir) {
    m_progressDir = baseDir;
    char path[1024];
    std::snprintf(path, sizeof(path), "%ssaves/minigames.json",
                  baseDir.c_str());
#if defined(_MSC_VER)
    {
        std::error_code ec;
        const std::filesystem::path p(path);
        if (p.has_parent_path())
            std::filesystem::create_directories(p.parent_path(), ec);
    }
#endif
    std::ofstream f(path);
    if (!f.good()) {
        Logger::Warning("Minigame: could not write progress %s", path);
        return;
    }
    const int count = int(MinigameType::Count);
    f << "{\n  \"version\": 1,\n  \"wins\": [";
    for (int i = 0; i < count; ++i)
        f << m_wins[i] << (i + 1 < count ? ", " : "");
    f << "],\n  \"best\": [";
    for (int i = 0; i < count; ++i)
        f << m_best[i] << (i + 1 < count ? ", " : "");
    f << "]\n}\n";
    m_progressDirty = false;
    Logger::Info("Minigame: progress saved (%s)", path);
}

// --- OPENBULLY_MINIGAME_DEMO scripted run -------------------------------------
//
// Story (engine-time driven, ~45 s): scramble WIN -> quiz FAIL + R retry ->
// quiz WIN -> mash ESC-forfeit -> mash WIN. Presses latch for one frame and
// go through Input::SetRawKey, so games see them exactly like a keyboard.
// The engine injects each frame BEFORE gameplay systems read input.

void MinigameManager::DriveDemo(double now, Input* input,
                                const std::string& baseDir) {
    if (!input) return;
    if (m_progressDir.empty()) LoadProgress(baseDir);

    // Latch one raw key down for a single frame (released on a later tick).
    auto press = [&](int sc) {
        if (sc <= 0 || m_demoKeyHeld) return;
        input->SetRawKey(SDL_Scancode(sc), true);
        m_demoKeyHeld = true;
        m_demoNextAt  = now + 0.05; // release gate (next entry)
        ++m_demoPressCount;
    };
    auto release = [&]() {
        if (!m_demoKeyHeld) return;
        for (int sc = SDL_SCANCODE_A; sc <= SDL_SCANCODE_Z; ++sc)
            input->SetRawKey(SDL_Scancode(sc), false);
        for (int sc = SDL_SCANCODE_1; sc <= SDL_SCANCODE_9; ++sc)
            input->SetRawKey(SDL_Scancode(sc), false);
        input->SetRawKey(SDL_SCANCODE_SPACE, false);
        input->SetRawKey(SDL_SCANCODE_ESCAPE, false);
        input->SetRawKey(SDL_SCANCODE_R, false);
        input->SetRawKey(SDL_SCANCODE_E, false);
        input->SetRawKey(SDL_SCANCODE_RETURN, false);
        m_demoKeyHeld = false;
        m_demoNextAt  = now + 0.08; // small gap between presses
    };

    if (m_demoStep == DemoStep::Done) {
        if (m_demoKeyHeld) release();
        // A class session (or another caller) opened an attempt outside the
        // script: auto-drive it through intro/play/result once, then Done.
        if (IsActive()) {
            m_demoExternal = true;
            m_demoStep     = DemoStep::AwaitIntro;
        } else {
            return;
        }
    }

    // Release any latched key once its gate expires, then keep scripting.
    if (m_demoKeyHeld && now >= m_demoNextAt) release();

    switch (m_demoStep) {
        case DemoStep::WaitStart:
            if (now >= 1.0) {
                // Integration shape #1 (English class would call this).
                if (StartMinigame(MinigameType::WordScramble, /*seed=*/1234))
                    m_demoStep = DemoStep::AwaitIntro;
                else
                    m_demoStep = DemoStep::Done;
            }
            break;

        case DemoStep::AwaitIntro:
            if (m_phase == Phase::Intro && !m_demoKeyHeld &&
                now >= m_demoNextAt) {
                press(int(SDL_SCANCODE_SPACE));
                m_demoPressCount = 0; // intro SPACE is not a game input
                m_demoStep = DemoStep::PlayingAuto;
            }
            break;

        case DemoStep::PlayingAuto:
            if (m_phase == Phase::Playing && !m_demoKeyHeld &&
                now >= m_demoNextAt)
                press(m_game ? m_game->DemoKey(m_demoPressCount) : 0);
            if (m_phase == Phase::Result) {
                Logger::Info("MINIGAME DEMO: result %s passed=%d score=%d "
                             "forfeited=%d, attempts=%d",
                             MinigameTypeName(m_type),
                             int(m_lastResult.passed), m_lastResult.score,
                             int(m_lastResult.forfeited), m_attempts);
                m_demoPressCount = 0;
                // Forfeits continue straight out (a retry makes no sense
                // after ESC); fails exercise the R-retry path.
                m_demoStep = m_lastResult.passed || m_lastResult.forfeited
                    ? DemoStep::AwaitContinue
                    : DemoStep::AwaitFailRetry;
            }
            break;

        case DemoStep::AwaitFailRetry:
            if (m_phase == Phase::Result && m_phaseTimer > 0.5f &&
                !m_demoKeyHeld && now >= m_demoNextAt) {
                press(int(SDL_SCANCODE_R));
                m_demoNextAt = now + 0.25;
                m_demoStep   = DemoStep::AwaitIntro;
            }
            break;

        case DemoStep::AwaitContinue:
            if (m_phase == Phase::Result && m_phaseTimer > 0.5f &&
                !m_demoKeyHeld && now >= m_demoNextAt) {
                press(int(SDL_SCANCODE_E));
                m_demoNextAt = now + 1.2;
                if (m_demoExternal || m_demoBump >= 3) {
                    m_demoExternal = false;
                    m_demoStep     = DemoStep::Done;
                } else {
                    m_demoStep = DemoStep::WaitNextActivity;
                }
            }
            break;

        case DemoStep::WaitNextActivity:
            if (m_phase != Phase::Inactive || now < m_demoNextAt) break;
            // Queue the next scripted class activity.
            if (m_demoBump == 0) {
                m_demoFailArmed = true; // MathQuiz attempt #1 plays to FAIL
                m_demoBump = 1;
                if (StartMinigame(MinigameType::MathQuiz, /*seed=*/4321))
                    m_demoStep = DemoStep::AwaitIntro;
                else
                    m_demoStep = DemoStep::Done;
            } else if (m_demoBump == 1) {
                m_demoBump = 2;
                if (StartMinigame(MinigameType::ButtonMash, /*seed=*/999))
                    m_demoStep = DemoStep::AwaitIntroMashForfeit;
                else
                    m_demoStep = DemoStep::Done;
            } else if (m_demoBump == 2) {
                m_demoBump = 3;
                if (StartMinigame(MinigameType::ButtonMash, /*seed=*/777))
                    m_demoStep = DemoStep::AwaitIntro;
                else
                    m_demoStep = DemoStep::Done;
            } else {
                if (!m_demoLogged) {
                    m_demoLogged = true;
                    Logger::Info("MINIGAME DEMO COMPLETE: intro/win/fail/"
                                 "retry/forfeit paths all exercised");
                }
                m_demoStep = DemoStep::Done;
            }
            break;

        case DemoStep::AwaitIntroMashForfeit:
            if (m_phase == Phase::Intro && !m_demoKeyHeld &&
                now >= m_demoNextAt) {
                press(int(SDL_SCANCODE_SPACE));
                m_demoPressCount = 0; // intro SPACE is not a game input
                m_demoForfeitAt = now + 1.5; // ESC mid-play => forfeit path
                m_demoStep      = DemoStep::PlayingMashForfeit;
            }
            break;

        case DemoStep::PlayingMashForfeit:
            if (m_phase == Phase::Playing && now >= m_demoForfeitAt) {
                press(int(SDL_SCANCODE_ESCAPE));
                Logger::Info("MINIGAME DEMO: ESC forfeit sent mid-mash");
                m_demoStep = DemoStep::PlayingAuto; // banner lands -> E
            }
            break;

        default:
            m_demoStep = DemoStep::Done;
            break;
    }
}

} // namespace Minigames
} // namespace OpenBully
