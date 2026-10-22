// OpenBully: Main Entry Point
// Phase 2 - bgfx-backed bootstrap: window + engine loop

#include "platform/window.h"
#include "platform/logger.h"
#include "platform/crash_report.h"
#include "engine/engine.h"
#include "rendering/renderer.h" // complete Renderer type for Engine's dtor

#include <string>

int main(int argc, char* argv[]) {
    using namespace OpenBully;

    Logger::Init();
    InstallCrashReporter(); // Track 49: log module+offset on unhandled faults
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: logs visible when piped
    Logger::Info("OpenBully v0.1.0 - Bootstrap Phase 2 (bgfx)");

    // Parse command-line arguments
    bool testMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--test-triangle") {
            testMode = true;
        }
    }
    (void)testMode; // Phase 2: triangle always renders; flag reserved for later tracks
    if (testMode) {
        Logger::Info("Test triangle mode enabled");
    }

    // Create window
    Window window("OpenBully", 1280, 720);
    if (!window.IsValid()) {
        Logger::Error("Failed to create window");
        return 1;
    }

    // Initialize engine (owns bgfx renderer)
    Engine engine;
    if (!engine.Init(window)) {
        Logger::Error("Failed to initialize engine");
        return 1;
    }

    // Main loop
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            // Track 40: the quest journal needs the wheel delta, and SDL
            // PollEvent drains MOUSEWHEEL before Input::Poll can see it.
            if (event.type == SDL_MOUSEWHEEL)
                engine.OnMouseWheel(event.wheel.y);
            // Track 40: ESC is not handled here - the engine consumes it to
            // close the quest journal, and only surfaces QuitRequested() when
            // no menu ate the press. Window close (SDL_QUIT) still exits.
        }

        engine.Update();
        if (engine.QuitRequested()) {
            running = false;
        }
        engine.Render();
        // Present handled by bgfx inside engine.Render() (Renderer::Frame).
    }

    // Cleanup
    engine.Shutdown();
    Logger::Info("OpenBully shutdown complete");

    return 0;
}

// NOTE: a temporary SEH crash reporter was prototyped here for the Track 49
// verification (printing module+offset for the int3 crash); removed again —
// the root cause was found before it was needed (see the Jimmy void guard in
// Engine::UpdateJimmy and the deferred boss spawns).
