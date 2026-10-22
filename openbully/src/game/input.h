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
    Sprint,       // LShift (speed multiplier)
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

private:
    bool m_down[int(Button::Count)]     = {};
    bool m_lastDown[int(Button::Count)] = {};
    bool m_quitRequested                = false;
    int  m_mouseDX                      = 0;
    int  m_mouseDY                      = 0;
};

} // namespace OpenBully
