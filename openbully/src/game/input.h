#pragma once

// Input: per-frame keyboard + mouse state for FPS-style camera control.
//
// Usage (once per frame, from Engine::Update):
//   Input input;
//   input.Poll();          // snapshot keyboard + mouse deltas
//   if (input.quitRequested) ...
//   camera.Update(deltaTime, input);
//
// Buttons are mapped to a game-centric enum so callers never touch raw
// SDL scancodes.

#include <SDL2/SDL.h>

namespace OpenBully {

// Logical buttons bound in Poll(). Extend as gameplay lands.
enum class Button : int {
    Forward,      // W
    Back,         // S
    Left,         // A
    Right,        // D
    Up,           // Space (fly up / jump later)
    Down,         // LCtrl (fly down)
    Sprint,       // LShift (speed multiplier / dodge modifier, Track 25)
    Mute,         // M (Track 21: toggle audio mute)
    Attack,       // Track 25: LMB (punch; stamina 10)
    Block,        // Track 25: RMB (hold to block; 50% damage reduction)
    Count
};

class Input {
public:
    Input();

    // Polls the current keyboard/mouse snapshot. Call exactly once per frame.
    void Poll();

    // True only on the frame Escape transitions to pressed.
    bool QuitRequested() const { return m_quitRequested; }

    // Button held down this frame?
    bool IsDown(Button b) const { return m_down[int(b)]; }
    // Button pressed this frame (was up last frame)?
    bool IsPressed(Button b) const {
        return m_down[int(b)] && !m_lastDown[int(b)];
    }

    // Mouse movement since last Poll(), in pixels (relative mode).
    int MouseDX() const { return m_mouseDX; }
    int MouseDY() const { return m_mouseDY; }

    // Test/demo hooks: inject button state or mouse delta after Poll()
    // (OPENBULLY_CAM_DEMO flythrough; keeps Camera::Update the only consumer).
    void SetButton(Button b, bool down) { m_down[int(b)] = down; }
    void SetMouseDelta(int dx, int dy)  { m_mouseDX = dx; m_mouseDY = dy; }

    // Track 20: raw-scancode edge detection for debug keys (H/J/K HUD
    // testing) that are not worth a logical Button each. True only on the
    // frame the key transitions up -> down.
    bool WasKeyPressed(SDL_Scancode sc) const {
        return sc > SDL_SCANCODE_UNKNOWN && sc < SDL_NUM_SCANCODES
            && m_rawDown[sc] && !m_lastRawDown[sc];
    }

private:
    bool m_down[int(Button::Count)]     = {};
    bool m_lastDown[int(Button::Count)] = {};
    bool m_quitRequested                = false;
    int  m_mouseDX                      = 0;
    int  m_mouseDY                      = 0;

    // Track 20: full keyboard snapshot + previous frame's copy (edge state).
    bool m_rawDown[SDL_NUM_SCANCODES]     = {};
    bool m_lastRawDown[SDL_NUM_SCANCODES] = {};
};

} // namespace OpenBully
