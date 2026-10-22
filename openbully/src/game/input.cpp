#include "input.h"

#include "../platform/logger.h"

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
    SDL_SCANCODE_LSHIFT // Sprint
};

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

    // Escape -> quit. Also accept SDL_QUIT (window close) via the event pump:
    // main.cpp owns the event loop, but events may not have been pumped yet
    // this frame; drain quit/escape here defensively. Poll() must be called
    // after the main loop's SDL_PollEvent batch.
    m_quitRequested = keys[SDL_SCANCODE_ESCAPE] != 0;
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
