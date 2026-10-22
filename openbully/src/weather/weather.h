#pragma once

// Track 34: Weather system — states, particle effects, fog, audio integration.
//
// WeatherManager is a singleton that the Engine updates each frame. It manages
// the weather state machine (CLEAR -> RAIN -> CLEAR -> SNOW -> CLEAR auto cycle),
// particle systems (rain drops, snowflakes, splashes), exponential fog, and
// weather-related audio. Pressing W cycles through weather manually.

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

namespace OpenBully {

class Renderer;

enum class WeatherType : int {
    CLEAR = 0,
    RAIN,
    SNOW,
    FOG,
    Count
};

// One particle in the weather system.
struct Particle {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float life = 0.0f;
    float maxLife = 0.0f;
    bool active = false;
};

class WeatherManager {
public:
    static WeatherManager& Get();

    void Init();
    void Update(float dt, const glm::vec3& camPos, const glm::mat4& view);
    void Render(Renderer* renderer, const glm::mat4& view, const glm::mat4& proj);

    void CycleWeatherManual();

    // Phase 10 integration: set the weather state directly (save/load restore).
    // Delegates to TransitionTo so fog/audio timers behave like a normal change.
    void SetState(WeatherType type) { TransitionTo(type); }

    WeatherType GetType() const { return m_current; }
    float GetIntensity() const { return m_intensity; }
    float GetFogDensity() const { return m_fogDensity; }
    glm::vec3 GetFogColor() const { return m_fogColor; }
    uint32_t GetSkyColor() const;

    static const char* TypeName(WeatherType t);

private:
    WeatherManager() = default;

    void TransitionTo(WeatherType type);
    void UpdateRain(float dt, const glm::vec3& camPos);
    void UpdateSnow(float dt, const glm::vec3& camPos);
    void UpdateSplash(float dt);
    void RespawnRain(Particle& p, const glm::vec3& camPos);
    void RespawnSnow(Particle& p, const glm::vec3& camPos);
    void EmitSplash(const glm::vec3& pos);

    float RandomFloat() {
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 17;
        m_rng ^= m_rng << 5;
        return float(int32_t(m_rng)) / 2147483648.0f;
    }

    // Weather state
    WeatherType m_current = WeatherType::CLEAR;
    float m_timer = 0.0f;
    float m_intensity = 0.0f;
    int m_cycleIndex = 0;

    // Fog
    float m_fogDensity = 0.0f;
    glm::vec3 m_fogColor{0.55f, 0.55f, 0.55f};

    // Camera state (for billboarding)
    glm::vec3 m_camRight{1.0f, 0.0f, 0.0f};
    glm::vec3 m_camUp{0.0f, 1.0f, 0.0f};

    // Particles
    std::vector<Particle> m_rainDrops;
    std::vector<Particle> m_snowFlakes;
    std::vector<Particle> m_splashParticles;

    // Config
    static constexpr uint32_t kMaxRain = 500;
    static constexpr uint32_t kMaxSnow = 300;
    static constexpr uint32_t kMaxSplash = 250;
    static constexpr float kRainFallSpeed = 15.0f;
    static constexpr float kSnowFallSpeed = 3.0f;
    static constexpr float kSpawnHeight = 20.0f;
    static constexpr float kSpawnRadius = 30.0f;
    static constexpr float kRainLife = 4.0f;
    static constexpr float kSnowLife = 12.0f;
    static constexpr float kSplashLife = 0.4f;
    static constexpr float kFogMaxDensity = 0.05f;

    // Auto cycle: CLEAR 60s -> RAIN 45s -> CLEAR 60s -> SNOW 30s -> repeat
    static constexpr WeatherType kAutoCycle[] = {
        WeatherType::CLEAR, WeatherType::RAIN, WeatherType::CLEAR, WeatherType::SNOW
    };
    static constexpr float kAutoDurations[] = { 60.0f, 45.0f, 60.0f, 30.0f };
    static constexpr int kAutoCycleLen = 4;

    // Manual cycle order (W key): CLEAR -> RAIN -> SNOW -> FOG -> CLEAR
    static constexpr WeatherType kManualCycle[] = {
        WeatherType::CLEAR, WeatherType::RAIN, WeatherType::SNOW, WeatherType::FOG
    };
    static constexpr int kManualCycleLen = 4;

    uint32_t m_rng = 0x12345678u;
};

} // namespace OpenBully
