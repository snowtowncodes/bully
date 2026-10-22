#pragma once

// Engine: frame update + rendering orchestration (bgfx-backed, Phase 2).

#include "rendering/renderer.h"
#include "rendering/mesh.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace OpenBully {

class Window;

// One World.img NIF mesh loaded, uploaded to the GPU and tracked for the
// debug camera fit. Kept alive for the engine's lifetime.
struct WorldMesh {
    std::string name;      // NIF file name (e.g. "in_longdraw06.nif")
    GpuMesh gpu;           // GPU buffers
    float boundsMin[3] = {0, 0, 0};
    float boundsMax[3] = {0, 0, 0};
};

class Engine {
public:
    Engine() = default;
    ~Engine(); // out-of-line: unique_ptr to forward-declared Renderer

    bool Init(const Window& window);
    void Update();
    void Render();
    void Shutdown();

private:
    void LogFrameTiming();

    // Loads up to maxMeshes NIFs from `dir` into GPU meshes. Returns the
    // number loaded. Skips files that fail to parse, logs a summary.
    size_t LoadWorldGeometry(const char* dir, size_t maxMeshes);

    bool     m_initialized = false;
    double   m_time = 0.0;
    uint64_t m_lastPerf = 0;

    // Frame timing stats (rolling, logged once per second)
    uint32_t m_framesAccum = 0;
    double   m_frameMsAccum = 0.0;
    double   m_lastTimingLog = 0.0;

    // World geometry (World.img NIFs) + the Phase-2 reference triangle.
    std::vector<WorldMesh> m_worldMeshes;
    GpuMesh m_triangle;     // single buffer pair, drawn via the 2D path
    bool m_triangleValid = false;
    void* m_nativeWindowHandle = nullptr;
    uint64_t m_frameCount = 0;

    // Orbit camera state (Update integrates yaw/pitch; Render builds matrices)
    float m_camYaw = 0.0f;
    float m_camPitch = 0.0f;
    float m_camDistance = 50.0f;
    float m_sceneCenter[3] = {0, 0, 0};

    std::unique_ptr<Renderer> m_renderer;
};

} // namespace OpenBully
