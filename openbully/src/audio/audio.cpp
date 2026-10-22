// Track 21: Audio implementation — SDL2 device + procedural SFX + mixer.
// See audio.h for the backend decision and mixing model.

#include "audio/audio.h"

#include "../platform/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace OpenBully {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// One-pole low-pass (frame-domain): y += a * (x - y).
class LowPass {
public:
    // cutoffHz clamped to [20, sampleRate/2].
    void Setup(float sampleRate, float cutoffHz) {
        const float nyq = 0.5f * sampleRate;
        const float c = std::min(std::max(cutoffHz, 20.0f), nyq * 0.95f);
        const float w = std::tan(kPi * c / sampleRate);
        m_a = w / (1.0f + w); // equivalent-time one-pole coefficient
        m_y = 0.0f;
    }
    float Process(float x) {
        m_y += m_a * (x - m_y);
        return m_y;
    }
private:
    float m_a = 0.5f;
    float m_y = 0.0f;
};

// xorshift RNG: deterministic, fast, no <random> state bloat.
uint32_t NextRandom(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
float RandomFloat(uint32_t& state) { // [-1, 1]
    const uint32_t v = NextRandom(state);
    return float(int32_t(v)) / 2147483648.0f;
}

// Resample a Sound to the device rate with linear interpolation (used by the
// user-.wav path when SDL's converter is unavailable for a format pair).
void ResampleLinear(const Sound& src, int srcChannels, int srcRate,
                    int dstChannels, int dstRate, Sound& dst) {
    if (srcRate <= 0 || srcChannels <= 0 || dstRate <= 0 || dstChannels <= 0)
        return;
    const uint32_t srcFrames = uint32_t(src.samples.size()) / srcChannels;
    const uint32_t dstFrames = uint32_t(uint64_t(srcFrames) * dstRate / srcRate);
    dst.samples.resize(size_t(dstFrames) * dstChannels);
    for (uint32_t f = 0; f < dstFrames; ++f) {
        const double t = double(f) * srcRate / dstRate;
        const uint32_t i0 = uint32_t(t);
        const uint32_t i1 = std::min(i0 + 1, srcFrames - 1);
        const float frac = float(t - double(i0));
        for (int c = 0; c < dstChannels; ++c) {
            const float s0 = float(src.samples[size_t(i0) * srcChannels + std::min(c, srcChannels - 1)]);
            const float s1 = float(src.samples[size_t(i1) * srcChannels + std::min(c, srcChannels - 1)]);
            dst.samples[size_t(f) * dstChannels + c] = Sint16(std::lround(s0 + (s1 - s0) * frac));
        }
    }
}

const char* SoundFileName(SoundId id) {
    switch (id) {
        case SoundId::Footstep:    return "footstep.wav";
        case SoundId::UiClick:     return "click.wav";
        case SoundId::AmbientWind: return "wind.wav";
        default:                   return "";
    }
}

const char* SoundName(SoundId id) {
    switch (id) {
        case SoundId::Footstep:    return "footstep";
        case SoundId::UiClick:     return "ui-click";
        case SoundId::AmbientWind: return "ambient-wind";
        default:                   return "?";
    }
}

} // namespace

AudioManager::~AudioManager() {
    if (m_device)
        Shutdown();
}

bool AudioManager::Init() {
    if (m_device)
        return true; // already initialized

    // The Window module only inits SDL_INIT_VIDEO; bring up the audio
    // subsystem explicitly (SDL_OpenAudioDevice would auto-init too).
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        Logger::Error("Audio: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{};
    want.freq = 44100;          // task spec: 44.1 kHz 16-bit stereo
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;        // ~23 ms at 44.1 kHz
    want.callback = &AudioManager::MixCallback;
    want.userdata = this;

    SDL_AudioSpec have{};
    m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (m_device == 0) {
        Logger::Error("Audio: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return false;
    }
    if (have.format != AUDIO_S16SYS || (have.channels != 1 && have.channels != 2)) {
        Logger::Error("Audio: unsupported device format (fmt=0x%x ch=%u)",
                      unsigned(have.format), unsigned(have.channels));
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
        return false;
    }
    m_deviceSpec = have;
    m_deviceChannels = have.channels;
    m_deviceRate = have.freq;

    Logger::Info("Audio device: %d Hz, %d channel%s (SDL driver: %s, buffer %u frames)",
                 m_deviceRate, m_deviceChannels,
                 m_deviceChannels == 2 ? "s (stereo)" : "",
                 SDL_GetCurrentAudioDriver(), unsigned(have.samples));

    m_voices.reserve(kMaxVoices);
    if (!GenerateProceduralSounds()) {
        Logger::Error("Audio: procedural sound generation failed");
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
        return false;
    }

    SDL_PauseAudioDevice(m_device, 0); // start mixing
    return true;
}

void AudioManager::Shutdown() {
    if (m_device) {
        SDL_PauseAudioDevice(m_device, 1);
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_voices.clear();
        m_ambientPlaying = false;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    Logger::Info("Audio: device closed");
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

bool AudioManager::GenerateProceduralSounds() {
    m_sounds[size_t(SoundId::Footstep)]    = GenerateFootstep();
    m_sounds[size_t(SoundId::UiClick)]     = GenerateClick();
    m_sounds[size_t(SoundId::AmbientWind)] = GenerateWind();
    for (const Sound& s : m_sounds)
        if (!s.valid())
            return false;
    Logger::Info("Audio: procedural SFX ready (footstep %u frames, click %u, wind %u @ %d Hz)",
                 m_sounds[size_t(SoundId::Footstep)].FrameCount(m_deviceChannels),
                 m_sounds[size_t(SoundId::UiClick)].FrameCount(m_deviceChannels),
                 m_sounds[size_t(SoundId::AmbientWind)].FrameCount(m_deviceChannels),
                 m_deviceRate);
    return true;
}

// Footstep: short low-passed noise burst (the "scuff") + a soft low thump.
// Pure placeholder — real Bully footsteps are XACT bank entries we don't
// decode yet. ~90 ms, decays to silence well inside the 0.35 s cadence.
Sound AudioManager::GenerateFootstep() const {
    const float rate = float(m_deviceRate);
    const int frames = int(rate * 0.090f);
    Sound s;
    s.samples.assign(size_t(frames) * m_deviceChannels, 0);

    uint32_t rng = 0x1F123BB5u;
    LowPass scuffFilter;
    scuffFilter.Setup(rate, 1100.0f);
    LowPass thumpFilter;
    thumpFilter.Setup(rate, 160.0f);

    for (int f = 0; f < frames; ++f) {
        const float t = float(f) / rate;
        // Noise "scuff": fast exponential decay (~20 ms time constant).
        const float env = std::exp(-t * 110.0f);
        const float noise = scuffFilter.Process(RandomFloat(rng)) * env * 0.9f;
        // Low thump: 70 Hz sine decaying over ~60 ms, low-passed for softness.
        const float thump = std::sin(2.0f * kPi * 70.0f * t) *
                            std::exp(-t * 60.0f) * 0.55f;
        const float v = std::max(-1.0f, std::min(1.0f, noise + thumpFilter.Process(thump) * 2.2f));
        const int16_t q = int16_t(std::lround(v * 32767.0f * 0.55f));
        for (int c = 0; c < m_deviceChannels; ++c)
            s.samples[size_t(f) * m_deviceChannels + c] = q;
    }
    return s;
}

// UI click: a two-tone sine blip (C6 + octave), ~70 ms, exponential decay.
Sound AudioManager::GenerateClick() const {
    const float rate = float(m_deviceRate);
    const int frames = int(rate * 0.070f);
    Sound s;
    s.samples.assign(size_t(frames) * m_deviceChannels, 0);

    for (int f = 0; f < frames; ++f) {
        const float t = float(f) / rate;
        const float env = std::exp(-t * 70.0f);
        const float v = (std::sin(2.0f * kPi * 1046.5f * t) * 0.8f +
                         std::sin(2.0f * kPi * 2093.0f * t) * 0.2f) * env;
        const int16_t q = int16_t(std::lround(v * 32767.0f * 0.4f));
        for (int c = 0; c < m_deviceChannels; ++c)
            s.samples[size_t(f) * m_deviceChannels + c] = q;
    }
    return s;
}

// Ambient wind: heavily low-passed noise with a slow baked-in swell. The LFO
// runs an integer number of cycles over the loop length, and the head is
// crossfaded against the post-loop tail, so the loop point is inaudible.
Sound AudioManager::GenerateWind() const {
    const float rate = float(m_deviceRate);
    const float loopSec = 6.0f;
    const float fadeSec = 0.5f;
    const int loopFrames = int(rate * loopSec);
    const int fadeFrames = int(rate * fadeSec);

    // Raw (uncrossfaded) signal runs loopLen + fade long.
    std::vector<float> raw(size_t(loopFrames + fadeFrames));
    uint32_t rng = 0x9E3779B9u;         // base wind noise state
    uint32_t gustRng = 0xDEADBEEFu;     // independent gust noise state
    LowPass windFilter, gustFilter;
    windFilter.Setup(rate, 350.0f);
    gustFilter.Setup(rate, 900.0f);
    for (size_t i = 0; i < raw.size(); ++i) {
        const float t = float(i) / rate;
        // Envelope: integer cycles over the loop so endpoints match
        // (0.5 Hz * 6 s = 3 cycles), plus a fixed base level.
        const float lfo = 0.75f + 0.25f * std::sin(2.0f * kPi * 0.5f * t);
        const float base = windFilter.Process(RandomFloat(rng));
        const float gust = gustFilter.Process(RandomFloat(gustRng));
        raw[i] = (base * 1.6f + gust * 0.25f) * lfo;
    }

    // Crossfade the head into the tail: out[i] = raw[loop+i] blends down to
    // raw[i], so the wrap raw[loop-1] -> out[0] is continuous in the raw
    // signal and the seam is masked.
    Sound s;
    s.samples.assign(size_t(loopFrames) * m_deviceChannels, 0);
    for (int f = 0; f < loopFrames; ++f) {
        float v;
        if (f < fadeFrames) {
            const float w = float(f) / float(fadeFrames); // 0 -> 1 across head
            v = raw[size_t(loopFrames + f)] * (1.0f - w) + raw[size_t(f)] * w;
        } else {
            v = raw[size_t(f)];
        }
        const int16_t q = int16_t(std::lround(std::max(-1.0f, std::min(1.0f, v)) *
                                              32767.0f * 0.35f));
        for (int c = 0; c < m_deviceChannels; ++c)
            s.samples[size_t(f) * m_deviceChannels + c] = q;
    }
    return s;
}

// ---------------------------------------------------------------------------
// User .wav overrides (extensibility path)
// ---------------------------------------------------------------------------

int AudioManager::LoadUserWavs(const char* dir) {
    if (!dir || !*dir || !m_device)
        return 0;
    int loaded = 0;
    for (int i = 0; i < int(SoundId::Count); ++i) {
        const SoundId id = SoundId(i);
        std::string path = std::string(dir) + "/" + SoundFileName(id);
        if (LoadWavInto(id, path.c_str()))
            ++loaded;
    }
    if (loaded > 0)
        Logger::Info("Audio: %d user .wav override(s) loaded from %s", loaded, dir);
    return loaded;
}

bool AudioManager::LoadWavInto(SoundId id, const char* path) {
    SDL_AudioSpec src{};
    Uint8* raw = nullptr;
    Uint32 rawLen = 0;
    if (!SDL_LoadWAV(path, &src, &raw, &rawLen)) {
        SDL_ClearError(); // absence is normal
        return false;
    }

    // Convert to device format (S16SYS, device channels, device rate).
    SDL_AudioCVT cvt;
    const SDL_AudioFormat dstFormat = AUDIO_S16SYS;
    const bool needsCvt = src.format != dstFormat || src.channels != m_deviceChannels ||
                          src.freq != m_deviceRate;
    bool ok = false;
    if (!needsCvt) {
        Sound& dst = m_sounds[size_t(id)];
        dst.samples.assign(reinterpret_cast<const Sint16*>(raw),
                           reinterpret_cast<const Sint16*>(raw + rawLen));
        ok = dst.valid();
    } else if (SDL_BuildAudioCVT(&cvt, src.format, Uint8(src.channels), src.freq,
                                 dstFormat, Uint8(m_deviceChannels), m_deviceRate) >= 0) {
        Uint8* conv = static_cast<Uint8*>(SDL_malloc(size_t(rawLen) * size_t(cvt.len_mult)));
        if (conv) {
            std::memcpy(conv, raw, rawLen);
            cvt.buf = conv;
            cvt.len = int(rawLen);
            if (SDL_ConvertAudio(&cvt) == 0) {
                Sound& dst = m_sounds[size_t(id)];
                dst.samples.assign(reinterpret_cast<const Sint16*>(conv),
                                   reinterpret_cast<const Sint16*>(conv + cvt.len_cvt));
                ok = dst.valid();
            }
            SDL_free(conv);
        }
    }
    if (!ok)
        Logger::Warning("Audio: failed to convert %s (fmt=0x%x ch=%d rate=%d)",
                        path, unsigned(src.format), int(src.channels), int(src.freq));
    else
        Logger::Info("Audio: override '%s' <- %s (%u frames @ %d Hz)",
                     SoundName(id), path,
                     m_sounds[size_t(id)].FrameCount(m_deviceChannels), m_deviceRate);
    SDL_FreeWAV(raw);
    return ok;
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

void AudioManager::SetListener(const glm::vec3& position, float yawRadians) {
    m_listenerPos = position;
    m_listenerYaw = yawRadians;
}

void AudioManager::PlaySound(SoundId id, float volume,
                             const glm::vec3* pos3d, bool loop) {
    if (!m_device)
        return;
    const Sound& sound = m_sounds[size_t(id)];
    if (!sound.valid())
        return;

    volume = std::max(0.0f, std::min(1.0f, volume));
    float atten = 1.0f; // distance attenuation (inverse-square, clamped)
    float pan = 0.0f;   // -1 = hard left, +1 = hard right

    if (pos3d) {
        const glm::vec3 rel = *pos3d - m_listenerPos;
        const float dist = glm::length(rel);
        constexpr float kRefDist = 4.0f;  // full volume inside 4 units
        constexpr float kPanDist = 8.0f;  // full pan by 8 units lateral
        atten = (kRefDist * kRefDist) / (kRefDist * kRefDist + dist * dist);
        // Listener right vector for the camera-yaw convention (+Z forward):
        // right = (cos yaw, 0, -sin yaw) — matches Camera strafe math.
        const float cy = std::cos(m_listenerYaw), sy = std::sin(m_listenerYaw);
        const float relX = rel.x * cy + rel.z * (-sy);
        pan = std::max(-1.0f, std::min(1.0f, relX / kPanDist));
    }

    // Constant-power pan keeps perceived loudness stable across the field.
    Voice v{};
    v.sound = &sound;
    v.frame = 0;
    v.gainL = volume * atten * std::cos((pan + 1.0f) * kPi * 0.25f);
    v.gainR = volume * atten * std::sin((pan + 1.0f) * kPi * 0.25f);
    v.loop = loop;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_voices.size() >= kMaxVoices) {
        // Steal the oldest finished-or-non-looping voice; drop if all loop.
        for (Voice& old : m_voices) {
            if (!old.loop) {
                old = v;
                return;
            }
        }
        return; // voices exhausted by loops — drop the new one
    }
    m_voices.push_back(v);
}

void AudioManager::PlayFootstep(const glm::vec3& worldPos) {
    if (!m_device)
        return;
    const double now = double(SDL_GetTicks()) / 1000.0;
    if (now - m_lastFootstepTime < kFootstepCooldown)
        return;
    m_lastFootstepTime = now;
    ++m_footstepCount;
    // Verification counter: log the first step, then every 8th.
    if (m_footstepCount == 1 || m_footstepCount % 8 == 0)
        Logger::Info("Audio: footstep #%llu @ (%.1f, %.1f, %.1f)",
                     static_cast<unsigned long long>(m_footstepCount),
                     worldPos.x, worldPos.y, worldPos.z);
    PlaySound(SoundId::Footstep, 0.8f, &worldPos);
}

void AudioManager::StartAmbient() {
    if (!m_device || m_ambientPlaying)
        return;
    m_ambientPlaying = true;
    PlaySound(SoundId::AmbientWind, 0.6f, nullptr, /*loop=*/true);
    Logger::Info("Audio: ambient wind loop started (%u-frame seamless loop)",
                 m_sounds[size_t(SoundId::AmbientWind)].FrameCount(m_deviceChannels));
}

void AudioManager::StopAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_voices.clear();
    m_ambientPlaying = false;
}

void AudioManager::SetMasterVolume(float volume) {
    m_masterVolume.store(std::max(0.0f, std::min(1.0f, volume)));
}

void AudioManager::ToggleMute() {
    const bool was = m_muted.load();
    m_muted.store(!was);
    Logger::Info("Audio: %s", was ? "unmuted" : "muted (master volume 0)");
}

// ---------------------------------------------------------------------------
// Mixer (runs on the SDL audio thread)
// ---------------------------------------------------------------------------

void SDLCALL AudioManager::MixCallback(void* userdata, Uint8* stream, int len) {
    AudioManager* self = static_cast<AudioManager*>(userdata);
    // Zero first: mixing is additive and SDL gives us an uninitialized buffer.
    std::memset(stream, 0, size_t(len));
    const int frameCount = len / (int(sizeof(Sint16)) * self->m_deviceChannels);
    std::lock_guard<std::mutex> lock(self->m_mutex);
    self->Mix(reinterpret_cast<Sint16*>(stream), frameCount);
}

void AudioManager::Mix(Sint16* out, int frameCount) {
    if (frameCount <= 0 || m_voices.empty())
        return;

    const float master = m_muted.load() ? 0.0f : m_masterVolume.load();
    const int ch = m_deviceChannels;
    bool anyActive = false;

    for (Voice& v : m_voices) {
        if (!v.active) {
            anyActive = true; // pending removal below
            continue;
        }
        const uint32_t total = v.sound->FrameCount(ch);
        if (total == 0) {
            v.active = false;
            anyActive = true;
            continue;
        }
        const int maxFrames = int(total - v.frame);
        const int n = v.loop ? frameCount : std::min(frameCount, maxFrames);
        if (!v.loop && maxFrames <= 0) {
            v.active = false;
            anyActive = true;
            continue;
        }

        const Sint16* data = v.sound->samples.data();
        // Mono device: collapse the stereo gains to mono.
        const float gainM = 0.5f * (v.gainL + v.gainR);

        for (int f = 0; f < n; ++f) {
            const uint32_t src = (v.frame + uint32_t(f)) % total;
            if (ch == 2) {
                const float l = float(data[size_t(src) * 2 + 0]) * v.gainL * master;
                const float r = float(data[size_t(src) * 2 + 1]) * v.gainR * master;
                out[size_t(f) * 2 + 0] = Sint16(std::clamp(
                    int(out[size_t(f) * 2 + 0]) + int(std::lround(l)), -32768, 32767));
                out[size_t(f) * 2 + 1] = Sint16(std::clamp(
                    int(out[size_t(f) * 2 + 1]) + int(std::lround(r)), -32768, 32767));
            } else {
                const float m = float(data[size_t(src)]) * gainM * master;
                out[f] = Sint16(std::clamp(int(out[f]) + int(std::lround(m)), -32768, 32767));
            }
        }

        if (v.loop) {
            v.frame = (v.frame + uint32_t(n)) % total;
        } else {
            v.frame += uint32_t(n);
            if (v.frame >= total) {
                v.active = false;
                anyActive = true;
            }
        }
    }

    if (anyActive) // compact finished voices
        m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(),
                                      [](const Voice& v) { return !v.active; }),
                       m_voices.end());
}

} // namespace OpenBully
