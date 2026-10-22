#pragma once

// Engine: frame update + rendering orchestration (bgfx-backed, Phase 2).

#include "rendering/renderer.h"
#include "physics/collision.h"
#include "rendering/mesh.h"
#include "entities/entity.h"
#include "game/camera.h"
#include "game/input.h"
#include "game/player.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenBully {

class Window;

// One World.img NIF mesh loaded, uploaded to the GPU and tracked for the
// debug camera fit. Kept alive for the engine's lifetime.
struct WorldMesh {
    std::string name;      // NIF file name (e.g. "in_longdraw06.nif")
    GpuMesh gpu;           // GPU buffers
    // Sub-mesh list snapshot: index ranges + resolved texture handles.
    struct SubRange {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        GpuTexture texture;   // invalid = renderer placeholder
    };
    std::vector<SubRange> subs;
    float boundsMin[3] = {0, 0, 0};
    float boundsMax[3] = {0, 0, 0};
    uint32_t collisionId = 0; // CollisionWorld object id (Track 13)
};

class Engine {
public:
    Engine() = default;
    ~Engine(); // out-of-line: unique_ptr to forward-declared Renderer

    bool Init(const Window& window);
    void Update();
    void Render();
    void Shutdown();

    // True once Input reports Escape (or the window-close event fired).
    // main.cpp polls this after Update().
    bool QuitRequested() const { return m_quitRequested; }

private:
    void LogFrameTiming();
    void RegisterLevelEntities();
    void SpawnTestEntities(const glm::vec3& center);
    void UpdateEntities(float dt);
    void BuildEntityRenderList();
    bool SpawnJimmy(const glm::vec3& position);
    void UpdateJimmy(float dt);
    void UpdateJimmyCamera(float dt);

    // Loads up to maxMeshes NIFs from `dir` into GPU meshes. Returns the
    // number loaded. Skips files that fail to parse, logs a summary.
    // `texDir` is the extracted .nft directory ("" = textures disabled).
    size_t LoadWorldGeometry(const char* dir, size_t maxMeshes,
                             const char* texDir);

    // Resolves a texture name ("wall01") to a GPU texture. Looks the name up
    // in the texture-name index (see BuildTextureIndex), loads the owning
    // .nft, and caches the upload. Missing names return an invalid handle
    // (placeholder is drawn instead) and are negative-cached.
    GpuTexture AcquireTexture(const char* texDir, const std::string& name);

    // One-time scan of every .nft in `texDir`: maps each contained texture's
    // source name ("wall01") to the .nft file that holds its pixels. This is
    // the Bully texture-dictionary model: shapes name a texture, and any NFT
    // may hold it (only ~2,450 of 5,720 models have a same-named .nft).
    void BuildTextureIndex(const char* texDir);

    bool     m_initialized = false;
    double   m_time = 0.0;
    uint64_t m_lastPerf = 0;

    // Frame timing stats (rolling, logged once per second)
    uint32_t m_framesAccum = 0;
    double   m_frameMsAccum = 0.0;
    double   m_lastTimingLog = 0.0;

    std::vector<WorldMesh> m_worldMeshes;

    // Track 13: collision world over registered level-geometry AABBs.
    std::unique_ptr<CollisionWorld> m_collisionWorld;
    bool m_debugCollision = false; // OPENBULLY_DEBUG_COLLISION=1
    bool m_debugPhysics   = false; // OPENBULLY_DEBUG_PHYSICS=1 (Jimmy sphere)
    std::string m_groundRaycastInfo; // latest down-raycast result (logged with frame timing)
    GpuMesh m_triangle;     // single buffer pair, drawn via the 2D path
    bool m_triangleValid = false;
    // Track 14: index into m_worldMeshes per entity mesh component.
    std::vector<uint32_t> m_entityToWorldMesh;
    void* m_nativeWindowHandle = nullptr;
    uint64_t m_frameCount = 0;

    // Track 11 texture cache: texture name -> GPU texture (invalid = known
    // missing, placeholder drawn). Kept alive until Shutdown.
    std::unordered_map<std::string, GpuTexture> m_textureCache;
    // texture-dictionary index: source name -> owning .nft file base name.
    std::unordered_map<std::string, std::string> m_textureIndex;
    std::string m_texDir;               // active textures dir ("" = disabled)
    size_t m_textureLoadFails = 0;
    size_t m_texturedSubMeshes = 0;
    size_t m_placeholderSubMeshes = 0;

    // FPS free camera (Track 10). Spawn outside the geometry bounds,
    // looking at the scene center; Update integrates input, Render builds
    // matrices. Track 14: camera collides with entity world AABBs.
    Camera m_camera;
    Input  m_input;
    bool   m_quitRequested = false;

    // Track 14: entity system. Level geometry is registered as static
    // entities (Transform + Mesh + Collision, no physics); test boxes carry
    // the same triple and a physics component. Render iterates entities.
    EntityManager m_entities;
    GpuMesh       m_testBox;            // shared unit-cube GPU geometry
    bool          m_testBoxValid = false;

    // Track 15: player character.
    EntityID      m_jimmyId;            // invalid when OPENBULLY_NO_JIMMY set
    glm::vec3     m_jimmySpawn{0.0f, 0.0f, 0.0f};
    glm::vec3     m_camDesired{0.0f};   // camera follow target (lerped toward)
    bool          m_jimmyActive = false; // 3rd-person follow enabled
    GpuMesh       m_jimmyMesh;          // dedicated player-mesh GPU buffers
    bool          m_jimmyMeshValid = false;
    std::vector<WorldMesh::SubRange> m_jimmySubs;

    // Per-entity GPU render record: which GpuMesh + sub-ranges to draw.
    // The WorldMesh list stays the storage for level geometry; entity
    // MeshComponent.subIndex points into it via m_worldMeshes.
    struct EntityRender {
        EntityID id;
        const GpuMesh* gpu;
        std::vector<WorldMesh::SubRange> subs;
        glm::mat4 model;
    };
    std::vector<EntityRender> m_entityRender;

    // Track 12: one-shot verification capture (OPENBULLY_SHOT env var),
    // requested after ~1s of frames so the first draws are on screen.
    std::string m_screenshotName;
    bool        m_screenshotPending = false;

    std::unique_ptr<Renderer> m_renderer;
};

} // namespace OpenBully
