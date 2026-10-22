#include "engine.h"
#include "audio/audio.h"
#include "formats/nif_loader.h"
#include "formats/nft_loader.h"
#include "formats/agr_loader.h"
#include "../entities/entity.h"
#include "../vehicles/vehicle.h"
#include "../vehicles/vehicle_feel.h" // Track 33: handling feel, wheelie, crash
#include "../platform/logger.h"
#include "../platform/window.h"
#include "../rendering/mesh.h"
#include "../rendering/renderer.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
// SDL_syswm.h pulls in windows.h, whose DrawText/DrawTextA macro pair would
// rewrite UIRenderer::DrawText call sites below. The method stays DrawText
// in the class; drop both macros for this translation unit.
#ifdef DrawText
#undef DrawText
#endif
#ifdef DrawTextA
#undef DrawTextA
#endif
#include <bx/bx.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace OpenBully {
namespace {

// Deterministic frame-loop RNG for AI idle timers / wander targets.
// Seeded once per process; every NPC shares the stream (fine for demo AI).
static uint32_t g_aiRng = 0x9E3779B9u;
static float nextRandom01() {
    uint32_t& x = g_aiRng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return float(x % 10000u) / 10000.0f;
}

// Track 28: reaction tuning. File scope so UpdateNpcs (KO fall + flinch) and
// the reaction system below share one definition set.
static constexpr float kWitnessRadius     = 15.0f; // LOS fight-detection range
static constexpr float kHelpCallRadius    = 20.0f; // same-type help-call range
static constexpr int   kCrowdCap          = 8;     // watchers per fight
static constexpr float kCrowdRadiusMin    = 4.0f;  // crowd formation slot radii
static constexpr float kCrowdRadiusMax    = 6.0f;
static constexpr float kFightActiveTime   = 10.0f; // fight stays hot (watch)
static constexpr float kRespawnDelay      = 10.0f; // KO -> respawn at home
static constexpr float kFleeHealthFrac28  = 0.5f;  // flee threshold (health %)
static constexpr float kFleeChance        = 0.5f;
static constexpr float kAlertTimeMin      = 0.4f;  // deciding-pause range
static constexpr float kAlertTimeMax      = 0.8f;
static constexpr float kFallTime          = 0.5f;  // KO fall-over duration
static constexpr float kFlinchTime        = 0.35f; // procedural flinch lean

// Track 27: state names for the AI transition log (verification aid).
static const char* AiStateName(AIState s) {
    switch (s) {
    case AIState::Idle:       return "IDLE";
    case AIState::Wander:     return "WANDER";
    case AIState::FacePlayer: return "FACE_PLAYER";
    case AIState::Patrol:     return "PATROL";
    case AIState::Alert:      return "ALERT";
    case AIState::Chase:      return "CHASE";
    case AIState::Attack:     return "ATTACK";
    case AIState::Flee:       return "FLEE";
    case AIState::Return:     return "RETURN";
    case AIState::Watch:      return "WATCH"; // Track 28
    case AIState::Ko:         return "KO";    // Track 28
    }
    return "?";
}

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

// Track 20: unit cube for the objective beacon column (same Mesh layout as
// the test box; scaled per-frame by the model matrix).
Mesh MakeBeaconCubeMesh() {
    Mesh mesh;
    const float h = 0.5f;
    struct Face { float n[3]; float quad[4][3]; };
    const Face faces[6] = {
        { { 0, 0, 1}, { { -h,-h, h}, { h,-h, h}, { h, h, h}, { -h, h, h} } },
        { { 0, 0,-1}, { {  h,-h,-h}, { -h,-h,-h}, { -h, h,-h}, {  h, h,-h} } },
        { { 1, 0, 0}, { {  h,-h, h}, {  h,-h,-h}, {  h, h,-h}, {  h, h, h} } },
        { {-1, 0, 0}, { { -h,-h,-h}, { -h,-h, h}, { -h, h, h}, { -h, h,-h} } },
        { { 0, 1, 0}, { { -h, h, h}, {  h, h, h}, {  h, h,-h}, { -h, h,-h} } },
        { { 0,-1, 0}, { { -h,-h,-h}, {  h,-h,-h}, {  h,-h, h}, { -h,-h, h} } },
    };
    for (const Face& f : faces) {
        const uint32_t base = uint32_t(mesh.vertices.size());
        for (int i = 0; i < 4; ++i)
            mesh.vertices.push_back({ f.quad[i][0], f.quad[i][1], f.quad[i][2],
                                      f.n[0], f.n[1], f.n[2], 0.0f, 0.0f });
        mesh.indices.insert(mesh.indices.end(),
            { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    mesh.subMeshes.push_back({ "beacon", 0, uint32_t(mesh.indices.size()) });
    return mesh;
}

} // namespace

Engine::~Engine() = default; // deletes Renderer via unique_ptr (complete type here)

// Lowercase a texture name for case-insensitive lookup (the NIF source
// names and the .nft dictionary keys differ in case routinely).
static std::string LowerAscii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void Engine::BuildTextureIndex(const char* texDir) {
    if (!texDir || !*texDir) return;
    DIR* dp = ::opendir(texDir);
    if (!dp) return;
    std::vector<std::string> files;
    while (dirent* ent = ::readdir(dp)) {
        std::string n = ent->d_name;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".nft")
            files.push_back(n);
    }
    ::closedir(dp);
    std::sort(files.begin(), files.end());

    size_t namesFound = 0;
    for (const std::string& file : files) {
        const std::string path = std::string(texDir) + "/" + file;
        const std::vector<std::string> texNames = ListNftTextureNames(path);
        const std::string base = file.substr(0, file.size() - 4);
        for (const std::string& tn : texNames) {
            // First writer wins (deterministic: alphabetical file order).
            const std::string key = LowerAscii(tn);
            if (!key.empty() && !m_textureIndex.count(key)) {
                m_textureIndex.emplace(key, base);
                namesFound++;
            }
        }
    }
    Logger::Info("Texture index: %zu names -> %zu .nft dictionaries",
                 namesFound, files.size());
}

GpuTexture Engine::AcquireTexture(const char* texDir, const std::string& name) {
    if (!texDir || !*texDir || name.empty()) return GpuTexture{};
    const std::string key = LowerAscii(name);
    auto it = m_textureCache.find(key);
    if (it != m_textureCache.end()) return it->second;

    // First request for this name: find the owning dictionary via the index,
    // falling back to a same-named .nft (Bully convention for many models).
    GpuTexture tex;
    std::string nftBase;
    auto idx = m_textureIndex.find(key);
    if (idx != m_textureIndex.end()) {
        nftBase = idx->second;
    } else {
        nftBase = name; // <name>.nft fallback
    }
    const std::string path = std::string(texDir) + "/" + nftBase + ".nft";
    tex = m_renderer->LoadTexture(path, name);
    if (tex.valid() && idx == m_textureIndex.end()) {
        // Fallback hit: remember the mapping for later submeshes.
        m_textureIndex.emplace(key, nftBase);
    }
    if (!tex.valid()) {
        m_textureLoadFails++;
        Logger::Warning("Texture '%s' not resolvable (tried %s.nft)",
                        name.c_str(), nftBase.c_str());
    }
    m_textureCache.emplace(key, tex);
    return tex;
}

size_t Engine::LoadWorldGeometry(const char* dir, size_t maxMeshes,
                                 const char* texDir) {
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
    // OPENBULLY_MESH=<file.nif>: load only that mesh (debug/verification;
    // single meshes give the auto-fit camera tight bounds).
    char meshFilter[256] = {};
    if (const char* mf = std::getenv("OPENBULLY_MESH")) {
        std::strncpy(meshFilter, mf, sizeof(meshFilter) - 1);
        Logger::Info("Mesh filter active: %s", meshFilter);
    }
    for (const std::string& name : names) {
        if (loaded >= maxMeshes) break;
        if (meshFilter[0] && name != meshFilter) continue;

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

        // DEBUG(Track 11 verify): bounds over all vertices vs indexed.
        {
            float dmin[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
            float dmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (const MeshVertex& v : mesh.vertices) {
                dmin[0] = std::min(dmin[0], v.x); dmax[0] = std::max(dmax[0], v.x);
                dmin[1] = std::min(dmin[1], v.y); dmax[1] = std::max(dmax[1], v.y);
                dmin[2] = std::min(dmin[2], v.z); dmax[2] = std::max(dmax[2], v.z);
            }
            Logger::Info("  mesh extent: (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)",
                         dmin[0], dmin[1], dmin[2], dmax[0], dmax[1], dmax[2]);
        }

        // Bounds for camera fitting — over INDEXED vertices only. NIFs can
        // carry orphan vertices far outside the drawn surface (observed on
        // 3_06CarBarr01: ±540-unit orphans vs a 4-unit mesh), which would
        // otherwise drag the auto-fit camera off the actual geometry.
        float bmin[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
        float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (uint32_t idx : mesh.indices) {
            const MeshVertex& v = mesh.vertices[idx];
            bmin[0] = std::min(bmin[0], v.x); bmax[0] = std::max(bmax[0], v.x);
            bmin[1] = std::min(bmin[1], v.y); bmax[1] = std::max(bmax[1], v.y);
            bmin[2] = std::min(bmin[2], v.z); bmax[2] = std::max(bmax[2], v.z);
        }

        WorldMesh wm;
        wm.name = name;
        wm.gpu = gpu;
        // Snapshot sub-mesh ranges + resolve each one's texture up front so
        // Render() only binds and draws (no NFT I/O in the frame loop).
        for (const MeshSubMesh& sub : mesh.subMeshes) {
            WorldMesh::SubRange sr;
            sr.indexOffset = sub.indexOffset;
            sr.indexCount = sub.indexCount;
            sr.texture = AcquireTexture(texDir, sub.textureName);
            if (sr.texture.valid()) m_texturedSubMeshes++;
            else m_placeholderSubMeshes++;
            wm.subs.push_back(sr);
        }
        std::copy(bmin, bmin + 3, wm.boundsMin);
        std::copy(bmax, bmax + 3, wm.boundsMax);
        // Track 13: register mesh bounds with the collision world so camera
        // collision, raycasts and proximity queries see level geometry.
        if (m_collisionWorld) {
            AABB box;
            std::copy(bmin, bmin + 3, box.min);
            std::copy(bmax, bmax + 3, box.max);
            wm.collisionId = m_collisionWorld->RegisterObject(box);
            Logger::Info("  collision box: (%.1f, %.1f, %.1f)-(%.1f, %.1f, %.1f)",
                         box.min[0], box.min[1], box.min[2],
                         box.max[0], box.max[1], box.max[2]);
        }
        m_worldMeshes.push_back(std::move(wm));
        loaded++;
    }

    Logger::Info("World geometry: %zu loaded, %zu skipped (<100 verts), %zu failed, %zu scanned",
                 loaded, skippedSmall, failed, names.size());
    return loaded;
}

// Track 14: register every loaded level mesh as a static entity carrying
// Transform + Mesh + Collision (world-space AABB, no physics component).
// Rendering still reads the sub-range list from m_worldMeshes; the entity
// layer is the query/index structure the camera and later systems use.
void Engine::RegisterLevelEntities() {
    for (uint32_t wi = 0; wi < m_worldMeshes.size(); ++wi) {
        const WorldMesh& wm = m_worldMeshes[wi];
        EntityID e = m_entities.Create();
        Transform t;
        t.position = 0.5f * (glm::make_vec3(wm.boundsMin) + glm::make_vec3(wm.boundsMax));
        t.scale    = glm::vec3(1.0f);
        m_entities.AddTransform(e, t);

        MeshComponent mc;
        mc.isBox = false;
        mc.worldMeshIndex = wi;
        m_entities.AddMesh(e, mc);

        CollisionComponent cc;
        cc.boundsMin = glm::make_vec3(wm.boundsMin) - t.position; // centered local AABB
        cc.boundsMax = glm::make_vec3(wm.boundsMax) - t.position;
        cc.layerMask = 1u; // layer 0: world/static
        m_entities.AddCollision(e, cc);
    }
    Logger::Info("Entities: %zu static level entities registered",
                 m_worldMeshes.size());
}

// Track 14 test: 10 unit boxes (Transform + Mesh + Collision + Physics)
// scattered around the anchor geometry in a ring, so they render visibly
// near the default camera spawn and exercise the query paths.
void Engine::SpawnTestEntities(const glm::vec3& center) {
    if (!m_testBoxValid) return;
    for (int i = 0; i < 10; ++i) {
        const float ang = float(i) * (6.2831853f / 10.0f);
        EntityID e = m_entities.Create();

        Transform t;
        t.position = center + glm::vec3(std::cos(ang) * 8.0f, 1.5f, std::sin(ang) * 8.0f);
        t.rotation = glm::vec3(0.0f, ang, 0.0f);
        t.scale    = glm::vec3(1.0f);
        m_entities.AddTransform(e, t);

        MeshComponent mc;
        mc.isBox = true;
        m_entities.AddMesh(e, mc);

        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-0.5f);
        cc.boundsMax = glm::vec3( 0.5f);
        cc.layerMask = 2u; // layer 1: dynamic/test
        m_entities.AddCollision(e, cc);

        PhysicsComponent pc;
        pc.gravity = -9.8f;
        m_entities.AddPhysics(e, pc);
    }
    Logger::Info("Entities: 10 test boxes spawned around (%.1f, %.1f, %.1f)",
                 center.x, center.y, center.z);

    // -- Track 14 verification ------------------------------------------------
    // 1) Query: the spawn center is within 8.71 units (8 ring + box half
    //    diagonal) of every box, so a 9-radius sphere must hit 10 entities
    //    on layer 2 and none on layer 1 (level entities).
    {
        const size_t nDyn = m_entities.CountNearby(center, 9.0f, 2u);
        const size_t nSta = m_entities.CountNearby(center, 9.0f, 1u);
        Logger::Info("Entities verify: CountNearby(layer2)=%zu (expect 10), "
                     "CountNearby(layer1)=%zu", nDyn, nSta);
        EntityID hit;
        const bool found = m_entities.QuerySphere(center, 9.0f, 2u, &hit);
        Logger::Info("Entities verify: QuerySphere hit=%s idx=%u gen=%u",
                     found ? "true" : "false", hit.index, hit.generation);

        // 2) Destroy + generation guard: recycle slot 0's first entity and
        //    check stale handles fail. Never target the player: Jimmy spawns
        //    at the query center (layer 2, lowest index), so with small
        //    mesh caps the first hit IS Jimmy (Track 21 verification hit
        //    this: the self-test silently deleted him and the follow
        //    camera/footsteps died with his stale handle).
        EntityID stale = hit;
        const bool isPlayer = m_jimmyId.IsValid() &&
                              stale.index == m_jimmyId.index &&
                              stale.generation == m_jimmyId.generation;
        if (stale.IsValid() && m_entities.IsAlive(stale) && !isPlayer) {
            m_entities.Destroy(stale);
            Logger::Info("Entities verify: destroyed idx=%u alive=%s (expect false)",
                         stale.index, m_entities.IsAlive(stale) ? "true" : "false");
            EntityID recycled = m_entities.Create(); // may reuse the slot
            Logger::Info("Entities verify: recycled idx=%u gen=%u (stale gen=%u)",
                         recycled.index, recycled.generation, stale.generation);
            const bool staleOk = !m_entities.IsAlive(stale);
            Logger::Info("Entities verify: stale handle rejected=%s",
                         staleOk ? "true" : "false");
            m_entities.Destroy(recycled); // clean up the verification entity
        }
        // Respawn the destroyed test box so the render count stays 10.
        const float ang = 0.0f;
        EntityID e = m_entities.Create();
        Transform t;
        t.position = center + glm::vec3(std::cos(ang) * 8.0f, 1.5f, std::sin(ang) * 8.0f);
        m_entities.AddTransform(e, t);
        MeshComponent mc; mc.isBox = true;
        m_entities.AddMesh(e, mc);
        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-0.5f); cc.boundsMax = glm::vec3(0.5f); cc.layerMask = 2u;
        m_entities.AddCollision(e, cc);
        PhysicsComponent pc; pc.gravity = -9.8f;
        m_entities.AddPhysics(e, pc);
    }
}

// Track 15: spawn Jimmy, the player character. Searches the loaded world
// meshes for the first "Jimmy"-prefixed NIF, falling back to any
// "Player"-prefixed mesh. The chosen mesh becomes a dynamic entity:
// Transform + Mesh + Collision + Physics + PlayerController. It starts 2u
// above the spawn point so gravity visibly settles it onto the floor.
bool Engine::SpawnJimmy(const glm::vec3& position) {
    m_jimmyId = EntityID{}; // invalid until a mesh is chosen

    // Preferred: a real player body from World.img. These are model-space
    // meshes (feet near origin), so the entity transform drives placement.
    // PLAYER.nif = the full skinned Jimmy body (verified in bully-re
    // tests/test_skin); Player_Mascot is the costume-head fallback.
    static const char* kPlayerNifs[] = { "PLAYER.nif", "Player_Mascot.nif", "GOG_Player.nif" };
    for (const char* playerNif : kPlayerNifs) {
        for (const char* dir : { "data/extracted/world", "../data/extracted/world" }) {
            const std::string path = std::string(dir) + "/" + playerNif;
            NifLoadResult nif = LoadNifFile(path);
            if (!nif.ok || nif.totalVertices < 10) continue;
            Mesh mesh;
            if (!MeshFromNif(nif, mesh)) continue;

            // Track 17: prefer a skinned rig (bone hierarchy + walk/idle
            // clips). Falls through to the static bake when the NIF carries
            // no skin or the .agr group was unavailable.
            if (!m_sharedClips.empty()) {
                const uint32_t rigIndex = CreateRig(nif);
                if (rigIndex != 0xFFFFFFFF) {
                    AnimRig& rig = m_animRigs[rigIndex];
                    m_jimmySkinnedMesh = rig.gpu;
                    m_jimmySkinnedValid = true;
                    m_jimmySkinnedSubs = rig.subs;
                    m_jimmyRigIndex = rigIndex;

                    EntityID e = m_entities.Create();
                    Transform t;
                    t.position = position + glm::vec3(0.0f, 2.0f, 0.0f);
                    t.scale = glm::vec3(rig.bodyScale);
                    m_entities.AddTransform(e, t);

                    MeshComponent mc;
                    mc.isBox = false;
                    mc.isJimmy = true;
                    m_entities.AddMesh(e, mc);

                    CollisionComponent cc;
                    cc.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
                    cc.boundsMax = glm::vec3( 0.5f, 1.8f,  0.5f);
                    cc.layerMask = 2u;
                    m_entities.AddCollision(e, cc);

                    PhysicsComponent pc;
                    pc.gravity = -9.8f;
                    m_entities.AddPhysics(e, pc);
                    m_entities.AddPlayer(e, PlayerController{});

                    AnimationComponent ac;
                    ac.rigIndex = rigIndex;
                    ac.playing = true;
                    ac.wasMoving = false;
                    m_entities.AddAnimation(e, ac);
                    // Start idle so the pose is alive even before moving.
                    if (rig.idleClip >= 0)
                        rig.controller.Play(rig.idleClip, true, 0.0f);

                    m_jimmyId = e;
                    Logger::Info("Jimmy spawned SKINNED: '%s' (%d bones, walk=%d "
                                 "idle=%d) at (%.1f, %.1f, %.1f)", playerNif,
                                 rig.controller.BoneCount(), rig.walkClip,
                                 rig.idleClip, t.position.x, t.position.y,
                                 t.position.z);
                    return true;
                }
            }

            // Bake node-hierarchy transforms (same as level geometry).
            {
                size_t vertBase = 0;
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
                        }
                    }
                    vertBase += geo.vertices.size();
                }
            }
            GpuMesh gpu = m_renderer->UploadMeshGpu(mesh);
            if (!gpu.valid()) continue;

            // Normalize the body to a 1.8u humanoid height: scale the entity
            // so the mesh's indexed Y extent maps to PlayerController::height.
            float ylo =  FLT_MAX, yhi = -FLT_MAX;
            for (uint32_t idx : mesh.indices) {
                const MeshVertex& v = mesh.vertices[idx];
                ylo = std::min(ylo, v.y); yhi = std::max(yhi, v.y);
            }
            float bodyScale = 1.0f;
            if (yhi > ylo) bodyScale = 1.8f / (yhi - ylo);

            // Sub-ranges + textures, stored on the engine (alive forever).
            m_jimmyMesh = gpu;
            m_jimmyMeshValid = true;
            m_jimmySubs.clear();
            for (const MeshSubMesh& sub : mesh.subMeshes) {
                WorldMesh::SubRange sr;
                sr.indexOffset = sub.indexOffset;
                sr.indexCount = sub.indexCount;
                sr.texture = AcquireTexture(m_texDir.c_str(), sub.textureName);
                m_jimmySubs.push_back(sr);
            }

            EntityID e = m_entities.Create();
            Transform t;
            t.position = position + glm::vec3(0.0f, 2.0f, 0.0f);
            t.scale = glm::vec3(bodyScale);
            m_entities.AddTransform(e, t);

            MeshComponent mc;
            mc.isBox = false;
            mc.isJimmy = true; // draws m_jimmyMesh via its own transform
            m_entities.AddMesh(e, mc);

            CollisionComponent cc;
            cc.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
            cc.boundsMax = glm::vec3( 0.5f, 1.8f,  0.5f);
            cc.layerMask = 2u;
            m_entities.AddCollision(e, cc);

            PhysicsComponent pc;
            pc.gravity = -9.8f;
            m_entities.AddPhysics(e, pc);

            m_entities.AddPlayer(e, PlayerController{});

            m_jimmyId = e;
            Logger::Info("Jimmy spawned: player mesh '%s' (%zu verts) at "
                         "(%.1f, %.1f, %.1f), sphere r=0.50", playerNif,
                         mesh.vertices.size(), t.position.x, t.position.y,
                         t.position.z);
            return true;
        }
    }

    // Fallback: reuse a loaded level mesh by name prefix (Jimmy*/Player*).
    int chosen = -1;
    for (int pass = 0; pass < 2 && chosen < 0; ++pass) {
        const char* prefix = pass == 0 ? "Jimmy" : "Player";
        const size_t plen = std::strlen(prefix);
        for (size_t i = 0; i < m_worldMeshes.size(); ++i) {
            if (m_worldMeshes[i].name.compare(0, plen, prefix) == 0) {
                chosen = int(i);
                break;
            }
        }
    }
    if (chosen < 0) {
        Logger::Warning("SpawnJimmy: no player mesh found; skipping player spawn");
        return false;
    }

    const WorldMesh& wm = m_worldMeshes[size_t(chosen)];
    EntityID e = m_entities.Create();

    Transform t;
    t.position = position + glm::vec3(0.0f, 2.0f, 0.0f); // drop-in from above
    m_entities.AddTransform(e, t);

    MeshComponent mc;
    mc.isBox = false;
    mc.worldMeshIndex = uint32_t(chosen);
    m_entities.AddMesh(e, mc);

    CollisionComponent cc;
    // Task spec: SphereCollision radius ~0.5. CollisionComponent is an AABB
    // store, so approximate the sphere with a small centered box; physics
    // uses the 0.5 radius directly for ground + wall resolution.
    cc.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
    cc.boundsMax = glm::vec3( 0.5f, 1.8f,  0.5f);
    cc.layerMask = 2u; // layer 1: dynamic
    m_entities.AddCollision(e, cc);

    PhysicsComponent pc;
    pc.gravity = -9.8f;
    m_entities.AddPhysics(e, pc);

    m_entities.AddPlayer(e, PlayerController{});

    m_jimmyId = e;
    Logger::Info("Jimmy spawned: entity mesh '%s' at (%.1f, %.1f, %.1f), "
                 "sphere r=0.50", wm.name.c_str(), t.position.x, t.position.y,
                 t.position.z);
    return true;
}

// Track 15/18: integrate Jimmy's physics + input each frame.
//
// Track 18 character-controller pipeline (per frame):
//   1. gravity: v.y += g*dt, clamped to maxFallSpeed; airborne applies
//      slight air control so falls stay steerable.
//   2. horizontal move: WASD in camera-yaw space; on slopes the move
//      direction is projected onto the ground plane (n x up) so walking
//      follows the surface angle instead of ploughing into it.
//   3. step-up: a horizontal probe finds blocking boxes; obstacles whose
//      top is within stepHeight raise the position (stair climbing).
//   4. ground: down raycast (GroundProbe). Within the snap window =>
//      stand on it, reset v.y; steep normal => slope slide accelerates
//      downhill. High landing speed => fall-damage log.
//   5. wall slide: sphere resolve with normal; incoming velocity projected
//      onto the wall tangent so Jimmy slides along walls, not into them.

// --- Track 25: dodge / block / stamina tuning -------------------------------

namespace {

// Task spec: dodge 0.5 s i-frames, 20 stamina; block 5 stamina/s;
// attack 10 stamina; regen 20/s when idle.
constexpr float kDodgeStaminaCost    = 20.0f;
constexpr float kDodgeInvincibleTime = 0.5f;   // s of i-frames from roll start
constexpr float kDodgeDuration       = 0.45f;  // s of roll movement (i-frames outlast it)
constexpr float kDodgeSpeed          = 8.0f;   // u/s roll translation
constexpr float kDodgeCooldown       = 0.6f;   // s between rolls (hold-shift guard)
constexpr float kBlockStaminaPerSec  = 5.0f;
constexpr float kAttackStaminaCost   = 10.0f;
constexpr float kAttackCooldown      = 0.5f;
constexpr float kCounterWindow       = 0.2f;   // block THIS soon before a hit
constexpr float kCounterDuration     = 0.6f;   // counter state lifetime
constexpr float kCounterAttackBonus  = 4.0f;   // next attack multiplier

} // namespace

void Engine::UpdateJimmy(float dt) {
    if (!m_jimmyActive || !m_jimmyId.IsValid() || dt <= 0.0f) return;
    // Track 31: mounted — the vehicle update owns position; player input and
    // gravity park until dismount.
    if (m_mountedVehicle.IsValid()) return;
    Transform* t = m_entities.GetTransform(m_jimmyId);
    PhysicsComponent* p = m_entities.GetPhysics(m_jimmyId);
    PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
    if (!t || !p || !pc) return;

    glm::vec3 pos = t->position;

    // Phase 9 integration: kill-plane respawn. The demo anchor geometry has
    // open roof edges and hollow rooms; a walk off an edge (auto-walk/demo
    // scripts) or a drop through a gap used to fall forever (silent exit at
    // y ~ -700 around 30-120 s). Recover to the spawn point instead: the
    // spawn was raycast-verified as standable surface in Init.
    {
        constexpr float kKillPlaneDrop = 200.0f; // below spawn = unrecoverable
        if (pos.y < m_jimmySpawn.y - kKillPlaneDrop || pos.y < -500.0f) {
            pos         = m_jimmySpawn;
            p->velocity = glm::vec3(0.0f);
            p->onGround = true;
            t->position = pos;
            Logger::Warning("Jimmy: kill-plane respawn at spawn point "
                            "(%.1f, %.1f, %.1f)", pos.x, pos.y, pos.z);
        }
    }

    // --- 1. Gravity ----------------------------------------------------------
    if (!p->onGround || p->velocity.y > 0.0f)
        p->velocity.y += p->gravity * dt;
    p->velocity.y = std::max(p->velocity.y, -p->maxFallSpeed);

    // --- 2. Horizontal control + slope following -----------------------------
    glm::vec3 wishMove(0.0f);
    {
        // Track 17 verification hook: OPENBULLY_AUTO_WALK=1 holds Forward so
        // headless screenshot runs exercise the walk cycle without input.
        static const bool kAutoWalk = std::getenv("OPENBULLY_AUTO_WALK") != nullptr;
        const float cy = std::cos(m_camera.GetYaw()), sy = std::sin(m_camera.GetYaw());
        if (m_input.IsDown(Button::Forward) || kAutoWalk) wishMove += glm::vec3(sy, 0.0f, cy);
        if (m_input.IsDown(Button::Back))    wishMove -= glm::vec3(sy, 0.0f, cy);
        if (m_input.IsDown(Button::Left))    wishMove -= glm::vec3(cy, 0.0f, -sy);
        if (m_input.IsDown(Button::Right))   wishMove += glm::vec3(cy, 0.0f, -sy);
    }
    // Track 25: block roots the character ("can't move while blocking").
    if (pc->state == DefenseState::Block) wishMove = glm::vec3(0.0f);
    // Track 25: dodge roll overrides WASD with the locked roll direction and
    // a full 360-degree spin over the roll (no dodge clip in C_Player.agr, so
    // the spec's translation+rotation fallback applies).
    if (pc->state == DefenseState::Dodge && glm::length(pc->dodgeDir) > 0.5f) {
        const float prevSpin = pc->dodgeRollSpin;
        pc->dodgeRollSpin = std::min(pc->dodgeRollSpin +
            (6.28318531f / kDodgeDuration) * dt, 6.28318531f);
        const float spinDy = pc->dodgeRollSpin - prevSpin;
        pc->facingYaw += spinDy;
        t->rotation.y = pc->facingYaw;
        pos += pc->dodgeDir * (kDodgeSpeed * dt);

        // Skip normal movement handling but still run collision/ground code:
        // fall through with wantsMove cleared (dodge motion already applied).
        wishMove = glm::vec3(0.0f);
    }
    // Track 24: movement is held while grappling (UpdateGrapple positions
    // both characters; input is ignored until the grapple ends).
    if (CombatComponent* cc = m_entities.GetCombat(m_jimmyId))
        if (cc->state != CombatState::None) wishMove = glm::vec3(0.0f);
    // Track 30: movement is held while a conversation is active (both
    // parties freeze; the NPC side is frozen in UpdateNpcs).
    if (m_dialogue.IsActive()) wishMove = glm::vec3(0.0f);

    const bool wantsMove = wishMove.x != 0.0f || wishMove.z != 0.0f;

    // Ground state for THIS frame, from the position we are about to leave.
    float groundY = -FLT_MAX;
    glm::vec3 groundNormal(0.0f, 1.0f, 0.0f);
    bool haveGround = false;
    if (m_collisionWorld) {
        const float probe[3] = { pos.x, pos.y, pos.z };
        float n[3];
        if (m_collisionWorld->GroundProbe(probe, 1.0f, groundY, n))
            haveGround = true, groundNormal = glm::vec3(n[0], n[1], n[2]);
    }
    // Allow a small tolerance so we keep "ground" state while walking over
    // tiny height changes before the snap fixes it below.
    const bool wasGrounded = haveGround && (pos.y - groundY) < pc->groundSnapMax + 0.1f;

    if (wantsMove) {
        glm::vec3 dir = glm::normalize(wishMove);
        // Follow the surface: project the move direction onto the ground
        // plane when standing on a non-flat surface (slope walking).
        if (wasGrounded && haveGround && groundNormal.y < 0.999f) {
            const glm::vec3 flat = dir - groundNormal * glm::dot(dir, groundNormal);
            const float len = glm::length(flat);
            if (len > 1e-4f) dir = flat / len;
        }
        const float speed = pc->moveSpeed *
            (m_input.IsDown(Button::Sprint) ? 2.0f : 1.0f);
        pos += dir * (speed * dt);

        // Face the movement direction (shortest-arc turn).
        const float targetYaw = std::atan2(dir.x, dir.z);
        float dyaw = targetYaw - pc->facingYaw;
        while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
        while (dyaw < -3.14159265f) dyaw += 6.28318531f;
        pc->facingYaw += dyaw * std::min(1.0f, pc->turnSpeed * dt);
        pc->facingValid = true;
        t->rotation.y = pc->facingYaw;
    }
    // Slope slide: steep surface pulls the character downhill regardless of
    // input (spec: character should naturally slide down steep slopes).
    p->sliding = false;
    if (wasGrounded && haveGround && groundNormal.y < p->slopeSlideCos) {
        p->sliding = true;
        const glm::vec3 downhill = glm::normalize(
            glm::vec3(groundNormal.x, 0.0f, groundNormal.z));
        glm::vec3 slideVel = p->HorizontalVelocity();
        slideVel += downhill * (p->slopeSlideAccel * dt);
        // Clamp slide speed so long slopes stay controllable.
        const float sv = glm::length(slideVel);
        if (sv > 8.0f) slideVel *= 8.0f / sv;
        pos += slideVel * dt;
    }
    // Air: keep existing horizontal momentum (no input control mid-air).

    // --- 3. Stair climbing / step-up -----------------------------------------
    // Cast the sphere forward at foot level; if a wall stops us but the box
    // top is within stepHeight, lift the position over it (smoothed).
    if (m_collisionWorld && (wantsMove || p->sliding)) {
        const float in[3] = { pos.x, pos.y + pc->radius, pos.z };
        float out[3], wallN[3];
        if (m_collisionWorld->ResolveSphere(in, pc->radius, out, wallN)) {
            const float pushX = out[0] - in[0], pushZ = out[2] - in[2];
            const bool horizontalBlock = std::fabs(pushX) + std::fabs(pushZ) > 1e-4f &&
                                         wallN[1] < 0.5f; // vertical-ish face
            if (horizontalBlock) {
                // Re-probe ground AT the pushed-back position: the down ray
                // from just above the climb ceiling lands on the step surface
                // if the obstacle is climbable.
                Ray probe;
                probe.origin[0] = out[0] - wallN[0] * pc->radius;
                probe.origin[1] = pos.y + p->stepHeight + 0.1f;
                probe.origin[2] = out[2] - wallN[2] * pc->radius;
                probe.direction[0] = 0.0f; probe.direction[1] = -1.0f; probe.direction[2] = 0.0f;
                RayHit hit;
                if (m_collisionWorld->Raycast(probe, hit) &&
                    hit.point[1] > pos.y + 0.05f &&               // actually a step up
                    hit.point[1] - pos.y <= p->stepHeight) {      // within climb height
                    // Smooth step: raise Y to the step top; the ground snap
                    // below keeps the motion visually continuous.
                    pos = glm::vec3(out[0] - wallN[0] * pc->radius * 0.9f,
                                    hit.point[1] + 0.01f,
                                    out[2] - wallN[2] * pc->radius * 0.9f);
                } else {
                    pos.x = out[0]; pos.z = out[2]; // plain wall push-out
                }
            } else {
                pos.x = out[0]; pos.y = out[1] - pc->radius; pos.z = out[2];
            }
        }
    }

    // Integrate vertical motion (falls / jumps).
    pos.y += p->velocity.y * dt;

    // --- 4. Ground detection + landing ---------------------------------------
    float newGroundY = -FLT_MAX;
    glm::vec3 newNormal(0.0f, 1.0f, 0.0f);
    bool groundedNow = false;
    if (m_collisionWorld) {
        const float probe[3] = { pos.x, pos.y, pos.z };
        float n[3];
        if (m_collisionWorld->GroundProbe(probe, 0.6f, newGroundY, n)) {
            newNormal = glm::vec3(n[0], n[1], n[2]);
            const float snap = pc->groundSnapMax;
            const float below = newGroundY - pos.y;
            // Snap down small distances (walking down slopes/stairs) and
            // land when the fall carried us into the surface.
            if (below >= -snap && below <= snap && p->velocity.y <= 0.0f) {
                groundedNow = true;
            } else if (pos.y <= newGroundY) { // tunneled through this frame
                groundedNow = true;
            }
        }
    }
    if (groundedNow) {
        const float impact = -p->velocity.y;
        pos.y = newGroundY; // stand exactly on the surface
        if (!p->onGround && impact > p->fallDamageThreshold)
            Logger::Warning("Jimmy landing: %.1f u/s (fall damage threshold %.1f)",
                            impact, p->fallDamageThreshold);
        p->velocity.y = 0.0f;
        p->onGround = true;
        p->groundNormalY = newNormal.y;
        // Downhill continuation while sliding: keep the slide velocity.
    } else {
        p->onGround = false;
        p->groundNormalY = 1.0f;
    }

    // --- 5. Wall slide --------------------------------------------------------
    // Resolve the capsule sphere against level AABBs; project the horizontal
    // velocity onto the wall tangent so momentum is preserved along walls.
    if (m_collisionWorld) {
        const float in[3] = { pos.x, pos.y + pc->radius, pos.z };
        float out[3], wallN[3];
        if (m_collisionWorld->ResolveSphere(in, pc->radius, out, wallN)) {
            pos.x = out[0]; pos.z = out[2];
            // Slide: remove the velocity component into the wall.
            glm::vec3 n2 = glm::vec3(wallN[0], 0.0f, wallN[2]);
            const float nlen = glm::length(n2);
            if (nlen > 1e-4f) {
                n2 /= nlen;
                glm::vec3 hv = p->HorizontalVelocity();
                const float into = glm::dot(hv, n2);
                if (into < 0.0f) {
                    hv -= n2 * into; // tangent projection = wall slide
                    p->velocity.x = hv.x;
                    p->velocity.z = hv.z;
                }
            }
        }
    }

    t->position = pos;

    // Track 21: footstep audio on a fixed cadence while walking on the ground
    // (play-cycle phase sync deferred; 0.35 s matches the deliverable's
    // cooldown, and AudioManager::PlayFootstep enforces it independently, so
    // a fast call rate can never machine-gun the sound).
    if (m_audio && wantsMove && p->onGround && !p->sliding) {
        m_footstepTimer += dt;
        if (m_footstepTimer >= kFootstepInterval) {
            m_footstepTimer = 0.0f;
            m_audio->PlayFootstep(pos);
        }
    }

    // Track 17: walk cycle while input moves him (grounded), idle otherwise.
    // Sprint could drive a faster walk later (clip speed scaling).
    UpdateEntityAnimation(m_jimmyId, wantsMove && !p->sliding, dt);
}

// Track 25: per-frame combat state machine for the player. Order of checks:
//   1. counters tick down (dodge/invincibility/cooldowns/state timers),
//   2. attack input (LMB) — blocked entirely while dodging,
//   3. dodge trigger (Shift + WASD press) — needs 20 stamina,
//   4. block hold (RMB) — 5 stamina/s, breaks when the pool empties.
// UpdateJimmy (runs right after) enforces the movement consequences:
// dodge overrides WASD with the locked roll, block zeroes it.
void Engine::UpdateJimmyCombat(float dt) {
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc || dt <= 0.0f) return;
    // Track 31: mounted — no attack/dodge/block input while driving; timers
    // above already ticked, movement-state machines stay parked.
    if (m_mountedVehicle.IsValid()) return;

    // --- 1. Tick timers ------------------------------------------------------
    if (pc->invincibleTimer > 0.0f) pc->invincibleTimer -= dt;
    if (pc->dodgeCooldown  > 0.0f) pc->dodgeCooldown  -= dt;
    if (pc->attackCooldown > 0.0f) pc->attackCooldown -= dt;

    if (pc->state == DefenseState::Dodge) {
        pc->stateTimer -= dt;
        if (pc->stateTimer <= 0.0f) {
            pc->state = DefenseState::Normal;
            Logger::Info("Dodge: roll ended");
        }
    } else if (pc->state == DefenseState::Counter) {
        pc->stateTimer -= dt;
        if (pc->stateTimer <= 0.0f) {
            pc->state = DefenseState::Normal;
            Logger::Info("Counter window expired (not spent)");
        }
    }

    const bool blocking = m_input.IsDown(Button::Block);

    // --- 2. Attack (LMB) ------------------------------------------------------
    // Can't attack during a dodge roll; otherwise costs 10 stamina. A
    // counter-armed attack consumes the armed flag for 4x damage.
    // Track 30: no attacks while a conversation is active.
    if (m_input.IsPressed(Button::Attack) && !m_dialogue.IsActive()) {
        // Track 26: an equipped weapon replaces the unarmed punch entirely
        // (dodge still blocks it; the weapon's own cooldown governs the rate
        // and counter-arming is honored inside WeaponTryAttack).
        if (pc->equippedSlot >= 0) {
            if (pc->state != DefenseState::Dodge && pc->attackCooldown <= 0.0f)
                WeaponTryAttack(pc);
        } else if (pc->state == DefenseState::Dodge) {
            Logger::Info("Attack ignored: dodging");
        } else if (pc->attackCooldown > 0.0f) {
            // rate-limited, no log spam
        } else if (!pc->stamina.Spend(kAttackStaminaCost)) {
            Logger::Info("Attack failed: not enough stamina (%.0f/%.0f)",
                         pc->stamina.current, pc->stamina.max);
        } else {
            pc->attackCooldown = kAttackCooldown;
            if (pc->counterArmed) {
                pc->counterArmed = false;
                pc->state = DefenseState::Normal;
                Logger::Info("COUNTER ATTACK! damage %d (stamina %.0f/%.0f)",
                             int(10.0f * kCounterAttackBonus),
                             pc->stamina.current, pc->stamina.max);
            } else {
                Logger::Info("Punch: damage 10 (stamina %.0f/%.0f)",
                             pc->stamina.current, pc->stamina.max);
            }
        }
    }

    // --- 3. Dodge trigger: Shift + a WASD direction press ---------------------
    // Only from Normal/Counter (not while blocking); needs 20 stamina.
    const bool dirPressed =
        m_input.IsPressed(Button::Forward) || m_input.IsPressed(Button::Back) ||
        m_input.IsPressed(Button::Left)    || m_input.IsPressed(Button::Right);
    if (m_input.IsDown(Button::Sprint) && dirPressed &&
        (pc->state == DefenseState::Normal || pc->state == DefenseState::Counter) &&
        pc->dodgeCooldown <= 0.0f) {
        if (!pc->stamina.Spend(kDodgeStaminaCost)) {
            Logger::Info("Dodge failed: not enough stamina (%.0f/%.0f)",
                         pc->stamina.current, pc->stamina.max);
        } else {
            // Roll direction: the WASD wish direction in camera-yaw space,
            // falling back to the current facing when only Shift was tapped.
            const float cy = std::cos(m_camera.GetYaw()), sy = std::sin(m_camera.GetYaw());
            glm::vec3 dir(0.0f);
            if (m_input.IsDown(Button::Forward)) dir += glm::vec3(sy, 0.0f, cy);
            if (m_input.IsDown(Button::Back))    dir -= glm::vec3(sy, 0.0f, cy);
            if (m_input.IsDown(Button::Left))    dir -= glm::vec3(cy, 0.0f, -sy);
            if (m_input.IsDown(Button::Right))   dir += glm::vec3(cy, 0.0f, -sy);
            if (dir.x == 0.0f && dir.z == 0.0f) {
                dir = glm::vec3(std::sin(pc->facingYaw), 0.0f, std::cos(pc->facingYaw));
                pc->facingValid = true;
            }
            pc->dodgeDir = glm::normalize(dir);
            pc->state           = DefenseState::Dodge;
            pc->stateTimer      = kDodgeDuration;
            pc->invincibleTimer = kDodgeInvincibleTime;
            pc->dodgeCooldown   = kDodgeCooldown;
            pc->dodgeRollSpin   = 0.0f;
            Logger::Info("Dodge roll: dir (%.2f, %.2f), i-frames %.1fs "
                         "(stamina %.0f/%.0f)",
                         pc->dodgeDir.x, pc->dodgeDir.z, kDodgeInvincibleTime,
                         pc->stamina.current, pc->stamina.max);
        }
    }

    // --- 4. Block hold (RMB) --------------------------------------------------
    // Entering block from Normal/Counter stamps the start time for the
    // counter-window check in DamagePlayer. Holding drains 5 stamina/s and
    // roots the character (enforced in UpdateJimmy); the pool breaking drops
    // the block. Blocked characters still regen nothing while draining.
    if (blocking && pc->state != DefenseState::Dodge) {
        if (pc->state != DefenseState::Block) {
            pc->state          = DefenseState::Block;
            pc->blockStartTime = float(m_time);
            Logger::Info("Block: entered (50%% damage reduction)");
        }
        if (!pc->stamina.Spend(kBlockStaminaPerSec * dt)) {
            pc->stamina.current = 0.0f;
            if (pc->state == DefenseState::Block) {
                pc->state = DefenseState::Normal;
                Logger::Info("Block broken: stamina depleted");
            }
        }
    } else if (pc->state == DefenseState::Block && !blocking) {
        pc->state = DefenseState::Normal;
        Logger::Info("Block: released");
    } else if (pc->state == DefenseState::Normal) {
        pc->stamina.Regen(dt); // idle/attacking/dodging regen at 20/s
    }
}

// Track 25: central incoming-damage gate. All player damage routes here.
//   - dodge i-frames: no damage at all,
//   - block: 50% reduction; blocking within kCounterWindow of the block
//     start (i.e. the block went up <0.2 s before the hit lands) arms the
//     counter state — the next attack deals 4x damage.
void Engine::DamagePlayer(int amount, const char* source) {
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc || amount <= 0) return;

    if (pc->invincibleTimer > 0.0f) {
        Logger::Info("Hit DODGED (%s): 0 damage (i-frames %.2fs left)",
                     source ? source : "?", std::max(0.0f, pc->invincibleTimer));
        return;
    }
    if (pc->state == DefenseState::Block) {
        const int reduced = amount / 2; // 50% reduction (task spec)
        pc->health.Damage(reduced);
        Logger::Info("Hit BLOCKED (%s): %d -> %d damage (health %d/%d)",
                     source ? source : "?", amount, reduced,
                     pc->health.current, pc->health.max);
        // Counter window: the block must have gone up less than 0.2 s ago.
        if (!pc->counterArmed &&
            float(m_time) - pc->blockStartTime < kCounterWindow) {
            pc->counterArmed = true;
            pc->state        = DefenseState::Counter;
            pc->stateTimer   = kCounterDuration;
            Logger::Info("COUNTER! well-timed block -> next attack x%.1f "
                         "(window %.1fs)", kCounterAttackBonus, kCounterDuration);
        }
        return;
    }
    pc->health.Damage(amount);
    Logger::Info("Hit (%s): %d damage (health %d/%d)",
                 source ? source : "?", amount,
                 pc->health.current, pc->health.max);
}


// Track 15: 3rd-person follow camera. Desired position sits behind and above
// Jimmy (offset (0, 1.5, -3) rotated into camera-yaw space so the orbit stays
// behind the view direction); the camera lerps toward it (0.1 factor) and
// looks at Jimmy's head (position + 1u up). Mouse look still orbits freely.
void Engine::UpdateJimmyCamera(float dt) {
    if (!m_jimmyActive || !m_jimmyId.IsValid()) return;
    const Transform* t = m_entities.GetTransform(m_jimmyId);
    if (!t) return;

    const glm::vec3 focus = t->position + glm::vec3(0.0f, 1.0f, 0.0f);
    const float yaw = m_camera.GetYaw(), pitch = m_camera.GetPitch();
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    // Behind = opposite the view direction, above = +Y lift (spec offset
    // (0, 1.5, -3), rotated by yaw/pitch so orbiting works).
    const glm::vec3 desired = focus -
        glm::vec3(sy * cp, sp, cy * cp) * 3.0f + glm::vec3(0.0f, 1.5f, 0.0f);

    // Smooth lag: exponential lerp, frame-rate independent form of the
    // 0.1 factor at 60 fps.
    const float a = 1.0f - std::pow(1.0f - 0.1f, dt * 60.0f);
    glm::vec3 pos = glm::mix(m_camera.GetPosition(), desired, a);

    // Keep the camera out of walls (Track 13 resolve, small radius).
    if (m_collisionWorld) {
        const float in[3] = { pos.x, pos.y, pos.z };
        float out[3];
        if (m_collisionWorld->ResolveSphere(in, 0.35f, out))
            pos = glm::vec3(out[0], out[1], out[2]);
    }

    m_camera.SetPosition(pos);
    // Re-aim yaw/pitch at Jimmy's head so the view always tracks him.
    const glm::vec3 to = focus - pos;
    m_camera.SetLookAt(to);
}

// Track 16: load character NIFs from World.img for NPC spawning. Any model
// that is not Jimmy/vehicle/level-geometry and has enough vertices counts.
// Character NIFs are model-space (feet near origin, standing on +Y), so no
// world-transform baking is needed and the entity transform drives placement.
size_t Engine::LoadNpcModels(const char* dir) {
    DIR* dp = ::opendir(dir);
    if (!dp) {
        Logger::Info("NPC model dir not found (skipping): %s", dir);
        return 0;
    }
    std::vector<std::string> names;
    while (dirent* ent = ::readdir(dp)) {
        std::string n = ent->d_name;
        if (n.size() > 4 && n.substr(n.size() - 4) == ".nif")
            names.push_back(n);
    }
    ::closedir(dp);
    std::sort(names.begin(), names.end());

    // Preferred, known-good humanoid prefixes first; then any CS_/character
    // looking file, so model variety does not depend on one naming family.
    // Exclusions: cutscene props, Jimmy, logos.
    auto isCharacterName = [](const std::string& n) {
        auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
        if (has("Jimmy") || has("JIMMY") || has("Jimmy")) return false;
        if (has("Screen") || has("Pedistal") || has("Bike")) return false;
        return has("CS_") || has("Girl") || has("BOY") || has("Boy") ||
               has("Nerd") || has("Jock") || has("Greaser") || has("Prep") ||
               has("Teacher") || has("TE_") || has("GN_") || has("R_Good") ||
               has("Kid") || has("Ped") || has("Zoe") || has("Lola") ||
               has("Mandy") || has("Pinky");
    };

    size_t loaded = 0, failed = 0;
    // Cap the collection: the engine cycles models for variety, so ~16
    // distinct bodies are plenty and keep startup fast (the name filter
    // below matches 100+ CS_* cutscene models).
    constexpr size_t kMaxNpcModels = 16;
    for (const std::string& name : names) {
        if (loaded >= kMaxNpcModels) break;
        if (!isCharacterName(name)) continue;
        const std::string path = std::string(dir) + "/" + name;
        NifLoadResult nif = LoadNifFile(path);
        if (!nif.ok || nif.totalVertices < 50) { if (!nif.ok) failed++; continue; }
        Mesh mesh;
        if (!MeshFromNif(nif, mesh)) { failed++; continue; }

        // Indexed-vertex height -> normalize the body to 1.8u (same scheme
        // as Jimmy so every NPC is a plausible humanoid scale).
        float ylo =  FLT_MAX, yhi = -FLT_MAX;
        for (uint32_t idx : mesh.indices) {
            const MeshVertex& v = mesh.vertices[idx];
            ylo = std::min(ylo, v.y); yhi = std::max(yhi, v.y);
        }
        if (yhi <= ylo) { failed++; continue; }
        const float bodyScale = 1.8f / (yhi - ylo);

        GpuMesh gpu = m_renderer->UploadMeshGpu(mesh);
        if (!gpu.valid()) { failed++; continue; }

        NpcModel model;
        model.name = name;
        model.gpu = gpu;
        model.bodyScale = bodyScale;
        model.sourcePath = path; // Track 17: for lazy rig creation
        for (const MeshSubMesh& sub : mesh.subMeshes) {
            WorldMesh::SubRange sr;
            sr.indexOffset = sub.indexOffset;
            sr.indexCount = sub.indexCount;
            sr.texture = AcquireTexture(m_texDir.c_str(), sub.textureName);
            model.subs.push_back(sr);
        }
        m_npcModels.push_back(std::move(model));
        loaded++;
    }
    Logger::Info("NPC models: %zu loaded, %zu failed (from %zu scanned)",
                 loaded, failed, names.size());
    return loaded;
}

// Track 16: spawn NPCs scattered around `center`. Model selection cycles
// through the loaded collection for variety; AI states are mixed so some
// stand idle and others wander.
void Engine::SpawnNpcs(const glm::vec3& center) {
    if (m_npcModels.empty()) {
        Logger::Warning("SpawnNpcs: no NPC models loaded, skipping spawn");
        return;
    }
    // OPENBULLY_NPC_COUNT=<n> verification hook; default 18 (10-20 spec).
    uint32_t count = 18;
    if (const char* env = std::getenv("OPENBULLY_NPC_COUNT")) {
        const int n = std::atoi(env);
        if (n >= 0) count = uint32_t(n);
    }

    // Deterministic PRNG (xorshift) so the scatter is reproducible.
    uint32_t rng = 0x1234ABCDu;
    uint32_t patrolCount = 0;  // Track 27: NPCs with a patrol route
    uint32_t prefectCount = 0; // Track 28: authority NPCs
    auto next = [&rng]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    auto unit = [&next]() { return float(next() % 10000u) / 10000.0f; }; // [0,1)

    // Track 29: with schedules loaded, the crowd is schedule-driven — each
    // NPC slot adopts one schedule and UpdateSchedules creates/destroys the
    // entity as windows open/close (spawn/despawn events are logged). The
    // legacy instant crowd below stays as the fallback when schedules are
    // unavailable, preserving Phase-5 behavior.
    if (m_schedules.ScheduleCount() > 0) {
        const size_t schedCount = m_schedules.ScheduleCount();
        m_scheduledNpcs.clear();
        m_scheduledNpcs.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            ScheduledNpc sn;
            sn.scheduleIndex = int(i % schedCount);
            sn.modelIndex    = i % m_npcModels.size();
            sn.seed = 0x9E3779B9u * (i + 1u) ^ 0x1234ABCDu;
            // Home base: the schedule's first SPAWNED window's spawn_point
            // (JSON refs resolved at load); jitter spreads the crowd.
            sn.spawnPoint = center;
            if (const NPCSchedule* sched =
                    m_schedules.GetSchedule(size_t(sn.scheduleIndex))) {
                for (const ScheduleWindow& w : sched->windows)
                    if (w.state == SpawnState::Spawned && w.hasSpawnPoint) {
                        sn.spawnPoint = w.spawnPoint;
                        break;
                    }
            }
            const float jAng  = unit() * 6.2831853f;
            const float jDist = 1.0f + unit() * 4.0f;
            sn.spawnPoint += glm::vec3(std::cos(jAng) * jDist, 0.0f,
                                       std::sin(jAng) * jDist);
            m_scheduledNpcs.push_back(sn);
        }
        Logger::Info("NPCs: %u schedule slots across %zu schedule types",
                     count, schedCount);
        UpdateSchedules(); // spawn whoever's window is active right now
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        // Scatter on a ring + jitter around the anchor center: spreads the
        // crowd across the level instead of piling them in one spot.
        const float ang  = unit() * 6.2831853f;
        const float dist = 4.0f + unit() * 16.0f;
        glm::vec3 pos = center + glm::vec3(std::cos(ang) * dist, 0.0f,
                                           std::sin(ang) * dist);

        // Ground snap: cast down from just above the center's surface level
        // (cast from very high would pierce through hollow geometry, e.g. a
        // rooftop, and land NPCs on interior floors below the play space).
        // A miss keeps the center height (never drop NPCs through the world).
        if (m_collisionWorld) {
            Ray down;
            down.origin[0] = pos.x; down.origin[1] = pos.y + 10.0f; down.origin[2] = pos.z;
            down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
            RayHit hit;
            if (m_collisionWorld->Raycast(down, hit)) pos.y = hit.point[1];
        }

        // Track 17: lazily build the model's skinned rig on first use (all
        // instances share it; static NpcModel mesh stays as fallback).
        NpcModel& model = m_npcModels[i % m_npcModels.size()];
        uint32_t rigIndex = model.rigIndex;
        if (rigIndex == 0xFFFFFFFF && !m_sharedClips.empty() &&
            !model.sourcePath.empty()) {
            const NifLoadResult nif = LoadNifFile(model.sourcePath);
            if (nif.ok) rigIndex = CreateRig(nif);
            model.rigIndex = rigIndex; // cached even on failure (no retry)
            if (rigIndex != 0xFFFFFFFF)
                Logger::Info("NPC '%s': skinned rig %u (%d bones)",
                             model.name.c_str(), rigIndex,
                             m_animRigs[rigIndex].controller.BoneCount());
        }
        // Track 17: per-instance controller clone so each NPC animates
        // independently (GPU mesh + clips are shared with the model's rig).
        if (rigIndex != 0xFFFFFFFF) rigIndex = CloneRig(rigIndex);

        EntityID e = m_entities.Create();
        Transform t;
        t.position = pos;
        t.scale = glm::vec3(model.bodyScale);
        t.rotation.y = unit() * 6.2831853f;
        m_entities.AddTransform(e, t);

        MeshComponent mc;
        mc.isBox = false;
        mc.isNpc = true;
        mc.npcModelIndex = i % m_npcModels.size();
        m_entities.AddMesh(e, mc);

        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
        cc.boundsMax = glm::vec3( 0.5f, 1.8f,  0.5f);
        cc.layerMask = 2u; // dynamic layer
        m_entities.AddCollision(e, cc);

        PhysicsComponent pc;
        pc.gravity = -9.8f;
        m_entities.AddPhysics(e, pc);

        AIComponent ai;
        ai.homePos = pos;
        ai.facingYaw = t.rotation.y;
        ai.lastPos = pos; // Track 27: stuck-detection baseline
        // Mix: ~1/3 linger idle, 2/3 head out after a short pause (targets
        // are picked through the normal IDLE path so ground checks apply).
        ai.state = (i % 3 == 0) ? AIState::Idle : AIState::Wander;
        ai.timer = 0.2f + unit() * 2.8f;
        ai.moveSpeed = 1.0f + unit() * 1.0f; // 1.0-2.0 u/s
        ai.wanderRadius = 3.0f;              // stay close on uneven geometry
        // Track 27: every 4th NPC walks a patrol loop — 4 ground-snapped
        // waypoints on a square around its spawn (3.5-5.5u radius, random
        // phase). Waypoints stay on the spawn plane; the walk re-snaps.
        if (i % 4 == 0) {
            const float radius = 3.5f + unit() * 2.0f;
            const float phase  = unit() * 6.2831853f;
            ai.patrolPoints.clear();
            ai.patrolPoints.reserve(4);
            for (int w = 0; w < 4; ++w) {
                const float a = phase + float(w) * 1.5707963f;
                glm::vec3 wp(pos.x + std::cos(a) * radius, pos.y,
                             pos.z + std::sin(a) * radius);
                if (m_collisionWorld) {
                    Ray down;
                    down.origin[0] = wp.x; down.origin[1] = pos.y + 10.0f;
                    down.origin[2] = wp.z;
                    down.direction[0] = 0.0f; down.direction[1] = -1.0f;
                    down.direction[2] = 0.0f;
                    RayHit hit;
                    if (m_collisionWorld->Raycast(down, hit) &&
                        std::fabs(hit.point[1] - pos.y) < 3.0f)
                        wp.y = hit.point[1];
                }
                ai.patrolPoints.push_back(wp);
            }
            ai.patrolIndex = 0;
            ++patrolCount;
        }
        // Track 28: designate the first two NPCs as prefects (authorities).
        // They react to witnessed fights by chasing + attacking the aggressor,
        // and BUSTED fires if a prefect defeats the player. Uses modelIndex
        // 0 (model 0 = first loaded character) so the pair shares a distinct
        // look from the crowd whenever more than one model exists.
        if (prefectCount < 2 && m_prefectIds[1].index == 0xFFFFFFFF) {
            ai.role = AIRole::Prefect;
            mc.npcModelIndex = 0;
            ai.sightRadius = 14.0f; // authorities keep a wider watch
            m_prefectIds[prefectCount] = e;
            ++prefectCount;
        }
        m_entities.AddAI(e, ai);
        // Track 23: melee combat state (stun/knockdown/combo on the target).
        m_entities.AddMeleeCombat(e);

        // Track 17: attach the animation state when the model has a rig;
        // start idle so the crowd poses immediately.
        if (rigIndex != 0xFFFFFFFF) {
            AnimationComponent ac;
            ac.rigIndex = rigIndex;
            ac.playing = true;
            ac.wasMoving = false;
            m_entities.AddAnimation(e, ac);
            AnimRig& rig = m_animRigs[rigIndex];
            if (rig.idleClip >= 0) rig.controller.Play(rig.idleClip, true, 0.0f);
        }

        m_npcCount++;
    }
    Logger::Info("NPCs: %u spawned around (%.1f, %.1f, %.1f) from %zu models "
                 "(%u on patrol, %u prefects)",
                 m_npcCount, center.x, center.y, center.z, m_npcModels.size(),
                 patrolCount, prefectCount);
}

// Track 16/27: NPC AI update. States:
//   IDLE    stand still; timer expires -> PATROL (when a route exists) or
//           pick a WANDER target.
//   WANDER  walk toward targetPos (ground raycast + wall resolve like
//           Jimmy); arrive -> IDLE for a random duration.
//   PATROL  Track 27: walk the patrolPoints loop in order; pause briefly at
//           each waypoint (through IDLE) and loop forever.
//   ALERT   Track 27: player seen within sightRadius (10u) + line of sight;
//           face them for a brief pause, then CHASE (or RETURN when lost).
//   CHASE   Track 27: direct steering toward the player (collision resolve
//           + sidestep after 2s without progress); ATTACK in melee range,
//           RETURN once the player has been unseen for 5s.
//   ATTACK  Track 27: in melee range (1.8u); face + swing through the Track
//           23 melee path with a 1.5s cooldown; CHASE when they drift out.
//   FLEE    Track 27: health below 20% while the player is near -> run away.
//   RETURN  Track 27: walk back to the patrol route / home, then IDLE.
void Engine::UpdateNpcs(float dt) {
    if (dt <= 0.0f) return;

    // Track 28: drain reaction events queued by combat/projectile code, then
    // scan for new witnesses (their events process next frame).
    ProcessAIEvents();
    ScanFightWitnesses();

    // Player position for perception/combat (Jimmy's feet transform).
    glm::vec3 playerPos(0.0f);
    bool havePlayer = false;
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        if (const Transform* jt = m_entities.GetTransform(m_jimmyId)) {
            playerPos = jt->position;
            havePlayer = true;
        }
    }

    auto shortestArc = [](float from, float to) {
        float d = to - from;
        while (d >  3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        return d;
    };

    // Track 27: line of sight — raycast from the NPC's chest to the player's
    // eye; a hit landing clearly short of the target means a wall between.
    auto hasLineOfSight = [this](const glm::vec3& from, const glm::vec3& to) {
        if (!m_collisionWorld) return true;
        const glm::vec3 delta = to - from;
        const float dist = glm::length(delta);
        if (dist < 1e-4f) return true;
        Ray ray;
        ray.origin[0] = from.x; ray.origin[1] = from.y; ray.origin[2] = from.z;
        ray.direction[0] = delta.x / dist;
        ray.direction[1] = delta.y / dist;
        ray.direction[2] = delta.z / dist;
        RayHit hit;
        if (!m_collisionWorld->Raycast(ray, hit)) return true;
        return hit.t >= dist - 0.4f; // hit past the target = clear
    };

    // Track 27: one movement step along `dir` at `speed` with the same ground
    // snap + wall sphere the wander walk uses. False = not walkable there.
    auto stepMove = [this](const glm::vec3& pos, const glm::vec3& dir,
                           float speed, float dt, glm::vec3& outPos) {
        glm::vec3 newPos = pos + dir * (speed * dt);
        float groundY = pos.y;
        bool walkable = false;
        if (m_collisionWorld) {
            Ray down;
            down.origin[0] = newPos.x; down.origin[1] = newPos.y + 4.0f;
            down.origin[2] = newPos.z;
            down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
            RayHit hit;
            if (m_collisionWorld->Raycast(down, hit)) {
                if (std::fabs(hit.point[1] - pos.y) < 3.0f) { // same-plane guard
                    groundY = hit.point[1];
                    walkable = true;
                }
            } else {
                groundY = pos.y;      // nothing below: keep walking level
                walkable = true;
            }
            // Wall resolve: 0.5-radius sphere like the player.
            const float in[3] = { newPos.x, newPos.y + 0.5f, newPos.z };
            float out[3];
            if (m_collisionWorld->ResolveSphere(in, 0.5f, out))
                newPos = glm::vec3(out[0], newPos.y, out[2]);
        } else {
            walkable = true; // no collision world: free movement
        }
        if (!walkable) return false;
        outPos = glm::vec3(newPos.x, groundY, newPos.z);
        return true;
    };

    m_entities.ForEachAI([&](EntityID id, Transform& t, AIComponent& ai) {
        // Track 28: KO'd NPCs fall over, lie down, then respawn. Runs before
        // every other branch - a downed NPC is out of the AI rotation.
        if (ai.state == AIState::Ko) {
            ai.fallTimer += dt;
            const float k = std::min(1.0f, ai.fallTimer / kFallTime);
            t.rotation.x = -1.5707963f * (k * k); // ease-in fall onto the back
            ai.koTimer -= dt;
            if (ai.koTimer <= 0.0f) RespawnNpc(id, ai, t);
            UpdateEntityAnimation(id, false, dt);
            return;
        }

        // Track 28: procedural flinch lean (C_Player.agr has no named
        // hit-reaction clip available - MAINPED.HXD is not extracted - so the
        // hit reaction is this recoil + the melee stun freeze).
        if (ai.flinchTimer > 0.0f) {
            ai.flinchTimer -= dt;
            if (ai.flinchTimer <= 0.0f) {
                ai.flinchTimer = 0.0f;
                t.rotation.x = 0.0f;
            } else {
                const float k = 1.0f - ai.flinchTimer / kFlinchTime;
                t.rotation.x = -std::sin(k * 3.14159265f) * 0.35f;
            }
        }

        // Track 24: grappled NPCs are frozen (held at arm's length by
        // Engine::UpdateGrapple); only the walk/idle driver runs.
        if (id == m_grappleTarget) {
            UpdateEntityAnimation(id, false, dt);
            return;
        }
        // Track 30: the NPC in conversation is frozen and keeps facing the
        // player (UpdateDialogue clears the handle on conversation end).
        if (id == m_dialogueNpc) {
            if (havePlayer) {
                const glm::vec3 toP = playerPos - t.position;
                if (glm::length(glm::vec2(toP.x, toP.z)) > 0.01f) {
                    const float targetYaw = std::atan2(toP.x, toP.z);
                    ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                                    std::min(1.0f, ai.turnSpeed * dt);
                    t.rotation.y = ai.facingYaw;
                }
            }
            UpdateEntityAnimation(id, false, dt); // idle in place
            return;
        }
        // Track 27: the NPC's own attack progresses here (rooted swing);
        // stunned/knocked-down NPCs skip AI (the melee tick restores them).
        if (Melee::CombatComponent* nmc = m_entities.GetMeleeCombat(id)) {
            if (nmc->state == Melee::CombatState::Attacking) {
                ProgressNpcAttack(id, nmc, dt);
                UpdateEntityAnimation(id, false, dt);
                return;
            }
            if (nmc->state != Melee::CombatState::Idle) {
                UpdateEntityAnimation(id, false, dt);
                return;
            }
        }
        const glm::vec3 pos = t.position;
        bool moving = false; // Track 17: drive the walk/idle state machine

        // Track 28: reaction target. An event-assigned entity (attacker /
        // aggressor) takes priority; without one the Track 27 target is the
        // player. Engaged states (ALERT/CHASE/ATTACK/FLEE) work against
        // targetPos so NPC-vs-NPC reactions (fight back, help calls, prefect
        // busts) reuse the same steering + attack path.
        const Transform* tgtT = ai.targetEntity.IsValid()
            ? m_entities.GetTransform(ai.targetEntity) : nullptr;
        if (ai.targetEntity.IsValid() && !tgtT)
            ai.targetEntity = EntityID{}; // stale handle: drop back to player
        const bool haveTgt = tgtT != nullptr;
        const glm::vec3 targetPos = tgtT ? tgtT->position : playerPos;
        // --- Perception (Track 27) ---------------------------------------
        const float distP = havePlayer
            ? glm::length(glm::vec3(playerPos.x - pos.x, 0.0f,
                                    playerPos.z - pos.z))
            : 1e9f;
        const float distT = haveTgt
            ? glm::length(glm::vec3(targetPos.x - pos.x, 0.0f,
                                    targetPos.z - pos.z))
            : distP;
        const bool seen = havePlayer && distP <= ai.sightRadius &&
            hasLineOfSight(pos + glm::vec3(0.0f, 1.6f, 0.0f),
                           playerPos + glm::vec3(0.0f, 1.4f, 0.0f));
        // Track 28: target-aware sight. With an event-assigned entity the LOS
        // check runs against it; WITHOUT one the player is the target (Track
        // 27 semantics), so `seenT` must fall back to `seen` — gating sight
        // on haveTgt alone made every player-targeted NPC flap ALERT↔RETURN
        // (sight "lost" it never had) instead of committing to CHASE.
        const bool seenT = haveTgt
            ? (distT <= ai.sightRadius &&
               HasLineOfSight(pos + glm::vec3(0.0f, 1.6f, 0.0f),
                              targetPos + glm::vec3(0.0f, 1.4f, 0.0f)))
            : seen;

        const bool engaged = ai.state == AIState::Alert ||
                             ai.state == AIState::Chase ||
                             ai.state == AIState::Attack;
        if (engaged) {
            if (seenT) ai.loseTimer = 0.0f;   // Track 28: target-aware sight
            else      ai.loseTimer += dt;
        } else {
            ai.loseTimer = 0.0f;
        }

        // Attack cooldown ticks between swings (any state; the swing itself
        // early-returns above, so a 1.5s gap is guaranteed after each attack).
        ai.attackCdTimer -= dt;

        // State transition helper: logs every change once (verification).
        auto setState = [&](AIState next) {
            if (ai.state == next) return;
            Logger::Info("NPC AI: entity %u %s -> %s (dist %.1f)", id.index,
                         AiStateName(ai.state), AiStateName(next),
                         distP < 1e8f ? distP : -1.0f);
            ai.state = next;
        };

        // ANY -> FLEE: health below the flee threshold while the danger
        // (player) is near. Re-arms whenever the player gets close again.
        if (ai.state != AIState::Flee && ai.health.max > 0 &&
            ai.health.Fraction() < ai.fleeHealthFrac && distT <= ai.sightRadius) {
            const glm::vec3 away = pos - targetPos;
            const float len = glm::length(glm::vec3(away.x, 0.0f, away.z));
            if (len > 1e-4f) ai.fleeDir = glm::vec3(away.x, 0.0f, away.z) / len;
            ai.stuckTimer = 0.0f; ai.sidestepTimer = 0.0f;
            setState(AIState::Flee);
        }

        // Detection: roaming states notice the player -> ALERT (face + pause).
        // Track 28: event-driven reactions skip this gate (their events put
        // the NPC straight into ALERT/CHASE with targetEntity already set).
        const bool roaming = ai.state == AIState::Idle ||
                             ai.state == AIState::Wander ||
                             ai.state == AIState::FacePlayer ||
                             ai.state == AIState::Patrol ||
                             ai.state == AIState::Return;
        if (roaming && seen) {
            ai.alertTimer = ai.alertDuration;
            setState(AIState::Alert);
        }

        // Shared stuck/sidestep steering (CHASE + RETURN): straight at the
        // goal; after `stuckTime` seconds without progress, sidestep
        // perpendicular for a moment and retry (simple pathfinding).
        auto steer = [&](const glm::vec3& dirIn, float speed) {
            glm::vec3 dir = dirIn;
            if (ai.sidestepTimer > 0.0f) {
                dir = ai.sidestepDir;
                ai.sidestepTimer -= dt;
                if (ai.sidestepTimer <= 0.0f) {
                    ai.stuckTimer = 0.0f;
                    ai.lastPos = pos;
                }
                return dir;
            }
            const float expected = speed * dt + 1e-5f;
            const float moved = glm::length(pos - ai.lastPos);
            ai.lastPos = pos;
            if (moved < expected * 0.4f) ai.stuckTimer += dt;
            else ai.stuckTimer = std::max(0.0f, ai.stuckTimer - dt * 2.0f);
            if (ai.stuckTimer >= ai.stuckTime) {
                const float side = nextRandom01() < 0.5f ? -1.0f : 1.0f;
                ai.sidestepDir = glm::vec3(-dirIn.z, 0.0f, dirIn.x) * side;
                ai.sidestepTimer = 0.7f;
                ai.stuckTimer = 0.0f;
                Logger::Info("NPC AI: entity %u blocked -> sidestep", id.index);
            }
            return dir;
        };

        switch (ai.state) {
        case AIState::Idle:
            ai.timer -= dt;
            // Track 29: schedule behavior override — pinned NPCs stay put
            // (idle at their post; no patrol/wander target picking).
            if (ai.lockIdle) { ai.timer = 1.0f; break; }
            if (ai.timer <= 0.0f) {
                // Track 27: walk the patrol route when one was assigned.
                if (!ai.patrolPoints.empty()) {
                    ai.targetPos =
                        ai.patrolPoints[ai.patrolIndex % ai.patrolPoints.size()];
                    ai.targetPos.y = pos.y; // mover re-snaps each frame
                    ai.hasTarget = true;
                    setState(AIState::Patrol);
                    break;
                }
                // Pick a random nearby point around home.
                const float ang  = float(nextRandom01()) * 6.2831853f;
                const float dist = 1.5f + float(nextRandom01()) * ai.wanderRadius;
                ai.targetPos = ai.homePos +
                    glm::vec3(std::cos(ang) * dist, 0.0f, std::sin(ang) * dist);
                // Snap the target onto the ground so arrival checks work.
                // If the ray misses (walked off a rooftop edge), cancel the
                // trip: re-target around home from the CURRENT position so
                // NPCs never walk into thin air.
                bool snapped = false;
                if (m_collisionWorld) {
                    Ray down;
                    down.origin[0] = ai.targetPos.x;
                    // Cast from just above the NPC's CURRENT plane: a +500
                    // ray pierces hollow buildings and returns a different
                    // roof's top face, rejecting every target.
                    down.origin[1] = pos.y + 10.0f;
                    down.origin[2] = ai.targetPos.z;
                    down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
                    RayHit hit;
                    if (m_collisionWorld->Raycast(down, hit)) {
                        // Reject targets on a different surface level (>3u
                        // away): NPCs stay on the surface they spawned on.
                        if (std::fabs(hit.point[1] - pos.y) < 3.0f) {
                            ai.targetPos.y = hit.point[1];
                            snapped = true;
                        }
                    } else {
                        // No geometry below the target: walk it level (the
                        // wander move-path handles misses the same way and
                        // the wall sphere keeps the NPC out of solids).
                        ai.targetPos.y = pos.y;
                        snapped = true;
                    }
                }
                if (!snapped) {
                    ai.hasTarget = false; ai.timer = 1.0f; break;
                }
                ai.hasTarget = true;
                setState(AIState::Wander);
            }
            break;

        case AIState::Wander: {
            if (!ai.hasTarget) { setState(AIState::Idle); ai.timer = 1.0f; break; }
            glm::vec3 to = ai.targetPos - pos;
            to.y = 0.0f;
            const float dist = glm::length(to);
            if (dist < 0.25f) {
                setState(AIState::Idle);
                ai.timer = ai.idleTimeMin + float(nextRandom01()) *
                           (ai.idleTimeMax - ai.idleTimeMin);
                break;
            }
            const glm::vec3 dir = to / dist;
            glm::vec3 newPos;
            if (!stepMove(pos, dir, ai.moveSpeed, dt, newPos)) {
                // walk-off rejected: idle here, pick again later
                setState(AIState::Idle);
                ai.timer = 1.0f;
                break;
            }
            t.position = newPos;
            moving = true; // Track 17: NPC is walking

            // Face the walk direction (smooth shortest-arc turn).
            const float targetYaw = std::atan2(dir.x, dir.z);
            ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                            std::min(1.0f, ai.turnSpeed * dt);
            t.rotation.y = ai.facingYaw;
            break;
        }

        case AIState::FacePlayer:
            // Legacy Track 16 soft-face: superseded by ALERT (sightRadius
            // >= faceRadius); falling through here resumes idling.
            setState(AIState::Idle);
            ai.timer = 0.5f;
            break;

        case AIState::Patrol: {
            if (!ai.hasTarget || ai.patrolPoints.empty()) {
                setState(AIState::Idle); ai.timer = 1.0f; break;
            }
            glm::vec3 to = ai.targetPos - pos;
            to.y = 0.0f;
            const float dist = glm::length(to);
            if (dist < 0.35f) {
                // Waypoint reached: loop to the next one after a short pause
                // (the pause runs through IDLE so ground checks re-apply).
                ai.patrolIndex = (ai.patrolIndex + 1) % ai.patrolPoints.size();
                setState(AIState::Idle);
                ai.timer = 0.4f + float(nextRandom01()) * 1.2f;
                break;
            }
            const glm::vec3 dir = to / dist;
            glm::vec3 newPos;
            if (!stepMove(pos, dir, ai.moveSpeed, dt, newPos)) {
                setState(AIState::Idle);
                ai.timer = 1.0f;
                break;
            }
            t.position = newPos;
            moving = true;
            const float targetYaw = std::atan2(dir.x, dir.z);
            ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                            std::min(1.0f, ai.turnSpeed * dt);
            t.rotation.y = ai.facingYaw;
            break;
        }

        case AIState::Alert:
            moving = false;
            if (haveTgt) { // face the intruder (Track 28: reaction target first)
                const float targetYaw = std::atan2(targetPos.x - pos.x,
                                                   targetPos.z - pos.z);
                ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                                std::min(1.0f, ai.turnSpeed * dt);
                t.rotation.y = ai.facingYaw;
            }
            if (!seenT && ai.loseTimer > 0.6f) { // lost interest
                ai.stuckTimer = 0.0f; ai.sidestepTimer = 0.0f; ai.lastPos = pos;
                setState(AIState::Return);
                break;
            }
            ai.alertTimer -= dt;
            // Track 28: crowd watchers transition straight to WATCH once the
            // pause expires, but ONLY genuine Track 28 crowd slots (formation
            // target + a fightPos matching a hot fight record). Everything
            // else — Track 27 player sighting (no target entity, no slot) and
            // entity-targeted reactions — goes to CHASE. Treating a plain
            // player sighting as a crowd slot made NPCs bounce
            // ALERT -> WATCH -> RETURN forever (WATCH disperses because no
            // hot fight exists at the slot) and never chase/attack.
            if (ai.alertTimer <= 0.0f &&
                (seenT || haveTgt || ai.hasTarget)) {
                bool crowdSlot = ai.hasTarget && !ai.targetEntity.IsValid();
                if (crowdSlot) {
                    crowdSlot = false;
                    for (const FightInfo& f : m_fights) {
                        if (glm::distance(f.pos, ai.fightPos) < 2.0f) {
                            crowdSlot = true;
                            break;
                        }
                    }
                }
                ai.stuckTimer = 0.0f; ai.sidestepTimer = 0.0f; ai.lastPos = pos;
                // Plain player sighting: stare, then disperse — do NOT auto-
                // chase. Track 28 reactions (fight-back, help calls, prefect
                // response) drive CHASE; ambient sight-chase pinned every NPC
                // in ATTACK on the player and made Track 30's talk-to-calm-NPC
                // unreachable (retail Bully NPCs gather, they don't swarm).
                setState(crowdSlot ? AIState::Watch : AIState::Return);
            }
            break;

        case AIState::Chase: {
            if (ai.loseTimer > ai.loseSightTime) { // target lost > 5s
                ai.stuckTimer = 0.0f; ai.sidestepTimer = 0.0f; ai.lastPos = pos;
                setState(AIState::Return);
                break;
            }
            if (distT <= ai.attackRange) { setState(AIState::Attack); break; }
            glm::vec3 to = targetPos - pos;
            to.y = 0.0f;
            const float dist = glm::length(to);
            if (dist < 1e-4f) break;
            const float speed = ai.moveSpeed * ai.chaseSpeedMult;
            const glm::vec3 dir = steer(to / dist, speed);
            glm::vec3 newPos;
            if (stepMove(pos, dir, speed, dt, newPos)) {
                t.position = newPos;
                moving = true;
            }
            // Face the run direction (or the blocked-heading while sliding).
            const float targetYaw = std::atan2(dir.x, dir.z);
            ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                            std::min(1.0f, ai.turnSpeed * dt);
            t.rotation.y = ai.facingYaw;
            break;
        }

        case AIState::Attack: {
            if (ai.loseTimer > ai.loseSightTime) {
                ai.stuckTimer = 0.0f; ai.sidestepTimer = 0.0f; ai.lastPos = pos;
                setState(AIState::Return);
                break;
            }
            if (distT > ai.attackRange * 1.3f) { // target drifted out
                ai.stuckTimer = 0.0f; ai.sidestepTimer = 0.0f; ai.lastPos = pos;
                setState(AIState::Chase);
                break;
            }
            moving = false; // rooted swing
            // Player fallback (Track 27): without an event-assigned target
            // entity the ATTACK target is the player (`targetPos` already
            // equals playerPos then); gating on haveTgt alone made player-
            // targeted NPCs stand in range without ever swinging.
            if (haveTgt || havePlayer) {
                const float targetYaw = std::atan2(targetPos.x - pos.x,
                                                   targetPos.z - pos.z);
                ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                                std::min(1.0f, ai.turnSpeed * dt);
                t.rotation.y = ai.facingYaw;
                if (ai.attackCdTimer <= 0.0f) {
                    StartNpcAttack(id, Melee::AttackType::LightPunch, targetYaw);
                    ai.attackCdTimer = ai.attackCooldown; // 1.5s (task spec)
                }
            }
            break;
        }

        case AIState::Flee: {
            // Far enough from the threat -> give up. Distance is measured to
            // the event-assigned target when there is one, otherwise to the
            // player (Track 27): gating on !haveTgt alone made every
            // player-targeted NPC bounce FLEE<->RETURN in place.
            const float safeDist = ai.sightRadius * 1.8f;
            const float effDist = haveTgt ? distT : distP;
            if (effDist > safeDist) {
                ai.stuckTimer = 0.0f; ai.sidestepTimer = 0.0f; ai.lastPos = pos;
                setState(AIState::Return);
                break;
            }
            // Keep the run direction away from the threat (Track 28:
            // threatPos survives target invalidation; default is the target).
            const glm::vec3 threat = ai.targetEntity.IsValid()
                ? targetPos : ai.threatPos;
            const glm::vec3 away = pos - threat;
            const float len = glm::length(glm::vec3(away.x, 0.0f, away.z));
            if (len > 1e-4f)
                ai.fleeDir = glm::vec3(away.x, 0.0f, away.z) / len;
            const float speed = ai.moveSpeed * ai.chaseSpeedMult * 1.25f;
            glm::vec3 newPos;
            if (stepMove(pos, ai.fleeDir, speed, dt, newPos)) {
                t.position = newPos;
                moving = true;
            }
            const float targetYaw = std::atan2(ai.fleeDir.x, ai.fleeDir.z);
            ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                            std::min(1.0f, ai.turnSpeed * dt);
            t.rotation.y = ai.facingYaw;
            break;
        }

        case AIState::Watch: {
            // Track 28 crowd behavior: stand at the formation slot (4-6 u
            // from the fight) and face it; disperse 10 s after the fight's
            // last activity (the fight record goes cold).
            bool hot = false;
            for (const FightInfo& f : m_fights) {
                if (glm::distance(f.pos, ai.fightPos) < 2.0f &&
                    m_time - f.lastActivity < kFightActiveTime) { hot = true; break; }
            }
            if (!hot) {
                ai.stuckTimer = 0.0f; ai.sidestepTimer = 0.0f; ai.lastPos = pos;
                ai.targetEntity = EntityID{};
                setState(AIState::Return);
                break;
            }
            if (ai.hasTarget) {
                glm::vec3 to = ai.targetPos - pos; to.y = 0.0f;
                const float dist = glm::length(to);
                if (dist > 0.35f) {
                    const glm::vec3 dir = to / dist;
                    glm::vec3 newPos;
                    if (stepMove(pos, dir, ai.moveSpeed, dt, newPos)) {
                        t.position = newPos;
                        moving = true;
                    }
                    const float targetYaw = std::atan2(dir.x, dir.z);
                    ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                                    std::min(1.0f, ai.turnSpeed * dt);
                    t.rotation.y = ai.facingYaw;
                    break;
                }
            }
            // At the slot (or the walk failed): stand and watch the fight.
            glm::vec3 toF = ai.fightPos - pos; toF.y = 0.0f;
            if (glm::length(toF) > 0.01f) {
                const float targetYaw = std::atan2(toF.x, toF.z);
                ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                                std::min(1.0f, ai.turnSpeed * dt);
                t.rotation.y = ai.facingYaw;
            }
            break;
        }

        case AIState::Return: {
            // Goal: the current patrol waypoint, or home when roaming free.
            glm::vec3 goal = ai.patrolPoints.empty()
                ? ai.homePos
                : ai.patrolPoints[ai.patrolIndex % ai.patrolPoints.size()];
            glm::vec3 to = goal - pos;
            to.y = 0.0f;
            const float dist = glm::length(to);
            if (dist < 0.4f) { // reached spawn/patrol route -> resume
                setState(AIState::Idle);
                ai.timer = 0.8f;
                break;
            }
            const glm::vec3 dir = steer(to / dist, ai.moveSpeed);
            glm::vec3 newPos;
            if (stepMove(pos, dir, ai.moveSpeed, dt, newPos)) {
                t.position = newPos;
                moving = true;
            }
            const float targetYaw = std::atan2(dir.x, dir.z);
            ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                            std::min(1.0f, ai.turnSpeed * dt);
            t.rotation.y = ai.facingYaw;
            break;
        }
        }

        // Track 17: play walk while moving, idle otherwise (crossfaded).
        UpdateEntityAnimation(id, moving, dt);
    });
}

// --- Track 17: skeletal animation ---------------------------------------------

// Loads Anim/C_Player.agr (the player/ped animation group; contains the walk
// cycles and idles) and builds the shared character rig from a skinned NIF.
// Walk/idle clip picks: the first decoded cycling animation that travels
// (walk) and the first that does not (idle), so the choice is data-driven
// rather than name-matched (group contents verified in docs/agr.md).
bool Engine::LoadAnimationRig() {
    if (!m_animRigs.empty()) return true; // already loaded

    // Scan every .agr in the extracted animation directory. C_Player.agr
    // carries the idles; the F_* groups carry the walk cycles (their ARROW
    // position tracks travel 1.0-3.5 m per cycle, docs/agr.md).
    // All ped clips are type-1002 with 36 bones.
    auto loadDir = [this](const char* dir) -> size_t {
        DIR* dp = ::opendir(dir);
        if (!dp) return 0;
        std::vector<std::string> names;
        while (dirent* ent = ::readdir(dp)) {
            std::string n = ent->d_name;
            if (n.size() > 4 && n.substr(n.size() - 4) == ".agr")
                names.push_back(n);
        }
        ::closedir(dp);
        std::sort(names.begin(), names.end());
        size_t total = 0;
        for (const std::string& n : names) {
            const std::string path = std::string(dir) + "/" + n;
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) continue;
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (sz <= 0) { std::fclose(f); continue; }
            std::vector<uint8_t> buf(static_cast<size_t>(sz));
            const size_t got = ::fread(buf.data(), 1, buf.size(), f);
            std::fclose(f);
            if (got != buf.size()) continue;
            std::vector<AgrAnim> anims;
            if (!AgrLoadGroup(buf.data(), buf.size(), anims)) continue;
            size_t decoded = 0;
            for (AgrAnim& a : anims) if (a.decoded) decoded++;
            Logger::Info("AGR: %s -> %zu/%zu animations decoded",
                         n.c_str(), decoded, anims.size());
            total += decoded;
            for (AgrAnim& a : anims)
                if (a.decoded) m_sharedClips.push_back(std::move(a));
        }
        return total;
    };
    size_t loaded = loadDir("data/extracted/anim");
    if (loaded == 0) loaded = loadDir("../data/extracted/anim");

    // Track 24: grapple animation group. The ped grapple clips stream from
    // World.img into "grap*" .agr groups; the .agr format stores no names,
    // so availability is filename-based. First match becomes the grapple
    // idle clip (played on both rigs); no match = pose-freeze fallback.
    {
        const char* dirs[] = { "data/extracted/anim", "../data/extracted/anim" };
        for (const char* dir : dirs) {
            DIR* dp = ::opendir(dir);
            if (!dp) continue;
            std::string found;
            while (dirent* ent = ::readdir(dp)) {
                std::string n = ent->d_name;
                std::string low = n;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
                if (n.size() > 4 && n.substr(n.size() - 4) == ".agr" &&
                    low.find("grap") != std::string::npos) { found = n; break; }
            }
            ::closedir(dp);
            if (found.empty()) continue;
            const std::string path = std::string(dir) + "/" + found;
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) continue;
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (sz <= 0) { std::fclose(f); continue; }
            std::vector<uint8_t> buf(static_cast<size_t>(sz));
            if (::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
                std::fclose(f);
                continue;
            }
            std::fclose(f);
            std::vector<AgrAnim> anims;
            if (AgrLoadGroup(buf.data(), buf.size(), anims)) {
                for (AgrAnim& a : anims) {
                    if (!a.decoded) continue;
                    m_grappleClipIndex = int(m_sharedClips.size());
                    m_sharedClips.push_back(std::move(a));
                    break;
                }
            }
            if (m_grappleClipIndex >= 0) {
                m_grappleAnimLoaded = true;
                Logger::Info("AGR: grapple clip #%d from %s",
                             m_grappleClipIndex, found.c_str());
                break;
            }
        }
        if (!m_grappleAnimLoaded)
            Logger::Info("AGR: no grapple .agr group found; "
                         "grapple uses the pose-freeze fallback");
    }
    if (m_sharedClips.empty()) {
        Logger::Warning("LoadAnimationRig: no .agr animations decoded; "
                        "characters will be static");
        return false;
    }
    Logger::Info("AGR: %zu animations total", loaded);

    // Pick walk + idle from the merged clip list. Walk: an ARROW (ground
    // travel) track of >0.5 m per cycle; prefer the travel closest to a
    // natural step (1.0-1.6 m). Idle: an in-place clip longer than 1 s.
    int walkIdx = -1, idleIdx = -1;
    float walkTravel = 0.0f;
    for (size_t i = 0; i < m_sharedClips.size(); i++) {
        const AgrAnim& a = m_sharedClips[i];
        if (a.duration <= 0.05f) continue;
        float travel[3];
        AgrTravel(a, travel);
        const float tlen = std::sqrt(travel[0]*travel[0] + travel[1]*travel[1] +
                                     travel[2]*travel[2]);
        if (tlen > 0.5f) {
            const float score = std::fabs(tlen - 1.4f); // ~1.4 m per two steps
            if (walkIdx < 0) { walkIdx = int(i); walkTravel = tlen; m_walkScore = score; }
            else if (score < m_walkScore) { walkIdx = int(i); walkTravel = tlen; m_walkScore = score; }
        } else if (a.duration > 1.0f && idleIdx < 0) {
            idleIdx = int(i); // first long in-place cycle
        }
    }
    if (walkIdx < 0 && idleIdx < 0) {
        Logger::Warning("LoadAnimationRig: no usable walk/idle clip found");
        return false;
    }
    Logger::Info("AGR clips: walk=#%d (travel %.2fm) idle=#%d",
                 walkIdx, walkTravel,
                 idleIdx);
    m_walkTravelM = walkTravel;

    // Shared clip data for rigs: both controllers reference the same decoded
    // animations (already moved into m_sharedClips by loadDir above).
    m_walkClipIndex = walkIdx;
    m_idleClipIndex = idleIdx;
    return true;
}

// Builds one rig from a skinned NIF: skeleton + skinned GPU mesh + clips.
// Returns the rig pool index, or 0xFFFFFFFF on failure.
uint32_t Engine::CreateRig(const NifLoadResult& nif) {
    // Skeleton root: the first valid skin's root.
    uint32_t skeletonRoot = 0xFFFFFFFF;
    for (const NifGeometryData& geo : nif.geometries) {
        if (geo.skinIndex != 0xFFFFFFFF && geo.skinIndex < nif.skins.size() &&
            nif.skins[geo.skinIndex].skeletonRoot != 0xFFFFFFFF) {
            skeletonRoot = nif.skins[geo.skinIndex].skeletonRoot;
            break;
        }
    }
    if (skeletonRoot == 0xFFFFFFFF) return 0xFFFFFFFF;

    AnimRig rig;
    if (!rig.controller.BuildSkeleton(nif, skeletonRoot)) return 0xFFFFFFFF;
    if (m_walkClipIndex >= 0) {
        rig.walkClip = rig.controller.AddClip("walk", m_sharedClips[size_t(m_walkClipIndex)]);
    }
    if (m_idleClipIndex >= 0) {
        rig.idleClip = rig.controller.AddClip("idle", m_sharedClips[size_t(m_idleClipIndex)]);
    }

    // Track 23: register the melee attack clips (punches/kicks from
    // C_Player.agr) on this rig so any entity can play them.
    if (m_combatClipsLoaded) {
        for (int t = 0; t < Melee::kAttackTypeCount; ++t) {
            const Melee::AttackData& atk = Melee::GetAttackData(Melee::GetAttackType(t));
            if (atk.clipIndex < 0 || atk.clipIndex >= int(m_combatClips.size())) continue;
            rig.attackClips[t] = rig.controller.AddClip(atk.clipName,
                m_combatClips[size_t(atk.clipIndex)], 1.0f);
        }
    }

    SkinnedMesh skin;
    std::vector<std::vector<float>> shapeBinds;
    if (!SkinnedMeshFromNif(nif, skin, &shapeBinds)) return 0xFFFFFFFF;
    // Register one bind palette per skinned shape (draw order matches
    // skin.subMeshes' shapeIndex; see Renderer::DrawMesh3DSubSkinned).
    for (const std::vector<float>& packed : shapeBinds)
        rig.controller.AddShapeBindPacked(packed.data());
    rig.gpu = m_renderer->UploadSkinnedMeshGpu(skin);
    if (!rig.gpu.valid()) return 0xFFFFFFFF;
    for (const MeshSubMesh& sub : skin.subMeshes) {
        WorldMesh::SubRange sr;
        sr.indexOffset = sub.indexOffset;
        sr.indexCount = sub.indexCount;
        sr.texture = AcquireTexture(m_texDir.c_str(), sub.textureName);
        sr.shapeIndex = sub.shapeIndex; // Track 17: per-shape palette pick
        rig.subs.push_back(sr);
    }

    // Body scale: normalize to 1.8u like the static path (NIF bind pose is
    // already standing; indexed Y extent maps to PlayerController::height).
    float ylo = FLT_MAX, yhi = -FLT_MAX;
    for (uint32_t idx : skin.indices) {
        const SkinnedMeshVertex& v = skin.vertices[idx];
        ylo = std::min(ylo, v.y); yhi = std::max(yhi, v.y);
    }
    rig.bodyScale = (yhi > ylo) ? 1.8f / (yhi - ylo) : 1.0f;

    m_animRigs.push_back(std::move(rig));
    return uint32_t(m_animRigs.size() - 1);
}

// Track 17: one controller per animated entity. Clones the source rig's
// controller (skeleton + clips) and SHARES its GPU mesh + sub-ranges, so
// each entity animates independently without re-uploading geometry.
uint32_t Engine::CloneRig(uint32_t sourceRigIndex) {
    if (sourceRigIndex >= m_animRigs.size()) return 0xFFFFFFFF;
    AnimRig clone;
    clone.controller = m_animRigs[sourceRigIndex].controller; // deep copy
    clone.gpu = m_animRigs[sourceRigIndex].gpu;               // shared handle
    clone.subs = m_animRigs[sourceRigIndex].subs;
    clone.bodyScale = m_animRigs[sourceRigIndex].bodyScale;
    clone.walkClip = m_animRigs[sourceRigIndex].walkClip;
    clone.idleClip = m_animRigs[sourceRigIndex].idleClip;
    // Track 27: attack clips registered on the source (CreateRig, when the
    // combat .agr was loaded first) must survive the clone, or NPC-side
    // StartNpcAttack silently no-ops with "clips not loaded".
    for (int t = 0; t < Melee::kAttackTypeCount; ++t)
        clone.attackClips[t] = m_animRigs[sourceRigIndex].attackClips[t];
    m_animRigs.push_back(std::move(clone));
    return uint32_t(m_animRigs.size() - 1);
}

// Per-entity animation state machine: walk when moving, idle when stopped.
// Called from UpdateJimmy / UpdateNpcs with the entity's movement flag.
// The walk clip plays at its authored ground speed (travel per cycle over
// duration); clip playback scales with the entity's actual speed so feet
// stay near the ground (no skating).
void Engine::UpdateEntityAnimation(EntityID id, bool moving, float dt) {
    AnimationComponent* ac = m_entities.GetAnimation(id);
    if (!ac || ac->rigIndex == 0xFFFFFFFF || ac->rigIndex >= m_animRigs.size())
        return;
    AnimRig& rig = m_animRigs[ac->rigIndex];
    AnimationController& ctrl = rig.controller;

    // Track 24: grapple hold. With a loaded grapple clip it plays on both
    // rigs; without one the pose freezes at the current frame (fallback).
    // wasMoving absorbs the current flag so playback resumes cleanly when
    // the grapple ends (EndGrapple also re-Plays idle after a clip hold).
    if (CombatComponent* cc = m_entities.GetCombat(id)) {
        if (cc->state != CombatState::None) {
            ac->wasMoving = moving;
            if (m_grappleClipIndex >= 0) {
                if (!rig.grapplePlaying) {
                    ctrl.Play(m_grappleClipIndex, true, 0.2f);
                    rig.grapplePlaying = true;
                }
                ctrl.Update(dt);
            }
            return;
        }
    }

    // Track 23: while Attacking/Stunned/KnockedDown the walk/idle switcher
    // must not stomp the attack (or hit-reaction) animation. The controller
    // still advances below; UpdateMeleeCombat re-selects idle/walk when the
    // melee state returns to Idle.
    const Melee::CombatComponent* mcom = m_entities.GetMeleeCombat(id);
    const bool meleeLocked = mcom && mcom->state != Melee::CombatState::Idle;

    if (!meleeLocked && (moving != ac->wasMoving || !ctrl.IsPlaying())) {
        if (moving && rig.walkClip >= 0) {
            ctrl.Play(rig.walkClip, true, 0.2f);
            ac->playing = true;
        } else if (!moving && rig.idleClip >= 0) {
            ctrl.Play(rig.idleClip, true, 0.2f);
            ac->playing = true;
        }
        ac->wasMoving = moving;
    }
    if (!meleeLocked) {
    // Speed-match the walk cycle: playback rate = actual speed / authored
    // ground speed (m per cycle over the cycle duration).
    if (moving && rig.walkClip >= 0 && m_walkTravelM > 0.01f) {
        const PhysicsComponent* p = m_entities.GetPhysics(id);
        float speed = p ? glm::length(p->HorizontalVelocity()) : 0.0f;
        const float base = m_walkTravelM /
            std::max(0.05f, m_sharedClips[size_t(m_walkClipIndex)].duration);
        if (AnimClip* clip = ctrl.ClipAt(rig.walkClip))
            clip->speed = std::clamp(speed / base, 0.25f, 4.0f);
    }
    }
    if (ac->playing) ctrl.Update(dt);
}

// Advances every attached controller (entities without a state change this
// frame still need their loops advanced).
void Engine::UpdateAnimations(float dt) {
    m_entities.ForEach([&](EntityID id, Transform&, const EntityManager::EntityRecord&) {
        AnimationComponent* ac = m_entities.GetAnimation(id);
        if (!ac || ac->rigIndex == 0xFFFFFFFF || ac->rigIndex >= m_animRigs.size())
            return;
        m_animRigs[ac->rigIndex].controller.Update(dt);
    });
}

// =========================================================================
// Track 23: melee combat.
// =========================================================================

// Decodes Anim/C_Player.agr (the World.img player animation group) once.
// The .agr carries no names; attack clips are picked by index through the
// MAINPED.HXD dictionary mapping (see combat.h for the verification table:
// 439 group records whose sizes sum to the file size exactly).
bool Engine::LoadCombatAnimations() {
    if (m_combatClipsLoaded) return true;
    for (const char* dir : { "data/extracted/combat", "../data/extracted/combat" }) {
        const std::string path = std::string(dir) + "/C_Player.agr";
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz <= 0) { std::fclose(f); continue; }
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        const size_t got = std::fread(buf.data(), 1, buf.size(), f);
        std::fclose(f);
        if (got != buf.size()) continue;
        if (!AgrLoadGroup(buf.data(), buf.size(), m_combatClips) || m_combatClips.empty())
            continue;
        Logger::Info("Combat anims: %zu decoded from %s (expect 439)",
                     m_combatClips.size(), path.c_str());
        break;
    }
    if (m_combatClips.empty()) {
        Logger::Warning("LoadCombatAnimations: C_Player.agr not found/decodable "
                        "(melee attacks will be pose-only)");
        return false;
    }
    // Clip-index sanity: every attack must point inside the decoded group.
    for (int t = 0; t < Melee::kAttackTypeCount; ++t) {
        const Melee::AttackData& atk = Melee::GetAttackData(Melee::GetAttackType(t));
        if (atk.clipIndex >= int(m_combatClips.size())) {
            Logger::Warning("Combat anims: %s index %d out of range (%zu decoded)",
                            atk.clipName, atk.clipIndex, m_combatClips.size());
            return false;
        }
        Logger::Info("Combat anims: %s = #%d (authored %.3fs)",
                     atk.clipName, atk.clipIndex, atk.authoredDur);
    }
    m_combatClipsLoaded = true;
    return true;
}

// Starts a player attack: locks the melee state machine into Attacking for
// the attack's effective duration, faces camera-forward and one-shot plays
// the attack clip at speed = authored / effective duration.
void Engine::StartMeleeAttack(Melee::AttackType type) {
    if (!m_jimmyId.IsValid()) return;
    Melee::CombatComponent* mc = m_entities.GetMeleeCombat(m_jimmyId);
    PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
    AnimationComponent* ac = m_entities.GetAnimation(m_jimmyId);
    if (!mc || !ac || mc->state != Melee::CombatState::Idle) return;
    if (ac->rigIndex == 0xFFFFFFFF || ac->rigIndex >= m_animRigs.size()) return;
    AnimRig& rig = m_animRigs[ac->rigIndex];
    if (rig.attackClips[int(type)] < 0) return; // clips not loaded

    const Melee::AttackData& atk = Melee::GetAttackData(type);
    mc->state       = Melee::CombatState::Attacking;
    mc->attack      = type;
    mc->attackTimer = atk.duration;
    mc->hitApplied  = false;

    if (pc) { // attack in the direction the camera looks
        pc->facingYaw  = m_camera.GetYaw();
        pc->facingValid = true;
        mc->attackYaw  = pc->facingYaw; // ResolveMeleeHit uses this
    }
    if (AnimClip* clip = rig.controller.ClipAt(rig.attackClips[int(type)])) {
        clip->speed = atk.authoredDur > 0.01f ? atk.authoredDur / atk.duration : 1.0f;
        rig.controller.Play(rig.attackClips[int(type)], /*loop=*/false, 0.1f);
    }
    Logger::Info("Melee: %s started (%.0f dmg, range %.1f, %.2fs)",
                 atk.clipName, atk.damage, atk.range, atk.duration);
}

// Per-frame melee update: input -> attack start, attack progression -> hit
// frames, damage/knockback/stun application, combo bookkeeping, and
// stun/knockdown timers for every melee-carrying entity.
void Engine::UpdateMeleeCombat(float dt) {
    if (dt <= 0.0f) return;
    if (m_meleePopupTimer > 0.0f) m_meleePopupTimer -= dt;

    // --- 1. Jimmy: attack input + progression -------------------------------
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        Melee::CombatComponent* mc = m_entities.GetMeleeCombat(m_jimmyId);
        PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
        AnimationComponent* ac = m_entities.GetAnimation(m_jimmyId);
        if (mc && pc && ac) {
            // Input (LMB light / RMB heavy / Q light kick / E heavy kick).
            // The demo script calls StartMeleeAttack directly instead. RMB is
            // also Track 25's block hold: the heavy attack fires on the press
            // edge, the block state applies while held (both specs coexist).
            static const bool s_demo = std::getenv("OPENBULLY_MELEE_DEMO") != nullptr;
            if (!s_demo && !m_dialogue.IsActive() && pc->equippedSlot < 0 &&
                pc->state != DefenseState::Dodge &&
                mc->state == Melee::CombatState::Idle) {
                if      (m_input.IsPressed(Button::Attack))
                    StartMeleeAttack(Melee::AttackType::LightPunch);
                else if (m_input.IsPressed(Button::Block))
                    StartMeleeAttack(Melee::AttackType::HeavyPunch);
                else if (m_input.WasKeyPressed(SDL_SCANCODE_Q))
                    StartMeleeAttack(Melee::AttackType::LightKick);
                else if (m_input.WasKeyPressed(SDL_SCANCODE_E))
                    StartMeleeAttack(Melee::AttackType::HeavyKick);
            }

            // Progression: tick the timer; inside the hit window run the
            // sphere check ONCE; on expiry return to Idle (miss resets combo).
            if (mc->state == Melee::CombatState::Attacking) {
                const Melee::AttackData& atk = Melee::GetAttackData(mc->attack);
                mc->attackTimer -= dt;
                const float progress = 1.0f - mc->attackTimer / atk.duration;
                if (!mc->hitApplied &&
                    progress >= atk.hitWindowStart && progress <= atk.hitWindowEnd) {
                    mc->hitApplied = true;
                    ResolveMeleeHit(mc, atk);
                }
                if (mc->attackTimer <= 0.0f) {
                    const bool missed = !mc->hitApplied;
                    mc->state = Melee::CombatState::Idle;
                    if (missed) mc->ResetCombo();
                }
            } else if (mc->state == Melee::CombatState::Idle) {
                // Task spec: combo resets after >2 s without a hit.
                if (mc->comboCount > 0) {
                    mc->comboIdleTimer += dt;
                    if (mc->comboIdleTimer > 2.0f) mc->ResetCombo();
                }
            }
        }
    }

    // --- 2. Every melee entity: stun / knockdown timers + knockback drift --
    m_entities.ForEach([&](EntityID id, Transform& t,
                           const EntityManager::EntityRecord&) {
        Melee::CombatComponent* c = m_entities.GetMeleeCombat(id);
        if (!c || id == m_jimmyId) return;
        // Knockback: positional drift (NPC physics is not integrated by
        // UpdateEntities, so the impulse decays here).
        if (glm::dot(c->knockbackVel, c->knockbackVel) > 1e-6f) {
            t.position += c->knockbackVel * dt;
            c->knockbackVel *= std::exp(-8.0f * dt);
        }
        if (c->state == Melee::CombatState::Stunned) {
            c->stunTimer -= dt;
            if (c->stunTimer <= 0.0f) c->state = Melee::CombatState::Idle;
        } else if (c->state == Melee::CombatState::KnockedDown) {
            if (c->downTimer > 0.0f) {
                c->downTimer -= dt;
                if (c->downTimer <= 0.0f) {
                    // Downed for good this demo (health is spent); drop out of
                    // the AI rotation but stay on the floor.
                    c->downTimer = 0.0f;
                }
            }
        }
    });
}

// The attacker's hit frame fired: find the enemy the forward sphere touches
// (sphere center = attacker + forward * range/2, radius = range/2), then
// apply damage, stun and knockback and update the combo.
void Engine::ResolveMeleeHit(Melee::CombatComponent* mc, const Melee::AttackData& atk) {
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt || !mc) return;
    const glm::vec3 fwd(std::sin(mc->attackYaw), 0.0f, std::cos(mc->attackYaw));
    const glm::vec3 center = jt->position + fwd * (0.5f * atk.range);
    const float radius = 0.5f * atk.range;

    // Nearest NPC whose world AABB the sphere overlaps (QuerySphere returns
    // only one arbitrary hit and may pick Jimmy, so the candidates are
    // filtered by AI-component + sphere-vs-AABB here).
    EntityID best{};
    float bestDist = FLT_MAX;
    glm::vec3 bestPos(0.0f);
    m_entities.ForEach([&](EntityID id, Transform& t,
                           const EntityManager::EntityRecord&) {
        if (id == m_jimmyId) return;
        if (!m_entities.GetAI(id)) return;
        glm::vec3 bmin, bmax;
        if (!m_entities.GetWorldAABB(id, bmin, bmax)) return;
        const glm::vec3 closest = glm::clamp(center, bmin, bmax);
        if (glm::distance(closest, center) > radius) return;
        const glm::vec3 to = t.position - jt->position;
        const float dxz = glm::length(glm::vec3(to.x, 0.0f, to.z));
        if (dxz > atk.range + 0.4f) return; // reach guard
        if (dxz < bestDist) { bestDist = dxz; best = id; bestPos = t.position; }
    });
    if (!best.IsValid()) {
        Logger::Info("Melee: %s whiffed (nothing in the %.1f sphere)",
                     atk.clipName, atk.range);
        return; // miss: combo resets on attack end
    }

    // Damage (+ combo finisher), stun, knockback. Track 28: the damage +
    // stun + fight registration + HIT event funnel through DamageNpc; the
    // knockback impulse stays here (attacker-relative).
    const float mult = mc->RegisterHit(mc->attack);
    const int dmg = int(atk.damage * mult + 0.5f);
    AIComponent* ai = m_entities.GetAI(best);
    Melee::CombatComponent* tmc = m_entities.GetMeleeCombat(best);
    const bool wasAlive = ai && ai->health.current > 0;
    DamageNpc(best, m_jimmyId, dmg, bestPos, atk.stunTime);
    ai = m_entities.GetAI(best); // DamageNpc may have KO'd it
    tmc = m_entities.GetMeleeCombat(best);
    if (tmc && wasAlive && (!ai || ai->health.current > 0)) {
        glm::vec3 dir = bestPos - jt->position;
        dir.y = 0.0f;
        const float len = glm::length(dir);
        if (len > 1e-4f)
            tmc->knockbackVel = (dir / len) * atk.knockback +
                                glm::vec3(0.0f, 0.6f, 0.0f);
    }
    mc->damageDealt += dmg;

    std::snprintf(m_meleePopup, sizeof(m_meleePopup),
                  mult > 1.0f ? "FINISHER -%d   COMBO x%d" : "HIT -%d   COMBO x%d",
                  dmg, mc->comboCount);
    m_meleePopupTimer = 1.5f;
    Logger::Info("Melee: %s hit NPC idx=%u for %d%s (health %d/%d, combo %d)",
                 atk.clipName, best.index, dmg,
                 mult > 1.0f ? " FINISHER" : "",
                 ai ? ai->health.current : 0, ai ? ai->health.max : 100,
                 mc->comboCount);
}

// OPENBULLY_MELEE_DEMO: scripted headless verification. Drives Jimmy straight
// toward the nearest NPC and chains Light -> Light -> Heavy so the attack
// animations, damage, stun and the 3-hit combo finisher show up in logs and
// screenshots without real input. Position is driven directly (not through
// button injection): this anchor's roof shell has no collision beneath the
// spawn (committed Track 21 runs free-fall identically), so the character is
// held at the target's plane for the duration of the combat script.
// (Own switch — OPENBULLY_COMBAT_DEMO is Track 25's dodge/block script.)
void Engine::DriveMeleeDemo(float dt) {
    if (!m_jimmyActive || !m_jimmyId.IsValid() || dt <= 0.0f) return;
    Melee::CombatComponent* mc = m_entities.GetMeleeCombat(m_jimmyId);
    PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
    if (!mc || !pc) return;

    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;
    // Nearest living NPC.
    EntityID best{};
    float bestDist = 1e9f;
    m_entities.ForEach([&](EntityID id, Transform& t,
                           const EntityManager::EntityRecord&) {
        if (id == m_jimmyId || !m_entities.GetAI(id)) return;
        if (const Melee::CombatComponent* c = m_entities.GetMeleeCombat(id))
            if (c->state == Melee::CombatState::KnockedDown) return;
        const float d = glm::length(t.position - jt->position);
        if (d < bestDist) { bestDist = d; best = id; }
    });
    if (!best.IsValid()) return;
    const Transform* nt = m_entities.GetTransform(best);
    if (!nt) return;

    // Walk (position-direct): face and approach the target.
    Transform* pt = m_entities.GetTransform(m_jimmyId);
    PhysicsComponent* pphys = m_entities.GetPhysics(m_jimmyId);
    if (!pt) return;
    glm::vec3 pos = pt->position;
    if (pphys && pos.y < nt->position.y - 1.0f) { // fallen off the shell
        pos.y = nt->position.y;
        pphys->velocity.y = 0.0f;
    }
    glm::vec3 to = nt->position - pos;
    to.y = 0.0f;
    const float dist = glm::length(to);
    if (dist > 1.1f && dist < 50.0f) {
        const glm::vec3 dir = to / dist;
        pos += dir * (3.0f * dt);
        pc->facingYaw   = std::atan2(dir.x, dir.z);
        pc->facingValid = true;
        pt->rotation.y  = pc->facingYaw;
    }
    pt->position = pos;

    // Snap the follow camera behind Jimmy: the exponential lerp trails a
    // position-driven demo badly (screenshots missed the fight entirely).
    {
        const float cy = std::cos(pc->facingYaw), sy = std::sin(pc->facingYaw);
        const glm::vec3 focus = pos + glm::vec3(0.0f, 1.2f, 0.0f);
        m_camera.SetPosition(focus - glm::vec3(sy, 0.0f, cy) * 3.2f +
                             glm::vec3(0.0f, 1.4f, 0.0f));
        m_camera.SetLookAt(focus - m_camera.GetPosition());
    }

    // Chain: light, light, heavy (3-hit combo), repeat.
    const double t = m_time;
    if (dist < 1.4f && mc->state == Melee::CombatState::Idle &&
        t - m_meleeDemoLastAtk > 1.4) {
        m_meleeDemoLastAtk = t;
        switch (m_meleeDemoStep++ % 3) {
        case 0: StartMeleeAttack(Melee::AttackType::LightPunch); break;
        case 1: StartMeleeAttack(Melee::AttackType::LightPunch); break;
        case 2: StartMeleeAttack(Melee::AttackType::HeavyPunch); break;
        }
        // Demo attacks aim at the scripted target, not the camera.
        if (dist > 1e-3f) mc->attackYaw = std::atan2(to.x, to.y);
    }
}

// =========================================================================
// Track 27: NPC attacks + AI verification demo.
// =========================================================================

// NPC-side twin of StartMeleeAttack: locks the melee state machine into
// Attacking, records the swing yaw and one-shot plays the attack clip at
// speed = authored / effective duration. The swing itself is progressed by
// ProgressNpcAttack from UpdateNpcs (UpdateMeleeCombat drives only Jimmy).
void Engine::StartNpcAttack(EntityID npc, Melee::AttackType type, float targetYaw) {
    Melee::CombatComponent* mc = m_entities.GetMeleeCombat(npc);
    AnimationComponent* ac = m_entities.GetAnimation(npc);
    if (!mc || !ac || mc->state != Melee::CombatState::Idle) return;
    if (ac->rigIndex == 0xFFFFFFFF || ac->rigIndex >= m_animRigs.size()) return;
    AnimRig& rig = m_animRigs[ac->rigIndex];
    if (rig.attackClips[int(type)] < 0) return; // clips not loaded

    const Melee::AttackData& atk = Melee::GetAttackData(type);
    mc->state       = Melee::CombatState::Attacking;
    mc->attack      = type;
    mc->attackTimer = atk.duration;
    mc->hitApplied  = false;
    mc->attackYaw   = targetYaw;

    if (AnimClip* clip = rig.controller.ClipAt(rig.attackClips[int(type)])) {
        clip->speed = atk.authoredDur > 0.01f ? atk.authoredDur / atk.duration : 1.0f;
        rig.controller.Play(rig.attackClips[int(type)], /*loop=*/false, 0.1f);
    }
}

// Advances an NPC's own attack: tick the timer, fire the hit once inside the
// hit window (-> ResolveNpcMeleeHit), return to Idle on expiry (miss resets
// the combo). Mirrors Jimmy's progression in UpdateMeleeCombat.
void Engine::ProgressNpcAttack(EntityID npc, Melee::CombatComponent* mc, float dt) {
    if (!mc || mc->state != Melee::CombatState::Attacking) return;
    const Melee::AttackData& atk = Melee::GetAttackData(mc->attack);
    mc->attackTimer -= dt;
    const float progress = 1.0f - mc->attackTimer / atk.duration;
    if (!mc->hitApplied &&
        progress >= atk.hitWindowStart && progress <= atk.hitWindowEnd) {
        mc->hitApplied = true;
        ResolveNpcMeleeHit(mc, atk, npc);
    }
    if (mc->attackTimer <= 0.0f) {
        const bool missed = !mc->hitApplied;
        mc->state = Melee::CombatState::Idle;
        if (missed) mc->ResetCombo();
    }
}

// An NPC's hit frame fired at the player: reach-check the player, then route
// the damage through DamagePlayer so Track 25 defenses apply (dodge i-frames
// -> 0 damage, block -> 50% + counter window). No knockback on the player:
// UpdateJimmy integrates velocity from input only (unwired, by design).
void Engine::ResolveNpcMeleeHit(Melee::CombatComponent* mc,
                                const Melee::AttackData& atk, EntityID attacker) {
    const Transform* nt = m_entities.GetTransform(attacker);
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!nt || !jt || !mc) return;
    const glm::vec3 to = jt->position - nt->position;
    const float dxz = glm::length(glm::vec3(to.x, 0.0f, to.z));
    if (dxz > atk.range + 0.6f) {
        Logger::Info("NPC melee: entity %u whiffed (player %.1fu > reach %.1fu)",
                     attacker.index, dxz, atk.range);
        return;
    }
    const int dmg = int(atk.damage + 0.5f);
    mc->damageDealt += dmg;
    DamagePlayer(dmg, "NPC punch");
    // Track 28: a prefect downed the player -> BUSTED stub.
    if (PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
        if (pc->health.current <= 0) {
            const AIComponent* aai = m_entities.GetAI(attacker);
            if (aai && aai->role == AIRole::Prefect) Busted();
        }
    }
}

// =========================================================================
// Track 28: NPC reaction system.
// =========================================================================

// Tuning (task-spec values) — moved to file scope above (shared with
// UpdateNpcs' KO/flinch branches).

void Engine::QueueAIEvent(const AIEvent& e) {
    m_aiEvents.push_back(e);
}

// Registers/refreshes a hot fight spot. Witnesses scan these; a fight stays
// hot for kFightActiveTime after its last hit.
void Engine::RegisterFight(const glm::vec3& pos, EntityID aggressor) {
    for (FightInfo& f : m_fights) {
        if (glm::distance(f.pos, pos) < 2.0f &&
            m_time - f.lastActivity < kFightActiveTime + 10.0f) {
            f.pos = pos;
            f.aggressor = aggressor;
            f.lastActivity = m_time;
            return;
        }
    }
    m_fights.push_back({pos, aggressor, m_time});
}

// Central NPC damage gate (Track 28). Applies damage, registers the fight
// spot, queues a HIT event for the reaction pass, plays the flinch and
// handles the KO transition at 0 health.
void Engine::DamageNpc(EntityID npc, EntityID attacker, int dmg,
                       const glm::vec3& hitPos, float stunTime) {
    AIComponent* ai = m_entities.GetAI(npc);
    if (!ai || ai->state == AIState::Ko) return; // already down: no re-trigger
    ai->health.Damage(dmg);
    RegisterFight(hitPos, attacker);
    if (ai->health.current <= 0) {
        KnockOutNpc(npc, attacker);
        return;
    }
    // Flinch: melee stun (freeze; knockback applied by the caller) + lean.
    ai->flinchTimer = kFlinchTime;
    if (Melee::CombatComponent* tmc = m_entities.GetMeleeCombat(npc)) {
        tmc->state    = Melee::CombatState::Stunned;
        tmc->stunTimer = std::max(0.3f, stunTime);
    }
    AIEvent e;
    e.type = AIEventType::Hit;
    e.sourceEntity = attacker;
    e.targetEntity = npc;
    e.position = hitPos;
    e.time = m_time;
    QueueAIEvent(e);
}

// NPC reached 0 health: KO - fall over, lie down, respawn after 10 s.
void Engine::KnockOutNpc(EntityID npc, EntityID attacker) {
    AIComponent* ai = m_entities.GetAI(npc);
    const Transform* t = m_entities.GetTransform(npc);
    if (!ai || !t || ai->state == AIState::Ko) return;
    ai->state     = AIState::Ko;
    ai->koTimer   = kRespawnDelay;
    ai->fallTimer = 0.0f;
    ai->hasTarget = false;
    ai->targetEntity = EntityID{};
    if (Melee::CombatComponent* tmc = m_entities.GetMeleeCombat(npc)) {
        tmc->state    = Melee::CombatState::KnockedDown;
        tmc->downTimer = 0.0f; // stays down until the respawn
    }
    Logger::Info("NPC idx=%u KO'd by idx=%u - down for %.0fs",
                 npc.index, attacker.index, kRespawnDelay);
    AIEvent e;
    e.type = AIEventType::NpcDefeated;
    e.sourceEntity = attacker;
    e.targetEntity = npc;
    e.position = t->position;
    e.time = m_time;
    QueueAIEvent(e);
    // Mission hook: completes an active DEFEAT_NPC objective on this NPC.
    m_missions.NotifyNpcDefeated(npc);
}

// KO timer expired: back to the original spawn point with fresh health.
void Engine::RespawnNpc(EntityID id, AIComponent& ai, Transform& t) {
    glm::vec3 pos = ai.homePos;
    if (m_collisionWorld) { // ground snap (level may have shifted under it)
        Ray down;
        down.origin[0] = pos.x; down.origin[1] = pos.y + 10.0f; down.origin[2] = pos.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) pos.y = hit.point[1];
    }
    t.position = pos;
    t.rotation.x = 0.0f;
    t.rotation.z = 0.0f;
    ai.health.current = ai.health.max;
    ai.state = AIState::Idle;
    ai.timer = 0.5f;
    ai.hasTarget = false;
    ai.targetEntity = EntityID{};
    ai.flinchTimer = 0.0f;
    ai.koTimer = 0.0f;
    ai.fallTimer = 0.0f;
    ai.loseTimer = 0.0f;
    ai.stuckTimer = 0.0f;
    ai.lastPos = pos;
    if (Melee::CombatComponent* tmc = m_entities.GetMeleeCombat(id)) {
        tmc->state       = Melee::CombatState::Idle;
        tmc->stunTimer   = 0.0f;
        tmc->downTimer   = 0.0f;
        tmc->damageTaken = 0;
    }
    Logger::Info("NPC idx=%u respawned at spawn point (health %d/%d)",
                 id.index, ai.health.current, ai.health.max);
}

// True when nothing registered in the collision world blocks the segment
// from -> to (eye-height raycast; 0.4 u skin so surface kisses pass).
bool Engine::HasLineOfSight(const glm::vec3& from, const glm::vec3& to) const {
    if (!m_collisionWorld) return true;
    const glm::vec3 d = to - from;
    const float len = glm::length(d);
    if (len < 0.1f) return true;
    Ray r;
    r.origin[0] = from.x; r.origin[1] = from.y; r.origin[2] = from.z;
    r.direction[0] = d.x / len; r.direction[1] = d.y / len; r.direction[2] = d.z / len;
    RayHit hit;
    return !(m_collisionWorld->Raycast(r, hit) && hit.t < len - 0.4f);
}

// Every roaming NPC with line of sight to a hot fight (within 15 u) emits
// WITNESSED_FIGHT; ProcessAIEvents turns those into prefect chases or crowd
// gathers. Also prunes long-cold fight records.
void Engine::ScanFightWitnesses() {
    for (size_t i = m_fights.size(); i-- > 0;) {
        if (m_time - m_fights[i].lastActivity > kFightActiveTime + 20.0f)
            m_fights.erase(m_fights.begin() + long(i));
    }
    if (m_fights.empty()) return;
    m_entities.ForEachAI([&](EntityID id, Transform& t, AIComponent& ai) {
        // Roaming states, or Track 27's idle player-ALERT (they are already
        // looking this way; the fight outranks staring at the player).
        const bool roaming =
            ai.state == AIState::Idle || ai.state == AIState::Wander ||
            ai.state == AIState::FacePlayer || ai.state == AIState::Patrol ||
            ai.state == AIState::Return;
        const bool idleAlert = ai.state == AIState::Alert && !ai.hasTarget;
        if (!roaming && !idleAlert) return; // busy or down
        const glm::vec3 eye = t.position + glm::vec3(0.0f, 1.6f, 0.0f);
        for (const FightInfo& f : m_fights) {
            if (m_time - f.lastActivity >= kFightActiveTime) continue; // cold
            if (f.aggressor == id) continue;                          // your own fight
            const glm::vec2 dxz(f.pos.x - t.position.x, f.pos.z - t.position.z);
            if (glm::length(dxz) > kWitnessRadius) continue;
            if (!HasLineOfSight(eye, f.pos + glm::vec3(0.0f, 1.0f, 0.0f))) continue;
            AIEvent e;
            e.type = AIEventType::WitnessedFight;
            e.sourceEntity = f.aggressor;
            e.targetEntity = id; // the witness (reaction target)
            e.position = f.pos;
            e.time = m_time;
            QueueAIEvent(e);
            break; // one fight reaction at a time
        }
    });
}

// Drains the reaction queue into AI state changes. Follow-up events (help
// calls) queue for the next frame, bounding per-frame work.
void Engine::ProcessAIEvents() {
    std::vector<AIEvent> events;
    events.swap(m_aiEvents);
    for (const AIEvent& e : events) {
        switch (e.type) {
        case AIEventType::Hit: {
            AIComponent* ai = m_entities.GetAI(e.targetEntity);
            if (!ai || ai->state == AIState::Ko) break;
            const Transform* st = m_entities.GetTransform(e.sourceEntity);
            if (ai->health.Fraction() < kFleeHealthFrac28 &&
                nextRandom01() < kFleeChance) {
                // Low health: chance to flee instead of fighting back.
                ai->state = AIState::Flee;
                ai->threatPos = st ? st->position : e.position;
                ai->targetEntity = e.sourceEntity;
                ai->hasTarget = false;
                ai->stuckTimer = 0.0f;
                ai->sidestepTimer = 0.0f;
                Logger::Info("NPC AI: entity %u flees (health %d/%d)",
                             e.targetEntity.index, ai->health.current,
                             ai->health.max);
            } else {
                // Fight back: chase + attack the attacker (works NPC-vs-NPC
                // and NPC-vs-player: the engaged states use targetEntity).
                ai->state = AIState::Chase;
                ai->targetEntity = e.sourceEntity;
                ai->hasTarget = false;
                ai->loseTimer = 0.0f;
                ai->stuckTimer = 0.0f;
                ai->sidestepTimer = 0.0f;
                ai->lastPos = m_entities.GetTransform(e.targetEntity)
                    ? m_entities.GetTransform(e.targetEntity)->position
                    : glm::vec3(0.0f);
                Logger::Info("NPC AI: entity %u fights back against idx=%u",
                             e.targetEntity.index, e.sourceEntity.index);
            }
            // Help call: nearby same-type NPCs hear it (processed next frame).
            AIEvent help;
            help.type = AIEventType::HelpCall;
            help.sourceEntity = e.targetEntity; // the caller (victim)
            help.targetEntity = e.sourceEntity; // the attacker
            help.position = e.position;
            help.time = m_time;
            QueueAIEvent(help);
            break;
        }

        case AIEventType::HelpCall: {
            const Transform* caller = m_entities.GetTransform(e.sourceEntity);
            const MeshComponent* cm = m_entities.GetMesh(e.sourceEntity);
            if (!caller || !cm || !cm->isNpc) break;
            m_entities.ForEachAI([&](EntityID id, Transform& t, AIComponent& ai) {
                (void)t;
                if (id == e.sourceEntity || id == e.targetEntity) return;
                if (ai.state != AIState::Idle && ai.state != AIState::Wander &&
                    ai.state != AIState::FacePlayer &&
                    ai.state != AIState::Patrol && ai.state != AIState::Return)
                    return;
                const MeshComponent* m = m_entities.GetMesh(id);
                if (!m || m->npcModelIndex != cm->npcModelIndex) return; // same type
                if (glm::distance(t.position, caller->position) > kHelpCallRadius)
                    return;
                ai.state = AIState::Chase;
                ai.targetEntity = e.targetEntity;
                ai.hasTarget = false;
                ai.loseTimer = 0.0f;
                ai.stuckTimer = 0.0f;
                ai.sidestepTimer = 0.0f;
                ai.lastPos = t.position;
                Logger::Info("NPC AI: entity %u answers help call from idx=%u",
                             id.index, e.sourceEntity.index);
            });
            break;
        }

        case AIEventType::WitnessedFight: {
            AIComponent* ai = m_entities.GetAI(e.targetEntity);
            if (!ai || ai->state == AIState::Ko) break;
            // Admissible from roaming states, or from Track 27's idle player-
            // ALERT (they are already looking this way; the fight outranks
            // staring at the player). Track 28 fight-alerts (hasTarget slot
            // set) are not re-interrupted.
            const bool roaming =
                ai->state == AIState::Idle || ai->state == AIState::Wander ||
                ai->state == AIState::FacePlayer || ai->state == AIState::Patrol ||
                ai->state == AIState::Return;
            const bool idlePlayerAlert =
                ai->state == AIState::Alert && !ai->hasTarget;
            if (!roaming && !idlePlayerAlert) break; // busy or down
            // Already deciding on this exact fight: do not re-arm the pause
            // every frame (the witness scan re-emits until the state leaves
            // ALERT). A Track 27 player-alert (fightPos unset) is converted.
            if (ai->state == AIState::Alert && ai->alertTimer > 0.0f &&
                glm::distance(ai->fightPos, e.position) < 2.0f) break;

            if (ai->role == AIRole::Prefect) {
                // Authority: brief pause, then chase + attack the aggressor.
                ai->state = AIState::Alert;
                ai->fightPos = e.position;
                ai->targetEntity = e.sourceEntity;
                ai->hasTarget = false;
                ai->alertTimer = kAlertTimeMin +
                    nextRandom01() * (kAlertTimeMax - kAlertTimeMin);
                ai->loseTimer = 0.0f;
                ai->stuckTimer = 0.0f;
                ai->sidestepTimer = 0.0f;
                Logger::Info("NPC AI: prefect %u saw the fight - moving in "
                             "on idx=%u", e.targetEntity.index,
                             e.sourceEntity.index);
                break;
            }

            // Civilian: crowd cap (8 per fight) - count current watchers.
            int watchers = 0;
            m_entities.ForEachAI([&](EntityID, Transform&, AIComponent& other) {
                if (other.state == AIState::Watch ||
                    (other.state == AIState::Alert &&
                     other.role != AIRole::Prefect &&
                     glm::distance(other.fightPos, e.position) < 2.0f))
                    watchers++;
            });
            if (watchers >= kCrowdCap) break; // crowd full: keep wandering

            // Formation slot: evenly spread angles, 4-6 u radius, ground-snapped.
            const float ang = float(watchers) * (6.2831853f / float(kCrowdCap));
            const float rad = kCrowdRadiusMin +
                float(watchers % 3) *
                ((kCrowdRadiusMax - kCrowdRadiusMin) / 2.0f);
            glm::vec3 slotPos = e.position +
                glm::vec3(std::cos(ang) * rad, 0.0f, std::sin(ang) * rad);
            slotPos.y = e.position.y;
            if (m_collisionWorld) {
                Ray down;
                down.origin[0] = slotPos.x; down.origin[1] = slotPos.y + 10.0f;
                down.origin[2] = slotPos.z;
                down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
                RayHit hit;
                if (m_collisionWorld->Raycast(down, hit) &&
                    std::fabs(hit.point[1] - e.position.y) < 3.0f)
                    slotPos.y = hit.point[1];
            }
            ai->state = AIState::Alert;
            ai->fightPos = e.position;
            ai->targetPos = slotPos;
            ai->hasTarget = true;
            ai->alertTimer = kAlertTimeMin +
                nextRandom01() * (kAlertTimeMax - kAlertTimeMin);
            ai->loseTimer = 0.0f;
            ai->stuckTimer = 0.0f;
            break;
        }

        case AIEventType::NpcDefeated:
            Logger::Info("AI event: NPC_DEFEATED idx=%u (by idx=%u)",
                         e.targetEntity.index, e.sourceEntity.index);
            break;
        }
    }
}

// Track 28 stub: a prefect defeated the player. Logs BUSTED, restores full
// health and teleports back to the spawn point.
void Engine::Busted() {
    Logger::Info("BUSTED");
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    Transform* jt = m_jimmyId.IsValid()
        ? m_entities.GetTransform(m_jimmyId) : nullptr;
    if (pc) {
        pc->health.current = pc->health.max;
        pc->state = DefenseState::Normal;
        pc->wanted = std::min(5, pc->wanted + 1);
    }
    if (jt) jt->position = m_jimmySpawn + glm::vec3(0.0f, 2.0f, 0.0f);
}

// OPENBULLY_NPC_DEMO: scripted headless verification of the Track 27 AI
// state machine. Position-direct drive (same approach as DriveMeleeDemo;
// the spawn-shell plane guard included):
//   t 0-24  walk toward the nearest living NPC at 2.5 u/s -> it should go
//           ALERT (sightRadius 10u + LOS), then CHASE, then ATTACK once
//           within 1.8u. Every transition is logged with entity + distance.
//   t 24    once: drop every engaged (CHASE/ATTACK) NPC below the 20% flee
//           threshold -> they must flip to FLEE and run away (visible in the
//           per-second AI census and the transition log).
//   t 24+   stand still; fleeing NPCs reach the give-up distance -> RETURN.
void Engine::DriveNpcDemo(float dt) {
    if (!m_jimmyActive || !m_jimmyId.IsValid() || dt <= 0.0f) return;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;
    const double t = m_time;

    // Nearest living NPC (not knocked down).
    EntityID best{};
    float bestDist = 1e9f;
    m_entities.ForEach([&](EntityID id, Transform& et,
                           const EntityManager::EntityRecord&) {
        if (id == m_jimmyId || !m_entities.GetAI(id)) return;
        if (const Melee::CombatComponent* c = m_entities.GetMeleeCombat(id))
            if (c->state == Melee::CombatState::KnockedDown) return;
        const float d = glm::length(et.position - jt->position);
        if (d < bestDist) { bestDist = d; best = id; }
    });
    if (!best.IsValid()) return;
    const Transform* nt = m_entities.GetTransform(best);
    if (!nt) return;
    const AIComponent* ai = m_entities.GetAI(best);

    glm::vec3 pos = jt->position;
    // Plane guard (same approach as DriveMeleeDemo: the spawn-shell has no
    // collision beneath Jimmy, so the character is held at the target NPC's
    // plane for the whole script — gravity is neutralized each frame).
    PhysicsComponent* pphys = m_entities.GetPhysics(m_jimmyId);
    if (pphys) {
        pos.y = nt->position.y;
        pphys->velocity.y = 0.0f;
    }

    PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
    const bool npcEngaged = ai && (ai->state == AIState::Attack ||
                                   ai->state == AIState::Flee);
    if (t < 24.0) {
        // Approach phase: close on the NPC, hold ground once it is on us.
        glm::vec3 to = nt->position - pos;
        to.y = 0.0f;
        const float dist = glm::length(to);
        if (dist > 1.1f && dist < 60.0f && !npcEngaged) {
            const glm::vec3 dir = to / dist;
            pos += dir * (2.5f * dt);
        }
        if (pc && dist > 1e-3f) { // always face the NPC (approach or stand)
            pc->facingYaw   = std::atan2(to.x / dist, to.z / dist);
            pc->facingValid = true;
        }
    } else {
        // Flee trigger: once, push every engaged NPC below the 20% threshold.
        static bool s_fleeTriggered = false;
        if (!s_fleeTriggered) {
            s_fleeTriggered = true;
            m_entities.ForEachAI([&](EntityID, const Transform&, AIComponent& a) {
                if ((a.state == AIState::Chase || a.state == AIState::Attack) &&
                    a.health.Fraction() >= a.fleeHealthFrac) {
                    a.health.Damage(int(float(a.health.max) * 0.9f));
                    Logger::Info("NPC demo: entity health dropped to %d/%d "
                                 "(below flee threshold)",
                                 a.health.current, a.health.max);
                }
            });
        }
    }

    Transform* pt = m_entities.GetTransform(m_jimmyId);
    if (!pt) return;
    pt->position = pos;

    // Snap the follow camera behind Jimmy so screenshots catch the fight.
    if (pc) {
        const float cy = std::cos(pc->facingYaw), sy = std::sin(pc->facingYaw);
        const glm::vec3 focus = pos + glm::vec3(0.0f, 1.2f, 0.0f);
        m_camera.SetPosition(focus - glm::vec3(sy, 0.0f, cy) * 3.2f +
                             glm::vec3(0.0f, 1.4f, 0.0f));
        m_camera.SetLookAt(focus - m_camera.GetPosition());
    }
}

// Track 14: integrate entity physics. Only entities with a PhysicsComponent
// move; the world floor (y = 0) grounds them, setting onGround and zeroing
// the vertical velocity.
void Engine::UpdateEntities(float dt) {
    if (dt <= 0.0f) return;
    m_entities.ForEachPhysics([this, dt](EntityID id, Transform& t, PhysicsComponent& p) {
        // Track 15: Jimmy integrates his own physics (raycast ground + wall
        // resolve); the y=0 world-floor clamp below would fight it.
        if (m_entities.GetPlayer(id)) return;
        // Track 16: NPCs are moved by the AI walk code (raycast ground snap
        // + wall resolve); generic gravity would drag them through floors.
        if (m_entities.GetAI(id)) return;
        if (p.gravity == 0.0f) return;
        p.velocity.y += p.gravity * dt;
        t.position += p.velocity * dt;
        if (t.position.y <= 0.5f) { // unit-box half-height above y=0 floor
            t.position.y = 0.5f;
            p.velocity.y = 0.0f;
            p.onGround = true;
        } else {
            p.onGround = false;
        }
    });
}

// Build the per-frame entity render list (model matrices + draw ranges).
// Level entities draw their WorldMesh sub ranges at identity transform
// (NIF vertices are baked world-space); test boxes use their Transform
// model matrix and the placeholder texture.
void Engine::BuildEntityRenderList() {
    m_entityRender.clear();
    m_entities.ForEach([&](EntityID id, Transform& t,
                           const EntityManager::EntityRecord& rec) {
        (void)rec;
        const MeshComponent* mc = m_entities.GetMesh(id);
        if (!mc) return;
        EntityRender er;
        er.model = mc->isBox ? t.Matrix() : glm::mat4(1.0f);
        // Track 17: skinned entity — draw the rig's GPU mesh with its
        // controller's bone palette.
        if (const AnimationComponent* ac = m_entities.GetAnimation(id)) {
            if (ac->rigIndex != 0xFFFFFFFF && ac->rigIndex < m_animRigs.size()) {
                const AnimRig& rig = m_animRigs[ac->rigIndex];
                if (rig.gpu.valid()) {
                    er.model = t.Matrix();
                    er.gpu = &rig.gpu;
                    er.subs = rig.subs;
                    er.rigIndex = ac->rigIndex;
                    m_entityRender.push_back(std::move(er));
                    return;
                }
            }
        }
        if (mc->isWeaponPickup) {
            // Track 26: weapon pickup — its own NIF model, transform-driven.
            if (mc->weaponModelIndex >= m_weaponModels.size()) return;
            const WeaponModel& wmodel = m_weaponModels[mc->weaponModelIndex];
            if (!wmodel.gpu.valid()) return;
            er.model = t.Matrix();
            er.gpu   = &wmodel.gpu;
            er.subs  = wmodel.subs;
            m_entityRender.push_back(std::move(er));
            return;
        }
        if (mc->isVehicle) {
            // Track 31: vehicle model, transform-driven (parked or driving).
            if (mc->vehicleModelIndex >= m_vehicleModels.size()) return;
            const VehicleModel& vmodel = m_vehicleModels[mc->vehicleModelIndex];
            if (!vmodel.gpu.valid()) return;
            er.model = t.Matrix();
            er.gpu   = &vmodel.gpu;
            er.subs  = vmodel.subs;
            m_entityRender.push_back(std::move(er));
            return;
        }
        if (mc->isJimmy) {
            if (!m_jimmyMeshValid) return;
            er.model = t.Matrix(); // player mesh is model-space: use transform
            er.gpu = &m_jimmyMesh;
            er.subs = m_jimmySubs;
        } else if (mc->isNpc) {
            if (mc->npcModelIndex >= m_npcModels.size()) return;
            const NpcModel& model = m_npcModels[mc->npcModelIndex];
            er.model = t.Matrix(); // NPC meshes are model-space
            er.gpu  = &model.gpu;
            er.subs = model.subs;
        } else if (mc->isBox) {
            if (!m_testBoxValid) return;
            er.gpu = &m_testBox;
            WorldMesh::SubRange sr;
            sr.indexOffset = 0;
            sr.indexCount  = m_testBox.indexCount;
            sr.texture     = mc->texture; // invalid -> renderer placeholder
            er.subs.push_back(sr);
        } else {
        const WorldMesh& wm = m_worldMeshes[mc->worldMeshIndex];
            er.gpu  = &wm.gpu;
            er.subs = wm.subs;
        }
        m_entityRender.push_back(std::move(er));
    });
}

Engine::Engine() = default; // members built here where the types are complete

// Track 20: mission system. Loads the hand-authored JSON mission file
// (data/missions/demo_missions.json) and registers trigger zones around the
// demo level. Positions in the file are expressed relative to runtime
// reference points (Jimmy's spawn, the anchor-mesh bounds) so the content
// stays aligned with wherever the level geometry lands.
bool Engine::InitMissions() {
    // Reference points for the JSON loader.
    std::unordered_map<std::string, glm::vec3> refs;
    refs["spawn"] = m_jimmySpawn;
    if (!m_worldMeshes.empty()) {
        const WorldMesh* anchor = &m_worldMeshes.front();
        for (const WorldMesh& wm : m_worldMeshes)
            if (wm.gpu.vertexCount > anchor->gpu.vertexCount) anchor = &wm;
        refs["anchor_center"] = 0.5f * (glm::make_vec3(anchor->boundsMin) +
                                         glm::make_vec3(anchor->boundsMax));
        refs["anchor_min"] = glm::make_vec3(anchor->boundsMin);
        refs["anchor_max"] = glm::make_vec3(anchor->boundsMax);
    }

    // Same probing convention as geometry/textures: CWD first, then the
    // source-tree fallback for dev-shell runs.
    const char* paths[] = {
        "data/missions/demo_missions.json",
        "../data/missions/demo_missions.json",
    };
    bool loaded = false;
    for (const char* p : paths) {
        if (std::ifstream f(p); f.good()) {
            f.close();
            loaded = m_missions.LoadFromJson(p, refs);
            break;
        }
    }
    if (!loaded) {
        Logger::Warning("Missions: demo_missions.json not found or empty "
                        "(mission system idle)");
        return false;
    }
    Logger::Info("Missions: %zu mission(s), %zu trigger zone(s) registered",
                 m_missions.MissionCount(), m_missions.TriggerCount());

    // Objective beacon geometry (renderer draws it with the emissive path).
    m_beaconMesh = m_renderer->UploadMeshGpu(MakeBeaconCubeMesh());
    m_beaconValid = m_beaconMesh.valid();
    if (!m_beaconValid) Logger::Warning("Beacon mesh upload failed (no objective marker)");
    return true;
}

// Track 20: vertical beacon column at the active objective's target. Only
// drawn while a mission is active; subtle pulse on the horizontal scale.
void Engine::RenderObjectiveMarker() {
    if (!m_beaconValid) return;
    const Mission* m = m_missions.ActiveMission();
    if (!m || m->currentObjective >= m->objectives.size()) return;
    const Objective& o = m->objectives[m->currentObjective];
    if (o.complete) return;

    // 8-unit column centered 4 above the target so it pierces the floor and
    // reads from a distance; gentle breathing pulse on the cross-section.
    const float pulse = 0.7f + 0.2f * float(std::sin(m_time * 4.0));
    const glm::vec3 center = o.targetPosition + glm::vec3(0.0f, 4.0f, 0.0f);
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), center) *
                            glm::scale(glm::mat4(1.0f),
                                       glm::vec3(pulse, 8.0f, pulse));
    m_renderer->DrawBeacon(m_beaconMesh, glm::value_ptr(model));
}

// Track 20: OPENBULLY_MISSION_DEMO=1 headless verification drive — steer the
// camera yaw toward the active objective's beacon and hold Forward so Jimmy
// walks into trigger zones and objective radii without OS-level input.
void Engine::DriveMissionDemo() {
    m_input.SetButton(Button::Forward, false);
    m_input.SetButton(Button::Sprint, false);
    m_input.SetMouseDelta(0, 0);
    const Mission* m = m_missions.ActiveMission();
    if (!m || m->currentObjective >= m->objectives.size()) return;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;
    const Objective& o = m->objectives[m->currentObjective];
    const float dx = o.targetPosition.x - jt->position.x;
    const float dz = o.targetPosition.z - jt->position.z;
    const float distXZ = std::sqrt(dx * dx + dz * dz);
    if (distXZ < 1.0f) return; // inside the check radius; stand still

    // Movement uses camera yaw (UpdateJimmy); aim the camera straight at the
    // objective each frame. SetLookAt maps (x,z) -> yaw directly, so carrying
    // the current pitch over reconstructs the same view direction with the
    // new yaw. (The old mouse-delta steering lost a per-frame tug-of-war with
    // the follow camera's own SetLookAt re-aim in UpdateJimmyCamera; forcing
    // the yaw makes drive + follow-cam agree: the camera settles behind Jimmy
    // along the very yaw he walks.)
    const float wantYaw = std::atan2(dx, dz);
    const float pitch = m_camera.GetPitch();
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    m_camera.SetLookAt(glm::vec3(std::sin(wantYaw) * cp, sp, std::cos(wantYaw) * cp));
    m_input.SetButton(Button::Forward, true);
    m_input.SetButton(Button::Sprint, distXZ > 6.0f);
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

    // Track 21: audio device + procedural SFX, opened after window/renderer
    // creation. Failure is non-fatal: the engine runs silently instead.
    m_audio = std::make_unique<AudioManager>();
    if (m_audio->Init()) {
        // User .wav overrides (data/audio/) are the extensibility path;
        // missing files are fine, the procedural sounds stay in place.
        if (m_audio->LoadUserWavs("data/audio") == 0)
            m_audio->LoadUserWavs("../data/audio");
    } else {
        Logger::Warning("Audio disabled: audio device could not be opened");
        m_audio.reset();
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
    // OPENBULLY_MESH_CAP=<n> (Track 12 verification hook): cap the loaded
    // mesh count so a single dense building fills the frame for lighting
    // screenshots without depending on scene spread.
    size_t meshCap = demoOnly ? 3 : 32;
    if (const char* cap = std::getenv("OPENBULLY_MESH_CAP")) {
        const int n = std::atoi(cap);
        if (n > 0) meshCap = size_t(n);
    }
    // Track 11: extracted .nft textures directory (same layout as the world
    // NIFs; populated by src/tools/extract_img.py --filter nft).
    const char* worldTexDirs[] = {
        "data/extracted/textures",
        "../data/extracted/textures",
    };
    const char* texDir = worldTexDirs[0];
    {
        // Pick the first existing texture dir (same probing as geometry).
        bool found = false;
        for (const char* d : worldTexDirs) {
            if (DIR* dp = ::opendir(d)) { ::closedir(dp); texDir = d; found = true; break; }
        }
        // OPENBULLY_NO_TEXTURES=1: force placeholder albedo everywhere so
        // lighting alone can be judged on untextured geometry (Track 12).
        const bool noTextures = std::getenv("OPENBULLY_NO_TEXTURES") != nullptr;
        if (!found || noTextures) texDir = ""; // textures disabled; placeholder everywhere
        if (noTextures) Logger::Info("OPENBULLY_NO_TEXTURES set: drawing untextured (placeholder albedo)");
    }
    m_texDir = texDir;
    BuildTextureIndex(m_texDir.c_str());

    // Track 13: collision world before geometry loads so meshes register
    // their bounds as they arrive. Debug flag enables wireframe boxes.
    m_collisionWorld = std::make_unique<CollisionWorld>();
    m_debugCollision = std::getenv("OPENBULLY_DEBUG_COLLISION") != nullptr;
    if (m_debugCollision) Logger::Info("OPENBULLY_DEBUG_COLLISION: wireframe boxes enabled");
    m_debugPhysics = std::getenv("OPENBULLY_DEBUG_PHYSICS") != nullptr;
    if (m_debugPhysics) Logger::Info("OPENBULLY_DEBUG_PHYSICS: Jimmy collision sphere debug enabled");

    size_t worldLoaded = LoadWorldGeometry("data/extracted/world", meshCap, texDir);
    if (worldLoaded == 0) {
        // Fallback: source-tree relative path (running from a dev shell).
        worldLoaded = LoadWorldGeometry("../data/extracted/world", meshCap, texDir);
    }
    Logger::Info("Textures: %zu cached, %zu failed, %zu textured / %zu placeholder submeshes",
                 m_textureCache.size(), m_textureLoadFails,
                 m_texturedSubMeshes, m_placeholderSubMeshes);
    if (demoOnly) Logger::Info("Demo-only mode: restricted to the 3 test meshes");

    // Track 21: level is loaded — start the ambient wind loop (seamless,
    // non-spatialized bed; idempotent).
    if (m_audio)
        m_audio->StartAmbient();
    if (worldLoaded == 0) {
        Logger::Info("No World.img geometry loaded; triangle-only mode");
        // Default spawn: spec fallback (0, 5, -10) looking toward origin.
        m_camera = Camera(glm::vec3(0.0f, 5.0f, -10.0f), 0.0f, -0.46f);
    } else {
        // Spawn next to the densest geometry, not the union bounds: world
        // meshes sit at their real level positions, often hundreds of units
        // apart, so a union-bounds backoff puts the whole scene in a few
        // pixels. Anchoring on the vertex-heaviest mesh guarantees full-frame
        // geometry; WASD explores from there.
        const WorldMesh* anchor = &m_worldMeshes.front();
        for (const WorldMesh& wm : m_worldMeshes)
            if (wm.gpu.vertexCount > anchor->gpu.vertexCount) anchor = &wm;

        const glm::vec3 center = 0.5f * (glm::make_vec3(anchor->boundsMin) +
                                         glm::make_vec3(anchor->boundsMax));
        const float radius = glm::length(glm::make_vec3(anchor->boundsMax) -
                                         glm::make_vec3(anchor->boundsMin)) * 0.5f;

        // Back off along -Z from the anchor center, outside its bounds, at
        // modest height for a three-quarter view. OPENBULLY_CAM_ZOOM=<f>
        // scales the backoff distance (verification hook).
        float backoffScale = 1.0f;
        if (const char* z = std::getenv("OPENBULLY_CAM_ZOOM"))
            backoffScale = float(std::atof(z));
        glm::vec3 spawn = center + glm::vec3(0.0f, radius * 0.35f + 2.0f,
            -(radius * 0.45f + 4.0f) * backoffScale);

        // Aim at the anchor center: yaw/pitch for LH +Z-forward convention.
        const glm::vec3 to = center - spawn;
        const float yaw   = std::atan2(to.x, to.z);
        const float pitch = std::atan2(to.y, glm::length(glm::vec2(to.x, to.z)));

        m_camera = Camera(spawn, yaw, pitch);
        Logger::Info("Camera spawn (%.1f, %.1f, %.1f) anchored on %s "
                     "(radius %.1f) -> center (%.1f, %.1f, %.1f)",
                     spawn.x, spawn.y, spawn.z, anchor->name.c_str(), radius,
                     center.x, center.y, center.z);
    }

    m_initialized = true;
    m_lastPerf = SDL_GetPerformanceCounter();

    // Track 12: single directional sun. SetLight takes the to-light
    // direction; the shader normalizes and negates it, matching the spec
    // travel direction normalize(vec3(0.3, -0.8, -0.5)). White light, 0.2
    // gray ambient floor (no pure-black shadows).
    {
        const float toLight[3] = { -0.3f, 0.8f, 0.5f };
        const float white[3]   = { 1.0f, 1.0f, 1.0f };
        const float ambient[3] = { 0.2f, 0.2f, 0.2f };
        m_renderer->SetLight(toLight, white, ambient);
    }

    // OPENBULLY_SHOT=<name>: dump one backbuffer capture ~1s in (after the
    // first frames have rendered) for automated visual verification.
    if (const char* shot = std::getenv("OPENBULLY_SHOT"))
        m_screenshotName = shot;

    // Track 29: OPENBULLY_TIME_START="HH:MM" overrides the initial game
    // clock (verification screenshots at different times of day).
    if (const char* ts = std::getenv("OPENBULLY_TIME_START")) {
        int h = -1, m = -1;
        if (std::sscanf(ts, "%d:%d", &h, &m) == 2 &&
            h >= 0 && h < 24 && m >= 0 && m < 60) {
            m_gameTimeMinutes = double(h * 60 + m);
            SyncGameClock();
            Logger::Info("Game time: start override %02d:%02d", h, m);
        } else {
            Logger::Warning("Game time: OPENBULLY_TIME_START='%s' is not HH:MM, ignored", ts);
        }
    }

    // Track 13/15: shared unit-cube mesh for debug wireframes.
    if (m_debugCollision || m_debugPhysics)
        m_renderer->InitDebugBox();

    if (m_collisionWorld)
        Logger::Info("Collision world: %zu registered objects", m_collisionWorld->ObjectCount());

    // Track 14: entity system — build the shared unit test box, register
    // level geometry as static entities, spawn the 10-box verification set
    // around the camera anchor.
    {
        Mesh box;
        const float h = 0.5f;
        box.vertices = {
            // +Z face
            { -h,-h, h, 0,0,1, 0,0 }, { h,-h, h, 0,0,1, 1,0 }, { h, h, h, 0,0,1, 1,1 }, { -h, h, h, 0,0,1, 0,1 },
            // -Z face
            {  h,-h,-h, 0,0,-1, 0,0 }, { -h,-h,-h, 0,0,-1, 1,0 }, { -h, h,-h, 0,0,-1, 1,1 }, {  h, h,-h, 0,0,-1, 0,1 },
            // +X face
            {  h,-h, h, 1,0,0, 0,0 }, {  h,-h,-h, 1,0,0, 1,0 }, {  h, h,-h, 1,0,0, 1,1 }, {  h, h, h, 1,0,0, 0,1 },
            // -X face
            { -h,-h,-h,-1,0,0, 0,0 }, { -h,-h, h,-1,0,0, 1,0 }, { -h, h, h,-1,0,0, 1,1 }, { -h, h,-h,-1,0,0, 0,1 },
            // +Y face
            { -h, h, h, 0,1,0, 0,0 }, {  h, h, h, 0,1,0, 1,0 }, {  h, h,-h, 0,1,0, 1,1 }, { -h, h,-h, 0,1,0, 0,1 },
            // -Y face
            { -h,-h,-h, 0,-1,0, 0,0 }, {  h,-h,-h, 0,-1,0, 1,0 }, {  h,-h, h, 0,-1,0, 1,1 }, { -h,-h, h, 0,-1,0, 0,1 },
        };
        for (int f = 0; f < 6; ++f) {
            const uint32_t b = uint32_t(f * 4);
            box.indices.insert(box.indices.end(), { b, b+1, b+2, b, b+2, b+3 });
        }
        box.subMeshes.push_back({ "test_box", 0, uint32_t(box.indices.size()) });
        m_testBox = m_renderer->UploadMeshGpu(box);
        m_testBoxValid = m_testBox.valid();
        if (!m_testBoxValid) Logger::Warning("Test box upload failed (entity meshes disabled)");
    }
    RegisterLevelEntities();

    // Track 17: decode the ped animation group (C_Player.agr) once, before
    // any skinned rig is built; SpawnJimmy/SpawnNpcs attach
    // AnimationComponents referencing it when clips are available.
    LoadAnimationRig();
    if (!m_sharedClips.empty())
        Logger::Info("Animation rig ready: walk=%d idle=%d (%zu clips)",
                     m_walkClipIndex, m_idleClipIndex, m_sharedClips.size());

    // Track 23: melee combat clips (Anim/C_Player.agr, MAINPED.HXD-indexed).
    // Loaded before any rig is built so CreateRig can register the punch/kick
    // clips on every skinned controller.
    LoadCombatAnimations();

    // Track 15: Jimmy, the player character. OPENBULLY_NO_JIMMY=1 skips the
    // spawn (Phase 3 free-camera mode preserved for regression).
    const bool noJimmy = std::getenv("OPENBULLY_NO_JIMMY") != nullptr;
    if (!noJimmy) {
        // Spawn on top of the anchor geometry's surface: cast down from high
        // above the anchor center and stand at the hit point (meshes are
        // registered as AABBs; their center can be inside a hollow room).
        glm::vec3 jimmySpawn(0.0f, 0.0f, 0.0f);
        glm::vec3 anchorCenter(0.0f, 0.0f, 0.0f);
        bool haveAnchor = false;
        if (!m_worldMeshes.empty()) {
            const WorldMesh* anchor = &m_worldMeshes.front();
            for (const WorldMesh& wm : m_worldMeshes)
                if (wm.gpu.vertexCount > anchor->gpu.vertexCount) anchor = &wm;
            anchorCenter = 0.5f * (glm::make_vec3(anchor->boundsMin) +
                                   glm::make_vec3(anchor->boundsMax));
            haveAnchor = true;
        }
        jimmySpawn = anchorCenter;
        if (haveAnchor && m_collisionWorld) {
            Ray down;
            down.origin[0] = anchorCenter.x;
            down.origin[1] = anchorCenter.y + 1000.0f;
            down.origin[2] = anchorCenter.z;
            down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
            RayHit hit;
            if (m_collisionWorld->Raycast(down, hit))
                jimmySpawn.y = hit.point[1]; // stand exactly on the surface
        }
        m_jimmySpawn = jimmySpawn;
        m_jimmyActive = SpawnJimmy(jimmySpawn);
        // Track 23: melee combat state on the player (covers every spawn path).
        if (m_jimmyActive) m_entities.AddMeleeCombat(m_jimmyId);
    } else {
        Logger::Info("OPENBULLY_NO_JIMMY set: free-camera mode (no player spawn)");
    }
    if (!m_jimmyActive && !noJimmy)
        Logger::Info("Jimmy inactive: free camera stays in control");

    // Track 16: NPC models + crowd spawn. Center on Jimmy's raycast-verified
    // surface position when available so NPCs share the visible play space
    // (the anchor bounds center can sit mid-air or inside hollow geometry).
    {
        size_t npcLoaded = LoadNpcModels("data/extracted/world");
        if (npcLoaded == 0)
            npcLoaded = LoadNpcModels("../data/extracted/world");
        glm::vec3 npcCenter(0.0f, 0.0f, 0.0f);
        if (m_jimmySpawn != glm::vec3(0.0f)) npcCenter = m_jimmySpawn;
        else if (!m_worldMeshes.empty()) {
            const WorldMesh* anchor = &m_worldMeshes.front();
            for (const WorldMesh& wm : m_worldMeshes)
                if (wm.gpu.vertexCount > anchor->gpu.vertexCount) anchor = &wm;
            npcCenter = 0.5f * (glm::make_vec3(anchor->boundsMin) +
                                glm::make_vec3(anchor->boundsMax));
        }
        // Track 29: load NPC schedules before the crowd spawns — with
        // schedules available SpawnNpcs routes through the schedule system.
        LoadSchedules();
        SpawnNpcs(npcCenter);
    }

    // Track 26: weapon models (bat/garbage lid/yardstick/slingshot from
    // World.img) + ground pickups around Jimmy so E-collect/1-5 equip are
    // immediately exercisable.
    if (!LoadWeaponModels()) {
        Logger::Warning("Weapons: no models loaded (weapon system disabled)");
    }
    SpawnWeaponPickups(m_jimmySpawn);

    if (!m_worldMeshes.empty()) {
        const WorldMesh* anchor = &m_worldMeshes.front();
        for (const WorldMesh& wm : m_worldMeshes)
            if (wm.gpu.vertexCount > anchor->gpu.vertexCount) anchor = &wm;
        SpawnTestEntities(0.5f * (glm::make_vec3(anchor->boundsMin) +
                                  glm::make_vec3(anchor->boundsMax)));
    } else {
        SpawnTestEntities(glm::vec3(0.0f, 0.5f, 0.0f));
    }

    // Track 20: mission system — load demo missions, register trigger zones,
    // and build the objective marker mesh. Needs Jimmy's spawn + anchor refs,
    // so it runs after the character/NPC setup above.
    InitMissions();

    // Track 30: dialogue system (same JSON probing convention as missions).
    InitDialogue();

    // Track 31: vehicles — load 4 drivable models + spawn one of each near
    // Jimmy (E mounts within 2.5u).
    if (LoadVehicleModels())
        SpawnVehicles(m_jimmyActive ? m_jimmySpawn : m_camera.GetPosition());

    // Track 20 (UI): HUD renderer. Runs on bgfx view 10 with the same
    // shaderRoot convention as Renderer::Init. Non-fatal on failure: the
    // world keeps rendering, the HUD stays off.
    if (!m_ui.Init(shaderRoot))
        Logger::Warning("UI renderer init failed; HUD disabled");
    else
        m_ui.SetScreenSize(unsigned(window.GetWidth()), unsigned(window.GetHeight()));
        // Track 23: popup centering uses the same backbuffer size.
        m_screenWidth  = unsigned(window.GetWidth());
        m_screenHeight = unsigned(window.GetHeight());

    Logger::Info("Engine initialized successfully");
    return true;
}

void Engine::Update() {
    if (!m_initialized) return;

    const Uint64 now = SDL_GetPerformanceCounter();
    const double deltaTime = double(now - m_lastPerf) / double(SDL_GetPerformanceFrequency());
    m_lastPerf = now;
    m_time += deltaTime;

    // Track 29: advance the global game clock (60x real time); schedules
    // react below after the NPC update.
    UpdateGameTime(float(deltaTime));

    // Input snapshot + camera integration (Track 10). Escape is surfaced by
    // Input; main.cpp also checks it via Engine::QuitRequested().
    m_input.Poll();
    if (m_input.QuitRequested()) m_quitRequested = true;

    // OPENBULLY_CAM_DEMO=1: scripted flythrough for headless verification of
    // the movement integration (OS-level key injection is unreliable from
    // automated environments). Drives the same Camera::Update path as real
    // input by overriding the polled buttons.
    static const bool s_camDemo = std::getenv("OPENBULLY_CAM_DEMO") != nullptr;
    if (s_camDemo) {
        const double t = m_time;
        // Phase (seconds): 0-1.5 W, 1.5-2.5 Shift+W, 2.5-3.3 pan right,
        // 3.3-4.3 D, 4.3-5.1 Space, then idle.
        m_input.SetButton(Button::Forward, t < 2.5);
        m_input.SetButton(Button::Sprint,  t >= 1.5 && t < 2.5);
        m_input.SetButton(Button::Right,   t >= 3.3 && t < 4.3);
        m_input.SetButton(Button::Up,      t >= 4.3 && t < 5.1);
        // Synthetic mouse look: ~0.4 rad total over the pan window
        // (1 px/frame at 240 fps * 0.002 rad/px ~= 0.38 rad).
        m_input.SetMouseDelta(t >= 2.5 && t < 3.3 ? 1 : 0, 0);
    }

    // OPENBULLY_JIMMY_DEMO=1: scripted WASD drive for headless verification
    // of the character movement (same injection approach as CAM_DEMO).
    static const bool s_jimmyDemo   = std::getenv("OPENBULLY_JIMMY_DEMO") != nullptr;
    static const bool s_missionDemo = std::getenv("OPENBULLY_MISSION_DEMO") != nullptr;
    static const bool s_combatDemo  = std::getenv("OPENBULLY_COMBAT_DEMO") != nullptr;
    if (s_missionDemo && m_jimmyActive) {
        // Track 20: scripted walk toward the active objective (beacon target)
        // so headless runs exercise trigger activation, objective completion,
        // and mission chaining without OS-level key injection.
        DriveMissionDemo();
    } else if (s_jimmyDemo && m_jimmyActive) {
        const double t = m_time;
        // Track 18 movement verification: ~120 s of varied phases so the
        // controller proves it never gets stuck. Pattern repeats every 15 s:
        // forward walk, sprint, strafes, a slow camera pan, backpedal, idle.
        const double phase = std::fmod(t, 15.0);
        m_input.SetButton(Button::Forward, phase < 4.0);
        m_input.SetButton(Button::Sprint,  phase >= 1.5 && phase < 4.0);
        m_input.SetButton(Button::Back,    phase >= 4.0 && phase < 6.0);
        m_input.SetButton(Button::Left,    phase >= 6.0 && phase < 8.0);
        m_input.SetButton(Button::Right,   phase >= 8.0 && phase < 10.0);
        // Slow orbit during the last stretch so the camera never clips a
        // wall the same way twice.
        m_input.SetMouseDelta(phase >= 10.0 && phase < 14.0 ? 2 : 0, 0);
    } else if (s_combatDemo && m_jimmyActive) {
        // Track 25: scripted dodge/block/attack drive for headless
        // verification. Pair with OPENBULLY_HUD_DEMO=1, which damages Jimmy
        // 10 at t=2,3,4,... so the scripted windows line up:
        //   1.9-3.2  block (RMB)    -> t=2 hit BLOCKED + COUNTER armed;
        //                               t=3 hit BLOCKED (plain)
        //   3.95-4.15 Shift+W tap   -> dodge roll; t=4 hit lands inside the
        //                               0.5 s i-frames -> DODGED (0 damage)
        //   6.0-6.1  LMB            -> punch spends 10 stamina; the armed
        //                               counter is consumed for the bonus
        //   7-9      idle           -> stamina regen (20/s) visible in the
        //                               once-per-second stamina log
        const double t = m_time;
        m_input.SetButton(Button::Block,  t >= 1.9 && t < 3.2);
        m_input.SetButton(Button::Sprint, t >= 3.95 && t < 4.5);
        m_input.SetButton(Button::Forward, t >= 3.95 && t < 4.15);
        m_input.SetButton(Button::Attack, t >= 6.0 && t < 6.1);
        // Once-per-second stamina trace so the log shows deplete + regen.
        static double s_lastStamLog = -1e9;
        if (t - s_lastStamLog >= 1.0) {
            s_lastStamLog = t;
            if (PlayerController* cpc = m_entities.GetPlayer(m_jimmyId))
                Logger::Info("Stamina: %.0f/%.0f (t=%.1f)",
                             cpc->stamina.current, cpc->stamina.max, t);
        }
    }

    // Track 23: scripted melee demo. Must inject BEFORE UpdateJimmy reads
    // m_input: Poll() at the top of the next frame wipes bottom-of-frame
    // SetButton writes (this bit us once — see git history).
    static const bool s_meleeDemoDrive = std::getenv("OPENBULLY_MELEE_DEMO") != nullptr;
    if (s_meleeDemoDrive && m_jimmyActive) DriveMeleeDemo(float(deltaTime));

    // Track 27: scripted NPC AI demo. Position-direct (like MELEE_DEMO) so it
    // must run before UpdateJimmy/UpdateNpcs consume the state this frame.
    static const bool s_npcDemo = std::getenv("OPENBULLY_NPC_DEMO") != nullptr;
    if (s_npcDemo && m_jimmyActive) DriveNpcDemo(float(deltaTime));

    m_camera.Update(float(deltaTime), m_input, /*ignoreMove=*/m_jimmyActive);

    // Track 14: entity update (physics integration on gravity carriers).
    UpdateEntities(float(deltaTime));

    // Track 15: Jimmy combat state, then input/physics, then the camera.
    UpdateJimmyCombat(float(deltaTime));
    UpdateJimmy(float(deltaTime));
    // Track 16: NPC AI (after physics so ground/wall state is settled).
    UpdateNpcs(float(deltaTime));

    // Track 29: schedule debug keys (T = +1 hour) + spawn/despawn/behavior
    // ticks driven by the game clock.
    HandleScheduleKeys();
    UpdateSchedules();

    // Track 30: dialogue input + typewriter. Runs before the combat/weapon
    // updates so their input handlers see the conversation-active gate the
    // same frame (E starts/advances the talk instead of attacking/pickup).
    UpdateDialogue(float(deltaTime));

    // Track 24: combat input + grapple state machine (after the NPC update
    // so hold positions settle this frame). The demo script runs ahead of
    // input handling the same way CAM/JIMMY demos inject buttons.
    static const bool s_grappleDemo = std::getenv("OPENBULLY_GRAPPLE_DEMO") != nullptr;
    if (s_grappleDemo) DriveGrappleDemo();
    HandleCombatInput();
    UpdateGrapple(float(deltaTime));

    // Track 23: melee combat — attack input/progression, hit frames,
    // damage/stun/knockback, combo. (The OPENBULLY_MELEE_DEMO script runs at
    // the top of Update so its button injection survives Poll().)
    UpdateMeleeCombat(float(deltaTime));

    // Track 26: weapon pickups/equip/attacks, then projectile integration.
    UpdateWeapons(float(deltaTime));
    UpdateProjectiles(float(deltaTime));

    // Track 31: vehicles — E toggle, mounted driving, then the camera (the
    // vehicle follow replaces UpdateJimmyCamera while mounted).
    UpdateVehicles(float(deltaTime));
    if (m_mountedVehicle.IsValid()) UpdateVehicleCamera(float(deltaTime));
    else UpdateJimmyCamera(float(deltaTime));

    // Track 20 (UI): H/J/K debug keys mutate vitals, then refresh the HUD
    // snapshot the renderer draws from (called again in Render so pauses
    // still present an accurate overlay).
    HandleHudDebugKeys();
    UpdateHUD(float(deltaTime));

    // Track 20: mission system — fire trigger zones and check objective
    // progress from Jimmy's position every frame.
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        if (const Transform* jt = m_entities.GetTransform(m_jimmyId))
            m_missions.Update(jt->position);
    }

    // Track 21: audio listener follows the camera; M toggles mute. IsPressed
    // fires on the down-transition frame only, so holding M doesn't flap.
    if (m_audio) {
        if (m_input.IsPressed(Button::Mute))
            m_audio->ToggleMute();
        m_audio->SetListener(m_camera.GetPosition(), m_camera.GetYaw());
    }

    // Track 13: camera collision (free camera only; the 3rd-person follow
    // camera resolves inside UpdateJimmyCamera). Skip when Jimmy is active:
    // WASD belongs to the character, and the camera was just placed by the
    // follow logic.
    bool didGroundRaycast = false;
    if (m_collisionWorld && !m_jimmyActive) {
        const glm::vec3 p = m_camera.GetPosition();
        const float radius = 0.35f;
        float fixed[3];
        if (m_collisionWorld->ResolveSphere(&p[0], radius, fixed)) {
            m_camera.SetPosition(glm::vec3(fixed[0], fixed[1], fixed[2]));
        }
        // Ground raycast: cast straight down from the (possibly corrected)
        // camera; report the floor height once per timing-log window.
        didGroundRaycast = true;
        Ray down;
        down.origin[0] = p.x; down.origin[1] = p.y; down.origin[2] = p.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "floor %.2f below camera (%.2f units)",
                          hit.point[1], p.y - hit.point[1]);
            m_groundRaycastInfo = buf;
        } else {
            m_groundRaycastInfo = "no hit";
        }
    }
    (void)didGroundRaycast;

    ++m_framesAccum;
    m_frameMsAccum += deltaTime * 1000.0;
    if (m_time - m_lastTimingLog >= 1.0) {
        LogFrameTiming();
        m_lastTimingLog = m_time;
    }
}

// --- Track 30: dialogue system ---------------------------------------------------

// Loads data/dialogue/demo_dialogue.json (hand-authored Bully-flavored trees;
// .lur bytecode and XACT banks are deferred per the asset catalog). Same
// probing convention as the mission loader: CWD first, then the source tree.
bool Engine::InitDialogue() {
    const char* paths[] = {
        "data/dialogue/demo_dialogue.json",
        "../data/dialogue/demo_dialogue.json",
    };
    bool loaded = false;
    for (const char* p : paths) {
        if (std::ifstream f(p); f.good()) {
            f.close();
            loaded = m_dialogue.LoadFile(p);
            break;
        }
    }
    if (!loaded) {
        Logger::Warning("Dialogue: demo_dialogue.json not found or empty "
                        "(dialogue system idle)");
        return false;
    }
    // Conversation end: release the NPC + complete the TALK_TO_NPC objective.
    m_dialogue.SetOnEnd([this] { OnDialogueEnded(); });
    return true;
}

// E-press path: start a conversation with the nearest NPC within 2.5u when
// NOT in combat (no grapple, no live melee exchange, player grounded in a
// normal defensive state). The tree is picked by the NPC's model index so
// different NPC types carry different conversations.
void Engine::TryStartDialogue() {
    if (m_dialogue.IsActive() || !m_jimmyActive || !m_jimmyId.IsValid()) return;
    if (m_dialogue.TreeCount() == 0) return;
    if (m_grappleTarget.IsValid()) return; // mid-grapple: no talking
    if (const Melee::CombatComponent* mc = m_entities.GetMeleeCombat(m_jimmyId))
        if (mc->state != Melee::CombatState::Idle) return; // mid-fight
    if (const PlayerController* pc = m_entities.GetPlayer(m_jimmyId))
        if (pc->state == DefenseState::Dodge) return;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;

    const EntityID npc = FindNearestNpc(jt->position, 2.5f);
    if (!npc.IsValid() || !m_entities.IsAlive(npc)) return;

    // Track 30: "not in combat" also covers the NPC side — an NPC fighting
    // (Chase/Attack), fleeing, or downed cannot be talked to. ALERT (the
    // sighted-player pause) stays allowed: it is a pre-combat face/pause, and
    // blocking it made E-to-talk nearly unreachable once Track 27's
    // sight-chase was restored (an approaching NPC is Alert at 10u and
    // already Chase by the time it is within 2.5u). Starting a conversation
    // de-escalates: the dialogue freeze pulls the NPC out of the AI rotation
    // and normal AI resumes when the conversation ends.
    if (const AIComponent* ai = m_entities.GetAI(npc)) {
        switch (ai->state) {
            case AIState::Chase:
            case AIState::Attack:
            case AIState::Flee:
            case AIState::Ko:
                return;
            default: break;
        }
    }
    if (const Melee::CombatComponent* nmc = m_entities.GetMeleeCombat(npc))
        if (nmc->state != Melee::CombatState::Idle) return;

    // Tree assignment: NPC model index cycles through the loaded trees
    // (students/prefects/bullies/nerds). Distinct NPC types get distinct
    // conversations; repeats wrap deterministically.
    int treeIndex = 0;
    if (const MeshComponent* mesh = m_entities.GetMesh(npc))
        treeIndex = int(mesh->npcModelIndex) % m_dialogue.TreeCount();

    if (m_dialogue.StartDialogue(m_dialogue.TreeId(treeIndex), "")) {
        m_dialogueNpc = npc;
        m_dialogueChoiceSel = 0;
        Logger::Info("Dialogue: talk started with NPC %u (tree '%s')",
                     npc.index, m_dialogue.TreeId(treeIndex).c_str());
    }
}

// Conversation-end hook (fires exactly once from DialogueManager::EndDialogue
// via the SetOnEnd callback): release the NPC and complete any active
// mission's TALK_TO_NPC objective (mission hook deliverable).
void Engine::OnDialogueEnded() {
    m_dialogueNpc = {};
    m_dialogueChoiceSel = 0;
    Logger::Info("Dialogue: conversation ended (normal AI resumes)");
    for (uint32_t i = 0; i < m_missions.MissionCount(); ++i) {
        const Mission* m = m_missions.GetMission(i);
        if (!m || m->state != MissionState::Active) continue;
        if (m->currentObjective >= m->objectives.size()) continue;
        const Objective& o = m->objectives[m->currentObjective];
        if (o.type == ObjectiveType::TalkToNpc && !o.complete) {
            Logger::Info("Dialogue: completing TALK_TO_NPC objective for '%s'",
                         m->id.c_str());
            m_missions.CompleteObjective(i, m->currentObjective);
            break;
        }
    }
}

// Per-frame dialogue logic: input (start/advance/choose) + typewriter tick.
// Ordering note: this runs right after UpdateNpcs and BEFORE the melee and

// --- Track 31: vehicles -------------------------------------------------------

// Loads the 4 drivable vehicle NIFs from the extracted World.img set. Every
// model bakes its node-hierarchy transforms (same as level geometry), gets a
// size-normalizing scale (longest horizontal extent -> per-type length), and
// resolves sub-mesh textures through the shared NFT dictionary.
bool Engine::LoadVehicleModels() {
    struct Spec { const char* nif; VehicleType type; float length; };
    static const Spec kSpecs[] = {
        { "bike.nif",      VehicleType::Bike,  2.0f },
        { "Policecar.nif", VehicleType::Car,   4.6f },
        { "Moped.nif",     VehicleType::Moped, 1.9f },
        { "GoCart.nif",    VehicleType::Kart,  2.2f },
    };
    const char* dirs[] = { "data/extracted/world", "../data/extracted/world" };

    size_t loaded = 0;
    for (const Spec& spec : kSpecs) {
        bool done = false;
        for (const char* dir : dirs) {
            const std::string path = std::string(dir) + "/" + spec.nif;
            NifLoadResult nif = LoadNifFile(path);
            if (!nif.ok || nif.totalVertices < 10) continue;
            Mesh mesh;
            if (!MeshFromNif(nif, mesh)) continue;

            // Bake node-hierarchy transforms into the vertices (positions
            // through the full chain, normals through rotation only).
            size_t vertBase = 0;
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
                        if (v < geo.normals.size()) {
                            const float nx = wm[0]*nrm.x + wm[4]*nrm.y + wm[8]*nrm.z;
                            const float ny = wm[1]*nrm.x + wm[5]*nrm.y + wm[9]*nrm.z;
                            const float nz = wm[2]*nrm.x + wm[6]*nrm.y + wm[10]*nrm.z;
                            mv.nx = nx; mv.ny = ny; mv.nz = nz;
                        }
                    }
                }
                vertBase += geo.vertices.size();
            }

            GpuMesh gpu = m_renderer->UploadMeshGpu(mesh);
            if (!gpu.valid()) continue;

            VehicleModel vm;
            vm.name = spec.nif;
            vm.type = spec.type;
            vm.gpu  = gpu;
            for (const MeshSubMesh& sub : mesh.subMeshes) {
                WorldMesh::SubRange sr;
                sr.indexOffset = sub.indexOffset;
                sr.indexCount  = sub.indexCount;
                sr.texture     = AcquireTexture(m_texDir.c_str(), sub.textureName);
                vm.subs.push_back(sr);
            }

            // Indexed bounds (orphans excluded, same as level geometry).
            glm::vec3 lo( FLT_MAX), hi(-FLT_MAX);
            for (uint32_t idx : mesh.indices) {
                const MeshVertex& v = mesh.vertices[idx];
                lo = glm::min(lo, glm::vec3(v.x, v.y, v.z));
                hi = glm::max(hi, glm::vec3(v.x, v.y, v.z));
            }
            const glm::vec3 ext = glm::max(hi - lo, glm::vec3(0.001f));
            const float horiz = std::max(ext.x, ext.z);
            vm.drawScale = spec.length / horiz;
            vm.length    = spec.length;
            vm.height    = ext.y * vm.drawScale;
            // Forward convention: longest horizontal axis is the vehicle's
            // forward; its sign is remembered so mounted driving aligns the
            // model with motion (models vary; verified per-type at spawn).
            vm.length    = ext.z > ext.x ? spec.length : spec.length;

            Logger::Info("Vehicle model '%s' (%s): %zu verts, extent (%.1f, %.1f, %.1f), scale %.3f",
                         spec.nif, VehicleTypeName(spec.type), mesh.vertices.size(),
                         ext.x, ext.y, ext.z, vm.drawScale);
            m_vehicleModels.push_back(std::move(vm));
            loaded++;
            done = true;
            break;
        }
        if (!done)
            Logger::Warning("Vehicles: '%s' not found or failed to load", spec.nif);
    }
    Logger::Info("Vehicles: %zu/4 models loaded", loaded);
    return loaded > 0;
}

// Spawns one entity per loaded vehicle model in a ring around `center`, each
// raycast-dropped onto the ground. Vehicles render as static models (their
// own Transform drives the model matrix) until mounted.
void Engine::SpawnVehicles(const glm::vec3& center) {
    if (m_vehicleModels.empty()) return;
    for (size_t i = 0; i < m_vehicleModels.size(); ++i) {
        const VehicleModel& vm = m_vehicleModels[i];
        const float ang = float(i) * (6.2831853f / float(m_vehicleModels.size())) + 0.4f;
        // Phase 9 integration: ground-verified ring placement. The fixed 6u
        // ring could sit past a rooftop edge, where the down-ray finds no
        // surface — the vehicle then floats at center.y + 2 and mounting it
        // seeds the physics body mid-air (0 wheel contacts -> both rider and
        // vehicle free-fall). Pull the ring point toward `center` until the
        // down-ray hits, so every vehicle spawns on standable ground.
        glm::vec3 pos = center + glm::vec3(std::cos(ang) * 6.0f, 2.0f, std::sin(ang) * 6.0f);
        bool grounded = false;
        if (m_collisionWorld) {
            for (float radius = 6.0f; radius >= 0.0f; radius -= 1.0f) {
                const glm::vec3 p = center + glm::vec3(std::cos(ang) * radius, 0.0f,
                                                        std::sin(ang) * radius);
                Ray down;
                down.origin[0] = p.x; down.origin[1] = p.y + 50.0f; down.origin[2] = p.z;
                down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
                RayHit hit;
                if (m_collisionWorld->Raycast(down, hit)) {
                    pos = glm::vec3(p.x, hit.point[1] + 0.05f, p.z);
                    grounded = true;
                    if (radius < 6.0f)
                        Logger::Info("Vehicle spawn: pulled ring in to r=%.0f "
                                     "(no ground at r=6)", radius);
                    break;
                }
            }
        }
        if (!grounded)
            pos.y = center.y; // last resort: level with the verified center

        EntityID e = m_entities.Create();
        Transform t;
        t.position = pos;
        t.scale    = glm::vec3(vm.drawScale);
        m_entities.AddTransform(e, t);

        MeshComponent mc;
        mc.isBox = false;
        mc.isVehicle = true;
        mc.vehicleModelIndex = uint32_t(i);
        m_entities.AddMesh(e, mc); // vehicle draw path keys off the component

        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-vm.length * 0.5f, 0.0f, -vm.length * 0.5f);
        cc.boundsMax = glm::vec3( vm.length * 0.5f, vm.height,  vm.length * 0.5f);
        cc.layerMask = 2u;
        m_entities.AddCollision(e, cc);

        VehicleComponent vc;
        vc.type = vm.type;
        vc.yaw  = ang + 3.14159265f; // face inward
        vc.wheelCount = (vm.type == VehicleType::Car || vm.type == VehicleType::Kart) ? 4 : 2;
        // Wheel anchors in local model space (pre-scale): corners for 4-wheel;
        // bikes mirror the single axle pair into all 4 slots.
        const float wx = vm.length * 0.32f, wz = vm.length * 0.18f;
        vc.wheelPositions[0] = glm::vec3( wx, 0.0f,  wz);
        vc.wheelPositions[1] = glm::vec3( wx, 0.0f, -wz);
        vc.wheelPositions[2] = glm::vec3(-wx, 0.0f,  wz);
        vc.wheelPositions[3] = glm::vec3(-wx, 0.0f, -wz);
        m_entities.AddVehicle(e, vc);

        m_vehicleIds.push_back(e);
        Logger::Info("Vehicle spawned: %s (%s) at (%.1f, %.1f, %.1f)",
                     vm.name.c_str(), VehicleTypeName(vm.type), pos.x, pos.y, pos.z);
    }
}

// Nearest vehicle entity within maxDist of pos (invalid when none).
EntityID Engine::FindNearestVehicle(const glm::vec3& pos, float maxDist) const {
    EntityID best{};
    float bestDist = maxDist;
    for (const EntityID& id : m_vehicleIds) {
        if (!id.IsValid() || !m_entities.IsAlive(id)) continue;
        const Transform* t = m_entities.GetTransform(id);
        if (!t) continue;
        const float d = glm::length(t->position - pos);
        if (d < bestDist) { bestDist = d; best = id; }
    }
    return best;
}

// E-press toggle. Mount guards: not while a dialogue is active/starting,
// mid-grapple, mid-melee, or dodging. Dismount always allowed.
void Engine::TryToggleVehicleMount() {
    if (m_mountedVehicle.IsValid()) { DismountVehicle(); return; }
    if (!m_jimmyActive || !m_jimmyId.IsValid()) return;
    if (m_dialogue.IsActive() || m_dialogueNpc.IsValid()) return; // talk takes E
    if (m_grappleTarget.IsValid()) return;
    if (const Melee::CombatComponent* mc = m_entities.GetMeleeCombat(m_jimmyId))
        if (mc->state != Melee::CombatState::Idle) return;
    if (const PlayerController* pc = m_entities.GetPlayer(m_jimmyId))
        if (pc->state == DefenseState::Dodge) return;

    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;
    const EntityID veh = FindNearestVehicle(jt->position, 2.5f);
    if (!veh.IsValid()) return;
    MountVehicle(veh);
}

void Engine::MountVehicle(EntityID vehicle) {
    if (!vehicle.IsValid() || !m_entities.IsAlive(vehicle)) return;
    VehicleComponent* vc = m_entities.GetVehicle(vehicle);
    Transform* vt = m_entities.GetTransform(vehicle);
    if (!vc || !vt || vc->mounted) return;
    Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;

    vc->mounted = true;
    vc->rider   = m_jimmyId;
    vc->speed   = 0.0f;
    vc->steerAngle = 0.0f;
    m_mountedVehicle = vehicle;

    // Hide the player mesh (rider sits inside the model); keep physics/anim
    // state alive so dismount restores exactly.
    m_entities.RemoveMesh(m_jimmyId);
    // Snap Jimmy onto the vehicle so camera focus + dismount offset start sane.
    jt->position = vt->position;

    m_preMountCamPos = m_camera.GetPosition();
    m_preMountPitch  = m_camera.GetPitch();
    // Void-guard reference (see UpdateMountedVehiclePhysics).
    m_vehicleMountSeedPos = vt->position + glm::vec3(0.0f, 0.8f, 0.0f);
    m_vehicleVoidFloorY   = vt->position.y - 100.0f;
    // Track 32: DEMO=2 closed-loop driver orbits the midpoint of all
    // spawned vehicles (the courtyard deck center — the bike's own spawn
    // sits near the roof rim, where an orbit rolls off the edge). The bike
    // is also repositioned to the deck center so the orbit has room; its
    // old spot by the rim would send every circle off the drop.
    m_vehicleDemoCenter = glm::vec3(0.0f);
    int nVeh = 0;
    for (EntityID id : m_vehicleIds) {
        if (const Transform* t2 = m_entities.GetTransform(id)) {
            m_vehicleDemoCenter += t2->position; ++nVeh;
        }
    }
    if (nVeh > 0) m_vehicleDemoCenter /= float(nVeh);
    else m_vehicleDemoCenter = vt->position;
    if (m_vehicleIds.size() > 1) {
        // Reposition the mounted vehicle to a clear spot next to the deck
        // center (the exact midpoint overlaps the parked car), at ground
        // level found by a downward raycast.
        glm::vec3 spot = m_vehicleDemoCenter + glm::vec3(2.0f, 0.0f, 2.0f);
        if (m_collisionWorld) {
            Ray down;
            down.origin[0] = spot.x; down.origin[1] = spot.y + 30.0f; down.origin[2] = spot.z;
            down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
            RayHit hit;
            if (m_collisionWorld->Raycast(down, hit)) spot.y = hit.point[1];
        }
        if (Transform* tSelf = m_entities.GetTransform(vehicle))
            tSelf->position = spot;
        m_vehicleDemoCenter = spot;
        m_vehicleMountSeedPos = spot + glm::vec3(0.0f, 0.8f, 0.0f);
    }

    if (PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
        pc->facingYaw   = vc->yaw;
        pc->facingValid = true;
    }
    jt->rotation.y = vc->yaw;

    // Track 33: bind the audio manager once, then engine-start sfx + hum.
    if (m_audio)
        SetVehicleAudio(m_audio.get());
    VehicleSfxEngineStart(vc->type, vt->position);

    Logger::Info("Vehicle: mounted %s (entity %u)",
                 VehicleTypeName(vc->type), vehicle.index);
}

void Engine::DismountVehicle() {
    if (!m_mountedVehicle.IsValid()) return;
    VehicleComponent* vc = m_entities.GetVehicle(m_mountedVehicle);
    Transform* vt = m_entities.GetTransform(m_mountedVehicle);
    m_mountedVehicle = {};
    if (!vc || !vt) return;

    vc->mounted = false;
    vc->rider   = EntityID{};
    vc->speed   = 0.0f;

    Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;

    // Place the player beside the vehicle (left side), on the ground.
    const glm::vec3 fwd(std::sin(vc->yaw), 0.0f, std::cos(vc->yaw));
    const glm::vec3 right(fwd.z, 0.0f, -fwd.x);
    glm::vec3 pos = vt->position - right * (vt->scale.x * 1.2f + 0.6f);
    if (m_collisionWorld) {
        Ray down;
        down.origin[0] = pos.x; down.origin[1] = pos.y + 3.0f; down.origin[2] = pos.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) pos.y = hit.point[1];
    }
    jt->position = pos;
    if (PhysicsComponent* p = m_entities.GetPhysics(m_jimmyId)) {
        p->velocity    = glm::vec3(0.0f);
        p->onGround    = true;
    }

    // Restore the player mesh.
    MeshComponent mc;
    mc.isBox   = false;
    mc.isJimmy = true;
    m_entities.AddMesh(m_jimmyId, mc);

    Logger::Info("Vehicle: dismounted %s at (%.1f, %.1f, %.1f)",
                 VehicleTypeName(vc->type), pos.x, pos.y, pos.z);
}

// Per-frame vehicle logic: E toggle, mounted driving (throttle/steer, world
// collision, ground follow), wheel anchor update, and rider sync.
void Engine::UpdateVehicles(float dt) {
    // Track 31 verification hook: OPENBULLY_VEHICLE_DEMO=1 scripts mount ->
    // drive -> dismount for headless screenshots (same philosophy as the
    // other *_DEMO hooks). t=2s mount, 2-6s drive forward, t=6s dismount.
    static const bool s_vehicleDemo = std::getenv("OPENBULLY_VEHICLE_DEMO") != nullptr;
    if (s_vehicleDemo && m_jimmyActive) {
        static double s_demoTrace = 0.0;
        if (m_time < 8.0 && m_time - s_demoTrace >= 1.0) {
            s_demoTrace = m_time;
            if (Transform* jt = m_entities.GetTransform(m_jimmyId)) {
                const EntityID nearestVeh = FindNearestVehicle(jt->position, 60.0f);
                const Transform* nt = m_entities.GetTransform(nearestVeh);
                Logger::Info("VehicleDemo: t=%.0f mounted=%s jimmy=(%.1f,%.1f,%.1f) nearest=%s dist=%.1f",
                             m_time, m_mountedVehicle.IsValid() ? "yes" : "no",
                             jt->position.x, jt->position.y, jt->position.z,
                             nt ? "veh" : "none",
                             nt ? glm::length(nt->position - jt->position) : -1.0f);
            }
        }
        if (m_time >= 2.0 && !m_mountedVehicle.IsValid() && m_time < 5.9) {
            // Teleport Jimmy to the nearest vehicle (position-direct, like
            // DIALOGUE_DEMO): the spawn ring can sit across a courtyard the
            // walk can't cross. Y snaps to the vehicle's ground. DEMO=3
            // targets the bike specifically — the wheelie script needs one,
            // and the nearest ring vehicle may be the car (wheelies are
            // bike-only in the feel layer).
            if (Transform* jt = m_entities.GetTransform(m_jimmyId)) {
                static const int s_demoModeSel =
                    std::atoi(std::getenv("OPENBULLY_VEHICLE_DEMO"));
                EntityID veh{};
                if (s_demoModeSel == 3) {
                    for (const EntityID& id : m_vehicleIds) {
                        if (!id.IsValid() || !m_entities.IsAlive(id)) continue;
                        const VehicleComponent* c = m_entities.GetVehicle(id);
                        if (c && c->type == VehicleType::Bike) { veh = id; break; }
                    }
                }
                if (!veh.IsValid()) veh = FindNearestVehicle(jt->position, 60.0f);
                if (const Transform* vt2 = m_entities.GetTransform(veh)) {
                    glm::vec3 to = vt2->position - jt->position;
                    to.y = 0.0f;
                    const float d = glm::length(to);
                    jt->position.y = vt2->position.y; // same ground
                    if (PhysicsComponent* pp = m_entities.GetPhysics(m_jimmyId))
                        pp->velocity = glm::vec3(0.0f);
                    if (d > 1.6f) jt->position += to / d * (3.0f * dt);
                    else TryToggleVehicleMount();
                }
            }
        }
        if (m_mountedVehicle.IsValid()) {
            static const int s_demoMode =
                std::atoi(std::getenv("OPENBULLY_VEHICLE_DEMO"));
            if (s_demoMode == 1) {
                // Track 31 short script: drive forward with a slight curve
                // between 3-5.9 s, dismount after 6 s.
                m_input.SetButton(Button::Forward, m_time >= 3.0 && m_time < 5.9);
                m_input.SetButton(Button::Right,   m_time >= 4.0 && m_time < 4.8);
                if (m_time >= 6.0) TryToggleVehicleMount(); // dismount
            } else if (s_demoMode == 3) {
                // Track 33 wheelie script: kick-start straight ahead to build
                // speed (steering authority is speed-proportional), then hold
                // Shift from t=4.5 s -> wheelie lifts; stunt log ~3 s later;
                // the progressive instability then over-rotates -> eject ->
                // CRASH log. Void guard recovers any edge excursion.
                const double t = m_time;
                if (t < 4.5) {
                    m_input.SetButton(Button::Forward, true);
                    m_input.SetButton(Button::Back,    false);
                    m_input.SetButton(Button::Left,    false);
                    m_input.SetButton(Button::Right,   false);
                    m_input.SetButton(Button::Sprint,  false);
                } else {
                    const glm::vec3 center = m_vehicleDemoCenter;
                    if (const Transform* jt2 = m_entities.GetTransform(m_mountedVehicle)) {
                        glm::vec3 toVeh = jt2->position - center;
                        toVeh.y = 0.0f;
                        const float dist = glm::length(toVeh);
                        glm::vec3 goal;
                        if (dist > 4.0f) goal = center;
                        else {
                            const glm::vec3 rad = dist > 0.1f ? toVeh / dist : glm::vec3(1, 0, 0);
                            goal = center + glm::vec3(-rad.z, 0.0f, rad.x) * 4.0f;
                        }
                        glm::vec3 toGoal = goal - jt2->position;
                        toGoal.y = 0.0f;
                        const float targetYaw = std::atan2(toGoal.x, toGoal.z);
                        float dyaw = targetYaw - m_vehicle.yaw;
                        while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
                        while (dyaw < -3.14159265f) dyaw += 6.28318531f;
                        const float steer = glm::clamp(dyaw * 2.0f, -1.0f, 1.0f);
                        m_input.SetButton(Button::Left,  steer < -0.15f);
                        m_input.SetButton(Button::Right, steer >  0.15f);
                        m_input.SetButton(Button::Forward, true); // keep driving
                        m_input.SetButton(Button::Back,    false);
                        // Track 33: hold Shift -> wheelie while moving.
                        m_input.SetButton(Button::Sprint, true);
                    }
                }
            } else if (s_demoMode == 2) {
                // Track 32 extended physics drive: closed-loop orbit around
                // the mount point (robust to spawn orientation/edges —
                // open-loop WASD scripts drove straight off the courtyard).
                // Exercises throttle, brake, both steer directions and the
                // reverse window; the 1 Hz log is the stability evidence.
                const glm::vec3 center = m_vehicleDemoCenter;
                if (const Transform* jt2 = m_entities.GetTransform(m_mountedVehicle)) {
                    glm::vec3 toVeh = jt2->position - center;
                    toVeh.y = 0.0f;
                    const float dist = glm::length(toVeh);
                    // Tangent point: circle the center at radius 3 (turn
                    // radius at the 4.5 u/s cruise cap is ~4.5u, so the
                    // circle stays drivable inside the courtyard).
                    glm::vec3 goal;
                    if (dist > 3.0f) goal = center;
                    else {
                        const glm::vec3 rad = dist > 0.1f ? toVeh / dist : glm::vec3(1, 0, 0);
                        goal = center + glm::vec3(-rad.z, 0.0f, rad.x) * 3.0f;
                    }
                    glm::vec3 toGoal = goal - jt2->position;
                    toGoal.y = 0.0f;
                    const float targetYaw = std::atan2(toGoal.x, toGoal.z);
                    float dyaw = targetYaw - m_vehicle.yaw;
                    while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
                    while (dyaw < -3.14159265f) dyaw += 6.28318531f;
                    // Edge probe: no ground 3 u ahead -> treat straight-ahead
                    // as forbidden and steer along the courtyard instead
                    // (the rooftop is ringed by drops; this keeps the orbit
                    // on the deck without relying on the void guard).
                    bool edgeAhead = false;
                    if (m_collisionWorld) {
                        const glm::vec3 probePos = jt2->position +
                            glm::vec3(std::sin(m_vehicle.yaw), 0.0f,
                                      std::cos(m_vehicle.yaw)) * 3.0f;
                        Ray down;
                        down.origin[0] = probePos.x;
                        down.origin[1] = probePos.y + 2.0f;
                        down.origin[2] = probePos.z;
                        down.direction[0] = 0.0f;
                        down.direction[1] = -1.0f;
                        down.direction[2] = 0.0f;
                        RayHit hit;
                        edgeAhead = !m_collisionWorld->Raycast(down, hit);
                    }
                    float steer = glm::clamp(dyaw * 2.0f, -1.0f, 1.0f);
                    if (edgeAhead) {
                        steer = 1.0f; // hard right until ground is ahead again
                    }
                    m_input.SetButton(Button::Left,  steer < -0.15f);
                    m_input.SetButton(Button::Right, steer >  0.15f);
                    // Always drive forward and steer toward the goal: the
                    // steering authority is speed-proportional, so waiting
                    // for alignment deadlocks at rest, and reverse/brake
                    // windows flip-flop around the facing-away boundary.
                    // Brake over the 4.5 u/s cruise cap, or near an edge
                    // (crawl instead of launching off the drop).
                    const bool fast = m_vehicle.speedAlongFacing >
                                      (edgeAhead ? 1.5f : 4.5f);
                    m_input.SetButton(Button::Forward, !fast);
                    m_input.SetButton(Button::Back,    fast);
                }
            }
        }
    }

    if (m_input.WasKeyPressed(SDL_SCANCODE_E) && !m_dialogue.IsActive())
        TryToggleVehicleMount();

    // Idle bob/spin is NOT applied to vehicles (they stay parked).
    if (!m_mountedVehicle.IsValid()) return;
    VehicleComponent* vc = m_entities.GetVehicle(m_mountedVehicle);
    Transform* vt = m_entities.GetTransform(m_mountedVehicle);
    if (!vc || !vt || !m_entities.IsAlive(m_mountedVehicle)) {
        m_mountedVehicle = {}; // vehicle destroyed underneath us
        return;
    }

    // --- Driving (Track 32 physics) -----------------------------------------
    // Throttle/brake from the same buttons as before; VehiclePhysics owns
    // the wheel raycasts, spring-damper suspension, drive/brake/steer,
    // drag and wall bounce. Result synced back into vc + the transform.
    UpdateMountedVehiclePhysics(dt, vc, vt);

    vt->rotation.x = m_vehicle.pitch + m_vehicle.cosmeticPitch;
    vt->rotation.y = vc->yaw;
    vt->rotation.z = m_vehicle.roll;

    // Rider follows the vehicle (hidden mesh, physics parked).
    if (Transform* jt = m_entities.GetTransform(m_jimmyId)) {
        jt->position  = vt->position + glm::vec3(0.0f, 0.5f, 0.0f);
        jt->rotation.y = vc->yaw;
    }
    if (PhysicsComponent* p = m_entities.GetPhysics(m_jimmyId)) {
        p->velocity = glm::vec3(0.0f);
        p->onGround = true;
    }
}

// Track 32: arcade vehicle physics for the mounted vehicle. One spring-
// damper wheel model drives everything: 4 downward raycasts (FL/FR/RL/RR),
// per-wheel suspension force along the ground normal, rear-wheel drive,
// brake/reverse, front-wheel steering with yaw rate ~ speed * steer, drag,
// gravity when airborne, and a sphere-vs-wall bounce (restitution 0.3).
// Body pitch/roll follow the wheel suspension deltas (terrain adaptation);
// a cosmetic pitch offset gives the weight-transfer feel.
void Engine::UpdateMountedVehiclePhysics(float dt, VehicleComponent* vc,
                                         Transform* vt) {
    const float throttle = m_input.IsDown(Button::Forward) ? 1.0f : 0.0f;
    const float brake    = m_input.IsDown(Button::Back)    ? 1.0f : 0.0f;
    const float steer    = (m_input.IsDown(Button::Right) ? 1.0f : 0.0f) -
                           (m_input.IsDown(Button::Left)  ? 1.0f : 0.0f);

    // (Re)bind the collision world and seed the body from the entity state
    // on the first physics frame after a mount (or after a world swap).
    if (m_vehicle.world != m_collisionWorld.get()) {
        m_vehicle.world = m_collisionWorld.get();
        m_vehiclePhysSeeded = false;
    }
    if (!m_vehiclePhysSeeded) {
        // Seed slightly compressed (chassis 0.8u above ground < restLength +
        // wheelRadius = 0.85) so the springs make contact on the first frame;
        // seeding at exactly the rest height left contacts=0 and the body
        // free-fell (suspension only pushes, never pulls).
        m_vehicle.position = vt->position + glm::vec3(0.0f, 0.8f, 0.0f);
        m_vehicle.velocity = glm::vec3(0.0f);
        m_vehicle.yaw      = vc->yaw;
        m_vehicle.yawRate  = 0.0f;
        m_vehiclePhysSeeded = true;
        m_vehicleAccum = 0.0f;
    }

    // Fixed-timestep stepping: the engine frame rate is uncapped (~250 fps
    // observed), and the drive/suspension constants are tuned per-step at
    // 60 Hz. Accumulate real time and run deterministic 1/60 s steps so the
    // behavior matches the headless probe exactly (stability guarantee).
    constexpr float kFixedDt = 1.0f / 60.0f;
    m_vehicleAccum += std::min(dt, 0.1f);
    int steps = 0;
    while (m_vehicleAccum >= kFixedDt && steps < 4) {
        m_vehicle.Update(kFixedDt, steer, throttle, brake);
        m_vehicleAccum -= kFixedDt;
        ++steps;
    }

    // Drive-gate debugging: dump the first 6 THROTTLED frames to stdout
    // (drive starts at t=3 in the demo, so mount-time frames are skipped).
    static int s_trace = 0;
    if (s_trace < 6 && throttle > 0.0f) { ++s_trace; m_vehicle.traceFrames = 1; }

    // Void guard (demo level has open edges): falling far below the mount
    // point means the vehicle drove off geometry — reset onto the mount spot
    // instead of falling forever.
    if (m_vehicle.position.y < m_vehicleVoidFloorY) {
        m_vehicle.position = m_vehicleMountSeedPos;
        m_vehicle.velocity = glm::vec3(0.0f);
        m_vehiclePhysSeeded = true;
        Logger::Info("Vehicle: void guard reset to mount point");
    }

    // Sync back: component yaw/speed/steer for Track 31's systems (camera,
    // FOV, feel layer), transform for rendering (pitch/roll from terrain).
    vc->yaw        = m_vehicle.yaw;
    vc->speed      = m_vehicle.speedAlongFacing;
    vc->steerAngle = m_vehicle.steerAngle;
    vt->position   = m_vehicle.position;

    // --- Track 33: handling feel (wheelie, crash/eject, per-type character) --
    UpdateVehicleFeel(dt, vc, vt, throttle, brake, steer);

    // Wheel anchors (component local space) for whoever needs them.
    const glm::vec3 fwd = m_vehicle.Forward(), rgt = m_vehicle.Right();
    for (int i = 0; i < 4; ++i)
        vc->wheelPositions[i] = rgt * m_vehicle.wheels[i].localOffset.x +
                                fwd * m_vehicle.wheels[i].localOffset.z;

    // Stability trace: once per second, log the driving state. In DEMO=2 the
    // first 3 samples also dump raw input/ground state (drive-gate evidence).
    static double s_lastLog = -1e9;
    if (m_time - s_lastLog >= 1.0) {
        s_lastLog = m_time;
        static double s_diagT = -10.0;
        if (m_time - s_diagT >= 1.0 && s_diagT < 0.0) s_diagT = m_time;
        if (m_time - s_diagT < 3.0) {
            Logger::Info("VehicleDiag: fwd=%d back=%d left=%d right=%d "
                         "thr=%.0f brk=%.0f contacts=%d yaw=%.2f",
                         m_input.IsDown(Button::Forward) ? 1 : 0,
                         m_input.IsDown(Button::Back) ? 1 : 0,
                         m_input.IsDown(Button::Left) ? 1 : 0,
                         m_input.IsDown(Button::Right) ? 1 : 0,
                         throttle, brake, m_vehicle.wheelContacts,
                         m_vehicle.yaw);
        }
        Logger::Info("Vehicle: t=%.0f pos=(%.1f,%.1f,%.1f) speed=%.2f "
                     "contacts=%d airborne=%s",
                     m_time, m_vehicle.position.x, m_vehicle.position.y,
                     m_vehicle.position.z, m_vehicle.speedAlongFacing,
                     m_vehicle.wheelContacts,
                     m_vehicle.Airborne() ? "yes" : "no");
    }
}

// Track 33: handling feel — feeds the per-frame input snapshot through the
// feel layer (per-type drive character, wheelie, crash/eject) and applies the
// results on top of the Track 32 body: yaw agility delta, wheelie pitch,
// crash tilt, and the eject/dismount path.
void Engine::UpdateVehicleFeel(float dt, VehicleComponent* vc, Transform* vt,
                               float throttle, float brake, float steer) {
    FeelState& feel = m_vehicleFeel;
    feel.type       = vc->type;
    feel.inThrottle = throttle > 0.5f;
    feel.inBrake    = brake > 0.5f;
    feel.inLeanL    = steer < -0.5f;
    feel.inLeanR    = steer > 0.5f;
    feel.inWheelie  = m_input.IsDown(Button::Sprint); // Shift = wheelie on bike

    const float preSpeed = m_vehicle.speedAlongFacing;
    FeelOut fo = StepFeel(feel, m_vehicle.position, m_vehicle.yaw, preSpeed, dt);

    // Feel yaw delta: per-type low-speed agility on top of the Track 32
    // steering (small damped rate, so it composes rather than fights it).
    m_vehicle.yaw += fo.yawDelta;
    vc->yaw = m_vehicle.yaw;

    // Wheelie pitch overlays the terrain pitch (cosmetic channel only).
    if (feel.wheelieAngle > 0.0f)
        m_vehicle.cosmeticPitch = fo.pitch;
    else if (m_vehicle.cosmeticPitch != 0.0f && !feel.crashed)
        m_vehicle.cosmeticPitch = 0.0f;

    // Crash: lay the body on its side; auto-right clears it after 3 s.
    if (feel.crashed)
        m_vehicle.roll = 80.0f * 3.14159265f / 180.0f;

    // Wall impact: the body lost its forward motion against geometry this
    // frame (contacts lost + velocity killed by the bounce) -> run the
    // crash/eject check against the pre-bounce forward speed.
    if (!feel.crashed && m_vehicle.wheelContacts == 0 &&
        glm::length(m_vehicle.velocity) < 0.5f &&
        std::fabs(preSpeed) > kEjectImpactSpeed) {
        FeelOut io;
        if (FeelImpact(feel, m_vehicle.position, m_vehicle.yaw,
                       std::fabs(preSpeed), io)) {
            fo = io; // promote the eject result
            m_vehicle.velocity = glm::vec3(0.0f);
            m_vehicle.speedAlongFacing = 0.0f;
            vc->speed = 0.0f;
            VehicleSfxCrash(io.impactSpeed, m_vehicle.position);
            Logger::Info("CRASH: %s rider ejected at %.1f u/s",
                         VehicleTypeName(vc->type), io.impactSpeed);
        }
    }

    // Events -> player placement / dismount.
    if (fo.ejectRider) {
        DismountVehicle();
        if (Transform* jt = m_entities.GetTransform(m_jimmyId))
            jt->position = fo.ejectPos;
    }
    if (fo.stuntLogged)
        Logger::Info("VEHICLE: %s wheelie stunt accepted",
                     VehicleTypeName(vc->type));
}

// Vehicle camera: behind and above the vehicle along its yaw, slight FOV
// widening handled by UpdateVehicleFov in Render. Runs instead of the normal
// Jimmy follow when mounted.
void Engine::UpdateVehicleCamera(float dt) {
    if (!m_mountedVehicle.IsValid()) return;
    const VehicleComponent* vc = m_entities.GetVehicle(m_mountedVehicle);
    const Transform* vt = m_entities.GetTransform(m_mountedVehicle);
    if (!vc || !vt) return;

    const glm::vec3 fwd(std::sin(vc->yaw), 0.0f, std::cos(vc->yaw));
    const glm::vec3 focus = vt->position + glm::vec3(0.0f, 1.2f, 0.0f);
    glm::vec3 desired = focus - fwd * 5.5f + glm::vec3(0.0f, 2.0f, 0.0f);

    // Smooth follow, frame-rate independent.
    const float a = 1.0f - std::pow(1.0f - 0.15f, dt * 60.0f);
    glm::vec3 pos = glm::mix(m_camera.GetPosition(), desired, a);

    // Keep the camera out of walls.
    if (m_collisionWorld) {
        const float in[3] = { pos.x, pos.y, pos.z };
        float out[3];
        if (m_collisionWorld->ResolveSphere(in, 0.35f, out))
            pos = glm::vec3(out[0], out[1], out[2]);
    }

    m_camera.SetPosition(pos);
    m_camera.SetLookAt(focus - pos);
}

// Per-frame dialogue logic: input (start/advance/choose) + typewriter tick.
// Ordering note: this runs right after UpdateNpcs and BEFORE the melee and
// weapon updates, so their !m_dialogue.IsActive() input gates are consistent
// within the same frame.
void Engine::UpdateDialogue(float dt) {
    // OPENBULLY_DIALOGUE_DEMO=1: auto-start a conversation at t=1s for
    // headless screenshot/stability verification (same philosophy as the
    // other *_DEMO hooks), then auto-advance ~2 lines/s so the whole tree
    // plays out (typewriter reveal, next hops, choice picks, end hook) —
    // without this the demo stuck forever on the first line's full reveal.
    static const bool s_demo = std::getenv("OPENBULLY_DIALOGUE_DEMO") != nullptr;
    if (s_demo) {
        if (!m_dialogue.IsActive()) {
            // Walk Jimmy (position-direct, plane-guarded like DriveNpcDemo)
            // toward the nearest NPC so the talk reaches 2.5u: ambient NPCs
            // never close in on their own (Alert freezes them, Return walks
            // them home), so the demo must bring the player to them.
            if (m_jimmyActive && m_jimmyId.IsValid()) {
                if (Transform* pt = m_entities.GetTransform(m_jimmyId)) {
                    EntityID talkNpc = FindNearestNpc(pt->position, 60.0f);
                    if (const Transform* nt = m_entities.GetTransform(talkNpc)) {
                        if (PhysicsComponent* pp = m_entities.GetPhysics(m_jimmyId)) {
                            pt->position.y = nt->position.y;
                            pp->velocity.y = 0.0f;
                        }
                        glm::vec3 to = nt->position - pt->position;
                        to.y = 0.0f;
                        const float d = glm::length(to);
                        if (d > 1.6f && d < 55.0f)
                            pt->position += to / d * (2.5f * dt);
                        if (PlayerController* dpc = m_entities.GetPlayer(m_jimmyId);
                            dpc && d > 1e-3f) {
                            dpc->facingYaw   = std::atan2(to.x / d, to.z / d);
                            dpc->facingValid = true;
                            pt->rotation.y   = dpc->facingYaw;
                        }
                    }
                }
            }
            // Try (re)start every 2s while idle: after a conversation ends
            // the next one begins within 2s, so a soak run cycles
            // start -> play -> end repeatedly (end hook + mission hookup
            // exercised every cycle).
            static double s_lastTry = 0.0;
            if (m_time >= 1.0 && m_time - s_lastTry > 2.0) {
                s_lastTry = m_time;
                TryStartDialogue();
            }
        } else {
            // Advance the instant the typewriter finishes the line; pick
            // choice 0 when a menu opens (1s dwell so the menu shows).
            static double s_menuSince = 0.0;
            const std::vector<DialogueChoice>* demoChoices =
                m_dialogue.ActiveChoices();
            if (demoChoices && !demoChoices->empty()) {
                if (s_menuSince == 0.0) s_menuSince = m_time;
                if (m_time - s_menuSince > 1.0) {
                    Logger::Info("Dialogue demo: picking choice 0 of %zu",
                                 demoChoices->size());
                    m_dialogue.ChooseOption(0);
                    s_menuSince = 0.0;
                }
            } else {
                s_menuSince = 0.0;
                if (m_dialogue.LineFullyRevealed()) m_dialogue.Advance();
            }
        }
    }

    if (m_dialogue.IsActive()) {
        const std::vector<DialogueChoice>* choices = m_dialogue.ActiveChoices();
        if (choices && !choices->empty()) {
            // Choice menu: number keys 1-4 pick directly; arrows + Enter move
            // the highlighted row. W/S mirror the arrows (movement is frozen
            // during conversation, so they cannot conflict).
            if      (m_input.WasKeyPressed(SDL_SCANCODE_1)) m_dialogue.ChooseOption(0);
            else if (m_input.WasKeyPressed(SDL_SCANCODE_2)) m_dialogue.ChooseOption(1);
            else if (m_input.WasKeyPressed(SDL_SCANCODE_3)) m_dialogue.ChooseOption(2);
            else if (m_input.WasKeyPressed(SDL_SCANCODE_4)) m_dialogue.ChooseOption(3);
            else {
                if (m_input.WasKeyPressed(SDL_SCANCODE_UP) ||
                    m_input.WasKeyPressed(SDL_SCANCODE_W))
                    m_dialogueChoiceSel = (m_dialogueChoiceSel + int(choices->size()) - 1)
                                          % int(choices->size());
                if (m_input.WasKeyPressed(SDL_SCANCODE_DOWN) ||
                    m_input.WasKeyPressed(SDL_SCANCODE_S))
                    m_dialogueChoiceSel = (m_dialogueChoiceSel + 1) % int(choices->size());
                if (m_input.WasKeyPressed(SDL_SCANCODE_RETURN) ||
                    m_input.WasKeyPressed(SDL_SCANCODE_KP_ENTER))
                    m_dialogue.ChooseOption(m_dialogueChoiceSel);
            }
        } else if (m_input.WasKeyPressed(SDL_SCANCODE_E) ||
                   m_input.WasKeyPressed(SDL_SCANCODE_SPACE)) {
            // Skip line: reveal the full line; press again for the next.
            m_dialogue.Advance();
        }
        m_dialogue.Update(dt); // typewriter reveal (0.03 s per character)
    } else if (m_input.WasKeyPressed(SDL_SCANCODE_E)) {
        // Start: E near an NPC when not in combat.
        TryStartDialogue();
    }
}

// --- Track 24: grappling system -------------------------------------------------

// Nearest alive NPC (AI carrier) within maxDist of pos (XZ distance).
EntityID Engine::FindNearestNpc(const glm::vec3& pos, float maxDist) {
    EntityID best; // default = invalid
    float bestD2 = maxDist * maxDist;
    m_entities.ForEachAI([&](EntityID id, const Transform& t, AIComponent&) {
        const glm::vec3 d = t.position - pos;
        const float d2 = d.x * d.x + d.z * d.z;
        if (d2 < bestD2) { bestD2 = d2; best = id; }
    });
    return best;
}

// Pairs Jimmy (attacker, CombatState::Grabbing) with the nearest NPC
// (target, CombatState::Grappled) within maxDist units.
bool Engine::TryStartGrapple(float maxDist) {
    if (!m_jimmyActive || !m_jimmyId.IsValid() || m_grappleTarget.IsValid())
        return false;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return false;
    const EntityID target = FindNearestNpc(jt->position, maxDist);
    if (!target.IsValid()) {
        Logger::Info("Grapple: no NPC within %.1f units", maxDist);
        return false;
    }
    CombatComponent* jc = m_entities.AddCombat(m_jimmyId);
    CombatComponent* tc = m_entities.AddCombat(target);
    if (!jc || !tc) return false;
    // Re-fetch: the second AddCombat may reallocate the dense component
    // array, leaving the first pointer dangling (classic sparse-set hazard).
    jc = m_entities.GetCombat(m_jimmyId);
    tc = m_entities.GetCombat(target);
    if (!jc || !tc) return false;
    jc->state = CombatState::Grabbing;
    jc->stateTimer = 0.0f;
    jc->moveCooldown = 0.0f;
    jc->move = GrappleMove::None;
    tc->state = CombatState::Grappled;
    tc->stateTimer = 0.0f;
    tc->mashCount = 0;
    tc->npcMashTimer = 0.0f;
    m_grappleTarget = target;
    Logger::Info("Grapple: started (npc #%u, mash threshold %d)",
                 target.index, tc->mashRequired);
    // Grapple animation: the World.img grapple clip plays on both rigs when
    // it loaded; otherwise UpdateEntityAnimation freezes the current pose.
    if (m_grappleClipIndex >= 0) {
        for (EntityID id : { m_jimmyId, target }) {
            AnimationComponent* ac = m_entities.GetAnimation(id);
            if (ac && ac->rigIndex < m_animRigs.size()) {
                m_animRigs[ac->rigIndex].controller.Play(m_grappleClipIndex,
                                                          true, 0.2f);
                m_animRigs[ac->rigIndex].grapplePlaying = true;
            }
        }
    }
    return true;
}

// Tears the grapple down: both combat states reset, walk/idle playback
// resumes cleanly. `targetBrokeFree` only labels the log line.
void Engine::EndGrapple(bool targetBrokeFree) {
    if (!m_grappleTarget.IsValid()) return;
    if (CombatComponent* jc = m_entities.GetCombat(m_jimmyId)) {
        jc->state = CombatState::None;
        jc->moveCooldown = 0.0f;
        jc->move = GrappleMove::None;
    }
    if (CombatComponent* tc = m_entities.GetCombat(m_grappleTarget)) {
        tc->state = CombatState::None;
        tc->mashCount = 0;
    }
    Logger::Info("Grapple: ended (%s)",
                 targetBrokeFree ? "target broke free" : "released");
    for (EntityID id : { m_jimmyId, m_grappleTarget }) {
        AnimationComponent* ac = m_entities.GetAnimation(id);
        if (!ac) continue;
        ac->wasMoving = false; // forces the walk/idle state machine to re-Play
        if (ac->rigIndex >= m_animRigs.size()) continue;
        AnimRig& rig = m_animRigs[ac->rigIndex];
        rig.grapplePlaying = false;
        // With a grapple clip active we must hand playback back explicitly
        // (the walk/idle edge detector only fires on a moving-state change).
        if (m_grappleClipIndex >= 0 && rig.idleClip >= 0)
            rig.controller.Play(rig.idleClip, true, 0.2f);
    }
    m_grappleTarget = EntityID{};
}

// Attacker grapple move: damage the held target; the throw (and a KO) end
// the grapple. Cooldowns keep the moves from machine-gunning.
void Engine::GrappleAttack(GrappleMove move) {
    if (!m_grappleTarget.IsValid()) return;
    CombatComponent* jc = m_entities.GetCombat(m_jimmyId);
    CombatComponent* tc = m_entities.GetCombat(m_grappleTarget);
    if (!jc || !tc || jc->state != CombatState::Grabbing) return;
    if (jc->moveCooldown > 0.0f) return;

    static const GrappleMoveInfo kMoves[3] = {
        {  8, 0.6f, false, "headlock strike" },
        { 12, 0.8f, false, "knee strike"     },
        { 20, 1.0f, true,  "throw"           },
    };
    const int idx = int(move) - int(GrappleMove::HeadlockStrike);
    if (idx < 0 || idx > 2) return;
    const GrappleMoveInfo& info = kMoves[idx];
    jc->move = move;
    jc->moveCooldown = info.cooldown;
    tc->health -= info.damage;
    Logger::Info("Grapple %s: %d damage -> NPC health %d",
                 info.name, info.damage, tc->health);
    if (tc->health <= 0) {
        Logger::Info("Grapple: NPC KO'd by %s", info.name);
        EndGrapple(false);
        return;
    }
    if (info.endsGrapple) {
        Logger::Info("Grapple: throw completes - target released");
        EndGrapple(false);
    }
}

// Per-frame combat input: G enters/ends the grapple, U/I/O are grapple moves
// for the attacker and mash presses for a held human target.
void Engine::HandleCombatInput() {
    if (!m_jimmyId.IsValid()) return;
    CombatComponent* jc = m_entities.GetCombat(m_jimmyId);
    if (!jc) return;
    const bool active = m_grappleTarget.IsValid();

    if (m_input.WasKeyPressed(SDL_SCANCODE_G)) {
        if (!active) {
            TryStartGrapple();
        } else if (jc->state == CombatState::Grabbing) {
            Logger::Info("Grapple: attacker released (G)");
            EndGrapple(false);
        }
    }
    if (!active) return;

    if (jc->state == CombatState::Grabbing) {
        // Attacker: grapple-specific moves.
        if (m_input.WasKeyPressed(SDL_SCANCODE_U))
            GrappleAttack(GrappleMove::HeadlockStrike);
        else if (m_input.WasKeyPressed(SDL_SCANCODE_I))
            GrappleAttack(GrappleMove::KneeStrike);
        else if (m_input.WasKeyPressed(SDL_SCANCODE_O))
            GrappleAttack(GrappleMove::Throw);
    } else if (jc->state == CombatState::Grappled) {
        // Held human target: rapid button mashing counts presses; after
        // mashRequired edges the target breaks free.
        const bool mash = m_input.WasKeyPressed(SDL_SCANCODE_U) ||
                          m_input.WasKeyPressed(SDL_SCANCODE_I) ||
                          m_input.WasKeyPressed(SDL_SCANCODE_O) ||
                          m_input.WasKeyPressed(SDL_SCANCODE_SPACE);
        if (mash) {
            jc->mashCount++;
            Logger::Info("Grapple: mash %d/%d",
                         jc->mashCount, jc->mashRequired);
            if (jc->mashCount >= jc->mashRequired) {
                jc->state = CombatState::BreakingFree;
                jc->stateTimer = 0.0f;
                Logger::Info("Grapple: broke free after %d presses",
                             jc->mashCount);
            }
        }
    }
}

// Grapple state machine: cooldowns, target break-free progress (NPC targets
// mash on a simulated timer; human targets via HandleCombatInput), and the
// pair hold (arm's-length offset + mutual facing).
void Engine::UpdateGrapple(float dt) {
    if (dt <= 0.0f) return;
    if (!m_jimmyId.IsValid()) return;
    if (!m_grappleTarget.IsValid()) return;
    if (!m_entities.IsAlive(m_jimmyId) || !m_entities.IsAlive(m_grappleTarget)) {
        m_grappleTarget = EntityID{};
        return;
    }
    CombatComponent* jc = m_entities.GetCombat(m_jimmyId);
    CombatComponent* tc = m_entities.GetCombat(m_grappleTarget);
    if (!jc || !tc) { m_grappleTarget = EntityID{}; return; }

    if (jc->moveCooldown > 0.0f) jc->moveCooldown -= dt;

    // Target break-free progress.
    if (tc->state == CombatState::Grappled) {
        // NPC targets mash on a simulated cadence (one press per interval).
        tc->npcMashTimer += dt;
        if (tc->npcMashTimer >= tc->npcMashInterval) {
            tc->npcMashTimer = 0.0f;
            tc->mashCount++;
            if (tc->mashCount >= tc->mashRequired) {
                tc->state = CombatState::BreakingFree;
                tc->stateTimer = 0.0f;
                Logger::Info("Grapple: NPC broke free after %d presses",
                             tc->mashCount);
            }
        }
    } else if (tc->state == CombatState::BreakingFree) {
        tc->stateTimer += dt;
        if (tc->stateTimer >= 0.6f) EndGrapple(true);
    }

    // Pair maintenance: target held at arm's length, both face each other.
    if (!m_grappleTarget.IsValid()) return; // ended by the break above
    Transform* jt = m_entities.GetTransform(m_jimmyId);
    Transform* tt = m_entities.GetTransform(m_grappleTarget);
    if (!jt || !tt) return;
    glm::vec3 toT = tt->position - jt->position;
    toT.y = 0.0f;
    const float d = glm::length(toT);
    if (d > 1e-4f) {
        const glm::vec3 dir = toT / d;
        if (PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
            pc->facingYaw = std::atan2(dir.x, dir.z);
            pc->facingValid = true;
            jt->rotation.y = pc->facingYaw;
        }
        const float hold = 0.85f;
        tt->position = jt->position + dir * hold;
        if (AIComponent* ai = m_entities.GetAI(m_grappleTarget)) {
            ai->facingYaw = std::atan2(-dir.x, -dir.z);
            tt->rotation.y = ai->facingYaw;
        }
    }
    tt->position.y = jt->position.y;
}

// OPENBULLY_GRAPPLE_DEMO=1: scripted headless verification. Teleports Jimmy
// in front of the nearest NPC, starts the grapple, lands a headlock strike
// and a knee strike, then accelerates the NPC's mash so the break-free path
// runs. Pair with OPENBULLY_SHOT for the verification screenshot.
void Engine::DriveGrappleDemo() {
    if (!m_jimmyActive || !m_jimmyId.IsValid()) return;
    static int sStage = 0;
    const double t = m_time;
    if (sStage == 0 && t >= 1.0) {
        Transform* jt = m_entities.GetTransform(m_jimmyId);
        if (!jt) return;
        const EntityID target = FindNearestNpc(jt->position, 30.0f);
        if (!target.IsValid()) return;
        const Transform* tt = m_entities.GetTransform(target);
        if (!tt) return;
        glm::vec3 to = tt->position - jt->position;
        to.y = 0.0f;
        const float d = glm::length(to);
        if (d > 1e-4f) {
            const glm::vec3 dir = to / d;
            jt->position = tt->position - dir * 1.2f;
            if (PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
                pc->facingYaw = std::atan2(dir.x, dir.z);
                jt->rotation.y = pc->facingYaw;
            }
        }
        sStage = 1;
    }
    if (sStage == 1 && t >= 1.5) {
        if (TryStartGrapple(2.0f)) sStage = 2;
        else sStage = 0; // retry: the NPC may have wandered out of reach
    }
    if (sStage == 2 && t >= 3.5) {
        Logger::Info("GrappleDemo: stage 2 -> headlock strike");
        GrappleAttack(GrappleMove::HeadlockStrike);
        sStage = 3;
    }
    if (sStage == 3 && t >= 4.5) {
        Logger::Info("GrappleDemo: stage 3 -> knee strike");
        GrappleAttack(GrappleMove::KneeStrike);
        sStage = 4;
    }
    if (sStage == 4 && t >= 5.5) {
        if (CombatComponent* tc = m_entities.GetCombat(m_grappleTarget))
            tc->npcMashInterval = 0.05f; // rapid mash -> break-free demo
        sStage = 5;
    }
}

// Track 20 (UI): debug keys for HUD verification.
//   H - damage Jimmy 10 (health bar shrinks)
//   J - wanted level +1 (star fills, clamped 0..5)
//   K - wanted level -1 (star empties)
// Uses Input::WasKeyPressed edge detection, so holding a key fires once.
void Engine::HandleHudDebugKeys() {
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc) return; // no player: nothing to mutate (free-camera mode)

    // OPENBULLY_HUD_DEMO=1: scripted vitals for headless HUD verification
    // (same philosophy as CAM_DEMO/JIMMY_DEMO: OS key injection is
    // unreliable from automated environments). t=1s: wanted -> 3;
    // t>=2s: -10 health per second.
    static const bool s_hudDemo = std::getenv("OPENBULLY_HUD_DEMO") != nullptr;
    if (s_hudDemo) {
        if (m_time >= 1.0 && pc->wanted < 3) {
            pc->wanted = 3;
            Logger::Info("HUD demo: wanted -> 3");
        }
        static double s_lastHit = -1e9;
        if (m_time >= 2.0 && m_time - s_lastHit >= 1.0) {
            s_lastHit = m_time;
            // Track 25: route through DamagePlayer so the HUD demo also
            // exercises dodge i-frames / block reduction / counter window.
            DamagePlayer(10, "HUD demo");
            Logger::Info("HUD demo: damage 10 -> health %d/%d",
                         pc->health.current, pc->health.max);
        }
    }

    if (m_input.WasKeyPressed(SDL_SCANCODE_H)) {
        DamagePlayer(10, "HUD debug");
        Logger::Info("HUD debug: damage 10 -> health %d/%d",
                     pc->health.current, pc->health.max);
    }
    if (m_input.WasKeyPressed(SDL_SCANCODE_J)) {
        pc->wanted = std::min(5, pc->wanted + 1);
        Logger::Info("HUD debug: wanted -> %d", pc->wanted);
    }
    if (m_input.WasKeyPressed(SDL_SCANCODE_K)) {
        pc->wanted = std::max(0, pc->wanted - 1);
        Logger::Info("HUD debug: wanted -> %d", pc->wanted);
    }
}

// Track 20 (UI): refresh the plain-data HUD snapshot from live game state.
// Runs after gameplay updates so the overlay always reflects this frame.
void Engine::UpdateHUD(float dt) {
    (void)dt;

    // Game clock: advanced by UpdateGameTime (Track 29, 60x real time);
    // this snapshot only mirrors the accumulator for the HUD.
    m_hudState.clockMinutes = m_clockMinutes;

    // Player vitals + minimap anchor.
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        if (const PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
            m_hudState.health = pc->health.current;
            m_hudState.maxHealth = pc->health.max;
            m_hudState.wanted = pc->wanted;
            m_hudState.money = pc->money;
            // Track 26: weapon inventory slots for the HUD.
            for (int i = 0; i < HUDState::kMaxWeaponSlots; ++i) {
                HUDState::WeaponSlotInfo& info = m_hudState.weaponSlots[i];
                info.valid = info.equipped = false;
                info.name[0] = '\0'; info.durability = info.ammo = 0;
                const InventorySlot& slot = pc->slots[i];
                if (slot.type == WeaponType::Unarmed) continue;
                const WeaponData& data = GetWeaponDataById(slot.weaponId);
                info.valid = true;
                info.equipped = (pc->equippedSlot == i);
                std::snprintf(info.name, sizeof(info.name), "%s", data.name);
                info.durability = slot.durability;
                info.ammo = slot.ammo;
            }
            // Track 25: stamina pool + combat state for the HUD.
            m_hudState.stamina = int(pc->stamina.current);
            m_hudState.maxStamina = int(pc->stamina.max);
            m_hudState.combatState = int(pc->state);
        }

        // Track 24: grapple HUD status + mash progress.
        m_hudState.grappleState = 0;
        m_hudState.grappleMash = 0;
        if (m_grappleTarget.IsValid()) {
            if (CombatComponent* jc = m_entities.GetCombat(m_jimmyId)) {
                m_hudState.grappleState =
                    jc->state == CombatState::Grabbing ? 1 :
                    jc->state == CombatState::Grappled ? 2 :
                    jc->state == CombatState::BreakingFree ? 3 : 0;
            }
            if (CombatComponent* tc = m_entities.GetCombat(m_grappleTarget))
                m_hudState.grappleMash = std::min(tc->mashCount, tc->mashRequired);
        }
        if (const Transform* jt = m_entities.GetTransform(m_jimmyId)) {
            m_hudState.playerX = jt->position.x;
            m_hudState.playerZ = jt->position.z;
            if (const PlayerController* pc = m_entities.GetPlayer(m_jimmyId))
                m_hudState.playerYaw = pc->facingYaw;
        }
    }

    // Nearby NPC dots for the minimap (player-relative screen mapping
    // happens in UIRenderer::DrawHUD; here we only collect world XZ).
    m_hudState.npcDotCount = 0;
    m_entities.ForEachAI([&](EntityID, const Transform& t, const AIComponent&) {
        if (m_hudState.npcDotCount >= HUDState::kMaxMinimapDots) return;
        m_hudState.npcDots[m_hudState.npcDotCount].x = t.position.x;
        m_hudState.npcDots[m_hudState.npcDotCount].z = t.position.z;
        ++m_hudState.npcDotCount;
    });
}

// --- Track 29: time-of-day + NPC schedules -----------------------------------

// Advances the global game clock at 60x real time (1 real second = 1 game
// minute). Frame hitches are clamped so an alt-tab pause cannot
// fast-forward the day. The HUD clock mirrors this accumulator.
void Engine::UpdateGameTime(float dt) {
    double step = double(dt) * 60.0; // 60x: 1 real second = 1 game minute
    if (step < 0.0)   step = 0.0;
    if (step > 120.0) step = 120.0;  // clamp hitches to 2 game-minutes
    m_gameTimeMinutes += step;
    SyncGameClock();
}

// Derives the integer HUD clock from the fractional game-time accumulator.
void Engine::SyncGameClock() {
    while (m_gameTimeMinutes >= 24.0 * 60.0) m_gameTimeMinutes -= 24.0 * 60.0;
    if (m_gameTimeMinutes < 0.0) m_gameTimeMinutes = 0.0;
    m_clockMinutes = int(m_gameTimeMinutes) % (24 * 60);
    m_hudState.clockMinutes = m_clockMinutes;
}

// Track 29 debug: T fast-forwards 1 hour (schedule testing without waiting
// through the 60x clock). OPENBULLY_TIME_DEMO=1 drives the same +1h path
// every 3 s for automated verification (OS key injection is unreliable).
void Engine::HandleScheduleKeys() {
    bool advanceHour = m_input.WasKeyPressed(SDL_SCANCODE_T);
    static const bool s_timeDemo = std::getenv("OPENBULLY_TIME_DEMO") != nullptr;
    if (s_timeDemo && !advanceHour) {
        static int s_lastStep = -1;
        const int step = int(m_time / 3.0); // one hour every 3 s
        if (step >= 1 && step <= 8 && step != s_lastStep) {
            s_lastStep = step;
            advanceHour = true;
            Logger::Info("Time demo: scripted +1h (step %d)", step);
        }
    }
    if (advanceHour) {
        m_gameTimeMinutes += 60.0;
        SyncGameClock();
        Logger::Info("Schedule debug: +1h -> game time %02d:%02d",
                     GetGameTime().hour, GetGameTime().minute);
        UpdateSchedules(); // react on the same frame
    }
}

// Track 29: load the schedule set (mission-JSON probing convention: CWD
// first, then the source tree; refs resolved like the mission loader).
bool Engine::LoadSchedules() {
    std::unordered_map<std::string, glm::vec3> refs;
    refs["spawn"] = m_jimmySpawn;
    if (!m_worldMeshes.empty()) {
        const WorldMesh* anchor = &m_worldMeshes.front();
        for (const WorldMesh& wm : m_worldMeshes)
            if (wm.gpu.vertexCount > anchor->gpu.vertexCount) anchor = &wm;
        refs["anchor_center"] = 0.5f * (glm::make_vec3(anchor->boundsMin) +
                                        glm::make_vec3(anchor->boundsMax));
        refs["anchor_min"] = glm::make_vec3(anchor->boundsMin);
        refs["anchor_max"] = glm::make_vec3(anchor->boundsMax);
    }
    const char* paths[] = {
        "data/schedules/npc_schedules.json",
        "../data/schedules/npc_schedules.json",
    };
    for (const char* p : paths) {
        if (std::ifstream f(p); f.good()) {
            f.close();
            if (m_schedules.LoadFromJson(p, refs)) {
                Logger::Info("Schedules: %zu loaded from %s",
                             m_schedules.ScheduleCount(), p);
                return true;
            }
            return false;
        }
    }
    Logger::Warning("Schedules: npc_schedules.json not found (schedules idle)");
    return false;
}

// Track 29: create the entity for one scheduled NPC slot at its window's
// spawn point (ground-snapped). The slot's rig clone is built once and
// re-used across respawns so day cycles do not accumulate rigs.
EntityID Engine::SpawnScheduledNpc(ScheduledNpc& sn, const ScheduleWindow& w) {
    const NpcModel& model = m_npcModels[sn.modelIndex];
    glm::vec3 pos = w.hasSpawnPoint ? w.spawnPoint : sn.spawnPoint;
    if (m_collisionWorld) {
        Ray down;
        down.origin[0] = pos.x; down.origin[1] = pos.y + 10.0f; down.origin[2] = pos.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) pos.y = hit.point[1];
    }

    // Lazily build + clone the model's skinned rig once per slot.
    uint32_t rigIndex = sn.rigIndex;
    if (rigIndex == 0xFFFFFFFF) {
        rigIndex = model.rigIndex;
        if (rigIndex == 0xFFFFFFFF && !m_sharedClips.empty() &&
            !model.sourcePath.empty()) {
            const NifLoadResult nif = LoadNifFile(model.sourcePath);
            if (nif.ok) rigIndex = CreateRig(nif);
            m_npcModels[sn.modelIndex].rigIndex = rigIndex; // cached (no retry)
            if (rigIndex != 0xFFFFFFFF)
                Logger::Info("NPC '%s': skinned rig %u (%d bones)",
                             model.name.c_str(), rigIndex,
                             m_animRigs[rigIndex].controller.BoneCount());
        }
        if (rigIndex != 0xFFFFFFFF) rigIndex = CloneRig(rigIndex);
        sn.rigIndex = rigIndex;
    }

    // Deterministic per-slot variation (same pattern as the legacy crowd).
    uint32_t rng = sn.seed ? sn.seed : 0x1234ABCDu;
    auto next = [&rng]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    auto unit = [&next]() { return float(next() % 10000u) / 10000.0f; };

    EntityID e = m_entities.Create();
    Transform t;
    t.position = pos;
    t.scale = glm::vec3(model.bodyScale);
    t.rotation.y = unit() * 6.2831853f;
    m_entities.AddTransform(e, t);

    MeshComponent mc;
    mc.isBox = false;
    mc.isNpc = true;
    mc.npcModelIndex = sn.modelIndex;
    m_entities.AddMesh(e, mc);

    CollisionComponent cc;
    cc.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
    cc.boundsMax = glm::vec3( 0.5f, 1.8f,  0.5f);
    cc.layerMask = 2u; // dynamic layer
    m_entities.AddCollision(e, cc);

    PhysicsComponent pc;
    pc.gravity = -9.8f;
    m_entities.AddPhysics(e, pc);

    AIComponent ai;
    ai.homePos = pos;
    ai.facingYaw = t.rotation.y;
    ai.lastPos = pos; // Track 27: stuck-detection baseline
    ai.state = AIState::Idle;
    ai.timer = 0.2f + unit() * 2.8f;
    ai.moveSpeed = 1.0f + unit() * 1.0f;
    ai.wanderRadius = 3.0f;
    ai.scheduleIndex = sn.scheduleIndex;
    // Window behavior override: idle pins the NPC at its post.
    ai.lockIdle = (w.behavior == ScheduleBehavior::Idle);
    // Track 27: every 4th scheduled NPC (by slot index) walks a patrol loop —
    // 4 ground-snapped waypoints on a square around its spawn point.
    if (sn.scheduleIndex % 4 == 0) {
        const float radius = 3.5f + unit() * 2.0f;
        const float phase  = unit() * 6.2831853f;
        ai.patrolPoints.clear();
        ai.patrolPoints.reserve(4);
        for (int wp = 0; wp < 4; ++wp) {
            const float a = phase + float(wp) * 1.5707963f;
            glm::vec3 point(pos.x + std::cos(a) * radius, pos.y,
                            pos.z + std::sin(a) * radius);
            if (m_collisionWorld) {
                Ray down;
                down.origin[0] = point.x; down.origin[1] = pos.y + 10.0f;
                down.origin[2] = point.z;
                down.direction[0] = 0.0f; down.direction[1] = -1.0f;
                down.direction[2] = 0.0f;
                RayHit hit;
                if (m_collisionWorld->Raycast(down, hit) &&
                    std::fabs(hit.point[1] - pos.y) < 3.0f)
                    point.y = hit.point[1];
            }
            ai.patrolPoints.push_back(point);
        }
        ai.patrolIndex = 0;
    }
    // Track 28: authority NPCs. Cap of 2 (task spec): prefer scheduled spawns
    // whose schedule is named "prefect" (data/npc_schedules.json); if no
    // prefect schedule spawned yet, the first two spawned slots fill the cap.
    if (m_prefectIds[1].index == 0xFFFFFFFF) {
        bool isPrefect = false;
        if (const NPCSchedule* sched =
                m_schedules.GetSchedule(size_t(sn.scheduleIndex)))
            isPrefect = sched->name.find("prefect") != std::string::npos;
        else if (m_prefectIds[0].index == 0xFFFFFFFF)
            isPrefect = true; // fallback: first spawned slot
        if (isPrefect) {
            ai.role = AIRole::Prefect;
            ai.sightRadius = 14.0f; // authorities keep a wider watch
            if      (m_prefectIds[0].index == 0xFFFFFFFF) m_prefectIds[0] = e;
            else                                          m_prefectIds[1] = e;
            Logger::Info("NPC AI: entity %u designated PREFECT", e.index);
        }
    }
    m_entities.AddAI(e, ai);
    m_entities.AddMeleeCombat(e);

    if (rigIndex != 0xFFFFFFFF) {
        AnimationComponent ac;
        ac.rigIndex = rigIndex;
        ac.playing = true;
        ac.wasMoving = false;
        m_entities.AddAnimation(e, ac);
        AnimRig& rig = m_animRigs[rigIndex];
        if (rig.idleClip >= 0) rig.controller.Play(rig.idleClip, true, 0.0f);
    }

    sn.entity = e;
    sn.lastWindowIndex = -1;   // UpdateSchedules applies the window state
    sn.walkToSpawnLogged = false;
    if (const NPCSchedule* sched =
            m_schedules.GetSchedule(size_t(sn.scheduleIndex)))
        Logger::Info("Schedule[%s]: spawned NPC model=%u at %02d:%02d",
                     sched->name.c_str(), sn.modelIndex,
                     GetGameTime().hour, GetGameTime().minute);
    return e;
}

// Track 29: destroy a scheduled NPC's entity. Called when the game clock
// leaves the NPC's spawned window; the slot stays registered for respawn.
void Engine::DespawnScheduledNpc(ScheduledNpc& sn, const char* reason) {
    if (!sn.entity.IsValid()) return;
    if (!m_entities.IsAlive(sn.entity)) {
        sn.entity = EntityID{}; // externally destroyed (KO cleanup etc.)
        return;
    }
    // Never despawn mid-conversation (Track 30); retried next frame after
    // the dialogue ends and clears m_dialogueNpc.
    if (sn.entity == m_dialogueNpc) return;
    // Release a held NPC first so the grapple never references a corpse.
    if (sn.entity == m_grappleTarget) EndGrapple(false);
    const NPCSchedule* sched =
        m_schedules.GetSchedule(size_t(sn.scheduleIndex));
    Logger::Info("Schedule[%s]: despawned NPC (%s) at %02d:%02d",
                 sched ? sched->name.c_str() : "?", reason,
                 GetGameTime().hour, GetGameTime().minute);
    m_entities.Destroy(sn.entity);
    sn.entity = EntityID{};
    sn.lastWindowIndex = -1;
    sn.walkToSpawnLogged = false;
}

// Track 29: per-frame schedule tick. For every scheduled NPC slot: resolve
// the active window at the current game minute; spawn when a spawned window
// opens, despawn when it closes, apply the window's behavior override on
// transitions, and stage the despawn walk 10 in-game minutes early.
void Engine::UpdateSchedules() {
    if (m_scheduledNpcs.empty()) return;
    const int nowMin = m_clockMinutes;
    for (ScheduledNpc& sn : m_scheduledNpcs) {
        const NPCSchedule* sched =
            m_schedules.GetSchedule(size_t(sn.scheduleIndex));
        if (!sched) continue;
        const int wi = sched->WindowIndexAt(nowMin);
        if (wi < 0 || sched->windows[wi].state == SpawnState::Despawned) {
            DespawnScheduledNpc(sn, wi < 0 ? "no window" : "window end");
            continue;
        }
        const ScheduleWindow& w = sched->windows[wi];
        if (!sn.entity.IsValid() || !m_entities.IsAlive(sn.entity)) {
            if (sn.entity.IsValid()) sn.entity = EntityID{}; // stale handle
            SpawnScheduledNpc(sn, w);
            continue;
        }
        // Behavior override on window change.
        if (wi != sn.lastWindowIndex) {
            sn.lastWindowIndex = wi;
            sn.walkToSpawnLogged = false;
            if (AIComponent* ai = m_entities.GetAI(sn.entity)) {
                ai->homePos = sn.spawnPoint;
                ai->hasTarget = false;
                ai->state = AIState::Idle;
                ai->timer = 0.5f;
                ai->lockIdle = (w.behavior == ScheduleBehavior::Idle);
            }
            Logger::Info("Schedule[%s]: window %d active at %02d:%02d (%s)",
                         sched->name.c_str(), wi, GetGameTime().hour,
                         GetGameTime().minute,
                         w.behavior == ScheduleBehavior::Idle ? "idle" : "wander");
        }
        // Gradual transition: 10 in-game minutes before this window ends in
        // a despawn, walk back toward the spawn point (staging).
        const ScheduleWindow& next =
            sched->windows[(size_t(wi) + 1) % sched->windows.size()];
        if (next.state == SpawnState::Despawned && w.hasSpawnPoint) {
            const int untilEnd = (w.endMinute - nowMin + 24 * 60) % (24 * 60);
            if (untilEnd <= 10 && !sn.walkToSpawnLogged) {
                if (AIComponent* ai = m_entities.GetAI(sn.entity)) {
                    ai->lockIdle = false;
                    ai->state = AIState::Wander;
                    ai->targetPos = w.spawnPoint;
                    ai->targetPos.y = sn.entity.IsValid()
                        ? m_entities.GetTransform(sn.entity)->position.y
                        : ai->homePos.y; // mover re-snaps to ground
                    ai->hasTarget = true;
                    sn.walkToSpawnLogged = true;
                    Logger::Info("Schedule[%s]: NPC walking to despawn point "
                                 "(%d game-min left)", sched->name.c_str(), untilEnd);
                }
            }
        }
    }
}

void Engine::LogFrameTiming() {
    if (m_framesAccum == 0) return;
    const double avgMs = m_frameMsAccum / double(m_framesAccum);
    const double fps = 1000.0 / avgMs;
    const glm::vec3& p = m_camera.GetPosition();
    Logger::Info("Frame timing: %.2f ms avg (%.1f fps) | cam (%.1f, %.1f, %.1f) yaw %.2f pitch %.2f",
                 avgMs, fps, p.x, p.y, p.z, m_camera.GetYaw(), m_camera.GetPitch());
    if (!m_groundRaycastInfo.empty())
        Logger::Info("Ground raycast: %s", m_groundRaycastInfo.c_str());
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        const Transform* jt = m_entities.GetTransform(m_jimmyId);
        if (jt)
            Logger::Info("Jimmy: (%.2f, %.2f, %.2f) onGround=%d",
                         jt->position.x, jt->position.y, jt->position.z,
                         int(m_entities.GetPhysics(m_jimmyId) &&
                             m_entities.GetPhysics(m_jimmyId)->onGround));
    }
    // Track 16/27: NPC AI census (all state counts + one engaged sample).
    {
        unsigned counts[11] = {}; bool first = true;
        char sample[160] = {};
        char wanderSample[128] = {};
        m_entities.ForEachAI([&](EntityID, const Transform& t, const AIComponent& ai) {
            const int s = int(ai.state);
            if (s >= 0 && s < 11) counts[s]++;
            if (first && ai.state == AIState::Wander) {
                std::snprintf(wanderSample, sizeof(wanderSample),
                              "| wander sample: (%.1f, %.1f) -> target (%.1f, %.1f)",
                              t.position.x, t.position.z,
                              ai.targetPos.x, ai.targetPos.z);
                first = false;
            }
            // Track 27: prefer an engaged (chase/attack) NPC in the sample so
            // combat verification lines up with the transitions in the log.
            if (sample[0] == '\0' &&
                (ai.state == AIState::Chase || ai.state == AIState::Attack ||
                 ai.state == AIState::Flee)) {
                std::snprintf(sample, sizeof(sample),
                              "| engaged: entity at (%.1f, %.1f) state %s",
                              t.position.x, t.position.z, AiStateName(ai.state));
            }
        });
        Logger::Info("NPC AI: idle=%u wander=%u face=%u patrol=%u alert=%u "
                     "chase=%u attack=%u flee=%u return=%u watch=%u ko=%u %s%s",
                     counts[0], counts[1], counts[2], counts[3], counts[4],
                     counts[5], counts[6], counts[7], counts[8], counts[9],
                     counts[10], sample, wanderSample);
    }
    m_framesAccum = 0;
    m_frameMsAccum = 0.0;
}

void Engine::Render() {
    if (!m_initialized || !m_renderer) return;

    if (!m_worldMeshes.empty()) {
        // OPENBULLY_DEBUG_CAM=1: bypass the camera entirely — identity
        // view/projection and a scale-only model matrix, so the whole mesh
        // renders as a small model dead-center (pipeline verification).
        if (std::getenv("OPENBULLY_DEBUG_CAM")) {
            m_renderer->SetCameraView(glm::mat4(1.0f));
            m_renderer->DebugIdentityProjection();
            const float s = 0.002f;
            const glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(s, s, s));
            const float* mvp = glm::value_ptr(scale);
            for (const WorldMesh& wm : m_worldMeshes) {
                for (const WorldMesh::SubRange& sr : wm.subs) {
                    m_renderer->BindTexture(sr.texture);
                    m_renderer->DrawMesh3DSub(wm.gpu, sr.indexOffset,
                                              sr.indexCount, mvp);
                }
            }
            m_renderer->Frame();
            return;
        }
        // Free camera (Track 10): view matrix from camera. Projection stays
        // in the renderer; we fetch it once and build MVP per mesh here.
        // Track 31: mounted vehicles widen the FOV (per-type); walking
        // restores the 60-deg default. Rebuilt only on a change.
        {
            float fovDeg = 60.0f;
            if (m_mountedVehicle.IsValid()) {
                if (const MeshComponent* vmc = m_entities.GetMesh(m_mountedVehicle))
                    if (vmc->isVehicle && vmc->vehicleModelIndex < m_vehicleModels.size())
                        fovDeg = m_vehicleModels[vmc->vehicleModelIndex].fovDeg;
            }
            if (fovDeg != m_lastFovDeg) {
                m_renderer->SetProjectionFov(fovDeg);
                m_lastFovDeg = fovDeg;
            }
        }
        const glm::mat4 view = m_camera.GetViewMatrix();
        m_renderer->SetCameraView(view);
        const glm::mat4 proj = m_renderer->GetProjection();

        // World meshes draw in view 1 (3D), one draw per sub-mesh so each
        // binds its own base texture (Track 11).
        // Track 14: all 3D geometry draws through the entity list — level
        // meshes are static entities, test boxes are dynamic entities.
        BuildEntityRenderList();
        for (const EntityRender& er : m_entityRender) {
            const glm::mat4 mvpEntity = proj * view * er.model;
            // Track 17: skinned draw — pack the palette per submesh (each
            // skinned shape has its own bind list; rigid shapes use the
            // bind-pose identity palette).
            if (er.rigIndex != 0xFFFFFFFF && er.rigIndex < m_animRigs.size()) {
                const AnimRig& rig = m_animRigs[er.rigIndex];
                const AnimationController& ctrl = rig.controller;
                std::vector<glm::mat4> palette;
                for (const WorldMesh::SubRange& sr : er.subs) {
                    ctrl.ShapePalette(sr.shapeIndex >= 0
                                          ? size_t(sr.shapeIndex) : SIZE_MAX,
                                      palette);
                    m_renderer->BindTexture(sr.texture);
                    m_renderer->DrawMesh3DSubSkinned(
                        *er.gpu, sr.indexOffset, sr.indexCount,
                        glm::value_ptr(mvpEntity),
                        palette.empty() ? nullptr
                                        : glm::value_ptr(palette.front()),
                        uint32_t(palette.size()));
                }
                continue;
            }
            if (er.subs.empty()) {
                m_renderer->DrawMesh3D(*er.gpu, glm::value_ptr(mvpEntity));
                continue;
            }
            for (const WorldMesh::SubRange& sr : er.subs) {
                m_renderer->BindTexture(sr.texture);
                m_renderer->DrawMesh3DSub(*er.gpu, sr.indexOffset,
                                          sr.indexCount,
                                          glm::value_ptr(mvpEntity));
            }
        }

        // Track 26: the equipped weapon drawn in Jimmy's hand (position
        // offset in facing-yaw space; bone attach comes with skinned hands).
        RenderWeaponInHand(view, proj);

        // Camera collision: resolve against entity world AABBs (layer 0 =
        // world). Push the camera out of any box it intersects.
        {
            glm::vec3 p = m_camera.GetPosition();
            m_entities.ForEach([&](EntityID id, Transform& t, const EntityManager::EntityRecord&) {
                (void)t;
                glm::vec3 mn, mx;
                if (!m_entities.GetWorldAABB(id, mn, mx)) return;
                if (p.x > mn.x && p.x < mx.x && p.y > mn.y && p.y < mx.y &&
                    p.z > mn.z && p.z < mx.z) {
                    // Penetration depths; exit along the smallest axis.
                    const glm::vec3 pen = glm::min(mx - p, p - mn);
                    if (pen.x <= pen.y && pen.x <= pen.z)
                        p.x = (p.x - mn.x < mx.x - p.x) ? mn.x : mx.x;
                    else if (pen.y <= pen.z)
                        p.y = (p.y - mn.y < mx.y - p.y) ? mn.y : mx.y;
                    else
                        p.z = (p.z - mn.z < mx.z - p.z) ? mn.z : mx.z;
                }
            });
            m_camera.SetPosition(p);
        }

        // Track 13 debug: wireframe collision boxes around registered
        // geometry (OPENBULLY_DEBUG_COLLISION=1).
        if (m_debugCollision && m_collisionWorld) {
            for (const WorldMesh& wm : m_worldMeshes) {
                const glm::vec3 lo = glm::make_vec3(wm.boundsMin);
                const glm::vec3 hi = glm::make_vec3(wm.boundsMax);
                const glm::vec3 c = 0.5f * (lo + hi);
                const glm::vec3 s = glm::max(hi - lo, glm::vec3(0.05f));
                const glm::mat4 model = glm::translate(glm::mat4(1.0f), c) *
                                        glm::scale(glm::mat4(1.0f), s);
                m_renderer->DrawDebugBox(glm::value_ptr(model));
            }
        }

        // Track 15 debug: render Jimmy's collision sphere as a wireframe box
        // (OPENBULLY_DEBUG_PHYSICS=1).
        if (m_debugPhysics && m_jimmyActive && m_jimmyId.IsValid()) {
            const Transform* jt = m_entities.GetTransform(m_jimmyId);
            if (jt) {
                const PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
                const float r = pc ? pc->radius : 0.5f;
                const glm::vec3 c = jt->position + glm::vec3(0.0f, r, 0.0f);
                const glm::mat4 model = glm::translate(glm::mat4(1.0f), c) *
                                        glm::scale(glm::mat4(1.0f),
                                                   glm::vec3(2.0f * r));
                m_renderer->DrawDebugBox(glm::value_ptr(model));
            }
        }

        // Track 20: objective marker beacon (active mission only).
        RenderObjectiveMarker();

        // Keep the reference triangle as a small 2D overlay in view 0 so the
        // Phase-2 regression baseline stays visibly green.
        m_renderer->DrawMesh(0, 3, 0.6f, -0.6f, 0.15f, 0.15f);
    } else {
        // Triangle-only mode (pre-extraction): full-size reference triangle.
        m_renderer->DrawMesh(0, 3, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    // Track 20 (UI): HUD overlay on view 10, after every world view so it
    // composites on top of the frame just assembled.
    if (m_ui.IsInitialized()) {
        m_ui.BeginFrame();
        m_ui.DrawHUD(m_hudState);
        // Track 23: melee hit feedback. Top-center damage popup with the
        // combo count, and a bottom-left combo meter while a chain is live.
        const float white = 0xffffffffu;
        if (m_meleePopupTimer > 0.0f && m_meleePopup[0]) {
            const float w = m_ui.MeasureText(m_meleePopup);
            const float scale = 3.0f; // chunky; text metrics are 6 px/char
            m_ui.DrawText(0.5f * (float(m_screenWidth) - w * scale),
                          0.22f * float(m_screenHeight), m_meleePopup,
                          m_meleePopupTimer < 0.4f ? 0xffff8000u : white);
        }
        if (m_jimmyId.IsValid()) {
            if (const Melee::CombatComponent* mc = m_entities.GetMeleeCombat(m_jimmyId)) {
                if (mc->comboCount > 1) {
                    char buf[48];
                    std::snprintf(buf, sizeof(buf), "COMBO x%d", mc->comboCount);
                    m_ui.DrawText(24.0f, float(m_screenHeight) - 48.0f, buf, white);
                    // Combo timer bar (2 s window per the task spec).
                    const float frac = 1.0f - std::min(mc->comboIdleTimer, 2.0f) / 2.0f;
                    m_ui.DrawRect(24.0f, float(m_screenHeight) - 38.0f,
                                  120.0f * frac, 6.0f, 0xff40a0ffff);
                }
            }
        }

        // Track 30: dialogue box (bottom-center panel, typewriter text,
        // choice buttons while a choice menu is pending).
        if (m_dialogue.IsActive()) {
            const std::vector<DialogueChoice>* choices = m_dialogue.ActiveChoices();
            std::vector<std::string> choiceTexts;
            if (choices)
                for (const DialogueChoice& c : *choices)
                    choiceTexts.push_back(c.text);
            if (m_dialogueChoiceSel >= int(choiceTexts.size()))
                m_dialogueChoiceSel = int(choiceTexts.size()) - 1;
            m_ui.DrawDialogueBox(m_dialogue.Speaker().c_str(),
                                 m_dialogue.VisibleText(),
                                 /*showAdvanceHint=*/choices == nullptr,
                                 choiceTexts,
                                 m_dialogueChoiceSel);
        }
    }

    m_renderer->Frame(); // present (VSync via BGFX_RESET_VSYNC)

    // One-shot verification capture (see OPENBULLY_SHOT in Init).
    // OPENBULLY_SHOT_DELAY=<sec>: delay the capture so later-scene shots
    // (e.g. NPC wander movement) can be taken after loading settles.
    // OPENBULLY_SHOT_EVERY=<sec>: recurring captures "<name>_N.tga" (Track 17
    // animation proof: consecutive frames differ when bones animate).
    float shotDelay = 1.0f;
    if (const char* d = std::getenv("OPENBULLY_SHOT_DELAY"))
        shotDelay = float(std::atof(d));
    static float s_every = -1.0f;
    if (s_every < 0.0f) {
        s_every = 0.0f;
        if (const char* e = std::getenv("OPENBULLY_SHOT_EVERY"))
            s_every = float(std::atof(e));
    }
    if (!m_screenshotName.empty() && m_time >= double(shotDelay)) {
        if (s_every > 0.0f) {
            static double s_lastShot = -1e9;
            if (m_time - s_lastShot >= double(s_every)) {
                s_lastShot = m_time;
                static int s_shotIndex = 0;
                char seq[512];
                std::snprintf(seq, sizeof(seq), "%s_%d", m_screenshotName.c_str(), s_shotIndex++);
                m_renderer->RequestScreenshot(seq);
            }
        } else if (!m_screenshotPending) {
            m_screenshotPending = true;
            m_renderer->RequestScreenshot(m_screenshotName.c_str());
        }
    }
    ++m_frameCount;
}

void Engine::Shutdown() {
    if (!m_initialized) return;

    Logger::Info("Shutting down engine...");
    // Track 21: stop audio first so the callback never runs against dead
    // state while the GPU-side teardown proceeds.
    if (m_audio) {
        m_audio->Shutdown();
        m_audio.reset();
    }
    for (WorldMesh& wm : m_worldMeshes)
        m_renderer->DestroyGpuMesh(wm.gpu);
    m_worldMeshes.clear();
    for (auto& [key, tex] : m_textureCache)
        if (tex.valid()) m_renderer->DestroyTexture(tex);
    m_textureCache.clear();
    if (m_triangleValid) {
        m_renderer->DestroyGpuMesh(m_triangle);
        m_triangleValid = false;
    }
    if (m_testBoxValid) {
        m_renderer->DestroyGpuMesh(m_testBox);
        m_testBoxValid = false;
    }
    if (m_jimmyMeshValid) {
        m_renderer->DestroyGpuMesh(m_jimmyMesh);
        m_jimmyMeshValid = false;
    }
    // Track 20 (UI): release the HUD program/textures before bgfx dies.
    m_ui.Shutdown();
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    m_initialized = false;
    Logger::Info("Engine shutdown complete");
}

// --- Track 26: weapon system ---------------------------------------------------

// Loads the weapon NIFs (bat / garbage lid / yardstick / slingshot) from the
// extracted World.img set. Each model is normalized to ~0.9 units for hand
// carrying; pickups draw at 1.3x that. Returns false when nothing loaded.
bool Engine::LoadWeaponModels() {
    static const char* kDirs[] = { "data/extracted/world", "../data/extracted/world" };
    size_t loaded = 0;
    for (int wi = 1; wi <= 4; ++wi) { // table order: Bat..Slingshot (skip Unarmed)
        const WeaponData& data = GetWeaponData(WeaponType(wi));
        if (!data.modelNif) continue;
        bool ok = false;
        for (const char* dir : kDirs) {
            const std::string path = std::string(dir) + "/" + data.modelNif;
            NifLoadResult nif = LoadNifFile(path);
            if (!nif.ok || nif.totalVertices < 10) continue;
            Mesh mesh;
            if (!MeshFromNif(nif, mesh)) continue;
            // Bake node-hierarchy transforms (same as level geometry) when
            // the NIF carries them.
            {
                size_t vertBase = 0;
                for (const NifGeometryData& geo : nif.geometries) {
                    float wm[16];
                    if (NifWorldTransform(nif, geo.nodeIndex, wm)) {
                        for (size_t v = 0; v < geo.vertices.size(); v++) {
                            MeshVertex& mv = mesh.vertices[vertBase + v];
                            const NifVector3& p = geo.vertices[v];
                            mv.x = wm[0]*p.x + wm[4]*p.y + wm[8]*p.z  + wm[12];
                            mv.y = wm[1]*p.x + wm[5]*p.y + wm[9]*p.z  + wm[13];
                            mv.z = wm[2]*p.x + wm[6]*p.y + wm[10]*p.z + wm[14];
                        }
                    }
                    vertBase += geo.vertices.size();
                }
            }
            // Normalize: max dimension -> 0.9u (a bat-sized prop in hand).
            glm::vec3 lo( FLT_MAX), hi(-FLT_MAX);
            for (uint32_t idx : mesh.indices) {
                const MeshVertex& v = mesh.vertices[idx];
                lo = glm::min(lo, glm::vec3(v.x, v.y, v.z));
                hi = glm::max(hi, glm::vec3(v.x, v.y, v.z));
            }
            const float maxDim = std::max(hi.x - lo.x,
                                 std::max(hi.y - lo.y, hi.z - lo.z));
            const float drawScale = maxDim > 1e-4f ? 0.9f / maxDim : 1.0f;

            GpuMesh gpu = m_renderer->UploadMeshGpu(mesh);
            if (!gpu.valid()) continue;
            WeaponModel model;
            model.name = data.modelNif;
            model.type = data.type;
            model.id = wi;
            model.gpu = gpu;
            model.drawScale = drawScale;
            for (const MeshSubMesh& sub : mesh.subMeshes) {
                WorldMesh::SubRange sr;
                sr.indexOffset = sub.indexOffset;
                sr.indexCount = sub.indexCount;
                sr.texture = AcquireTexture(m_texDir.c_str(), sub.textureName);
                model.subs.push_back(sr);
            }
            m_weaponModels.push_back(std::move(model));
            Logger::Info("Weapon model '%s' loaded (%zu verts, scale %.3f)",
                         data.modelNif, mesh.vertices.size(), drawScale);
            ok = true;
            break;
        }
        if (!ok) Logger::Warning("Weapon model '%s' failed to load", data.modelNif);
        else loaded++;
    }
    return loaded > 0;
}

// Spawns one pickup entity per weapon model near `center`, ground-snapped.
void Engine::SpawnWeaponPickups(const glm::vec3& center) {
    if (m_weaponModels.empty()) return;
    static const glm::vec3 kOffsets[] = {
        { 2.0f, 0.0f,  1.0f }, { -2.5f, 0.0f, 0.5f },
        { 1.5f, 0.0f, -2.0f }, {  0.5f, 0.0f, 2.5f },
    };
    for (size_t i = 0; i < m_weaponModels.size(); ++i) {
        const WeaponModel& wmodel = m_weaponModels[i];
        const WeaponData& data = GetWeaponDataById(wmodel.id);
        glm::vec3 pos = center + kOffsets[i % 4];
        if (m_collisionWorld) {
            Ray down;
            down.origin[0] = pos.x; down.origin[1] = pos.y + 6.0f; down.origin[2] = pos.z;
            down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
            RayHit hit;
            if (m_collisionWorld->Raycast(down, hit)) pos.y = hit.point[1];
        }

        EntityID e = m_entities.Create();
        Transform t;
        t.position = pos + glm::vec3(0.0f, 0.45f, 0.0f); // hover above ground
        t.scale = glm::vec3(wmodel.drawScale * 1.3f);
        t.rotation.y = float(i) * 1.7f; // varied initial spin
        m_entities.AddTransform(e, t);

        MeshComponent mc;
        mc.isWeaponPickup = true;
        mc.weaponModelIndex = uint32_t(i);
        m_entities.AddMesh(e, mc);

        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-0.5f); cc.boundsMax = glm::vec3(0.5f);
        cc.layerMask = 2u;
        m_entities.AddCollision(e, cc);

        WeaponComponent w;
        w.type = wmodel.type;
        w.weaponId = wmodel.id;
        w.durability = data.durability;
        w.ammo = data.ammo;
        m_entities.AddWeapon(e, w);

        Logger::Info("Weapon pickup '%s' spawned at (%.1f, %.1f, %.1f)",
                     data.name, pos.x, pos.y, pos.z);
    }
}

// Per-frame weapon logic: pickup (E), slot selection (1-5), pickup bob/spin.
void Engine::UpdateWeapons(float dt) {
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc) return;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;

    // Track 26 verification hook: OPENBULLY_WEAPON_DEMO=1 scripts pickup ->
    // equip -> slingshot fire for headless screenshots (t=1s pickup, t=2.5s
    // fire). Same philosophy as CAM_DEMO/JIMMY_DEMO.
    static const bool s_weaponDemo = std::getenv("OPENBULLY_WEAPON_DEMO") != nullptr;
    if (s_weaponDemo && m_jimmyActive) {
        if (m_time >= 1.0 && pc->equippedSlot < 0) {
            // Auto-pickup: prefer the slingshot (projectile proof for the
            // screenshot); otherwise the nearest weapon pickup entity.
            EntityID best{};
            float bestDist = 1e9f;
            bool bestRanged = false;
            m_entities.ForEach([&](EntityID id, Transform& t,
                                   const EntityManager::EntityRecord&) {
                if (id == m_jimmyId || !m_entities.GetWeapon(id)) return;
                const WeaponComponent* w = m_entities.GetWeapon(id);
                const bool ranged = w && w->type == WeaponType::RangedProjectile;
                const float d = glm::length(t.position - jt->position);
                if (ranged || d < bestDist) {
                    if (ranged || (!bestRanged && d < bestDist)) {
                        bestDist = d; best = id; bestRanged = ranged;
                    }
                }
            });
            if (best.IsValid() && m_entities.IsAlive(best)) {
                const WeaponComponent* wc = m_entities.GetWeapon(best);
                for (int i = 0; i < 5 && wc; ++i) {
                    if (pc->slots[i].type != WeaponType::Unarmed) continue;
                    pc->slots[i].type = wc->type;
                    pc->slots[i].weaponId = wc->weaponId;
                    pc->slots[i].durability = wc->durability;
                    pc->slots[i].ammo = wc->ammo;
                    pc->equippedSlot = i;
                    const WeaponData& data = GetWeaponDataById(wc->weaponId);
                    Logger::Info("Weapon demo: picked up %s (slot %d)",
                                 data.name, i);
                    m_entities.Destroy(best);
                    break;
                }
            }
        }
        if (m_time >= 2.5 && pc->equippedSlot >= 0 && pc->attackCooldown <= 0.0f) {
            WeaponTryAttack(pc); // slingshot if picked, else melee swing
        }
    }

    // --- Pickup: E near a dropped weapon -------------------------------------
    // Track 30: E is the dialogue key; a conversation takes precedence.
    if (m_input.WasKeyPressed(SDL_SCANCODE_E) && !m_dialogue.IsActive()) {
        EntityID best{};
        float bestDist = 2.2f; // pickup radius
        m_entities.ForEach([&](EntityID id, Transform& t,
                               const EntityManager::EntityRecord&) {
            if (id == m_jimmyId) return;
            if (!m_entities.GetWeapon(id)) return;
            const float d = glm::length(t.position - jt->position);
            if (d < bestDist) { bestDist = d; best = id; }
        });
        if (best.IsValid() && m_entities.IsAlive(best)) {
            const WeaponComponent* wc = m_entities.GetWeapon(best);
            int freeSlot = -1;
            for (int i = 0; i < 5; ++i)
                if (pc->slots[i].type == WeaponType::Unarmed) { freeSlot = i; break; }
            if (freeSlot < 0) {
                Logger::Info("Pickup failed: inventory full");
            } else if (wc) {
                const WeaponData& data = GetWeaponDataById(wc->weaponId);
                pc->slots[freeSlot].type = wc->type;
                pc->slots[freeSlot].weaponId = wc->weaponId;
                pc->slots[freeSlot].durability = wc->durability > 0
                    ? wc->durability : data.durability;
                pc->slots[freeSlot].ammo = wc->ammo;
                if (pc->equippedSlot < 0) pc->equippedSlot = freeSlot;
                Logger::Info("Picked up %s -> slot %d (durability %d, ammo %d)",
                             data.name, freeSlot, pc->slots[freeSlot].durability,
                             pc->slots[freeSlot].ammo);
                m_entities.Destroy(best);
            }
        }
    }

    // --- Slot selection: 1-5 (deferred during dialogue: 1-4 pick choices) ---
    static const SDL_Scancode kSlotKeys[5] = {
        SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
    };
    for (int i = 0; i < 5 && !m_dialogue.IsActive(); ++i) {
        if (m_input.WasKeyPressed(kSlotKeys[i])) {
            if (pc->slots[i].type == WeaponType::Unarmed) {
                Logger::Info("Slot %d empty", i + 1);
            } else {
                pc->equippedSlot = (pc->equippedSlot == i) ? -1 : i; // toggle sheath
                const WeaponData& data = GetWeaponDataById(pc->slots[i].weaponId);
                Logger::Info("Equipped slot %d: %s", i + 1,
                             pc->equippedSlot == i ? data.name : "(unarmed)");
            }
        }
    }

    // --- Pickup bob + spin (visual only) --------------------------------------
    m_entities.ForEach([&](EntityID id, Transform& t,
                           const EntityManager::EntityRecord&) {
        if (id == m_jimmyId) return;
        if (!m_entities.GetWeapon(id)) return;
        t.rotation.y += dt * 1.5f;
        t.position.y += std::sin(float(m_time) * 2.0f + float(id.index)) * dt * 0.08f;
    });
}

// LMB attack with an equipped weapon. Melee: hits every NPC within the
// weapon's range in front of Jimmy; the counter bonus (Track 25) multiplies
// damage. Ranged: consumes one ammo and spawns a projectile entity. Each
// successful hit (or shot) decrements durability; 0 breaks the weapon.
void Engine::WeaponTryAttack(PlayerController* pc) {
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt || pc->equippedSlot < 0 || pc->equippedSlot >= 5) return;
    InventorySlot& slot = pc->slots[pc->equippedSlot];
    if (slot.type == WeaponType::Unarmed) return;
    const WeaponData& data = GetWeaponDataById(slot.weaponId);

    const glm::vec3 fwd(std::sin(pc->facingYaw), 0.0f, std::cos(pc->facingYaw));
    int damage = data.damage;
    if (pc->counterArmed) {
        damage = int(float(damage) * 4.0f); // Track 25 counter bonus
        pc->counterArmed = false;
        pc->state = DefenseState::Normal;
        Logger::Info("COUNTER ATTACK with %s!", data.name);
    }
    pc->attackCooldown = data.attackCooldown;

    bool hitSomething = false;
    if (data.type == WeaponType::RangedProjectile) {
        // --- Slingshot: ammo gate + projectile spawn --------------------------
        if (slot.ammo <= 0) {
            Logger::Info("%s: out of ammo", data.name);
            return;
        }
        slot.ammo--;
        const glm::vec3 origin = jt->position + glm::vec3(0.0f, 1.3f, 0.0f) +
                                 fwd * 0.5f;
        const glm::vec3 vel = fwd * 18.0f + glm::vec3(0.0f, 1.2f, 0.0f);
        SpawnProjectile(origin, vel, damage);
        Logger::Info("%s fired (ammo left %d, damage %d)",
                     data.name, slot.ammo, damage);
        hitSomething = true; // durability still wears on each shot
    } else {
        // --- Melee: sweep every NPC inside the extended hit arc ---------------
        const float reach = data.range + 0.4f;
        m_entities.ForEach([&](EntityID id, Transform& t,
                               const EntityManager::EntityRecord&) {
            if (id == m_jimmyId) return;
            AIComponent* ai = m_entities.GetAI(id);
            if (!ai) return; // NPCs only (grapple targets keep their own flow)
            const glm::vec3 to = t.position - jt->position;
            const float dy = std::fabs(to.y);
            const float dxz = glm::length(glm::vec3(to.x, 0.0f, to.z));
            if (dy > 1.6f || dxz > reach) return;
            // Must be roughly in front of Jimmy (dot > 0.1).
            if (dxz > 1e-3f && glm::dot(glm::vec3(to.x, 0.0f, to.z) / dxz, fwd) < 0.1f)
                return;
            // Track 28: funnel through DamageNpc (flinch + reactions + KO).
            DamageNpc(id, m_jimmyId, damage, t.position, 0.4f);
            hitSomething = true;
            if (const AIComponent* ai2 = m_entities.GetAI(id))
                Logger::Info("%s hit NPC idx=%u for %d (health %d/%d)",
                             data.name, id.index, damage,
                             ai2->health.current, ai2->health.max);
        });
        if (!hitSomething)
            Logger::Info("%s swing: no target in range (%.1f)", data.name, reach);
    }

    // --- Durability: wear + break ---------------------------------------------
    if (hitSomething && data.durability > 0) {
        slot.durability--;
        Logger::Info("%s durability %d", data.name, slot.durability);
        if (slot.durability <= 0) {
            Logger::Info("%s BROKE!", data.name);
            SpawnDebris(jt->position + glm::vec3(0.0f, 1.2f, 0.0f));
            slot.type = WeaponType::Unarmed;
            slot.ammo = 0;
            pc->equippedSlot = -1;
        }
    }
}

// Projectile entity: Transform + Mesh(isBox); velocity/life live engine-side.
void Engine::SpawnProjectile(const glm::vec3& pos, const glm::vec3& vel, int dmg) {
    if (!m_testBoxValid) return;
    EntityID e = m_entities.Create();
    Transform t;
    t.position = pos;
    t.scale = glm::vec3(0.12f); // small rock
    m_entities.AddTransform(e, t);
    MeshComponent mc;
    mc.isBox = true;
    m_entities.AddMesh(e, mc);
    CollisionComponent cc;
    cc.boundsMin = glm::vec3(-0.5f); cc.boundsMax = glm::vec3(0.5f);
    cc.layerMask = 4u; // projectile layer (excluded from player queries)
    m_entities.AddCollision(e, cc);

    Projectile p;
    p.id = e;
    p.velocity = vel;
    p.life = 3.0f;
    p.damage = dmg;
    m_projectiles.push_back(p);
}

// Weapon-break effect: a short-lived spray of inert debris cubes.
void Engine::SpawnDebris(const glm::vec3& pos) {
    if (!m_testBoxValid) return;
    for (int i = 0; i < 6; ++i) {
        EntityID e = m_entities.Create();
        Transform t;
        t.position = pos;
        t.scale = glm::vec3(0.08f + 0.02f * float(i % 3));
        m_entities.AddTransform(e, t);
        MeshComponent mc;
        mc.isBox = true;
        m_entities.AddMesh(e, mc);
        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-0.5f); cc.boundsMax = glm::vec3(0.5f);
        cc.layerMask = 4u;
        m_entities.AddCollision(e, cc);

        const float a = float(i) * 1.0472f; // 60 deg steps
        Projectile p;
        p.id = e;
        p.velocity = glm::vec3(std::cos(a) * 2.5f, 3.0f + 0.5f * float(i % 3),
                               std::sin(a) * 2.5f);
        p.life = 0.7f;
        p.damage = 0; // inert
        m_projectiles.push_back(p);
    }
}

// Integrates projectiles: straight-line flight, world raycast per step,
// sphere hit vs NPC torsos (damage), life expiry. Debris (damage 0) gets
// gravity and damages nobody.
void Engine::UpdateProjectiles(float dt) {
    if (m_projectiles.empty()) return;
    for (size_t i = 0; i < m_projectiles.size();) {
        Projectile& p = m_projectiles[i];
        Transform* t = m_entities.GetTransform(p.id);
        if (!t) { // entity destroyed elsewhere: drop the record
            m_projectiles.erase(m_projectiles.begin() + long(i));
            continue;
        }
        p.life -= dt;
        if (p.damage > 0) { // flying rock: light drop
            p.velocity.y -= 2.5f * dt;
        } else { // debris: full gravity
            p.velocity.y -= 9.8f * dt;
        }
        const glm::vec3 oldPos = t->position;
        const glm::vec3 step = p.velocity * dt;

        bool kill = p.life <= 0.0f;
        // World hit: segment raycast from old to new position.
        if (!kill && m_collisionWorld && glm::length(step) > 1e-6f) {
            Ray r;
            r.origin[0] = oldPos.x; r.origin[1] = oldPos.y; r.origin[2] = oldPos.z;
            const float len = glm::length(step);
            r.direction[0] = step.x / len; r.direction[1] = step.y / len;
            r.direction[2] = step.z / len;
            RayHit hit;
            if (m_collisionWorld->Raycast(r, hit) &&
                glm::length(glm::make_vec3(hit.point) - oldPos) <= len) {
                Logger::Info("Projectile hit world at (%.1f, %.1f, %.1f)",
                             hit.point[0], hit.point[1], hit.point[2]);
                kill = true;
            }
        }
        // NPC hit: sphere vs torso (damage projectiles only).
        if (!kill && p.damage > 0) {
            const glm::vec3 newPos = oldPos + step;
            m_entities.ForEachAI([&](EntityID id, Transform& nt, AIComponent& ai) {
                if (kill || id == m_jimmyId) return;
                if (ai.state == AIState::Ko) return; // downed: no re-trigger
                const glm::vec3 torso = nt.position + glm::vec3(0.0f, 1.0f, 0.0f);
                if (glm::length(newPos - torso) < 0.8f) {
                    // Track 28: funnel through DamageNpc (flinch + reactions).
                    DamageNpc(id, m_jimmyId, p.damage, nt.position, 0.4f);
                    Logger::Info("Projectile hit NPC idx=%u for %d",
                                 id.index, p.damage);
                    kill = true;
                }
            });
        }

        if (kill) {
            m_entities.Destroy(p.id);
            m_projectiles.erase(m_projectiles.begin() + long(i));
            continue;
        }
        t->position = oldPos + step;
        ++i;
    }
}

// Resolves the equipped inventory slot to its loaded model (nullptr/unarmed).
const Engine::WeaponModel* Engine::EquippedWeaponModel(
    const PlayerController* pc, int* slotOut) const {
    if (slotOut) *slotOut = -1;
    if (!pc || pc->equippedSlot < 0 || pc->equippedSlot >= 5) return nullptr;
    const InventorySlot& slot = pc->slots[pc->equippedSlot];
    if (slot.type == WeaponType::Unarmed) return nullptr;
    for (const WeaponModel& m : m_weaponModels)
        if (m.id == slot.weaponId ||
            (slot.weaponId == 0 && m.type == slot.type)) {
            if (slotOut) *slotOut = pc->equippedSlot;
            return &m;
        }
    return nullptr;
}

// Draws the equipped weapon in Jimmy's hand: position offset in facing-yaw
// space (right side, chest height), pitched forward like a carried prop.
void Engine::RenderWeaponInHand(const glm::mat4& view, const glm::mat4& proj) {
    if (!m_jimmyActive || !m_jimmyId.IsValid()) return;
    const PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!pc || !jt) return;
    const WeaponModel* model = EquippedWeaponModel(pc);
    if (!model || !model->gpu.valid()) return;

    const float cy = std::cos(pc->facingYaw), sy = std::sin(pc->facingYaw);
    const glm::vec3 fwd(sy, 0.0f, cy);
    const glm::vec3 right(cy, 0.0f, -sy);
    const glm::vec3 handPos = jt->position + fwd * 0.30f + right * 0.30f +
                              glm::vec3(0.0f, 1.20f, 0.0f);
    const glm::mat4 modelM =
        glm::translate(glm::mat4(1.0f), handPos) *
        glm::rotate(glm::mat4(1.0f), pc->facingYaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), -0.5f, glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(model->drawScale));
    const glm::mat4 mvp = proj * view * modelM;
    for (const WorldMesh::SubRange& sr : model->subs) {
        m_renderer->BindTexture(sr.texture);
        m_renderer->DrawMesh3DSub(model->gpu, sr.indexOffset, sr.indexCount,
                                  glm::value_ptr(mvp));
    }
}

} // namespace OpenBully
