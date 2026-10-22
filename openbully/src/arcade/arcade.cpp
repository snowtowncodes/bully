// Track 45: Arcade mini-games implementation.
//
// Everything visual uses UIRenderer primitives (solid rects + the 5x7 bitmap
// font) on bgfx view 10 -- a chunky, deliberately low-fi cabinet screen.
// Both games run in field-normalized coordinates (0..1) that are scaled to
// the backbuffer at draw time, so a round renders identically at any window
// size. The renderers and the hit tests share the same geometry constants so
// a pixel that looks like a hit is a hit that scores.

#include "arcade.h"

#include "../entities/entity.h"
#include "../game/input.h"
#include "../physics/collision.h"
#include "../platform/logger.h"
#include "../ui/ui.h"
#include "../audio/audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace OpenBully {

namespace {

// --- Economy ------------------------------------------------------------------
constexpr int   kPlayCost       = 1;      // $1 per play (task spec)
constexpr int   kPrizeNewHigh   = 5;      // $5 cash prize for a new high score
constexpr int   kAchievementWins = 10;   // "Win 10 arcade games"
constexpr float kPromptRange    = 2.2f;   // world units to show [E] / play

// --- Nut Shots --------------------------------------------------------------
constexpr int   kNutRoundTime    = 30;    // 30 s round (task spec)
constexpr int   kNutTargetCount  = 6;     // 2 rails x 3 targets
constexpr float kNutThrowCd      = 0.25f; // s between throws
// Duck geometry in field units (y measured from the rail top line).
constexpr float kRailTopY[2]     = {0.30f, 0.46f};
constexpr float kDuckBodyOffset  = 0.045f; // body center above the rail line
constexpr float kDuckBodyHW      = 0.048f; // body half width
constexpr float kDuckBodyHH      = 0.026f; // body half height
constexpr float kDuckHeadOffset  = 0.030f; // head center above the body center
constexpr float kDuckHeadHW      = 0.020f;
constexpr float kDuckHeadHH      = 0.024f;
constexpr float kDuckRailSpeed[2] = {0.22f, 0.13f};
constexpr float kHitMargin       = 0.010f; // aim forgiveness

// --- Monkey Fling -------------------------------------------------------------
constexpr int   kFlingAttempts   = 3;      // flings per round
constexpr float kFlingPowerSpeed = 130.0f; // meter sweep, points/second
constexpr float kFlingGravity    = 1.6f;   // field units / s^2 (downward)
constexpr float kFlingVBase      = 0.35f;  // launch speed at power 0
constexpr float kFlingVMul       = 0.55f;  // launch speed added at power 100
constexpr float kFlingGroundY    = 0.80f;  // ground line (field-normalized)
constexpr float kFlingStartX     = 0.10f;  // slingshot x
constexpr float kFlingMeters     = 55.0f;  // full field width ~= meters

uint32_t Rgba(int r, int g, int b, int a = 255) {
    return (uint32_t(a) << 24) | (uint32_t(b) << 16) |
           (uint32_t(g) << 8) | uint32_t(r);
}

// Screen-centered text x for a string (font is 6 px/char at scale 1).
float TextCenterX(UIRenderer& ui, float sw, const std::string& text) {
    return 0.5f * (sw - ui.MeasureText(text));
}

// A cheap deterministic hash in [0,1) for per-attempt wind variation.
float Hash01(float seed) {
    const float h = std::fmod(std::sin(seed) * 43758.5453f, 1.0f);
    return h < 0.0f ? h + 1.0f : h;
}

} // namespace

// =========================================================================
// World cabinets
// =========================================================================

// Placement mirrors the vehicle ring strategy (Track 31): fixed offsets
// around `center`, each pulled inward step by step until the down-raycast
// finds floor, so a cabinet can never hang past the edge of the spawn roof.
// Two cabinets: Nut Shots ("dorm side") and Monkey Fling ("carnival side").
void ArcadeManager::SpawnCabinets(EntityManager& entities,
                                  CollisionWorld* collision,
                                  const glm::vec3& center) {
    struct Spec {
        ArcadeGameId game;
        const char*  name;
        const char*  tagline;
        float        dx, dz, yaw;
    };
    const Spec specs[] = {
        { ArcadeGameId::NutShots,    "NUT SHOTS",
          "MOVE MOUSE TO AIM  -  LMB THROWS",   -3.0f, -1.6f,  0.3f },
        { ArcadeGameId::MonkeyFling, "MONKEY FLING",
          "PRESS SPACE AT PEAK POWER TO FLING",  3.2f, -1.9f, -0.3f },
    };

    for (const Spec& s : specs) {
        // Ground-verified position (pull toward the center when off the edge).
        glm::vec3 pos = center + glm::vec3(s.dx, 0.0f, s.dz);
        bool grounded = false;
        if (collision) {
            const float len = std::sqrt(s.dx * s.dx + s.dz * s.dz);
            const float ux  = len > 1e-5f ? s.dx / len : 0.0f;
            const float uz  = len > 1e-5f ? s.dz / len : 0.0f;
            for (float r = len; r >= 0.0f; r -= 0.5f) {
                const glm::vec3 p = center + glm::vec3(ux * r, 0.0f, uz * r);
                Ray down;
                down.origin[0]    = p.x;
                down.origin[1]    = p.y + 50.0f;
                down.origin[2]    = p.z;
                down.direction[0] = 0.0f;
                down.direction[1] = -1.0f;
                down.direction[2] = 0.0f;
                RayHit hit;
                if (collision->Raycast(down, hit)) {
                    pos = glm::vec3(p.x, hit.point[1], p.z);
                    grounded = true;
                    break;
                }
            }
        }
        if (!grounded) pos.y = center.y; // last resort: level with the center

        EntityID e = entities.Create();
        Transform t;
        t.position = pos;
        t.rotation = glm::vec3(0.0f, s.yaw, 0.0f);
        t.scale    = glm::vec3(0.9f, 1.9f, 0.7f);
        t.position.y += 0.95f;   // the shared test box is centered at its origin
        entities.AddTransform(e, t);

        MeshComponent mc;
        mc.isBox = true;
        entities.AddMesh(e, mc);

        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-0.45f, -0.95f, -0.35f);
        cc.boundsMax = glm::vec3( 0.45f,  0.95f,  0.35f);
        cc.layerMask = 2u;
        entities.AddCollision(e, cc);

        ArcadeCabinet cab;
        cab.entity   = e;
        cab.game     = s.game;
        cab.name     = s.name;
        cab.tagline  = s.tagline;
        cab.position = pos;
        m_cabinets.push_back(std::move(cab));

        Logger::Info("Arcade: cabinet '%s' at (%.1f, %.1f, %.1f)%s",
                     s.name, pos.x, pos.y, pos.z,
                     grounded ? "" : " (UNGROUND fallback)");
    }
    Logger::Info("Arcade: %zu cabinets spawned", m_cabinets.size());
}

void ArcadeManager::UpdateProximity(const glm::vec3& playerPos) {
    m_nearIndex = -1;
    float bestDist = kPromptRange;
    for (int i = 0; i < int(m_cabinets.size()); ++i) {
        const float d = glm::length(m_cabinets[size_t(i)].position - playerPos);
        if (d < bestDist) { bestDist = d; m_nearIndex = i; }
    }
    if (m_nearIndex < 0) {
        m_prompt[0] = '\0';
        return;
    }
    const ArcadeCabinet& cab = m_cabinets[size_t(m_nearIndex)];
    std::snprintf(m_prompt, sizeof(m_prompt), "[E] PLAY %s  ($1)",
                  cab.name.c_str());
}

// =========================================================================
// Entry / exit / round flow
// =========================================================================

bool ArcadeManager::TryEnterCabinet(int cabinetIndex, int& playerMoney) {
    if (IsActive() || cabinetIndex < 0 ||
        cabinetIndex >= int(m_cabinets.size())) return false;

    m_activeGame = int(m_cabinets[size_t(cabinetIndex)].game);
    m_state      = ArcadeState::Attract;
    m_stateTime  = 0.0f;
    m_demoTimer  = 0.0f;
    m_demoTimer2 = 0.0f;
    m_lastNewHigh = false;

    if (playerMoney >= kPlayCost) {
        playerMoney -= kPlayCost;
        ++m_save.plays;
        m_message[0] = '\0';
        if (m_audio) m_audio->PlaySound(SoundId::UiClick, 0.5f);
    } else {
        std::snprintf(m_message, sizeof(m_message), "NO COINS - NEED $1");
    }
    Logger::Info("Arcade: entered '%s' (%s), money now $%d",
                 m_cabinets[size_t(cabinetIndex)].name.c_str(),
                 m_message[0] == '\0' ? "coin inserted" : "broke",
                 playerMoney);
    return true;
}

void ArcadeManager::ExitGame() {
    if (m_state == ArcadeState::InWorld) return;
    const char* when = m_state == ArcadeState::Playing ? "mid-round"
                        : m_state == ArcadeState::RoundOver ? "round over"
                                                             : "attract";
    Logger::Info("Arcade: exited '%s' (%s) -- no refund",
                 ActiveCabinet() ? ActiveCabinet()->name.c_str() : "?", when);
    m_state     = ArcadeState::InWorld;
    m_stateTime = 0.0f;
}

void ArcadeManager::StartRound() {
    m_state     = ArcadeState::Playing;
    m_stateTime = 0.0f;
    m_demoTimer  = 0.0f;
    m_demoTimer2 = 0.0f;
    m_demoThrowCd = 0.2f;

    if (m_activeGame == int(ArcadeGameId::NutShots)) {
        m_nut = NutShotsState{};
        m_nut.timeLeft = float(kNutRoundTime);
        for (int i = 0; i < kNutTargetCount; ++i) {
            NutTarget& tgt = m_nut.targets[i];
            tgt.rail   = i / 3;
            tgt.alive  = true;
            tgt.head   = (i % 3) == 2;      // every 3rd target is the headshot
            tgt.x      = float(i % 3) / 3.0f + 0.08f;
            tgt.dir    = tgt.rail == 0 ? 1 : -1;
        }
    } else {
        m_monkey = MonkeyState{};
        m_monkey.x = kFlingStartX;
        m_monkey.y = kFlingGroundY;
    }
}

void ArcadeManager::EndRound(int& playerMoney) {
    m_state     = ArcadeState::RoundOver;
    m_stateTime = 0.0f;

    const int score = (m_activeGame == int(ArcadeGameId::NutShots))
                          ? m_nut.score : m_monkey.bestRound;
    int& high = m_save.highScores[m_activeGame];
    m_lastNewHigh = score > high;
    if (m_lastNewHigh) {
        high = score;
        ++m_save.wins;
        playerMoney += kPrizeNewHigh;
        if (m_audio) m_audio->PlaySound(SoundId::UiClick, 0.8f);
        Logger::Info("Arcade: NEW HIGH SCORE %d on '%s' (+$%d prize, wins=%d)",
                     score, ActiveCabinet() ? ActiveCabinet()->name.c_str() : "?",
                     kPrizeNewHigh, m_save.wins);
    }
    if (!m_save.achievement && m_save.wins >= kAchievementWins) {
        m_save.achievement = 1;
        Logger::Info("Arcade: ACHIEVEMENT UNLOCKED -- Arcade Winner "
                     "(%d arcade games won)", m_save.wins);
    }
}

const ArcadeCabinet* ArcadeManager::ActiveCabinet() const {
    for (const ArcadeCabinet& c : m_cabinets)
        if (int(c.game) == m_activeGame) return &c;
    return nullptr;
}

// =========================================================================
// Per-frame arcade-view update
// =========================================================================

void ArcadeManager::UpdateActive(Input& input, float dt, int& playerMoney,
                                 bool lmbPressed) {
    if (m_state == ArcadeState::InWorld) return;
    m_stateTime += dt;

    switch (m_state) {
        case ArcadeState::Attract: {
            // Space starts the paid round. Demo auto-starts after 0.6 s.
            bool start = input.WasKeyPressed(SDL_SCANCODE_SPACE);
            if (m_demo && m_stateTime >= 0.6f) start = true;
            if (start && std::strncmp(m_message, "NO COINS", 8) != 0) {
                m_message[0] = '\0';
                StartRound();
            }
            break;
        }
        case ArcadeState::Playing: {
            if (m_activeGame == int(ArcadeGameId::NutShots)) {
                if (m_demo) DriveNutShotsDemo(dt);
                UpdateNutShots(input, dt, lmbPressed);
                if (m_nut.timeLeft <= 0.0f) EndRound(playerMoney);
            } else {
                if (m_demo) DriveMonkeyFlingDemo(dt);
                UpdateMonkeyFling(input, dt);
                if (m_monkey.attempt >= kFlingAttempts && !m_monkey.flying)
                    EndRound(playerMoney);
            }
            break;
        }
        case ArcadeState::RoundOver: {
            if (m_demo && m_stateTime >= 4.0f) { ExitGame(); break; }
            if (input.WasKeyPressed(SDL_SCANCODE_SPACE)) {
                if (playerMoney >= kPlayCost) {
                    playerMoney -= kPlayCost;
                    ++m_save.plays;
                    m_message[0] = '\0';
                    StartRound();
                } else {
                    std::snprintf(m_message, sizeof(m_message),
                                  "NO COINS - NEED $1");
                }
            }
            break;
        }
        case ArcadeState::InWorld:
        default:
            break;
    }
}

// =========================================================================
// Nut Shots
// =========================================================================

void ArcadeManager::UpdateNutShots(Input& input, float dt, bool lmbPressed) {
    // Crosshair: relative mouse deltas (fed by the engine before its
    // strip-freeze). 720 px of movement spans the field.
    m_nut.crossX += float(input.MouseDX()) * 0.55f / 720.0f;
    m_nut.crossY += float(input.MouseDY()) * 0.55f / 720.0f;
    m_nut.crossX = std::clamp(m_nut.crossX, 0.02f, 0.98f);
    m_nut.crossY = std::clamp(m_nut.crossY, 0.02f, 0.98f);

    // Timer + cooldowns + puffs.
    m_nut.timeLeft -= dt;
    m_nut.throwCd  = std::max(0.0f, m_nut.throwCd - dt);
    for (float& p : m_nut.puffT)
        if (p > 0.0f) p = std::max(0.0f, p - dt * 2.5f);

    // Throw (LMB edge, captured by the engine pre-freeze): 0.12-0.3 s lerp to
    // the aimed point; hits resolve on impact.
    if (lmbPressed && m_nut.throwCd <= 0.0f) {
        m_nut.throwCd = kNutThrowCd;
        NutShot s;
        s.x = 0.5f;
        s.y = 1.02f;                    // bottom-center of the field
        s.tx = m_nut.crossX;
        s.ty = m_nut.crossY;
        const float dx = s.tx - s.x;
        const float dy = s.ty - s.y;
        s.dur = 0.12f + std::sqrt(dx * dx + dy * dy) * 0.25f;
        m_nut.shots.push_back(s);
        ++m_nut.throws;
        if (m_audio) m_audio->PlaySound(SoundId::UiClick, 0.2f);
    }

    // Flying nuts -> impact test.
    for (NutShot& s : m_nut.shots) {
        s.t += dt / s.dur;
        if (s.t < 1.0f) continue;
        for (int i = 0; i < kNutTargetCount; ++i) {
            NutTarget& tgt = m_nut.targets[i];
            if (!tgt.alive || tgt.falling) continue;
            const float cx   = tgt.x;
            const float bodyC = kRailTopY[tgt.rail] + kDuckBodyOffset;
            const float headC = bodyC - kDuckHeadOffset;
            if (s.tx > cx - kDuckHeadHW - kHitMargin &&
                s.tx < cx + kDuckHeadHW + kHitMargin &&
                s.ty > headC - kDuckHeadHH - kHitMargin &&
                s.ty < headC + kDuckHeadHH + kHitMargin) {
                tgt.falling = true;
                tgt.head    = true;
                m_nut.score += 2;
            } else if (s.tx > cx - kDuckBodyHW - kHitMargin &&
                       s.tx < cx + kDuckBodyHW + kHitMargin &&
                       s.ty > bodyC - kDuckBodyHH - kHitMargin &&
                       s.ty < bodyC + kDuckBodyHH + kHitMargin) {
                tgt.falling = true;
                tgt.head    = false;
                m_nut.score += 1;
            } else {
                continue;
            }
            ++m_nut.hits;
            m_nut.puffT[i] = 1.0f;
            if (m_audio) m_audio->PlaySound(SoundId::UiClick, 0.35f);
            break;
        }
    }
    m_nut.shots.erase(std::remove_if(m_nut.shots.begin(), m_nut.shots.end(),
                                     [](const NutShot& s) { return s.t >= 1.0f; }),
                      m_nut.shots.end());

    // Rail motion + fall/respawn state machines.
    for (int i = 0; i < kNutTargetCount; ++i) {
        NutTarget& tgt = m_nut.targets[i];
        if (tgt.falling) {
            tgt.fallT += dt * 2.0f;
            if (tgt.fallT >= 1.0f) {
                tgt.falling  = false;
                tgt.alive    = false;
                tgt.respawnT = 1.2f + float(i) * 0.1f;
            }
            continue;
        }
        if (!tgt.alive) {
            tgt.respawnT -= dt;
            if (tgt.respawnT <= 0.0f) {
                tgt.alive = true;
                tgt.x     = tgt.dir > 0 ? -0.05f : 1.05f;
            }
            continue;
        }
        tgt.x += tgt.dir * dt * kDuckRailSpeed[tgt.rail];
        if (tgt.x > 1.05f) tgt.x = -0.05f;
        if (tgt.x < -0.05f) tgt.x = 1.05f;
    }
}

void ArcadeManager::DriveNutShotsDemo(float dt) {
    // Figure-8 aim sweep + a throw every 0.9 s (no Input reads).
    m_demoTimer += dt;
    const float t = m_demoTimer;
    m_nut.crossX = 0.5f + 0.36f * std::sin(t * 1.9f);
    m_nut.crossY = 0.36f + 0.10f * std::sin(t * 3.1f);
    m_demoThrowCd -= dt;
    if (m_demoThrowCd <= 0.0f) {
        m_demoThrowCd = 0.9f;
        m_nut.throwCd = kNutThrowCd;
        NutShot s;
        s.x = 0.5f;
        s.y = 1.02f;
        s.tx = m_nut.crossX;
        s.ty = m_nut.crossY;
        const float dx = s.tx - s.x;
        const float dy = s.ty - s.y;
        s.dur = 0.12f + std::sqrt(dx * dx + dy * dy) * 0.25f;
        m_nut.shots.push_back(s);
        ++m_nut.throws;
    }
}

// =========================================================================
// Monkey Fling
// =========================================================================

void ArcadeManager::UpdateMonkeyFling(Input& input, float dt) {
    if (m_monkey.flying) {
        // Ballistic flight + per-attempt wind; lands on the ground line.
        m_monkey.x  += m_monkey.vx * dt;
        m_monkey.y  += m_monkey.vy * dt;
        m_monkey.vy += kFlingGravity * dt;
        m_monkey.vx += m_monkey.wind * 0.01f * dt;
        if (m_monkey.y >= kFlingGroundY && m_monkey.vy >= 0.0f) {
            m_monkey.y      = kFlingGroundY;
            m_monkey.flying = false;
            const float field = std::max(0.0f, m_monkey.x - kFlingStartX);
            m_monkey.lastDist  = int(field * kFlingMeters);
            m_monkey.bestRound = std::max(m_monkey.bestRound, m_monkey.lastDist);
            if (m_monkey.attempt >= 1 && m_monkey.attempt <= 3)
                m_monkey.markerX[m_monkey.attempt - 1] = m_monkey.x;
            if (m_audio) m_audio->PlaySound(SoundId::UiClick, 0.4f);
            Logger::Info("Arcade: fling %d lands at %d m (round best %d m)",
                         m_monkey.attempt, m_monkey.lastDist,
                         m_monkey.bestRound);
        }
        return;
    }
    if (m_monkey.attempt >= kFlingAttempts) return; // EndRound() fires next

    // Oscillating power meter.
    m_monkey.power += float(m_monkey.powerDir) * kFlingPowerSpeed * dt;
    if (m_monkey.power >= 100.0f) { m_monkey.power = 100.0f; m_monkey.powerDir = -1; }
    if (m_monkey.power <= 0.0f)   { m_monkey.power = 0.0f;   m_monkey.powerDir = 1; }

    // Space = fling at the current power.
    if (input.WasKeyPressed(SDL_SCANCODE_SPACE)) {
        m_monkey.flying = true;
        ++m_monkey.attempt;
        const float p = m_monkey.power / 100.0f;
        const float speed = kFlingVBase + p * kFlingVMul;
        m_monkey.vx =  speed * 1.05f;          // mostly horizontal
        m_monkey.vy = -speed * 0.85f;          // up (field y grows downward)
        m_monkey.x  = kFlingStartX;
        m_monkey.y  = kFlingGroundY - 0.06f;
        m_monkey.wind = (Hash01(m_stateTime * 12.9898f) - 0.5f) * 6.0f; // m/s
        Logger::Info("Arcade: fling %d at power %.0f%%, wind %+.1f m/s",
                     m_monkey.attempt, m_monkey.power, m_monkey.wind);
    }
}

void ArcadeManager::DriveMonkeyFlingDemo(float dt) {
    // Wait for the power band [85..99] and fling (no Input reads).
    if (m_monkey.flying || m_monkey.attempt >= kFlingAttempts) return;
    m_demoTimer2 += dt;
    if (m_monkey.power >= 85.0f && m_monkey.power <= 99.0f &&
        m_demoTimer2 > 0.35f) {
        m_demoTimer2 = 0.0f;
        m_monkey.flying = true;
        ++m_monkey.attempt;
        const float p = m_monkey.power / 100.0f;
        const float speed = kFlingVBase + p * kFlingVMul;
        m_monkey.vx =  speed * 1.05f;
        m_monkey.vy = -speed * 0.85f;
        m_monkey.x  = kFlingStartX;
        m_monkey.y  = kFlingGroundY - 0.06f;
        m_monkey.wind = (Hash01(m_stateTime * 12.9898f) - 0.5f) * 6.0f;
        Logger::Info("Arcade demo: fling %d at power %.0f", m_monkey.attempt,
                     m_monkey.power);
    }
}

// =========================================================================
// Persistence
// =========================================================================

void ArcadeManager::WriteSave(SaveData& out) const {
    out.arcadeHigh[0] = m_save.highScores[0];
    out.arcadeHigh[1] = m_save.highScores[1];
    out.arcadePlays        = m_save.plays;
    out.arcadeWins         = m_save.wins;
    out.arcadeAchievement  = m_save.achievement;
}

void ArcadeManager::ReadSave(const SaveData& in) {
    m_save.highScores[0] = in.arcadeHigh[0];
    m_save.highScores[1] = in.arcadeHigh[1];
    m_save.plays        = in.arcadePlays;
    m_save.wins         = in.arcadeWins;
    m_save.achievement  = in.arcadeAchievement;
}

// =========================================================================
// Rendering (bgfx view 10, full-screen cabinet screen)
// =========================================================================

void ArcadeManager::Render(UIRenderer& ui, float sw, float sh) const {
    if (m_state == ArcadeState::InWorld) return;
    ui.DrawRect(0.0f, 0.0f, sw, sh, Rgba(6, 8, 16));
    DrawCabFrame(ui, sw, sh);
    switch (m_state) {
        case ArcadeState::Attract:
            RenderAttract(ui, sw, sh);
            break;
        case ArcadeState::Playing:
            if (m_activeGame == int(ArcadeGameId::NutShots))
                RenderNutShots(ui, sw, sh);
            else
                RenderMonkeyFling(ui, sw, sh);
            break;
        case ArcadeState::RoundOver:
            RenderRoundOver(ui, sw, sh);
            break;
        case ArcadeState::InWorld:
            break;
    }
}

void ArcadeManager::DrawCabFrame(UIRenderer& ui, float sw, float sh) const {
    // Cabinet bezel: marquee strip on top + bottom frame line.
    const float u  = sh / 720.0f;
    ui.DrawRect(0.0f, 0.0f, sw, 24.0f * u, Rgba(110, 26, 26));
    const char* title = "NUT SHOTS";
    if (m_activeGame == int(ArcadeGameId::MonkeyFling)) title = "MONKEY FLING";
    const std::string big = title;
    const float titleW = ui.MeasureText(big) * 2.0f;
    ui.DrawText(0.5f * (sw - titleW), 3.0f * u, big, Rgba(255, 210, 100));
    const char* coin = "$1 PER PLAY";
    ui.DrawText(sw - ui.MeasureText(coin) - 10.0f, 8.0f * u, coin,
                Rgba(200, 180, 120));
    ui.DrawRect(0.0f, sh - 6.0f * u, sw, 4.0f, Rgba(110, 26, 26));
}

void ArcadeManager::RenderNutShots(UIRenderer& ui, float sw, float sh) const {
    // Play field + gallery back wall.
    const float fx = sw * 0.08f, fy = sh * 0.13f;
    const float fw = sw * 0.84f, fh = sh * 0.80f;
    ui.DrawRect(fx, fy, fw, fh, Rgba(22, 32, 58));
    for (int i = 0; i < 2; ++i)
        ui.DrawRect(fx, fy + fh * (kRailTopY[i] - 0.005f), fw, 2.0f,
                    Rgba(70, 56, 34));
    ui.DrawRect(fx, fy + fh * 0.88f, fw, fh * 0.12f, Rgba(72, 46, 24));
    // Gallery stripes on the back wall.
    for (int i = 0; i < 5; ++i)
        ui.DrawRect(fx + fw * (0.06f + i * 0.18f), fy + fh * 0.02f,
                    fw * 0.09f, fh * 0.14f, Rgba(34, 46, 74));

    // Targets (ducks: body + head + beak + eye).
    for (int i = 0; i < kNutTargetCount; ++i) {
        const NutTarget& tgt = m_nut.targets[i];
        const float bx   = fx + tgt.x * fw;
        const float bodyC = fy + fh * (kRailTopY[tgt.rail] + kDuckBodyOffset);
        const float headC = bodyC - fh * kDuckHeadOffset;
        if (tgt.alive && !tgt.falling) {
            ui.DrawRect(bx - fw * kDuckBodyHW, bodyC - fh * kDuckBodyHH,
                        fw * kDuckBodyHW * 2.0f, fh * kDuckBodyHH * 2.0f,
                        Rgba(226, 198, 44));
            ui.DrawRect(bx - fw * kDuckHeadHW, headC - fh * kDuckHeadHH,
                        fw * kDuckHeadHW * 2.0f, fh * kDuckHeadHH * 2.0f,
                        Rgba(226, 198, 44));
            const float beakX = tgt.dir > 0 ? bx + fw * kDuckHeadHW
                                            : bx - fw * (kDuckHeadHW + 0.010f);
            ui.DrawRect(beakX, headC - fh * 0.006f, fw * 0.010f, fh * 0.012f,
                        Rgba(240, 130, 30));
            ui.DrawRect(bx + (tgt.dir > 0 ? fw * 0.006f : 0.0f),
                        headC - fh * 0.012f, fw * 0.006f, fh * 0.012f,
                        Rgba(24, 24, 24));
        } else if (tgt.falling) {
            const float k = 1.0f - tgt.fallT;
            ui.DrawRect(bx - fw * kDuckBodyHW * k, bodyC + fh * tgt.fallT * 0.10f,
                        fw * kDuckBodyHW * 2.0f * k, fh * kDuckBodyHH * 2.0f * k,
                        Rgba(226, 198, 44, int(90 + 160 * k)));
        }
        if (m_nut.puffT[i] > 0.0f) {
            const float r = fw * 0.055f * (1.4f - m_nut.puffT[i]);
            ui.DrawRect(bx - r, bodyC - r, r * 2.0f, r * 2.0f,
                        Rgba(255, 240, 190, int(220 * m_nut.puffT[i])));
        }
    }

    // Flying nuts (flight lerp + slight arc).
    for (const NutShot& s : m_nut.shots) {
        const float k = s.t;
        const float nx = fx + (s.x + (s.tx - s.x) * k) * fw;
        const float ny = fy + (s.y + (s.ty - s.y) * k) * fh - 6.0f * (k * (1.0f - k));
        ui.DrawRect(nx - 3.5f, ny - 3.5f, 7.0f, 7.0f, Rgba(150, 96, 40));
    }

    // Crosshair.
    const float cx = fx + m_nut.crossX * fw;
    const float cy = fy + m_nut.crossY * fh;
    ui.DrawRect(cx - 1.5f, cy - 11.0f, 3.0f, 22.0f, Rgba(255, 120, 80));
    ui.DrawRect(cx - 11.0f, cy - 1.5f, 22.0f, 3.0f, Rgba(255, 120, 80));

    // HUD row.
    char buf[80];
    const float hudY = sh * 0.075f;
    std::snprintf(buf, sizeof(buf), "SCORE %d", m_nut.score);
    ui.DrawText(fx, hudY, buf, Rgba(255, 240, 160));
    std::snprintf(buf, sizeof(buf), "TIME %d", int(std::ceil(m_nut.timeLeft)));
    ui.DrawText(TextCenterX(ui, sw, buf), hudY, buf,
                m_nut.timeLeft < 6.0f ? Rgba(255, 90, 70) : Rgba(220, 220, 220));
    std::snprintf(buf, sizeof(buf), "BEST %d", HighScore(ArcadeGameId::NutShots));
    ui.DrawText(fx + fw - ui.MeasureText(buf), hudY, buf, Rgba(160, 220, 255));
    std::snprintf(buf, sizeof(buf), "HITS %d/%d", m_nut.hits, m_nut.throws);
    ui.DrawText(fx + fw * 0.5f - ui.MeasureText(buf) * 0.5f, hudY + 12.0f, buf,
                Rgba(170, 180, 190));
}

void ArcadeManager::RenderMonkeyFling(UIRenderer& ui, float sw, float sh) const {
    const float fx = sw * 0.06f, fy = sh * 0.12f;
    const float fw = sw * 0.88f, fh = sh * 0.80f;
    ui.DrawRect(fx, fy, fw, fh, Rgba(30, 46, 82));
    const float groundY = fy + fh * kFlingGroundY;
    ui.DrawRect(fx, groundY, fw, fy + fh - groundY, Rgba(84, 122, 52));

    // Distance ruler (every 10 m).
    char lbl[16];
    for (int m = 0; m <= int(kFlingMeters); m += 10) {
        const float rx = fx + (kFlingStartX + float(m) / kFlingMeters) * fw;
        ui.DrawRect(rx, groundY - 10.0f, 2.0f, 10.0f, Rgba(220, 220, 160));
        std::snprintf(lbl, sizeof(lbl), "%d", m);
        ui.DrawText(rx - ui.MeasureText(lbl) * 0.5f, groundY + 4.0f, lbl,
                    Rgba(210, 220, 170));
    }

    // Landing markers (earlier attempts).
    for (int i = 0; i < 3; ++i) {
        if (m_monkey.markerX[i] > 2.0f) continue; // unused (1e9 default)
        ui.DrawRect(fx + m_monkey.markerX[i] * fw - 1.5f, groundY - 26.0f,
                    3.0f, 26.0f, Rgba(255, 190, 70));
    }

    // Slingshot post + fork.
    const float sx = fx + kFlingStartX * fw;
    ui.DrawRect(sx - 2.5f, groundY - fh * 0.13f, 5.0f, fh * 0.13f,
                Rgba(110, 70, 40));
    ui.DrawRect(sx - 10.0f, groundY - fh * 0.13f, 16.0f, 5.0f, Rgba(130, 86, 46));

    // Monkey (body + head + ears + tail). Spins faster while flying.
    const float mx = fx + m_monkey.x * fw;
    const float my = fy + m_monkey.y * fh;
    const float bodyW = 18.0f * (sh / 720.0f);
    const float bodyH = 22.0f * (sh / 720.0f);
    ui.DrawRect(mx - bodyW * 0.5f, my - bodyH, bodyW, bodyH,
                Rgba(120, 70, 40));
    ui.DrawRect(mx - bodyW * 0.25f, my - bodyH * 1.35f, bodyW * 0.5f,
                bodyH * 0.42f, Rgba(152, 98, 54));
    ui.DrawRect(mx - bodyW * 0.5f - bodyW * 0.18f, my - bodyH * 1.30f,
                bodyW * 0.18f, bodyW * 0.18f, Rgba(152, 98, 54));
    ui.DrawRect(mx + bodyW * 0.5f, my - bodyH * 1.30f,
                bodyW * 0.18f, bodyW * 0.18f, Rgba(152, 98, 54));
    ui.DrawRect(mx - bodyW * 0.5f - bodyW * 0.30f, my - bodyH * 0.70f,
                bodyW * 0.30f, bodyW * 0.16f, Rgba(120, 70, 40));
    ui.DrawRect(mx - 1.5f, my - bodyH * 0.92f, 3.0f, 3.0f, Rgba(240, 240, 220));

    // Power meter (left column, only while aiming).
    const float pw = 14.0f;
    const float px = fx + 10.0f;
    const float phM = fh * 0.52f;
    ui.DrawRect(px - 2.0f, groundY - phM - 4.0f, pw + 4.0f, phM + 4.0f,
                Rgba(8, 10, 20));
    const float filled = phM * (m_monkey.power / 100.0f);
    const uint32_t barCol = m_monkey.power > 85.0f ? Rgba(90, 230, 90)
                        : m_monkey.power > 50.0f ? Rgba(230, 210, 60)
                                                  : Rgba(220, 80, 50);
    ui.DrawRect(px, groundY - 4.0f - filled, pw, filled, barCol);
    std::snprintf(lbl, sizeof(lbl), "POWER");
    ui.DrawText(px + pw * 0.5f - ui.MeasureText(lbl) * 0.5f, groundY - phM - 20.0f,
                lbl, Rgba(200, 200, 210));

    // Wind indicator.
    std::snprintf(lbl, sizeof(lbl), "WIND %+.1f", (double)m_monkey.wind);
    ui.DrawText(fx + fw - ui.MeasureText(lbl) - 10.0f, fy + 8.0f, lbl,
                Rgba(190, 210, 255));

    // HUD row.
    char buf[96];
    const float hudY = sh * 0.075f;
    std::snprintf(buf, sizeof(buf), "ATTEMPT %d/%d",
                  std::min(m_monkey.attempt + 1, kFlingAttempts), kFlingAttempts);
    ui.DrawText(fx, hudY, buf, Rgba(255, 240, 160));
    std::snprintf(buf, sizeof(buf), "LAST %dM   BEST %dM", m_monkey.lastDist,
                  m_monkey.bestRound);
    ui.DrawText(TextCenterX(ui, sw, buf), hudY, buf, Rgba(220, 220, 220));
    std::snprintf(buf, sizeof(buf), "HI %dM", HighScore(ArcadeGameId::MonkeyFling));
    ui.DrawText(fx + fw - ui.MeasureText(buf), hudY, buf, Rgba(160, 220, 255));
}

void ArcadeManager::RenderAttract(UIRenderer& ui, float sw, float sh) const {
    const ArcadeCabinet* cab = ActiveCabinet();
    const std::string name = cab ? cab->name : "ARCADE";
    const std::string tag  = cab ? cab->tagline : "";
    const float s = sh / 720.0f;

    const bool broke = std::strncmp(m_message, "NO COINS", 8) == 0;
    const bool blink = std::fmod(m_stateTime, 1.0f) < 0.6f;

    // Backdrop stripes + title.
    for (int i = 0; i < 7; ++i)
        ui.DrawRect(sw * (0.10f + i * 0.115f), sh * 0.20f, sw * 0.06f, sh * 0.30f,
                    Rgba(30, 40, 66));
    const float nameW = ui.MeasureText(name) * 3.5f;
    ui.DrawText(0.5f * (sw - nameW), sh * 0.26f, name, Rgba(255, 210, 80));
    ui.DrawText(TextCenterX(ui, sw, tag), sh * 0.36f, tag,
                Rgba(170, 180, 200));

    if (broke) {
        const std::string msg = "NO COINS - NEED $1";
        if (blink) ui.DrawText(TextCenterX(ui, sw, msg), sh * 0.50f, msg,
                               Rgba(255, 80, 60));
    } else if (blink) {
        const std::string msg = "INSERT COIN";
        ui.DrawText(TextCenterX(ui, sw, msg), sh * 0.50f, msg,
                    Rgba(120, 255, 120));
    }

    const std::string hint = broke ? "ESC = WALK AWAY (NO REFUND)"
                                   : "SPACE = START GAME";
    ui.DrawText(TextCenterX(ui, sw, hint), sh * 0.58f, hint,
                Rgba(200, 200, 210));
    char hi[48];
    std::snprintf(hi, sizeof(hi), "HIGH SCORE %d", HighScore(ArcadeGameId(m_activeGame)));
    ui.DrawText(TextCenterX(ui, sw, hi), sh * 0.66f, hi, Rgba(160, 220, 255));
    char win[48];
    std::snprintf(win, sizeof(win), "TOTAL WINS %d/10", m_save.wins);
    ui.DrawText(TextCenterX(ui, sw, win), sh * 0.72f, win,
                AchievementUnlocked() ? Rgba(255, 200, 120)
                                      : Rgba(150, 160, 180));
    ui.DrawText(0.0f, sh - 22.0f * s, "$1 PER PLAY - PRIZE $5 FOR A NEW HIGH SCORE",
                Rgba(140, 145, 160));
}

void ArcadeManager::RenderRoundOver(UIRenderer& ui, float sw, float sh) const {
    const ArcadeCabinet* cab = ActiveCabinet();
    const std::string name = cab ? cab->name : "ARCADE";
    const float s = sh / 720.0f;

    const int score = (m_activeGame == int(ArcadeGameId::NutShots))
                          ? m_nut.score : m_monkey.bestRound;
    const int high = HighScore(ArcadeGameId(m_activeGame));

    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s -- FINAL", name.c_str());
    ui.DrawText(TextCenterX(ui, sw, buf), sh * 0.24f, buf, Rgba(255, 210, 80));
    std::snprintf(buf, sizeof(buf), "SCORE %d", score);
    ui.DrawText(TextCenterX(ui, sw, buf), sh * 0.34f, buf, Rgba(255, 255, 255));

    if (m_lastNewHigh && std::fmod(m_stateTime, 0.8f) < 0.55f) {
        const std::string nh = "NEW HIGH SCORE! +$5";
        ui.DrawText(TextCenterX(ui, sw, nh), sh * 0.45f, nh,
                    Rgba(120, 255, 120));
    }
    std::snprintf(buf, sizeof(buf), "HIGH SCORE %d", high);
    ui.DrawText(TextCenterX(ui, sw, buf), sh * 0.54f, buf, Rgba(160, 220, 255));

    std::snprintf(buf, sizeof(buf), "TOTAL WINS %d/10", m_save.wins);
    ui.DrawText(TextCenterX(ui, sw, buf), sh * 0.60f, buf,
                AchievementUnlocked() ? Rgba(255, 200, 120)
                                      : Rgba(150, 160, 180));

    if (AchievementUnlocked() && std::fmod(m_stateTime, 2.0f) < 1.6f) {
        const std::string ach = "ACHIEVEMENT UNLOCKED: ARCADE WINNER";
        ui.DrawText(TextCenterX(ui, sw, ach), sh * 0.66f, ach,
                    Rgba(255, 210, 120));
    }
    const std::string again = "SPACE = PLAY AGAIN ($1)";
    ui.DrawText(TextCenterX(ui, sw, again), sh * 0.74f, again,
                Rgba(200, 200, 210));
    const std::string out = "ESC = EXIT (NO REFUND)";
    ui.DrawText(TextCenterX(ui, sw, out), sh * 0.80f, out,
                Rgba(120, 120, 140));
}

} // namespace OpenBully
