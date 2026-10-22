#pragma once

// Track 45: Arcade mini-games (Nut Shots + Monkey Fling).
//
// Bully's Bullworth Academy hosts arcade cabinets; this track adds two
// playable ones to OpenBully. ArcadeManager owns:
//   - the world cabinets (placeholder-box entities placed around the player
//     spawn, ground-raycast verified like the vehicle ring),
//   - the proximity prompt + E-to-play entry (costs $1 per play),
//   - the full-screen arcade view (bgfx view 10, UIRenderer primitives --
//     rects + the 5x7 bitmap font only),
//   - per-game high scores, play counts, win count and the "Arcade Winner"
//     achievement (10 wins), all persisted through SaveData (F5/F9),
//   - the $5 new-high-score cash prize.
//
// Modality contract (mirrors the Track 40 quest journal): while IsActive()
// the Engine clears every Button, zeroes the mouse deltas AFTER feeding them
// to UpdateActive() and intercepts the Input ESC latch -- ESC exits the game
// view (no refund) instead of quitting the app.

#include "../engine/save_system.h"
#include "../entities/entity_id.h"
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace OpenBully {

class UIRenderer;      // ui.h (fwd: Render takes a reference)
class AudioManager;    // audio.h (fwd: one-shot SFX)
class Input;           // game/input.h
class EntityManager;   // entities/entity.h
class CollisionWorld;  // physics/collision.h

// The two playable games. Cabinet order == save-array order.
enum class ArcadeGameId : int { NutShots = 0, MonkeyFling = 1, Count = 2 };

// One world cabinet (entity + display strings). Entities are placeholder
// boxes (MeshComponent::isBox); the arcade view is fully 2D UI. `position`
// snapshots the spawn placement so proximity needs no entity queries.
struct ArcadeCabinet {
    EntityID     entity{};                 // invalid until SpawnCabinets
    ArcadeGameId game     = ArcadeGameId::NutShots;
    std::string  name;                    // "Nut Shots"
    std::string  tagline;                 // attract-screen blurb
    glm::vec3    position{0.0f};          // placement snapshot (world)
};

// Arcade progression snapshot persisted inside SaveData (World section);
// defaults match a fresh save.
struct ArcadeSaveState {
    int highScores[int(ArcadeGameId::Count)] = {0, 0};
    int plays       = 0;   // coins inserted (rounds paid for)
    int wins        = 0;   // rounds that set a new high score
    int achievement = 0;   // 1 = "Arcade Winner" unlocked (10 wins)
};

// Arcade view state machine:
//   InWorld   -- closed; the engine shows a "[E] PLAY <NAME> ($1)" prompt.
//   Attract   -- coin inserted ($1, no refund); PRESS SPACE TO START.
//   Playing   -- the mini-game runs (30 s round / 3 flings).
//   RoundOver -- final score / high score / +$5 prize banner; SPACE replays.
enum class ArcadeState : int { InWorld = 0, Attract = 1, Playing = 2, RoundOver = 3 };

class ArcadeManager {
public:
    ArcadeManager() = default;

    // --- World integration ---------------------------------------------------
    // Spawns the cabinets around `center` with the vehicle-style ground
    // pull-in (down-raycast; shrink the offset radius until floor is found).
    void SpawnCabinets(EntityManager& entities, CollisionWorld* collision,
                       const glm::vec3& center);
    void SetAudio(AudioManager* audio) { m_audio = audio; }

    // --- Per-frame (world side) -----------------------------------------------
    // Proximity scan against the placement snapshots: sets the near-cabinet
    // index and the "[E] PLAY ..." prompt text.
    void UpdateProximity(const glm::vec3& playerPos);
    int  NearIndex() const { return m_nearIndex; }
    bool NearCabinet() const { return m_nearIndex >= 0; }
    const char* Prompt() const { return m_prompt; }

    // E near a cabinet -> open the arcade view (deducts $1 immediately;
    // without coins the view still opens, Attract shows a NO COINS message).
    // Returns true when the view was entered.
    bool TryEnterCabinet(int cabinetIndex, int& playerMoney);

    // Cabinet placement snapshot (world side; demo walk target + count).
    int      CabinetCount() const { return int(m_cabinets.size()); }
    glm::vec3 CabinetPosition(int index) const {
        return (index >= 0 && index < int(m_cabinets.size()))
                   ? m_cabinets[size_t(index)].position : glm::vec3(0.0f);
    }

    // --- Per-frame (arcade view side) ------------------------------------------
    bool IsActive() const { return m_state != ArcadeState::InWorld; }
    ArcadeState State() const { return m_state; }

    // Runs one frame of the open arcade view (attract wait, mini-game
    // movement/throws/timer, round-end effects: high score, +$5 prize,
    // achievement). The engine calls this BEFORE its button/mouse strip-freeze
    // and passes the LMB edge explicitly (logical buttons are already cleared
    // by the time this runs); Space/Enter arrive through the raw-scancode edge
    // API which the freeze does not touch. ESC is engine-owned (QuitRequested
    // latch -> ExitGame).
    void UpdateActive(Input& input, float dt, int& playerMoney, bool lmbPressed);

    // ESC anywhere: close the view, keep the coin (no refund).
    void ExitGame();

    // --- Rendering --------------------------------------------------------------
    // Full-screen arcade view (view 10, drawn after DrawHUD). No-op inactive.
    void Render(UIRenderer& ui, float screenW, float screenH) const;

    // --- Persistence (SaveData World section) -------------------------------------
    void WriteSave(SaveData& out) const;
    void ReadSave(const SaveData& in);

    // OPENBULLY_ARCADE_DEMO=1: scripted rounds for headless verification.
    void SetDemo(bool demo) { m_demo = demo; }

    int  HighScore(ArcadeGameId g) const { return m_save.highScores[int(g)]; }
    int  Wins() const { return m_save.wins; }
    bool AchievementUnlocked() const { return m_save.achievement != 0; }

private:
    // --- Mini-game internals -------------------------------------------------------
    void StartRound();                        // Attract/RoundOver -> Playing
    void EndRound(int& playerMoney);          // high score / prize / win rollup
    void UpdateNutShots(Input& input, float dt, bool lmbPressed);
    void UpdateMonkeyFling(Input& input, float dt);

    // Demo drivers (OPENBULLY_ARCADE_DEMO): synthesize aim/throws/flings.
    void DriveNutShotsDemo(float dt);
    void DriveMonkeyFlingDemo(float dt);

    // --- Rendering helpers ----------------------------------------------------------
    void DrawCabFrame(UIRenderer& ui, float sw, float sh) const;
    void RenderNutShots(UIRenderer& ui, float sw, float sh) const;
    void RenderMonkeyFling(UIRenderer& ui, float sw, float sh) const;
    void RenderAttract(UIRenderer& ui, float sw, float sh) const;
    void RenderRoundOver(UIRenderer& ui, float sw, float sh) const;

    const ArcadeCabinet* ActiveCabinet() const;

    // --- World state -----------------------------------------------------------------
    std::vector<ArcadeCabinet> m_cabinets;
    int       m_nearIndex = -1;               // cabinet within prompt range
    char      m_prompt[96] = {};              // world interaction prompt

    // --- Arcade view state ------------------------------------------------------------
    ArcadeState m_state       = ArcadeState::InWorld;
    int         m_activeGame  = 0;            // ArcadeGameId of the open cabinet
    char        m_message[64] = {};           // transient line (NO COINS!)
    float       m_stateTime   = 0.0f;         // seconds in the current state
    bool        m_lastNewHigh = false;        // round just set a record (banner)

    // --- Nut Shots runtime (field-normalized coords, 0..1) -------------------------------
    struct NutTarget {
        bool  alive   = false;
        bool  falling = false;                // hit: tumble animation
        float fallT   = 0.0f;                 // 0..1 tumble progress
        float x = 0.0f;                       // rail position (field 0..1)
        int   rail  = 0;                      // 0 = top, 1 = bottom
        bool  head  = false;                  // head hit = 2 pts, body = 1
        int   dir   = 1;                      // rail motion direction
        float respawnT = 0.0f;                // hidden countdown before respawn
    };
    struct NutShot {
        float x = 0.5f, y = 1.0f;             // flight position
        float tx = 0.5f, ty = 0.5f;           // aimed target
        float t = 0.0f;                       // 0..1 flight progress
        float dur = 0.2f;                     // total flight time (s)
    };
    struct NutShotsState {
        float crossX = 0.5f, crossY = 0.6f;   // crosshair (field-normalized)
        NutTarget targets[6];                 // 2 rails x 3
        std::vector<NutShot> shots;
        float timeLeft = 30.0f;
        int   score    = 0;
        int   hits     = 0;
        int   throws   = 0;
        float throwCd  = 0.0f;                // min interval between throws (s)
        float puffT[6] = {};                  // hit puffs (one per target slot)
    };
    NutShotsState m_nut;

    // --- Monkey Fling runtime (field-normalized coords) ----------------------------------
    struct MonkeyState {
        int   attempt   = 0;                  // flings used this round (0..3)
        int   bestRound = 0;                  // best distance this round (m)
        int   lastDist  = 0;                  // last fling distance (m)
        bool  flying    = false;
        float power     = 0.0f;               // oscillating meter 0..100
        int   powerDir  = 1;
        float x = 0.10f, y = 0.80f;           // flight position
        float vx = 0.0f, vy = 0.0f;
        float wind      = 0.0f;               // per-attempt wind (m/s, + = right)
        float markerX[3] = {1e9f, 1e9f, 1e9f};// landing markers (field x)
    };
    MonkeyState m_monkey;

    // --- Progression + demo ------------------------------------------------------------
    ArcadeSaveState m_save;
    AudioManager* m_audio = nullptr;          // coin/prize blips (optional)
    bool  m_demo = false;
    float m_demoTimer  = 0.0f;                // demo phase clock inside a state
    float m_demoTimer2 = 0.0f;                // demo cadence (fling gate)
    float m_demoThrowCd = 0.0f;               // demo cadence (nut throws)
};

} // namespace OpenBully
