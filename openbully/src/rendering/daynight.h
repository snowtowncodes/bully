#pragma once

// Track 35: Day/Night cycle computation.
// Computes sky color, sun direction, light color, and ambient from game hour.
// Header-only: pure math with no state, called once per frame by Engine::Render.

#include <cmath>
#include <cstddef>

namespace OpenBully {
namespace DayNight {

struct State {
    float sky[3];        // sky clear color (RGB 0-1)
    float lightDir[3];   // to-light direction (not normalized)
    float lightColor[3]; // light tint * intensity (RGB 0-1)
    float ambient;       // ambient level (scalar, applied as gray)

    State() {
        sky[0] = sky[1] = sky[2] = 0.0f;
        lightDir[0] = lightDir[1] = lightDir[2] = 0.0f;
        lightColor[0] = lightColor[1] = lightColor[2] = 0.0f;
        ambient = 0.0f;
    }
};

// --- Interpolation helpers ---------------------------------------------------

inline float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

struct KeyScalar { float hour; float value; };
struct KeyRGB     { float hour; float r, g, b; };

inline float InterpScalar(const KeyScalar* keys, size_t count, float hour) {
    if (count == 0) return 0.0f;
    if (hour <= keys[0].hour) return keys[0].value;
    if (hour >= keys[count - 1].hour) return keys[count - 1].value;
    for (size_t i = 0; i < count - 1; ++i) {
        if (hour >= keys[i].hour && hour < keys[i + 1].hour) {
            const float t = (hour - keys[i].hour) / (keys[i + 1].hour - keys[i].hour);
            return Lerp(keys[i].value, keys[i + 1].value, t);
        }
    }
    return keys[count - 1].value;
}

inline void InterpRGB(const KeyRGB* keys, size_t count, float hour, float out[3]) {
    if (count == 0) { out[0] = out[1] = out[2] = 0.0f; return; }
    if (hour <= keys[0].hour) {
        out[0] = keys[0].r; out[1] = keys[0].g; out[2] = keys[0].b; return;
    }
    if (hour >= keys[count - 1].hour) {
        out[0] = keys[count-1].r; out[1] = keys[count-1].g; out[2] = keys[count-1].b; return;
    }
    for (size_t i = 0; i < count - 1; ++i) {
        if (hour >= keys[i].hour && hour < keys[i + 1].hour) {
            const float t = (hour - keys[i].hour) / (keys[i + 1].hour - keys[i].hour);
            out[0] = Lerp(keys[i].r, keys[i + 1].r, t);
            out[1] = Lerp(keys[i].g, keys[i + 1].g, t);
            out[2] = Lerp(keys[i].b, keys[i + 1].b, t);
            return;
        }
    }
    out[0] = keys[count-1].r; out[1] = keys[count-1].g; out[2] = keys[count-1].b;
}

// --- Keyframes ---------------------------------------------------------------
// All keyframe arrays are ordered by hour and include a final key at 24.0
// matching the first key at 0.0 so the day wraps smoothly.

inline State Compute(float hour) {
    State s;

    // Normalize to [0, 24)
    while (hour >= 24.0f) hour -= 24.0f;
    if (hour < 0.0f) hour += 24.0f;

    // --- Sky color: black night -> orange dawn -> blue day -> orange dusk ---
    static const KeyRGB kSky[] = {
        {  0.0f, 0.02f, 0.03f, 0.06f },  // midnight  - dark blue-black
        {  4.5f, 0.02f, 0.03f, 0.06f },  // pre-dawn  - still dark
        {  5.5f, 0.18f, 0.10f, 0.16f },  // early dawn - purple haze
        {  6.0f, 0.70f, 0.40f, 0.25f },  // sunrise   - orange
        {  7.0f, 0.40f, 0.60f, 0.85f },  // morning   - light blue
        { 12.0f, 0.35f, 0.55f, 0.90f },  // noon      - bright blue
        { 16.0f, 0.40f, 0.55f, 0.80f },  // afternoon - slightly warm
        { 17.5f, 0.60f, 0.40f, 0.25f },  // late      - golden hour
        { 18.0f, 0.75f, 0.35f, 0.20f },  // sunset    - orange-red
        { 19.0f, 0.18f, 0.10f, 0.16f },  // dusk      - purple
        { 20.0f, 0.02f, 0.03f, 0.06f },  // night     - dark
        { 24.0f, 0.02f, 0.03f, 0.06f },  // midnight  - wrap
    };
    InterpRGB(kSky, sizeof(kSky) / sizeof(kSky[0]), hour, s.sky);

    // --- Light color (before intensity multiplication) ---
    static const KeyRGB kLightColor[] = {
        {  0.0f, 0.30f, 0.40f, 0.70f },  // night    - dim blue
        {  5.0f, 0.40f, 0.40f, 0.60f },  // pre-dawn
        {  6.0f, 1.00f, 0.60f, 0.30f },  // dawn     - orange
        {  7.0f, 1.00f, 0.90f, 0.70f },  // morning  - warm yellow
        { 12.0f, 1.00f, 0.95f, 0.85f },  // noon     - warm white
        { 17.0f, 1.00f, 0.85f, 0.60f },  // afternoon- warm
        { 18.0f, 1.00f, 0.50f, 0.20f },  // dusk     - orange
        { 19.0f, 0.50f, 0.40f, 0.60f },  // twilight - purple
        { 20.0f, 0.30f, 0.40f, 0.70f },  // night    - dim blue
        { 24.0f, 0.30f, 0.40f, 0.70f },  // midnight - wrap
    };

    // --- Light intensity ---
    static const KeyScalar kIntensity[] = {
        {  0.0f, 0.20f },  // night
        {  5.0f, 0.20f },  // pre-dawn
        {  6.0f, 0.50f },  // dawn
        {  7.0f, 0.90f },  // morning
        { 12.0f, 1.00f },  // noon
        { 17.0f, 0.90f },  // afternoon
        { 18.0f, 0.50f },  // dusk
        { 19.0f, 0.25f },  // twilight
        { 20.0f, 0.20f },  // night
        { 24.0f, 0.20f },  // midnight - wrap
    };

    // --- Ambient ---
    static const KeyScalar kAmbient[] = {
        {  0.0f, 0.10f },  // night
        {  5.0f, 0.10f },  // pre-dawn
        {  6.0f, 0.20f },  // dawn
        {  7.0f, 0.35f },  // morning
        { 12.0f, 0.40f },  // noon
        { 17.0f, 0.35f },  // afternoon
        { 18.0f, 0.20f },  // dusk
        { 19.0f, 0.12f },  // twilight
        { 20.0f, 0.10f },  // night
        { 24.0f, 0.10f },  // midnight - wrap
    };

    // Light color * intensity
    float lcRaw[3];
    InterpRGB(kLightColor, sizeof(kLightColor) / sizeof(kLightColor[0]), hour, lcRaw);
    const float intensity = InterpScalar(kIntensity, sizeof(kIntensity) / sizeof(kIntensity[0]), hour);
    s.lightColor[0] = lcRaw[0] * intensity;
    s.lightColor[1] = lcRaw[1] * intensity;
    s.lightColor[2] = lcRaw[2] * intensity;

    // Ambient
    s.ambient = InterpScalar(kAmbient, sizeof(kAmbient) / sizeof(kAmbient[0]), hour);

    // Sun direction: (hour - 6) / 24 * 2π
    //   6am:  angle=0     -> cos=1, sin=0   (eastern horizon)
    //  12pm:  angle=π/2   -> cos=0, sin=1   (overhead)
    //   6pm:  angle=π     -> cos=-1,sin=0   (western horizon)
    //   0am:  angle=-π/2  -> cos=0, sin=-1  (below)
    const float angle = (hour - 6.0f) / 24.0f * 2.0f * 3.14159265358979f;
    s.lightDir[0] = std::cos(angle);  // east-west
    s.lightDir[1] = std::sin(angle);  // up-down
    s.lightDir[2] = 0.3f;             // constant north-south tilt

    return s;
}

}  // namespace DayNight
}  // namespace OpenBully
