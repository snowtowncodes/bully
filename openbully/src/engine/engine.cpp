#include "engine.h"
#include "formats/nif_loader.h"
#include "../platform/logger.h"
#include "../platform/window.h"
#include "../rendering/mesh.h"
#include "../rendering/renderer.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <bx/bx.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <string>
#include <vector>

namespace OpenBully {
namespace {

// Phase-2 test triangle in NDC, built with Track 3's Mesh/MeshVertex layout:
// pos(x,y,z) + normal(nx,ny,nz) + uv(u,v). The shader derives color from uv.
Mesh MakeTestTriangle() {
    Mesh mesh;
    mesh.vertices = {
        {  0.0f,  0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   0.5f, 1.0f }, // top
        { -0.5f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   0.0f, 0.0f }, // bottom-left
        {  0.5f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f,   1.0f, 0.0f }, // bottom-right
    };
    mesh.indices = { 0, 1, 2 };
    mesh.subMeshes.push_back({ "test_triangle", 0, 3 });
    return mesh;
}

// Returns true when the NIF is plausibly loadable level geometry: it parsed
// and carries at least 100 vertices (per task: prioritize big meshes; tiny
// collision-only or helper blocks are skipped).
bool IsLoadableGeometry(const NifLoadResult& nif) {
    return nif.ok && nif.totalVertices >= 100;
}

} // namespace

Engine::~Engine() = default; // deletes Renderer via unique_ptr (complete type here)

size_t Engine::LoadWorldGeometry(const char* dir, size_t maxMeshes) {
    DIR* dp = ::opendir(dir);
    if (!dp) {
        Logger::Info("World geometry dir not found (skipping): %s", dir);
        return 0;
    }

    // Collect .nif names first so we can sort and cap deterministically.
    std::vector<std::string> names;
    while (dirent* ent = ::readdir(dp)) {
        std::string n = ent->d_name;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".nif")
            names.push_back(n);
    }
    ::closedir(dp);

    // Deterministic order, demo set first: the verified test meshes named in
    // the task spec (room interior + car barrier + 1950 fridge) render as a
    // coherent little scene; the remaining slots fill alphabetically.
    static const char* kDemoMeshes[] = {
        "JimmysRoom01.nif",
        "3_06CarBarr01.nif",
        "1950Fridge.nif",
    };
    auto rank = [](const std::string& n) {
        for (size_t i = 0; i < sizeof(kDemoMeshes) / sizeof(kDemoMeshes[0]); i++)
            if (n == kDemoMeshes[i]) return static_cast<int>(i);
        return 1000;
    };
    std::sort(names.begin(), names.end(), [&rank](const std::string& a, const std::string& b) {
        const int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        return a < b;
    });

    size_t loaded = 0, skippedSmall = 0, failed = 0;
    for (const std::string& name : names) {
        if (loaded >= maxMeshes) break;

        const std::string path = std::string(dir) + "/" + name;
        NifLoadResult nif = LoadNifFile(path);
        if (!IsLoadableGeometry(nif)) {
            if (nif.ok) skippedSmall++;   // parsed but < 100 verts
            else failed++;
            continue;
        }

        Mesh mesh;
        if (!MeshFromNif(nif, mesh)) {
            failed++;
            continue;
        }

        // Bake each geometry's world transform (NiNode hierarchy) into the
        // vertices: positions move through the full chain, normals through
        // rotation only (uniform scale assumed for level geometry).
        // MeshFromNif concatenates geometries in order; recompute the vertex
        // ranges per geometry from the sub-mesh list.
        {
            size_t vertBase = 0;
            size_t geomIdx = 0;
            for (const NifGeometryData& geo : nif.geometries) {
                float wm[16];
                if (NifWorldTransform(nif, geo.nodeIndex, wm)) {
                    for (size_t v = 0; v < geo.vertices.size(); v++) {
                        MeshVertex& mv = mesh.vertices[vertBase + v];
                        const NifVector3& p = geo.vertices[v];
                        const float x = wm[0]*p.x + wm[4]*p.y + wm[8]*p.z + wm[12];
                        const float y = wm[1]*p.x + wm[5]*p.y + wm[9]*p.z + wm[13];
                        const float z = wm[2]*p.x + wm[6]*p.y + wm[10]*p.z + wm[14];
                        mv.x = x; mv.y = y; mv.z = z;
                        const NifVector3& nrm = geo.normals[v];
                        const float nx = wm[0]*nrm.x + wm[4]*nrm.y + wm[8]*nrm.z;
                        const float ny = wm[1]*nrm.x + wm[5]*nrm.y + wm[9]*nrm.z;
                        const float nz = wm[2]*nrm.x + wm[6]*nrm.y + wm[10]*nrm.z;
                        mv.nx = nx; mv.ny = ny; mv.nz = nz;
                    }
                }
                vertBase += geo.vertices.size();
                ++geomIdx;
            }
        }

        GpuMesh gpu = m_renderer->UploadMeshGpu(mesh);
        if (!gpu.valid()) {
            failed++;
            continue;
        }

        // Bounds for camera fitting (vertex positions in mesh space).
        float bmin[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
        float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (const MeshVertex& v : mesh.vertices) {
            bmin[0] = std::min(bmin[0], v.x); bmax[0] = std::max(bmax[0], v.x);
            bmin[1] = std::min(bmin[1], v.y); bmax[1] = std::max(bmax[1], v.y);
            bmin[2] = std::min(bmin[2], v.z); bmax[2] = std::max(bmax[2], v.z);
        }

        WorldMesh wm;
        wm.name = name;
        wm.gpu = gpu;
        std::copy(bmin, bmin + 3, wm.boundsMin);
        std::copy(bmax, bmax + 3, wm.boundsMax);
        m_worldMeshes.push_back(std::move(wm));
        loaded++;
    }

    Logger::Info("World geometry: %zu loaded, %zu skipped (<100 verts), %zu failed, %zu scanned",
                 loaded, skippedSmall, failed, names.size());
    return loaded;
}

bool Engine::Init(const Window& window) {
    Logger::Info("Initializing engine...");

    // Native window handle for bgfx (no GL context needed anymore).
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window.GetHandle(), &wmInfo)) {
        Logger::Error("SDL_GetWindowWMInfo failed: %s", SDL_GetError());
        return false;
    }
#if defined(_WIN32)
    void* nativeHandle = wmInfo.info.win.window;
#else
    void* nativeHandle = nullptr; // Phase 2 targets Windows; extend per-platform later
#endif
    m_nativeWindowHandle = nativeHandle;

    m_renderer = std::make_unique<Renderer>();
    const char* shaderRoot = SDL_GetBasePath();
    if (!m_renderer->Init(nativeHandle,
                          unsigned(window.GetWidth()),
                          unsigned(window.GetHeight()),
                          shaderRoot)) {
        Logger::Error("Renderer initialization failed");
        return false;
    }

    // Reference triangle (2D NDC path) — kept per Phase-2 regression baseline.
    const Mesh triangle = MakeTestTriangle();
    m_triangle = m_renderer->UploadMeshGpu(triangle);
    m_triangleValid = m_triangle.valid();
    if (!m_triangleValid) {
        Logger::Error("Mesh upload failed");
        return false;
    }

    // World.img level geometry: NIFs extracted to data/extracted/world next
    // to the executable (deployed by the build) or the source-tree fallback.
    // Loaded AFTER the renderer so GPU handles are valid; capped for the
    // Phase-3 milestone (full streaming comes later).
    // OPENBULLY_DEMO_ONLY=1 restricts the scene to the verified demo trio
    // (JimmysRoom01 + 3_06CarBarr01 + 1950Fridge) for focused screenshots.
    const bool demoOnly = std::getenv("OPENBULLY_DEMO_ONLY") != nullptr;
    const size_t meshCap = demoOnly ? 3 : 32;
    size_t worldLoaded = LoadWorldGeometry("data/extracted/world", meshCap);
    if (worldLoaded == 0) {
        // Fallback: source-tree relative path (running from a dev shell).
        worldLoaded = LoadWorldGeometry("../data/extracted/world", meshCap);
    }
    if (demoOnly) Logger::Info("Demo-only mode: restricted to the 3 test meshes");
    if (worldLoaded == 0) {
        Logger::Info("No World.img geometry loaded; triangle-only mode");
        // Default spawn: spec fallback (0, 5, -10) looking toward origin.
        m_camera = Camera(glm::vec3(0.0f, 5.0f, -10.0f), 0.0f, -0.46f);
    } else {
        // Union of loaded mesh bounds; spawn outside the bounds looking at
        // the scene center so geometry fills the frame on startup.
        glm::vec3 bmin( FLT_MAX,  FLT_MAX,  FLT_MAX);
        glm::vec3 bmax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (const WorldMesh& wm : m_worldMeshes) {
            bmin = glm::min(bmin, glm::make_vec3(wm.boundsMin));
            bmax = glm::max(bmax, glm::make_vec3(wm.boundsMax));
        }
        const glm::vec3 center = 0.5f * (bmin + bmax);
        const float radius = glm::length(bmax - bmin) * 0.5f;

        // Back off along -Z from the center, outside the bounds, at 40%
        // height for a three-quarter view of the level.
        glm::vec3 spawn = center + glm::vec3(0.0f, radius * 0.2f, -(radius * 1.6f + 10.0f));

        // Aim at the center: yaw/pitch for LH +Z-forward convention.
        const glm::vec3 to = center - spawn;
        const float yaw   = std::atan2(to.x, to.z);
        const float pitch = std::atan2(to.y, glm::length(glm::vec2(to.x, to.z)));

        m_camera = Camera(spawn, yaw, pitch);
        Logger::Info("Camera spawn (%.1f, %.1f, %.1f) -> center (%.1f, %.1f, %.1f)",
                     spawn.x, spawn.y, spawn.z, center.x, center.y, center.z);
    }

    m_initialized = true;
    m_lastPerf = SDL_GetPerformanceCounter();
    Logger::Info("Engine initialized successfully");
    return true;
}

void Engine::Update() {
    if (!m_initialized) return;

    const Uint64 now = SDL_GetPerformanceCounter();
    const double deltaTime = double(now - m_lastPerf) / double(SDL_GetPerformanceFrequency());
    m_lastPerf = now;
    m_time += deltaTime;

    // Input snapshot + camera integration (Track 10). Escape is surfaced by
    // Input; main.cpp also checks it via Engine::QuitRequested().
    m_input.Poll();
    if (m_input.QuitRequested()) m_quitRequested = true;
    m_camera.Update(float(deltaTime), m_input);

    ++m_framesAccum;
    m_frameMsAccum += deltaTime * 1000.0;
    if (m_time - m_lastTimingLog >= 1.0) {
        LogFrameTiming();
        m_lastTimingLog = m_time;
    }
}

void Engine::LogFrameTiming() {
    if (m_framesAccum == 0) return;
    const double avgMs = m_frameMsAccum / double(m_framesAccum);
    const double fps = 1000.0 / avgMs;
    Logger::Info("Frame timing: %.2f ms avg (%.1f fps)", avgMs, fps);
    m_framesAccum = 0;
    m_frameMsAccum = 0.0;
}

void Engine::Render() {
    if (!m_initialized || !m_renderer) return;

    if (!m_worldMeshes.empty()) {
        // Orbit camera around the scene center (Gamebryo LH, Y-up).
        const float cp = std::cos(m_camPitch), sp = std::sin(m_camPitch);
        const float cy = std::cos(m_camYaw),   sy = std::sin(m_camYaw);
        float eye[3] = {
            m_sceneCenter[0] + m_camDistance * cp * sy,
            m_sceneCenter[1] + m_camDistance * sp,
            m_sceneCenter[2] + m_camDistance * cp * cy,
        };
        const float up[3] = { 0.0f, 0.0f, 1.0f };
        m_renderer->SetCamera(eye, m_sceneCenter, up);

        // World meshes draw in view 1 (3D). NIF vertices are already in
        // local/world space; identity model transform per mesh for now —
        // per-node hierarchy transforms land with full scene assembly.
        const float identity[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1,
        };
        for (const WorldMesh& wm : m_worldMeshes)
            m_renderer->DrawMesh3D(wm.gpu, identity);

        // Keep the reference triangle as a small 2D overlay in view 0 so the
        // Phase-2 regression baseline stays visibly green.
        m_renderer->DrawMesh(0, 3, 0.6f, -0.6f, 0.15f, 0.15f);
    } else {
        // Triangle-only mode (pre-extraction): full-size reference triangle.
        m_renderer->DrawMesh(0, 3, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    m_renderer->Frame(); // present (VSync via BGFX_RESET_VSYNC)
    ++m_frameCount;
}

void Engine::Shutdown() {
    if (!m_initialized) return;

    Logger::Info("Shutting down engine...");
    for (WorldMesh& wm : m_worldMeshes)
        m_renderer->DestroyGpuMesh(wm.gpu);
    m_worldMeshes.clear();
    if (m_triangleValid) {
        m_renderer->DestroyGpuMesh(m_triangle);
        m_triangleValid = false;
    }
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    m_initialized = false;
    Logger::Info("Engine shutdown complete");
}

} // namespace OpenBully
