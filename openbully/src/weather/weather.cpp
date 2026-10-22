// Track 34: Weather system implementation.
// See weather.h for the interface and state-machine description.

#include "weather/weather.h"

#include "platform/logger.h"
#include "rendering/renderer.h"

#include <SDL2/SDL.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace OpenBully {

namespace {

// Builds a billboard quad (2 triangles = 6 vertices) into the vertex array.
// The quad is centered at `center`, sized (width x height), and oriented to
// face the camera using the provided right/up vectors (extracted from the
// view matrix). Vertex format is position-only (3 floats per vertex).
void AddBillboardQuad(std::vector<float>& verts, const glm::vec3& center,
                      float width, float height,
                      const glm::vec3& right, const glm::vec3& up) {
    const float hw = width * 0.5f;
    const float hh = height * 0.5f;
    const glm::vec3 r = right * hw;
    const glm::vec3 u = up * hh;

    const glm::vec3 v0 = center + r + u; // top-right
    const glm::vec3 v1 = center - r + u; // top-left
    const glm::vec3 v2 = center - r - u; // bottom-left
    const glm::vec3 v3 = center + r - u; // bottom-right

    // Triangle 1: v0, v1, v2
    verts.push_back(v0.x); verts.push_back(v0.y); verts.push_back(v0.z);
    verts.push_back(v1.x); verts.push_back(v1.y); verts.push_back(v1.z);
    verts.push_back(v2.x); verts.push_back(v2.y); verts.push_back(v2.z);
    // Triangle 2: v0, v2, v3
    verts.push_back(v0.x); verts.push_back(v0.y); verts.push_back(v0.z);
    verts.push_back(v2.x); verts.push_back(v2.y); verts.push_back(v2.z);
    verts.push_back(v3.x); verts.push_back(v3.y); verts.push_back(v3.z);
}

// Counts active particles in a vector.
int CountActive(const std::vector<Particle>& particles) {
    int count = 0;
    for (const Particle& p : particles)
        if (p.active) ++count;
    return count;
}

} // namespace

WeatherManager& WeatherManager::Get() {
    static WeatherManager instance;
    return instance;
}

const char* WeatherManager::TypeName(WeatherType t) {
    switch (t) {
        case WeatherType::CLEAR: return "CLEAR";
        case WeatherType::RAIN:  return "RAIN";
        case WeatherType::SNOW:  return "SNOW";
        case WeatherType::FOG:   return "FOG";
        default:                 return "?";
    }
}

void WeatherManager::Init() {
    m_rainDrops.resize(kMaxRain);
    m_snowFlakes.resize(kMaxSnow);
    m_splashParticles.resize(kMaxSplash);
    for (auto& p : m_rainDrops) p.active = false;
    for (auto& p : m_snowFlakes) p.active = false;
    for (auto& p : m_splashParticles) p.active = false;

    // Set initial weather from env var (for automated testing).
    const char* start = std::getenv("OPENBULLY_WEATHER_START");
    if (start) {
        if (std::strcmp(start, "RAIN") == 0) {
            TransitionTo(WeatherType::RAIN);
            Logger::Info("Weather: OPENBULLY_WEATHER_START=RAIN override");
        } else if (std::strcmp(start, "SNOW") == 0) {
            TransitionTo(WeatherType::SNOW);
            Logger::Info("Weather: OPENBULLY_WEATHER_START=SNOW override");
        } else if (std::strcmp(start, "FOG") == 0) {
            TransitionTo(WeatherType::FOG);
            Logger::Info("Weather: OPENBULLY_WEATHER_START=FOG override");
        } else if (std::strcmp(start, "CLEAR") == 0) {
            TransitionTo(WeatherType::CLEAR);
        } else {
            Logger::Warning("Weather: unknown OPENBULLY_WEATHER_START='%s', using CLEAR", start);
            TransitionTo(WeatherType::CLEAR);
        }
    } else {
        TransitionTo(WeatherType::CLEAR);
    }
}

void WeatherManager::TransitionTo(WeatherType type) {
    if (m_current == type) return;

    Logger::Info("Weather: %s -> %s (timer %.0fs)",
                 TypeName(m_current), TypeName(type),
                 type == WeatherType::FOG ? 20.0f :
                 (type == WeatherType::RAIN ? 45.0f :
                  (type == WeatherType::SNOW ? 30.0f : 60.0f)));

    m_current = type;
    m_intensity = 0.0f;

    // Set duration based on type (FOG is manual-only, shorter duration).
    switch (type) {
        case WeatherType::CLEAR: m_timer = 60.0f; break;
        case WeatherType::RAIN:  m_timer = 45.0f; break;
        case WeatherType::SNOW:  m_timer = 30.0f; break;
        case WeatherType::FOG:   m_timer = 20.0f; break;
        default:                 m_timer = 60.0f; break;
    }
}

void WeatherManager::Update(float dt, const glm::vec3& camPos, const glm::mat4& view) {
    // OPENBULLY_WEATHER_FAST=1: speed up the cycle timer by 10x for testing.
    static const bool fastMode = std::getenv("OPENBULLY_WEATHER_FAST") != nullptr;
    const float timerDt = fastMode ? dt * 10.0f : dt;

    m_timer -= timerDt;

    // Ramp intensity: fade in over first 3s, fade out over last 3s.
    if (m_timer > 3.0f) {
        if (m_intensity < 1.0f) {
            m_intensity = std::min(1.0f, m_intensity + timerDt / 3.0f);
        }
    } else if (m_timer > 0.0f) {
        m_intensity = m_timer / 3.0f;
    } else {
        m_intensity = 0.0f;
        // Advance auto cycle.
        m_cycleIndex = (m_cycleIndex + 1) % kAutoCycleLen;
        TransitionTo(kAutoCycle[m_cycleIndex]);
    }

    // Update fog based on current weather.
    m_fogDensity = 0.0f;
    if (m_current == WeatherType::FOG) {
        m_fogDensity = m_intensity * kFogMaxDensity;
    } else if (m_current == WeatherType::RAIN) {
        m_fogDensity = m_intensity * 0.005f; // light mist during rain
    }

    // Extract camera right/up from view matrix for billboarding.
    // View matrix is column-major; rows of the rotation part give camera axes.
    m_camRight = glm::vec3(view[0][0], view[1][0], view[2][0]);
    m_camUp    = glm::vec3(view[0][1], view[1][1], view[2][1]);

    // Update particles (always update active ones, even during transitions).
    UpdateRain(dt, camPos);
    UpdateSnow(dt, camPos);
    UpdateSplash(dt);
}

void WeatherManager::UpdateRain(float dt, const glm::vec3& camPos) {
    const int targetCount = (int)(m_intensity * (float)kMaxRain);

    for (Particle& p : m_rainDrops) {
        if (!p.active) continue;

        p.position += p.velocity * dt;
        p.life -= dt;

        if (p.life <= 0.0f || p.position.y < camPos.y - 25.0f) {
            // Emit splash on ground contact (only during active rain).
            if (m_current == WeatherType::RAIN && m_intensity > 0.0f) {
                EmitSplash(p.position);
                RespawnRain(p, camPos);
            } else {
                p.active = false;
            }
        }
    }

    // Spawn new drops to match target count.
    if (m_current == WeatherType::RAIN && m_intensity > 0.0f) {
        int activeCount = CountActive(m_rainDrops);
        for (int i = 0; i < (int)m_rainDrops.size() && activeCount < targetCount; i++) {
            if (!m_rainDrops[i].active) {
                m_rainDrops[i].active = true;
                RespawnRain(m_rainDrops[i], camPos);
                activeCount++;
            }
        }
    }
}

void WeatherManager::UpdateSnow(float dt, const glm::vec3& camPos) {
    const int targetCount = (int)(m_intensity * (float)kMaxSnow);

    for (Particle& p : m_snowFlakes) {
        if (!p.active) continue;

        p.position += p.velocity * dt;
        p.life -= dt;

        if (p.life <= 0.0f || p.position.y < camPos.y - 25.0f) {
            if (m_current == WeatherType::SNOW && m_intensity > 0.0f) {
                RespawnSnow(p, camPos);
            } else {
                p.active = false;
            }
        }
    }

    // Spawn new flakes to match target count.
    if (m_current == WeatherType::SNOW && m_intensity > 0.0f) {
        int activeCount = CountActive(m_snowFlakes);
        for (int i = 0; i < (int)m_snowFlakes.size() && activeCount < targetCount; i++) {
            if (!m_snowFlakes[i].active) {
                m_snowFlakes[i].active = true;
                RespawnSnow(m_snowFlakes[i], camPos);
                activeCount++;
            }
        }
    }
}

void WeatherManager::UpdateSplash(float dt) {
    for (Particle& p : m_splashParticles) {
        if (!p.active) continue;

        p.position += p.velocity * dt;
        p.velocity.y -= 4.9f * dt; // reduced gravity for splash
        p.life -= dt;
        if (p.life <= 0.0f) p.active = false;
    }
}

void WeatherManager::RespawnRain(Particle& p, const glm::vec3& camPos) {
    // Spawn at top of frustum: camera.pos + up*20 + random XZ offset ±30u.
    const float rx = RandomFloat() * kSpawnRadius; // [-30, 30]
    const float rz = RandomFloat() * kSpawnRadius;
    p.position = camPos + glm::vec3(rx, kSpawnHeight, rz);
    p.velocity = glm::vec3(0.0f, -kRainFallSpeed, 0.0f);
    p.life = kRainLife;
    p.maxLife = kRainLife;
    p.active = true;
}

void WeatherManager::RespawnSnow(Particle& p, const glm::vec3& camPos) {
    // Spawn at top of frustum with slight horizontal drift.
    const float rx = RandomFloat() * kSpawnRadius;
    const float rz = RandomFloat() * kSpawnRadius;
    p.position = camPos + glm::vec3(rx, kSpawnHeight, rz);
    // Slow fall with horizontal drift ±1 XZ.
    p.velocity = glm::vec3(RandomFloat() * 1.0f, -kSnowFallSpeed,
                           RandomFloat() * 1.0f);
    p.life = kSnowLife;
    p.maxLife = kSnowLife;
    p.active = true;
}

void WeatherManager::EmitSplash(const glm::vec3& pos) {
    // Emit 3-5 small splash particles radially.
    const int count = 3 + (int)((m_rng % 3));
    for (int i = 0; i < count; i++) {
        Particle* p = nullptr;
        for (auto& sp : m_splashParticles) {
            if (!sp.active) { p = &sp; break; }
        }
        if (!p) continue;

        const float angle = RandomFloat() * 3.14159f; // 0 to pi
        const float speed = 1.0f + RandomFloat() * 2.0f;
        p->position = pos + glm::vec3(0.0f, 0.1f, 0.0f);
        p->velocity = glm::vec3(std::cos(angle) * speed,
                                1.0f + RandomFloat(),
                                std::sin(angle) * speed);
        p->life = kSplashLife;
        p->maxLife = kSplashLife;
        p->active = true;
    }
}

void WeatherManager::CycleWeatherManual() {
    // Find current position in manual cycle.
    int cur = -1;
    for (int i = 0; i < kManualCycleLen; i++) {
        if (kManualCycle[i] == m_current) { cur = i; break; }
    }
    const int next = (cur + 1) % kManualCycleLen;
    const WeatherType newType = kManualCycle[next];

    Logger::Info("Weather: manual cycle -> %s", TypeName(newType));

    TransitionTo(newType);

    // Sync auto cycle index so the auto cycle resumes correctly.
    if (newType == WeatherType::FOG) {
        m_cycleIndex = -1; // FOG is not in auto cycle; will go to CLEAR (index 0).
    } else {
        // Find the auto cycle index for this type.
        for (int i = 0; i < kAutoCycleLen; i++) {
            if (kAutoCycle[i] == newType) { m_cycleIndex = i; break; }
        }
    }
}

uint32_t WeatherManager::GetSkyColor() const {
    // Base sky color: dim blue-gray (0x56, 0x4c, 0x40) = (86, 76, 64).
    float r = 0x56 / 255.0f;
    float g = 0x4c / 255.0f;
    float b = 0x40 / 255.0f;

    switch (m_current) {
        case WeatherType::CLEAR:
            break; // base color
        case WeatherType::RAIN:
            // Darken toward gray-blue.
            r = r * (1.0f - m_intensity) + 0.18f * m_intensity;
            g = g * (1.0f - m_intensity) + 0.22f * m_intensity;
            b = b * (1.0f - m_intensity) + 0.28f * m_intensity;
            break;
        case WeatherType::SNOW:
            // Lighten toward white-gray.
            r = r * (1.0f - m_intensity) + 0.75f * m_intensity;
            g = g * (1.0f - m_intensity) + 0.78f * m_intensity;
            b = b * (1.0f - m_intensity) + 0.82f * m_intensity;
            break;
        case WeatherType::FOG:
            // Lerp toward fog color (gray).
            const float lerp = m_intensity;
            r = r * (1.0f - lerp) + 0.55f * lerp;
            g = g * (1.0f - lerp) + 0.55f * lerp;
            b = b * (1.0f - lerp) + 0.55f * lerp;
            break;
    }

    r = std::clamp(r, 0.0f, 1.0f);
    g = std::clamp(g, 0.0f, 1.0f);
    b = std::clamp(b, 0.0f, 1.0f);

    // bgfx clear color format: 0xAABBGGRR
    return uint32_t(r * 255.0f)
         | (uint32_t(g * 255.0f) << 8)
         | (uint32_t(b * 255.0f) << 16);
}

void WeatherManager::Render(Renderer* renderer, const glm::mat4& view,
                            const glm::mat4& proj) {
    const glm::mat4 mvp = proj * view;
    const float* mvpPtr = glm::value_ptr(mvp);

    // Build rain vertices: drops (motion-blur streaks) + splashes (small quads).
    // Streaks are 0.05 x 2.0 u so they read as visible lines at typical
    // camera distances; a literal 0.02 x 0.5 u drop is sub-pixel wide and
    // effectively invisible from >10 u away.
    std::vector<float> rainVerts;
    rainVerts.reserve((kMaxRain + kMaxSplash) * 6 * 3);

    for (const Particle& p : m_rainDrops) {
        if (!p.active) continue;
        AddBillboardQuad(rainVerts, p.position, 0.05f, 2.0f, m_camRight, m_camUp);
    }
    for (const Particle& p : m_splashParticles) {
        if (!p.active) continue;
        // Small expanding quad: grows as it fades.
        const float t = 1.0f - (p.life / p.maxLife); // 0 = fresh, 1 = dying
        const float size = 0.05f + t * 0.1f;
        AddBillboardQuad(rainVerts, p.position, size, size, m_camRight, m_camUp);
    }

    // Track 34 debug: verify the particle pipeline runs (once per 10 s).
    static double s_lastDbg = -1e9;
    {
        const double now = double(SDL_GetTicks()) / 1000.0;
        if (now - s_lastDbg >= 10.0) {
            s_lastDbg = now;
            Logger::Info("Weather render: %zu rain verts, right=(%.2f,%.2f,%.2f)",
                         rainVerts.size() / 3,
                         m_camRight.x, m_camRight.y, m_camRight.z);
        }
    }

    if (!rainVerts.empty()) {
        renderer->DrawRain(rainVerts.data(), uint32_t(rainVerts.size() / 3),
                           mvpPtr, 0.65f);
    }

    // Build snow vertices: billboard quads (0.4 u — visible at distance).
    std::vector<float> snowVerts;
    snowVerts.reserve(kMaxSnow * 6 * 3);

    for (const Particle& p : m_snowFlakes) {
        if (!p.active) continue;
        AddBillboardQuad(snowVerts, p.position, 0.4f, 0.4f, m_camRight, m_camUp);
    }

    if (!snowVerts.empty()) {
        renderer->DrawSnow(snowVerts.data(), uint32_t(snowVerts.size() / 3),
                           mvpPtr, 0.8f);
    }
}

} // namespace OpenBully
