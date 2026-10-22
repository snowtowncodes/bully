#pragma once

// Window: SDL2 window wrapper. Phase 2+: rendering is bgfx-based, so no
// GL context is created here — bgfx uses the native OS window handle.

#include <SDL2/SDL.h>
#include <string>

namespace OpenBully {

class Window {
public:
    Window(const std::string& title, int width, int height);
    ~Window();

    bool IsValid() const { return m_window != nullptr; }
    SDL_Window* GetHandle() const { return m_window; }

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    SDL_Window* m_window = nullptr;
    int m_width;
    int m_height;
};

} // namespace OpenBully
