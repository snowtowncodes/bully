#pragma once

// Track 21: Audio — SDL2 audio device + procedural SFX + software mixer.
//
// Backend decision (see docs/track21_audio.md): Bully's real audio lives in
// XACT banks (.bin/.rsm/.SEA) which we do NOT decode yet. Track 21 instead
// feeds hand-generated PCM straight to the audio device through a small
// in-engine mixer built on the base SDL2 audio callback (SDL_audio.h).
// SDL2_mixer is deliberately NOT vendored: the procedural buffers + optional
// user .wav files need nothing it provides (its value is compressed-format
// decode, which only becomes relevant once XACT bank decoding lands).
//
// Mixing model: one SDL audio device (44.1 kHz / 16-bit / stereo requested),
// callback mixes up to kMaxVoices Voice entries additively with per-voice
// constant-power pan gains and a master volume. All sounds are pre-converted
// to the device format at load/generation time, so the mixer is format-free.
//
// 3D positioning (placeholder-grade, good enough for footsteps): distance
// attenuation is inverse-square clamped to 1 inside a reference distance;
// pan comes from the source's lateral offset along the listener's right
// vector (listener = camera position + yaw, updated per frame by the Engine).

#include <SDL2/SDL.h>
#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace OpenBully {

// Sound ids. The fixed set keeps the mixer simple; future tracks can extend
// the enum + kSoundNames + the generator switch together.
enum class SoundId : int {
    Footstep,    // short low-passed noise burst (procedural)
    UiClick,     // sine blip (procedural)
    AmbientWind, // loopable low-passed noise bed (procedural)
    RainLoop,    // loopable band-passed noise bed (procedural, Track 34)
    Count
};

// One decoded/generated PCM buffer, already in device format
// (AUDIO_S16SYS, interleaved, m_deviceChannels channels, m_deviceRate Hz).
struct Sound {
    std::vector<Sint16> samples;   // interleaved frames
    bool loopable = false;         // informational (loop state lives on Voice)

    uint32_t FrameCount(int channels) const {
        return channels > 0 ? uint32_t(samples.size() / channels) : 0;
    }
    bool valid() const { return !samples.empty(); }
};

class AudioManager {
public:
    AudioManager() = default;
    ~AudioManager(); // out-of-line: SDL device must be closed before members die

    // Opens the audio device (44.1 kHz, 16-bit, stereo requested; whatever
    // the driver grants is logged and used), generates the procedural sounds
    // at the device rate, and unpauses mixing. Non-fatal on failure: callers
    // check Initialized() and simply skip audio.
    bool Init();
    // Stops and closes the device. Safe to call twice.
    void Shutdown();
    bool Initialized() const { return m_device != 0; }

    // Listener state for 3D sounds (Engine calls once per frame).
    void SetListener(const glm::vec3& position, float yawRadians);

    // Plays a sound. volume in [0,1]; pos3d == nullptr plays non-spatialized
    // (full stereo, no attenuation). loop only intended for the ambient bed.
    void PlaySound(SoundId id, float volume, const glm::vec3* pos3d = nullptr,
                   bool loop = false);

    // Footstep at a world position, throttled to a ~0.35 s internal cooldown
    // so callers can fire it every frame while moving. Counts every accepted
    // call in FootstepCount() (verification: log line every 8th step).
    void PlayFootstep(const glm::vec3& worldPos);
    uint64_t FootstepCount() const { return m_footstepCount; }

    // Starts the looping ambient wind bed (idempotent). Called on level load.
    void StartAmbient();

    // Stops every active voice (including the ambient loop).
    void StopAll();

    // Track 34: stops every voice playing `id` (weather transitions). Safe
    // to call when no voice of that sound is active.
    void StopSound(SoundId id);

    // Master volume in [0,1] (default 0.8). Mute multiplies it to 0 at mix
    // time without touching the stored value, so unmute restores it.
    void SetMasterVolume(float volume);
    float GetMasterVolume() const { return m_masterVolume.load(); }
    void ToggleMute();   // debug key M
    bool Muted() const { return m_muted.load(); }

    // Extensibility path: overrides procedural sounds with user .wav files
    // found in `dir` (footstep.wav / click.wav / wind.wav, by name). Each
    // file is converted to the device format on load. Returns how many
    // loaded. Missing directory/files are not an error.
    int LoadUserWavs(const char* dir);

private:
    // SDL audio callback -> Mix(). Runs on the SDL audio thread.
    static void SDLCALL MixCallback(void* userdata, Uint8* stream, int len);
    void Mix(Sint16* out, int frameCount); // called under m_mutex from callback

    bool GenerateProceduralSounds();
    Sound GenerateFootstep() const;
    Sound GenerateClick() const;
    Sound GenerateWind() const;      // seamless loop (crossfaded head)
    Sound GenerateRain() const;       // seamless loop (crossfaded head, Track 34)
    bool LoadWavInto(SoundId id, const char* path); // SDL_LoadWAV + convert

    SDL_AudioDeviceID m_device = 0;
    SDL_AudioSpec     m_deviceSpec{};     // what the driver actually granted
    int m_deviceChannels = 2;
    int m_deviceRate = 44100;

    // Immutable after Init (callback reads them; main thread owns lifetime).
    std::array<Sound, size_t(SoundId::Count)> m_sounds;

    struct Voice {
        const Sound* sound = nullptr;
        uint32_t frame = 0;    // playhead in frames
        float gainL = 0.0f;    // constant-power gains, includes volume*atten
        float gainR = 0.0f;
        bool loop = false;
        bool active = true;    // false = remove at next mix (StopAll)
    };
    static constexpr size_t kMaxVoices = 32;
    std::vector<Voice> m_voices;   // preallocated; mutated under m_mutex
    std::mutex m_mutex;            // protects m_voices (callback vs main thread)

    // Listener state (main thread writes, PlaySound reads — same thread).
    glm::vec3 m_listenerPos{0.0f};
    float     m_listenerYaw = 0.0f;

    // Master gain + mute (atomics: read in the audio callback lock-free).
    std::atomic<float> m_masterVolume{0.8f};
    std::atomic<bool>  m_muted{false};

    // Footstep cadence + verification counter (main thread only).
    static constexpr double kFootstepCooldown = 0.35; // seconds
    double   m_lastFootstepTime = -1e9;
    uint64_t m_footstepCount = 0;
    bool     m_ambientPlaying = false;
};

} // namespace OpenBully
