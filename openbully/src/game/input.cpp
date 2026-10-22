#include "input.h"

#include "../platform/logger.h"

#include <cstring>

namespace OpenBully {

namespace {

// Scancode -> logical button table (row order matches Button enum).
const SDL_Scancode kButtonScancodes[int(Button::Count)] = {
    SDL_SCANCODE_W,     // Forward
    SDL_SCANCODE_S,     // Back
    SDL_SCANCODE_A,     // Left
    SDL_SCANCODE_D,     // Right
    SDL_SCANCODE_SPACE, // Up
    SDL_SCANCODE_LCTRL, // Down
    SDL_SCANCODE_LSHIFT, // Sprint
    SDL_SCANCODE_N,     // Mute (Track 37: M reassigned to minimap toggle)
    SDL_SCANCODE_UNKNOWN, // Attack  (mouse)
    SDL_SCANCODE_UNKNOWN // Block   (mouse)
};

// Track 25: mouse-button bit for the mouse-mapped tail of the Button enum
// (Attack, Block). Index 0 = Button::Attack.
const Uint32 kMouseButtonMask[] = { SDL_BUTTON_LMASK, SDL_BUTTON_RMASK };

} // namespace

Input::Input() {
    // Relative mouse mode keeps the cursor inside the window and gives us
    // raw per-frame deltas. Ignored failure is non-fatal (headless/test envs).
    if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0)
        Logger::Warning("SDL_SetRelativeMouseMode failed: %s", SDL_GetError());
}

void Input::Poll() {
    // --- Keyboard -----------------------------------------------------------
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    for (int i = 0; i < int(Button::Count); ++i) {
        m_lastDown[i] = m_down[i];
        m_down[i]     = keys[kButtonScancodes[i]] != 0;
    }

    // Track 20: raw snapshot for WasKeyPressed (debug keys). memcpy keeps
    // the previous frame's state for the edge comparison.
    std::memcpy(m_lastRawDown, m_rawDown, sizeof(m_rawDown));
    for (int i = 0; i < SDL_NUM_SCANCODES; ++i)
        m_rawDown[i] = keys[i] != 0;

    // Track 25: mouse buttons polled after the keyboard loop (the two tail
    // entries of kButtonScancodes are 0 = not keyboard-mapped).
    const Uint32 mouseButtons = SDL_GetMouseState(nullptr, nullptr);
    for (int i = int(Button::Attack); i < int(Button::Count); ++i)
        m_down[i] = (mouseButtons & kMouseButtonMask[i - int(Button::Attack)]) != 0;

    // Escape -> quit. Also accept SDL_QUIT (window close) via the event pump:
    // main.cpp owns the event loop, but events may not have been pumped yet
    // this frame; drain quit/escape here defensively. Poll() must be called
    // after the main loop's SDL_PollEvent batch.
    // Escape -> quit. Edge-detected (fires once per press) so an in-game menu
    // can consume the press and call Input::ClearQuit() instead of quitting;
    // holding ESC does not re-latch the request on later frames.
    if (m_rawDown[SDL_SCANCODE_ESCAPE] && !m_lastRawDown[SDL_SCANCODE_ESCAPE])
        m_quitRequested = true;
    SDL_Event ev;
    while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT,
                          SDL_QUIT, SDL_QUIT) > 0) {
        m_quitRequested = true;
    }

    // --- Mouse --------------------------------------------------------------
    m_mouseDX = 0;
    m_mouseDY = 0;
    int dx = 0, dy = 0;
    SDL_GetRelativeMouseState(&dx, &dy);
    m_mouseDX = dx;
    m_mouseDY = dy;
}

} // namespace OpenBully
