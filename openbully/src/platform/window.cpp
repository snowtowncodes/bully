#include "window.h"
#include "logger.h"

namespace OpenBully {

Window::Window(const std::string& title, int width, int height)
    : m_width(width), m_height(height) {

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        Logger::Error("SDL_Init failed: %s", SDL_GetError());
        return;
    }

    // Plain window: bgfx presents via the native handle (HWND on Win32).
    // VSync is requested through bgfx (BGFX_RESET_VSYNC), not SDL_GL.
    m_window = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN
    );

    if (!m_window) {
        Logger::Error("SDL_CreateWindow failed: %s", SDL_GetError());
        return;
    }

    Logger::Info("Window created: %dx%d", width, height);
}

Window::~Window() {
    if (m_window) {
        SDL_DestroyWindow(m_window);
    }
    SDL_Quit();
}

} // namespace OpenBully
