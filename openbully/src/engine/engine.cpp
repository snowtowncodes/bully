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
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <filesystem>
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

// Track 49: boss-fight tuning. File scope so UpdateHUD (boss bar/QTE HUD
// sync) and the boss FSM below share one definition set.
// Phase -> melee cadence multipliers (smaller = attacks more often).
constexpr float kBossPhase2Mult = 0.72f;
constexpr float kBossPhase3Mult = 0.50f;
// Special cooldown shrink per phase (desperate bosses chain moves faster).
constexpr float kBossPhase2Cd   = 0.75f;
constexpr float kBossPhase3Cd   = 0.55f;
// QTE tuning. Windows shrink in phase 3 (desperation cuts both ways).
constexpr float kQteWindowBase   = 1.5f;
constexpr float kQteWindowPhase3 = 1.1f;
constexpr float kQteCooldown     = 1.5f;   // gap before the next prompt
constexpr float kQteResultFlash  = 0.9f;   // SUCCESS/TOO SLOW flash (s)
constexpr int   kQteDamageToBoss   = 14;    // boss HP cost on a win
constexpr int   kQteDamageToPlayer = 12;    // player HP cost on a miss
// Prompt keys (SDL scancodes; verified against SDL 2.30.12 headers):
// W=26 S=22 A=4 D=7.
const int kQteKeys[4] = { 26, 22, 4, 7 };

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
        m_lastGroundY = pos.y; // Track 49: void-guard reference level
        // Downhill continuation while sliding: keep the slide velocity.
    } else {
        p->onGround = false;
        p->groundNormalY = 1.0f;
        // Track 49: void guard. The spawn rooftop has walkable-looking edges;
        // falling through (boss chase pushes, scripted walks) sends Jimmy to
        // -inf, and the NaN-poisoned view/projection math trips asserts in
        // bgfx's debug validation. Falling >6u below the last ground level
        // (or any NaN) = off the world: respawn at the checkpoint (boss
        // fights treat it as a retry).
        const bool nan = pos.y != pos.y; // fast NaN check
        if (nan || pos.y < m_lastGroundY - 6.0f) {
            Logger::Warning("Jimmy: fell off the world (y %.1f, ground %.1f%s) - "
                            "respawning at checkpoint", pos.y, m_lastGroundY,
                            nan ? ", NaN" : "");
            bool bossActive = m_boss.entity.IsValid() &&
                m_entities.IsAlive(m_boss.entity);
            if (bossActive) {
                if (BossComponent* bc = m_entities.GetBoss(m_boss.entity))
                    bossActive = bc->engaged && !m_boss.victory;
            }
            if (bossActive) OnPlayerDefeatedByBoss();
            else {
                pos = m_jimmySpawn + glm::vec3(0.0f, 2.0f, 0.0f);
                p->velocity = glm::vec3(0.0f);
            }
        }
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

    const bool blocking = m_input.IsDown(Button::Block) && !m_aiming &&
                          !m_photoMode; // Track 39/47: RMB aims slingshot / photo viewfinder

    // --- 2. Attack (LMB) ------------------------------------------------------
    // Can't attack during a dodge roll; otherwise costs 10 stamina. A
    // counter-armed attack consumes the armed flag for 4x damage.
    // Track 30: no attacks while a conversation is active.
    if (m_input.IsPressed(Button::Attack) && !m_dialogue.IsActive()) {
        // Track 47: in photo mode the shutter consumes LMB — no punch, no
        // weapon attack (TakePhoto validates subjects + feeds missions).
        if (m_photoMode) {
            if (pc->attackCooldown <= 0.0f) {
                TakePhoto();
                pc->attackCooldown = 0.25f; // gentle shutter rate limit
            }
        }
        // Track 26: an equipped weapon replaces the unarmed punch entirely
        // (dodge still blocks it; the weapon's own cooldown governs the rate
        // and counter-arming is honored inside WeaponTryAttack).
        else if (pc->equippedSlot >= 0) {
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

    // Track 46: OPENBULLY_GOD=1 keeps the player alive for scripted stability
    // runs (the boss/AI tracks happily beat a mounted demo rider to death
    // mid-run, which ends the game before the stability window closes).
    static const bool s_god = std::getenv("OPENBULLY_GOD") != nullptr;
    if (s_god) return;

    // Track 41: the class session is a protected bubble — NPC fights outside
    // the (conceptual) classroom door cannot interrupt attendance.
    if (m_classSessionActive) {
        Logger::Info("Hit on player IGNORED (%s): class in session",
                     source ? source : "?");
        return;
    }
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
    // Track 49: NaN guard. A void fall used to poison focus with NaNs, and
    // the NaN view matrix tripped bgfx debug asserts (process exit 0x80000003).
    if (focus.x != focus.x || focus.y != focus.y || focus.z != focus.z) {
        m_camera.SetPosition(m_jimmySpawn + glm::vec3(0.0f, 4.0f, -6.0f));
        m_camera.SetLookAt(glm::vec3(0.0f, 1.0f, 0.0f));
        return;
    }
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
        // Track 42: faction membership from the model index (deterministic
        // clique round robin; prefects are overridden to Authority below).
        ai.faction = FactionForModel(uint32_t(i % m_npcModels.size()));
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
            ai.faction = Faction::Authority; // Track 42: authorities
            m_prefectIds[prefectCount] = e;
            ++prefectCount;
        }
        m_entities.AddAI(e, ai);
        // Track 23: melee combat state (stun/knockdown/combo on the target).
        m_entities.AddMeleeCombat(e);

        // Track 47: every NPC is a photo subject — prefects under their role
        // label, the rest under their model file name (minus .nif).
        if (ai.role == AIRole::Prefect) {
            RegisterPhotoTarget(e, "Prefect", Photo::TargetKind::Person, 1.8f);
        } else {
            std::string lbl = model.name;
            if (lbl.size() > 4 && lbl.substr(lbl.size() - 4) == ".nif")
                lbl.resize(lbl.size() - 4);
            RegisterPhotoTarget(e, lbl, Photo::TargetKind::Person, 1.8f);
        }

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
        // Track 49: bosses stay down for good (a respawned boss would break
        // the fight's victory/retry flow).
        if (ai.state == AIState::Ko) {
            ai.fallTimer += dt;
            const float k = std::min(1.0f, ai.fallTimer / kFallTime);
            t.rotation.x = -1.5707963f * (k * k); // ease-in fall onto the back
            ai.koTimer -= dt;
            if (ai.koTimer <= 0.0f && !m_entities.GetBoss(id))
                RespawnNpc(id, ai, t);
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

        // Track 48: the cutscene's scripted actors are frozen (their pose
        // comes from the timeline via ApplyCutscenePoses); the animation
        // driver below still picks walk/idle from the scripted move.
        if (IsCutsceneActor(id)) {
            UpdateEntityAnimation(id, CutsceneActorMoving(id), dt);
            return;
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
        // Track 49: boss entities run the dedicated boss FSM instead of the
        // regular Track 27 state machine (it handles its own melee progress,
        // specials and phases; bosses never flee or return home).
        if (m_entities.GetBoss(id)) {
            UpdateBossEntity(id, t, ai, dt);
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
        // Track 43: a hidden player cannot be spotted by authority at range
        // (within kHiddenBreakDist they notice the locker anyway). This makes
        // engaged authorities accumulate loseTimer and give up (RETURN).
        const bool hiddenBlind = m_wanted.hidden &&
            ai.role == AIRole::Prefect && // cops carry the prefect role
            distP > WantedSystem::kHiddenBreakDist;
        const bool seen = havePlayer && !hiddenBlind &&
            distP <= ai.sightRadius &&
            hasLineOfSight(pos + glm::vec3(0.0f, 1.6f, 0.0f),
                           playerPos + glm::vec3(0.0f, 1.4f, 0.0f));
        // Track 28: target-aware sight. With an event-assigned entity the LOS
        // check runs against it; WITHOUT one the player is the target (Track
        // 27 semantics), so `seenT` must fall back to `seen` — gating sight
        // on haveTgt alone made every player-targeted NPC flap ALERT↔RETURN
        // (sight "lost" it never had) instead of committing to CHASE.
        const bool seenT = hiddenBlind ? false
            : (haveTgt
            ? (distT <= ai.sightRadius &&
               HasLineOfSight(pos + glm::vec3(0.0f, 1.6f, 0.0f),
                              targetPos + glm::vec3(0.0f, 1.4f, 0.0f)))
            : seen);

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
        // Track 42: faction stance gates the sighting - Hostile factions skip
        // the deciding pause entirely (commit runs this frame), Friendly
        // factions never react to the player (they greet instead, below),
        // Neutral keeps the Track 27 face-then-disperse behavior.
        const FactionStance stance = m_factions.Stance(ai.faction);
        const bool roaming = ai.state == AIState::Idle ||
                             ai.state == AIState::Wander ||
                             ai.state == AIState::FacePlayer ||
                             ai.state == AIState::Patrol ||
                             ai.state == AIState::Return;
        if (roaming && seen && stance != FactionStance::Friendly) {
            ai.alertTimer = (stance == FactionStance::Hostile)
                ? 0.0f                     // attack on sight
                : ai.alertDuration;        // neutral: deciding pause
            setState(AIState::Alert);
        }
        // Track 42: friendly greeting - turn toward the player without
        // breaking the roam loop (high respect = more NPCs greet you).
        if (roaming && seen && stance == FactionStance::Friendly &&
            distP <= ai.faceRadius) {
            const float greetYaw = std::atan2(playerPos.x - pos.x,
                                              playerPos.z - pos.z);
            ai.facingYaw += shortestArc(ai.facingYaw, greetYaw) *
                            std::min(1.0f, ai.turnSpeed * dt);
            t.rotation.y = ai.facingYaw;
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
                // Plain player sighting: stare, then disperse - do NOT auto-
                // chase. Track 28 reactions (fight-back, help calls, prefect
                // response) drive CHASE; ambient sight-chase pinned every NPC
                // in ATTACK on the player and made Track 30's talk-to-calm-NPC
                // unreachable (retail Bully NPCs gather, they don't swarm).
                // Track 42 exception: a HOSTILE-faction sighting commits to
                // CHASE the player (respect < -50 = attack on sight).
                const bool hostileCommit =
                    stance == FactionStance::Hostile && !ai.targetEntity.IsValid();
                setState(crowdSlot ? AIState::Watch
                         : hostileCommit ? AIState::Chase : AIState::Return);
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
        // Track 48: a scripted actor's walk/idle comes from the timeline, not
        // the frozen input state — MOVE_TO in flight = walk, otherwise idle.
        const bool wantsWalk = moving || CutsceneActorMoving(id);
        if (wantsWalk && rig.walkClip >= 0) {
            ctrl.Play(rig.walkClip, true, 0.2f);
            ac->playing = true;
        } else if (!wantsWalk && rig.idleClip >= 0) {
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
            if (!s_demo && !m_dialogue.IsActive() && !m_journalOpen &&
                !m_cutscenePlaying && // Track 48: no attacks during cutscenes
                !m_photoMode && // Track 47: LMB is the shutter in photo mode
                pc->equippedSlot < 0 &&
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
    // Track 49: a counter-boosted hit on a boss opens a punish QTE
    // (success = extra boss damage; failure = the boss answers back).
    if (m_entities.GetBoss(best)) {
        if (PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
            if (pc->counterArmed) {
                if (BossComponent* bc = m_entities.GetBoss(best)) {
                    StartBossQTE(*bc, 1.2f);
                    Logger::Info("BOSS: counter-hit -> punish QTE");
                }
            }
        }
    }
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
    // Track 49: a BOSS downed the player -> boss retry (NOT the generic
    // Busted stub: bosses must not trigger the arrest cut on their kill).
    if (PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
        if (pc->health.current <= 0) {
            if (m_entities.GetBoss(attacker))
                OnPlayerDefeatedByBoss();
            else {
                const AIComponent* aai = m_entities.GetAI(attacker);
                if (aai && aai->role == AIRole::Prefect) Busted();
            }
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
    // Track 49: the boss duel is sanctioned - hitting the boss is not a
    // crime (no respect loss, no wanted heat, no prefect pile-on).
    const bool bossFight = m_entities.GetBoss(npc) != nullptr;
    // Track 42: the player's violence costs respect with the victim's faction
    // (witness reactions run through the HelpCall path below).
    if (attacker == m_jimmyId && !bossFight)
        ModifyRespect(NpcFaction(npc), FactionTuning::kHit, "hit faction member");
    RegisterFight(hitPos, attacker);
    // Track 43: player violence is a crime event - witnessed by any nearby
    // authority (+1 head) or doubled when the victim is authority itself
    // (+2). Before the KO early-return so knockouts charge too.
    if (attacker == m_jimmyId && !bossFight) PlayerViolence(npc, hitPos);
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
    // Track 42: a KO costs extra respect with the victim's faction.
    if (attacker == m_jimmyId)
        ModifyRespect(NpcFaction(npc), FactionTuning::kKo, "faction member KO'd");
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
    // Track 49: boss down -> victory sequence (banner, cutscene, mission).
    if (m_entities.GetBoss(npc)) OnBossDefeated(npc);
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
            // Track 42: the caller's faction - same-clique allies answer
            // (Bully crowds back their own), same-model stays as a fallback.
            const Faction callerFaction = NpcFaction(e.sourceEntity);
            m_entities.ForEachAI([&](EntityID id, Transform& t, AIComponent& ai) {
                (void)t;
                if (id == e.sourceEntity || id == e.targetEntity) return;
                if (ai.state != AIState::Idle && ai.state != AIState::Wander &&
                    ai.state != AIState::FacePlayer &&
                    ai.state != AIState::Patrol && ai.state != AIState::Return)
                    return;
                const MeshComponent* m = m_entities.GetMesh(id);
                const bool sameFaction = ai.faction == callerFaction;
                if (!m || (!sameFaction &&
                           m->npcModelIndex != cm->npcModelIndex)) return;
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

// Track 28 stub, Track 43 full implementation: the player was caught.
// Logs BUSTED, restores full health and teleports back to the spawn point.
void Engine::Busted() {
    Logger::Info("BUSTED");
    // Track 42: getting caught costs standing with the authorities.
    ModifyRespect(Faction::Authority, FactionTuning::kBust, "busted");
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

// =========================================================================
// Track 43: Wanted (trouble) system.
// =========================================================================

// The single gate for raising the trouble meter. `crime` carries the charge
// (+1 head normally, +2 for hitting authority); `reason` is the log tag.
// Per-crime throttling (kCrimeMinGap) keeps one ongoing fight from stacking
// five heads in five punches; the whistle plays only on the 0 -> 1 climb
// ("spotted"), and every climb stamps the HUD red flash.
void Engine::AddWanted(WantedCrime crime, const char* reason) {
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc) return;
    if (m_wanted.bustPhase != 0) return; // mid-bust cut: ignore further crimes
    // Throttle: the same crime category cannot re-charge inside the window
    // (a ten-punch brawl is still one "fighting" head; a second WINDOW after
    // the gap re-charges, so prolonged trouble still escalates).
    if (m_time - m_wanted.lastCrimeTime[size_t(crime)] <
        WantedSystem::kCrimeMinGap) return;
    m_wanted.lastCrimeTime[size_t(crime)] = m_time + 1e-9;

    const int charge = (crime == WantedCrime::AttackAuthority) ? 2 : 1;
    const int before = pc->wanted;
    pc->wanted = std::min(WantedSystem::kMaxHeads, pc->wanted + charge);
    if (pc->wanted == before) return; // already at max: meter pins at 5
    m_wanted.flashTimer = 1.2f;       // HUD red flash on every climb
    Logger::Info("WANTED: %s -> %d head%s (%s)", reason, pc->wanted,
                 pc->wanted == 1 ? "" : "s",
                 crime == WantedCrime::Fighting       ? "fighting" :
                 crime == WantedCrime::AttackAuthority ? "assault on authority" :
                 crime == WantedCrime::Vandalism       ? "vandalism" :
                 crime == WantedCrime::Trespass        ? "trespassing" :
                                                         "curfew violation");
    // "Spotted" whistle: the transition from clean to hunted.
    if (before == 0 && m_audio) m_audio->PlaySound(SoundId::Whistle, 0.9f);
}

// Combat hook: the player just damaged `victim`. Fighting raises the meter
// only when an authority actually sees it (within the hunt radius with LOS,
// or close enough that sight is moot); striking authority is always
// witnessed by the victim itself and worth double.
void Engine::PlayerViolence(EntityID victim, const glm::vec3& pos) {
    const AIComponent* vai = m_entities.GetAI(victim);
    if (vai && vai->role == AIRole::Prefect) { // cops share the prefect role
        AddWanted(WantedCrime::AttackAuthority, "attacked authority");
        return;
    }
    if (AnyAuthorityWitnessed(pos))
        AddWanted(WantedCrime::Fighting, "fighting");
}

// Projectile/world hook: the player just vandalized something at `pos`.
// Same witness rule as fighting; also counts as the "breaking windows"
// charge for the task table.
void Engine::VandalismCrime(const glm::vec3& pos) {
    if (AnyAuthorityWitnessed(pos))
        AddWanted(WantedCrime::Vandalism, "vandalism");
}

// True when any authority (prefect or cop) is within `radius` of `pos`
// with a clear line of sight (or simply very close, where LOS is moot).
bool Engine::AnyAuthorityWitnessed(const glm::vec3& pos) {
    const float closeSq = 6.0f * 6.0f;
    const float radSq = WantedSystem::kAuthorityHuntRange *
                        WantedSystem::kAuthorityHuntRange;
    bool seen = false;
    m_entities.ForEachAI([&](EntityID, const Transform& t,
                             const AIComponent& ai) {
        if (seen) return;
        if (ai.role != AIRole::Prefect) return; // cops carry the prefect role
        if (ai.state == AIState::Ko) return;
        const glm::vec3 d = t.position - pos;
        const float distSq = d.x * d.x + d.z * d.z;
        if (distSq > radSq) return;
        if (distSq <= closeSq ||
            HasLineOfSight(t.position + glm::vec3(0.0f, 1.6f, 0.0f),
                           pos + glm::vec3(0.0f, 1.2f, 0.0f)))
            seen = true;
    });
    return seen;
}

// True when any living authority stands within `radius` of the player
// (no LOS check: used for the hide-broken and leave-area decay gates).
bool Engine::AnyAuthorityNear(float radius) {
    if (!m_jimmyId.IsValid()) return false;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return false;
    const float radSq = radius * radius;
    bool inRange = false;
    m_entities.ForEachAI([&](EntityID, const Transform& t,
                             const AIComponent& ai) {
        if (inRange) return;
        if (ai.role != AIRole::Prefect) return; // cops carry the prefect role
        if (ai.state == AIState::Ko) return;
        const glm::vec3 d = t.position - jt->position;
        if (d.x * d.x + d.z * d.z <= radSq) inRange = true;
    });
    return inRange;
}

// Any authority currently pursuing Jimmy (chase/attack with him as target,
// or a plain player-targeted engagement)? Drives the wardrobe-reset rule
// and the "leave the area" slow decay.
bool Engine::AnyAuthorityChasing() {
    if (!m_jimmyId.IsValid()) return false;
    bool chasing = false;
    m_entities.ForEachAI([&](EntityID, const Transform&,
                             const AIComponent& ai) {
        if (chasing) return;
        if (ai.role != AIRole::Prefect) return; // cops carry the prefect role
        if (ai.state != AIState::Chase && ai.state != AIState::Attack &&
            ai.state != AIState::Alert) return;
        // Engaged on Jimmy specifically (explicit target or the fallback).
        if (!ai.targetEntity.IsValid() || ai.targetEntity == m_jimmyId)
            chasing = true;
    });
    return chasing;
}

// Builds the restricted (trespass) zones once, relative to Jimmy's spawn so
// they stay valid wherever the level loads. Two roof-edge strips, placed to
// NOT overlap the demo mission walk targets (x [-12.1, 12.9], z [-4.5, 3.5]
// relative to spawn) so the scripted mission drive never trips them.
void Engine::SetupTrespassZones() {
    if (!m_trespassZones.empty()) return;
    if (m_jimmySpawn == glm::vec3(0.0f)) return; // not initialized yet
    const glm::vec3 s = m_jimmySpawn;
    m_trespassZones.push_back({"roof_north_strip",
        s + glm::vec3(-13.5f, -2.0f, 4.2f), s + glm::vec3(13.5f, 4.0f, 8.2f)});
    m_trespassZones.push_back({"roof_west_edge",
        s + glm::vec3(-13.5f, -2.0f, -4.5f), s + glm::vec3(-12.2f, 4.0f, 4.2f)});
    Logger::Info("Wanted: %zu restricted zone(s) registered (trespass = +1 head)",
                 m_trespassZones.size());
}

// Per-frame wanted driver: HUD flash decay, the bust cut, curfew/trespass
// crime checks, the two escape decays (hidden / left the area), cop spawn
// and the authority response (hunt + grab-range bust).
void Engine::UpdateWanted(float dt) {
    if (dt <= 0.0f) return;

    // HUD flash decays even without a player (free camera still shows it).
    if (m_wanted.flashTimer > 0.0f)
        m_wanted.flashTimer = std::max(0.0f, m_wanted.flashTimer - dt);

    // The bust cut runs on engine time even mid-chase.
    if (m_wanted.bustPhase == 1) {
        m_wanted.bustTimer += dt;
        m_wanted.bustFade = std::min(1.0f, m_wanted.bustTimer /
                                               WantedSystem::kBustFadeSeconds);
        if (m_wanted.bustTimer >= WantedSystem::kBustFadeSeconds) FinishBust();
        return; // the world is behind the black screen
    }
    if (m_wanted.bustPhase == 2) {
        m_wanted.bustTimer -= dt;
        m_wanted.bustFade = std::max(0.0f, m_wanted.bustTimer /
                                               WantedSystem::kBustFadeSeconds);
        if (m_wanted.bustTimer <= 0.0f) {
            m_wanted.bustFade = 0.0f;
            m_wanted.bustPhase = 0;
            Logger::Info("BUSTED: back in control (outside the school)");
        }
        return;
    }

    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc) return;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;
    const glm::vec3 pos = jt->position;

    // --- Crime: restricted areas (trespass = +1 head per entry) ------------
    SetupTrespassZones();
    bool trespassing = false;
    for (const TrespassZone& z : m_trespassZones) {
        if (pos.x >= z.bmin.x && pos.x <= z.bmax.x &&
            pos.y >= z.bmin.y && pos.y <= z.bmax.y &&
            pos.z >= z.bmin.z && pos.z <= z.bmax.z) {
            trespassing = true;
            break;
        }
    }
    if (trespassing && !m_wanted.trespassInside) {
        m_wanted.trespassInside = true;
        AddWanted(WantedCrime::Trespass, "trespassing (restricted area)");
    } else if (!trespassing) {
        m_wanted.trespassInside = false;
    }

    // --- Crime: curfew (out after 23:00, until 07:00) ----------------------
    // One charge per night: the latch clears in the morning. Hiding out
    // skips the charge (a locker is technically indoors).
    const bool curfew = m_clockMinutes >= int(WantedSystem::kCurfewStartMin) ||
                        m_clockMinutes <  int(WantedSystem::kCurfewEndMin);
    if (curfew && !m_wanted.curfewCharged) {
        m_wanted.curfewCharged = true;
        if (!m_wanted.hidden)
            AddWanted(WantedCrime::Curfew, "curfew (out after 23:00)");
    } else if (!curfew) {
        m_wanted.curfewCharged = false;
    }

    // --- Escape: decay while hidden or out of reach ------------------------
    if (pc->wanted > 0) {
        if (m_wanted.hidden) {
            m_wanted.hideTimer += dt;
            m_wanted.safetyTimer = 0.0f;
            if (m_wanted.hideTimer >= WantedSystem::kHideDecayInterval) {
                m_wanted.hideTimer -= WantedSystem::kHideDecayInterval;
                pc->wanted = std::max(0, pc->wanted - 1);
                Logger::Info("WANTED: heat decays while hidden -> %d", pc->wanted);
            }
        } else if (!AnyAuthorityNear(WantedSystem::kAuthorityClearDist)) {
            // Left the area: nobody hunting within 30 u, heat bleeds slowly.
            m_wanted.safetyTimer += dt;
            m_wanted.hideTimer = 0.0f;
            if (m_wanted.safetyTimer >= WantedSystem::kLeaveDecayInterval) {
                m_wanted.safetyTimer -= WantedSystem::kLeaveDecayInterval;
                pc->wanted = std::max(0, pc->wanted - 1);
                Logger::Info("WANTED: heat decays out of sight -> %d", pc->wanted);
            }
        } else {
            m_wanted.safetyTimer = 0.0f;
            m_wanted.hideTimer = 0.0f;
        }
    }

    // --- Cops: spawn at 4+ heads, stand down below 4 -----------------------
    // Entity create/destroy is deferred to the top of the next frame
    // (ServiceCopRequests): mid-frame mutation reallocates the entity and rig
    // stores while other systems hold component/rig references.
    // OPENBULLY_NO_COPS=1: diagnostic gate - skips cop spawning entirely
    // (prefects still chase + bust; used to isolate the cop-spawn crash).
    static const bool s_noCops = std::getenv("OPENBULLY_NO_COPS") != nullptr;
    if (!s_noCops) {
        if (pc->wanted >= WantedSystem::kCopSpawnAtHeads && m_copIds.empty() &&
            !m_copsPending)
            m_copsPending = true;
        else if (pc->wanted < WantedSystem::kCopSpawnAtHeads &&
                 (!m_copIds.empty() || m_copsPending))
            m_copsStandDown = true;
    }

    // --- Authority response -------------------------------------------------
    const bool wasChased = m_wanted.chased;
    m_wanted.chased = false;
    if (pc->wanted > 0) {
        bool bustNow = false;
        // Post-bust walk-away grace: while the respawn i-frames tick, local
        // authorities do not re-aggro (retail lets you walk out of the office
        // first; without this the schedule-prefect camping the spawn busts
        // again the instant a new crime charges).
        const bool grace = pc->invincibleTimer > 0.0f;
        m_entities.ForEachAI([&](EntityID id, Transform& t, AIComponent& ai) {
            if (ai.role != AIRole::Prefect || ai.state == AIState::Ko) return;
            const float dxz = glm::length(
                glm::vec2(t.position.x - pos.x, t.position.z - pos.z));
            const bool engaged = ai.state == AIState::Alert ||
                                 ai.state == AIState::Chase ||
                                 ai.state == AIState::Attack;
            if (!engaged) {
                // Hot player: any authority that spots Jimmy gives chase
                // (skipped while hidden - that is the whole point of hiding,
                // and during the post-bust walk-away grace).
                if (m_wanted.hidden || grace) return;
                if (dxz > WantedSystem::kAuthorityHuntRange) return;
                if (!HasLineOfSight(t.position + glm::vec3(0.0f, 1.6f, 0.0f),
                                    pos + glm::vec3(0.0f, 1.2f, 0.0f))) return;
                ai.state = AIState::Chase;
                ai.targetEntity = m_jimmyId;
                ai.hasTarget = false;
                ai.loseTimer = 0.0f;
                ai.stuckTimer = 0.0f;
                ai.sidestepTimer = 0.0f;
                ai.lastPos = t.position;
                Logger::Info("WANTED: authority %u spotted Jimmy (wanted %d) "
                             "- giving chase", id.index, pc->wanted);
                return;
            }
            // Engaged on Jimmy: counts as chased; within grab range = busted.
            if (!ai.targetEntity.IsValid() || ai.targetEntity == m_jimmyId) {
                m_wanted.chased = true;
                if (dxz <= WantedSystem::kGrabRange) bustNow = true;
            }
        });
        // "Spotted" whistle when a pursuit begins (re-arms once pursuit ends).
        if (m_wanted.chased && !wasChased && m_audio)
            m_audio->PlaySound(SoundId::Whistle, 0.8f);
        if (bustNow) {
            BeginBust();
            return;
        }
    }
}

// 4+ heads: the police arrive. Two cop NPCs (prefect-role AI, tuned faster
// and sharper-sighted) walk in from kCopSpawnDistance and immediately chase.
void Engine::SpawnCops() {
    if (m_npcModels.empty() || !m_jimmyId.IsValid()) return;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;
    Logger::Info("WANTED: %d heads - cops called in", WantedSystem::kCopSpawnAtHeads);
    for (int i = 0; i < WantedSystem::kCopSpawnCount; ++i) {
        const float ang = float(i) * 3.14159265f + 0.7f;
        glm::vec3 pos = jt->position +
            glm::vec3(std::cos(ang) * WantedSystem::kCopSpawnDistance, 0.0f,
                      std::sin(ang) * WantedSystem::kCopSpawnDistance);
        // Ground snap from just above the player's plane (same guard as the
        // crowd spawn: never drop an NPC through hollow geometry).
        if (m_collisionWorld) {
            Ray down;
            down.origin[0] = pos.x; down.origin[1] = jt->position.y + 10.0f;
            down.origin[2] = pos.z;
            down.direction[0] = 0.0f; down.direction[1] = -1.0f;
            down.direction[2] = 0.0f;
            RayHit hit;
            if (m_collisionWorld->Raycast(down, hit) &&
                std::fabs(hit.point[1] - jt->position.y) < 6.0f)
                pos.y = hit.point[1];
            else
                pos.y = jt->position.y;
        }
        // Cop entities reuse the crowd's model-0 rig (same lazy-build path
        // as the schedule spawner) so they render + animate like prefects.
        NpcModel& model = m_npcModels[0];
        uint32_t rigIndex = model.rigIndex;
        if (rigIndex == 0xFFFFFFFF && !m_sharedClips.empty() &&
            !model.sourcePath.empty()) {
            const NifLoadResult nif = LoadNifFile(model.sourcePath);
            if (nif.ok) rigIndex = CreateRig(nif);
            model.rigIndex = rigIndex; // cached even on failure (no retry)
        }
        if (rigIndex != 0xFFFFFFFF) rigIndex = CloneRig(rigIndex);

        EntityID e = m_entities.Create();
        Transform t;
        t.position = pos;
        t.scale = glm::vec3(model.bodyScale);
        t.rotation.y = ang + 3.14159265f; // face the player
        m_entities.AddTransform(e, t);
        MeshComponent mc;
        mc.isBox = false;
        mc.isNpc = true;
        mc.npcModelIndex = 0;
        m_entities.AddMesh(e, mc);
        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
        cc.boundsMax = glm::vec3( 0.5f, 1.8f,  0.5f);
        cc.layerMask = 2u;
        m_entities.AddCollision(e, cc);
        PhysicsComponent phy;
        phy.gravity = -9.8f;
        m_entities.AddPhysics(e, phy);
        AIComponent ai;
        ai.homePos = pos;
        ai.facingYaw = t.rotation.y;
        ai.lastPos = pos;
        ai.role = AIRole::Prefect;      // authority: chases + busts like a prefect
        ai.state = AIState::Chase;
        ai.targetEntity = m_jimmyId;
        ai.hasTarget = false;
        ai.sightRadius = 24.0f;         // cops see farther than prefects
        ai.moveSpeed = 2.6f;            // cops are faster than the crowd
        ai.timer = 0.0f;
        ai.loseTimer = 0.0f;
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
        m_copIds.push_back(e);
        Logger::Info("WANTED: cop spawned at (%.1f, %.1f, %.1f) - chasing",
                     pos.x, pos.y, pos.z);
    }
}

// Heat dropped below 4 (or the bust cut): send the cops away.
void Engine::DespawnCops() {
    if (m_copIds.empty()) return;
    for (EntityID id : m_copIds)
        if (id.IsValid() && m_entities.IsAlive(id)) m_entities.Destroy(id);
    Logger::Info("WANTED: %zu cop(s) stood down", m_copIds.size());
    m_copIds.clear();
}

// Track 43: materializes deferred cop spawn/despawn requests at the TOP of
// the frame, before any system holds entity/rig references. SpawnCops clones
// animation rigs (reallocating m_animRigs) and DespawnCops destroys entities;
// doing either mid-frame (from UpdateWanted) risks dangling references in
// systems that already iterated the stores this frame.
void Engine::ServiceCopRequests() {
    if (m_copsStandDown) {
        m_copsStandDown = false;
        m_copsPending = false; // a stand-down cancels an un-materialized spawn
        DespawnCops();
    }
    if (m_copsPending) {
        m_copsPending = false;
        SpawnCops();
    }
}

// Busted! Starts the cut: fade to black (FinishBust applies the penalty at
// full black and fades back in). Safe to call repeatedly - the phase guard
// makes the second call a no-op.
void Engine::BeginBust() {
    if (m_wanted.bustPhase != 0) return;
    if (!m_jimmyId.IsValid()) return;
    m_wanted.bustPhase = 1;
    m_wanted.bustTimer = 0.0f;
    m_wanted.bustFade = 0.0f;
    if (m_audio) m_audio->PlaySound(SoundId::Whistle, 1.0f);
    Logger::Info("BUSTED! caught by authority - fading out");
}

// Full black: apply the bust penalty, cut to the spawn point, fade back in.
// Authority lets go (cops despawn, prefects resume their beat), fight
// records and the event queue clear, and any active "don't get busted"
// mission fails.
void Engine::FinishBust() {
    m_wanted.bustPhase = 2;
    m_wanted.bustTimer = WantedSystem::kBustFadeSeconds;

    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (pc) {
        const int hadWanted = pc->wanted;
        const int hadMoney  = pc->money;
        pc->wanted = 0;
        pc->health.current = pc->health.max;
        pc->state = DefenseState::Normal;
        pc->invincibleTimer = 6.0f; // walk-away grace after the bust cut
        const int loss = (hadMoney * WantedSystem::kMoneyLossPercent) / 100;
        pc->money = hadMoney - loss;
        Logger::Info("BUSTED: wanted %d -> 0, money -%d (kept %d)",
                     hadWanted, loss, pc->money);
    }

    // Respawn outside: at the spawn point (school front), ground-snapped.
    Transform* jt = m_jimmyId.IsValid()
        ? m_entities.GetTransform(m_jimmyId) : nullptr;
    glm::vec3 pos = m_jimmySpawn;
    if (m_collisionWorld) {
        Ray down;
        down.origin[0] = pos.x; down.origin[1] = pos.y + 10.0f;
        down.origin[2] = pos.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f;
        down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) pos.y = hit.point[1];
    }
    if (jt) {
        jt->position = pos + glm::vec3(0.0f, 0.2f, 0.0f);
        jt->rotation.x = 0.0f;
        jt->rotation.z = 0.0f;
    }
    m_camDesired = pos;

    // Everyone lets go: cops go home (deferred to the top of the frame -
    // destroying entities mid-frame can dangle references), prefects resume
    // their patrol.
    m_copsStandDown = true;
    m_copsPending = false;
    m_entities.ForEachAI([&](EntityID, Transform&, AIComponent& ai) {
        if (ai.role != AIRole::Prefect) return;
        ai.state = AIState::Idle;
        ai.targetEntity = EntityID{};
        ai.hasTarget = false;
        ai.loseTimer = 0.0f;
        ai.timer = 1.0f;
    });
    m_aiEvents.clear();
    m_fights.clear();
    m_wanted.hidden = false;
    m_wanted.hideTimer = 0.0f;
    m_wanted.safetyTimer = 0.0f;
    m_wanted.curfewCharged = false;

    // Mission hook: "don't get busted" objectives fail their mission.
    const int failed = m_missions.NotifyBust();
    if (failed > 0)
        Logger::Info("BUSTED: %d mission(s) failed (DON'T GET BUSTED)", failed);
}

// OPENBULLY_WANTED_DEMO=1: scripted wanted-system drive for headless
// verification (same philosophy as the other *_DEMO hooks - OS key
// injection is unreliable). Timeline (engine seconds):
//    1.0  assault on authority (+2)  -> wanted 2, whistle + HUD flash;
//         the nearby schedule-prefect gives chase (authority response)
//   ~4.5  prefect reaches grab range -> BUST: fade out, -50%% money,
//         respawn at the spawn point, wanted 0 (bust cut lands on its own)
//    8.0  trespass (+1)              -> wanted 1 (post-bust re-charge works)
//   10.0  hide (as if B pressed)     -> 1 head decays every 10 s
//   21.0  unhide                     -> wanted 0 (hide decay verified)
//   23.0  vandalism (+1)             -> wanted 1
//   25.0  assault (+2)               -> wanted 3
//   27.0  trespass (+1)              -> wanted 4 -> COPS SPAWN next frame
//   ~35   an authority reaches grab range -> second BUST (cops involved)
//   45.0  summary log line
void Engine::DriveWantedDemo(float dt) {
    (void)dt;
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc) return;
    static int s_step = 0;
    switch (s_step) {
    case 0:
        if (m_time >= 1.0) {
            s_step = 1;
            AddWanted(WantedCrime::AttackAuthority, "DEMO: punched a prefect");
        }
        break;
    case 1: // wait for the first bust cycle to complete (wanted back to 0)
        if (m_time >= 8.0) {
            s_step = 2;
            AddWanted(WantedCrime::Trespass, "DEMO: entered a restricted area");
        }
        break;
    case 2:
        if (m_time >= 10.0) {
            s_step = 3;
            m_wanted.hidden = true;
            m_wanted.hideTimer = 0.0f;
            Logger::Info("WANTED DEMO: hidden - decay every %.0f s",
                         WantedSystem::kHideDecayInterval);
        }
        break;
    case 3:
        if (m_time >= 21.0) {
            s_step = 4;
            m_wanted.hidden = false;
            Logger::Info("WANTED DEMO: unhidden (wanted now %d)", pc->wanted);
        }
        break;
    case 4:
        if (m_time >= 23.0) {
            s_step = 5;
            AddWanted(WantedCrime::Vandalism, "DEMO: smashed a window");
        }
        break;
    case 5:
        if (m_time >= 25.0) {
            s_step = 6;
            m_wanted.lastCrimeTime[size_t(WantedCrime::AttackAuthority)] = -1e9;
            AddWanted(WantedCrime::AttackAuthority, "DEMO: attacked authority");
        }
        break;
    case 6:
        if (m_time >= 27.0) {
            s_step = 7;
            AddWanted(WantedCrime::Trespass, "DEMO: entered a restricted area");
            Logger::Info("WANTED DEMO: 4 heads - expecting cops next frame");
        }
        break;
    case 7:
        if (m_time >= 45.0) {
            s_step = 8;
            Logger::Info("WANTED DEMO: complete (wanted=%d bustPhase=%d "
                         "cops=%zu money=%d health=%d hidden=%d)",
                         pc->wanted, m_wanted.bustPhase, m_copIds.size(),
                         pc->money, pc->health.current,
                         m_wanted.hidden ? 1 : 0);
        }
        break;
    default:
        break;
    }
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
// =========================================================================
// Track 42: faction respect (Nerds / Jocks / Greasers / Preppies / Bullies /
// Townies / Authority). One respect value per faction, clamped [-100, 100].
// Bands gate NPC perception: Hostile (< -50) attacks on sight, Friendly
// (> +50) never attacks and greets instead; Neutral keeps the Track 27
// face-then-disperse sighting. Overall respect = mean + mission bonus.
// =========================================================================

// Deterministic model -> faction mapping: the crowd is a round-robin mix of
// the 6 student cliques so every faction is represented wherever NPCs spawn.
// Authority comes only from AIRole::Prefect (applied at the spawn sites).
Faction Engine::FactionForModel(uint32_t modelIndex) {
    static const Faction kMix[6] = {
        Faction::Nerds, Faction::Jocks, Faction::Greasers,
        Faction::Preppies, Faction::Bullies, Faction::Townies,
    };
    return kMix[modelIndex % 6u];
}

// Faction of a live NPC entity. Authorities (role Prefect) always read as
// Faction::Authority regardless of the model-index mapping.
Faction Engine::NpcFaction(EntityID id) {
    if (!m_entities.IsAlive(id)) return Faction::Townies;
    const AIComponent* ai = m_entities.GetAI(id);
    if (!ai) return Faction::Townies;
    return (ai->role == AIRole::Prefect) ? Faction::Authority : ai->faction;
}

// Adds a respect delta to one faction (clamped inside FactionSystem) and
// logs the before/after pair - the primary verification signal for headless
// faction runs.
void Engine::ModifyRespect(Faction f, float delta, const char* reason) {
    const float before = m_factions.Get(f);
    if (delta == 0.0f) return;
    const float after = m_factions.Modify(f, delta, reason);
    const FactionStance st = m_factions.Stance(f);
    Logger::Info("RESPECT: %s %+.0f -> %.0f (%s) [%s]",
                 FactionName(f), after - before, after,
                 reason ? reason : "?", FactionStanceName(st));
}

// TAB toggles the stats overlay; F gifts the nearest NPC.
void Engine::HandleFactionKeys() {
    if (m_input.WasKeyPressed(SDL_SCANCODE_TAB)) {
        m_statsOpen = !m_statsOpen;
        Logger::Info("Faction stats: %s", m_statsOpen ? "OPEN" : "closed");
    }
    if (m_input.WasKeyPressed(SDL_SCANCODE_F))
        TryGiftToNpc();
}

// F key: give a gift to the nearest NPC within 3u. Small respect boost with
// that NPC's faction; gifts cannot reach a KO'd NPC (they are on the floor).
void Engine::TryGiftToNpc() {
    if (!m_jimmyActive || !m_jimmyId.IsValid()) return;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;
    static constexpr float kGiftRange = 3.0f;
    EntityID best{};
    float bestDist = 1e9f;
    m_entities.ForEachAI([&](EntityID id, const Transform& t, const AIComponent& ai) {
        if (ai.state == AIState::Ko) return;
        const float d = glm::length(t.position - jt->position);
        if (d < bestDist) { bestDist = d; best = id; }
    });
    if (!best.IsValid() || bestDist > kGiftRange) {
        Logger::Info("GIFT: no NPC within %.1fu (closest %.1fu)",
                     kGiftRange, bestDist);
        return;
    }
    const Faction f = NpcFaction(best);
    ModifyRespect(f, FactionTuning::kGift, "gift");
    AppendJournalLog(std::string("Gift given - ") + FactionName(f) +
                     " respect " + std::to_string(m_factions.GetRounded(f)));
}

// Flattens faction state into HUDState: per-faction rows (stats screen),
// the overall aggregate, and the nearest-NPC interaction readout.
void Engine::UpdateFactionHUD() {
    for (int i = 0; i < kFactionCount; ++i) {
        const Faction f = static_cast<Faction>(i);
        HUDState::FactionRow& row = m_hudState.factionRows[i];
        std::snprintf(row.name, sizeof(row.name), "%s", FactionName(f));
        row.respect = m_factions.GetRounded(f);
        row.stance  = static_cast<int>(m_factions.Stance(f));
    }
    m_hudState.overallRespect    = m_factions.GetRoundedOverall();
    m_hudState.completedMissions = m_factions.CompletedMissions();
    m_hudState.statsVisible      = m_statsOpen;

    // Interaction readout: nearest standing NPC within gift reach.
    m_hudState.factionBarVisible = false;
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        if (const Transform* jt = m_entities.GetTransform(m_jimmyId)) {
            static constexpr float kBarRange = 3.0f;
            EntityID best{};
            float bestDist = 1e9f;
            m_entities.ForEachAI([&](EntityID id, const Transform& t,
                                     const AIComponent& ai) {
                if (ai.state == AIState::Ko) return;
                const float d = glm::length(t.position - jt->position);
                if (d < bestDist) { bestDist = d; best = id; }
            });
            if (best.IsValid() && bestDist <= kBarRange) {
                const Faction f = NpcFaction(best);
                m_hudState.factionBarVisible = true;
                m_hudState.factionRespect = m_factions.GetRounded(f);
                m_hudState.factionStance  = static_cast<int>(m_factions.Stance(f));
                std::snprintf(m_hudState.factionName,
                              sizeof(m_hudState.factionName), "%s",
                              FactionName(f));
            }
        }
    }
}

// Applies a mission's faction respect reward on completion (fired through
// MissionManager::SetOnComplete). Also bumps the completed-mission tally
// that feeds the overall-respect aggregate.
void Engine::ApplyMissionReward(const Mission* m) {
    if (!m) return;
    m_factions.AddCompletedMission();
    if (m->respectFaction < 0 || m->respectFaction >= kFactionCount) return;
    const Faction f = static_cast<Faction>(m->respectFaction);
    const float amount = m->respectReward != 0
        ? float(m->respectReward) : FactionTuning::kMission;
    ModifyRespect(f, amount, "mission reward");
    AppendJournalLog(std::string("Mission complete - ") + FactionName(f) +
                     " respect " + std::to_string(m_factions.GetRounded(f)));
}

// UI color pack matching UIRenderer::PackRGBA (0xAARRGGBB literals used
// elsewhere in this file; this helper keeps the faction panel readable).
namespace {
inline uint32_t FactionCol(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t(a) << 24) | (uint32_t(b) << 16) |
           (uint32_t(g) << 8) | uint32_t(r);
}
} // namespace

// Track 42 overlay: (1) the interaction readout - nearest NPC's faction +
// respect while within gift range; (2) the TAB stats screen - one bar per
// faction (center-origin: negative red, positive green), the overall
// aggregate and the completed-mission tally.
void Engine::DrawFactionStats() {
    if (!m_ui.IsInitialized()) return;
    const float W = float(m_screenWidth);
    const float H = float(m_screenHeight);
    if (W <= 0.0f || H <= 0.0f) return;
    const float s = std::min(1.5f, std::max(0.75f, H / 720.0f));

    const uint32_t white    = FactionCol(235, 235, 240, 255);
    const uint32_t dim      = FactionCol(170, 170, 178, 220);
    const uint32_t hostile  = FactionCol(240, 90, 90, 255);
    const uint32_t neutral  = FactionCol(235, 210, 80, 255);
    const uint32_t friendly = FactionCol(90, 220, 110, 255);
    const uint32_t gold     = FactionCol(255, 215, 80, 255);
    const uint32_t panelBg  = FactionCol(12, 12, 18, 235);
    const uint32_t trackBg  = FactionCol(40, 40, 48, 220);
    const uint32_t border   = FactionCol(200, 190, 140, 180);
    const uint32_t veil     = FactionCol(0, 0, 0, 150);

    // --- interaction readout (top-left) ------------------------------------
    if (m_hudState.factionBarVisible) {
        char line[64];
        std::snprintf(line, sizeof(line), "%s: %+d (%s)",
                      m_hudState.factionName, m_hudState.factionRespect,
                      m_hudState.factionStance == 0 ? "HOSTILE" :
                      m_hudState.factionStance == 2 ? "FRIENDLY" : "NEUTRAL");
        const uint32_t col =
            m_hudState.factionStance == 0 ? hostile :
            m_hudState.factionStance == 2 ? friendly : neutral;
        m_ui.DrawText(24.0f * s, 24.0f * s, line, col);
        // Center-origin mini bar under the label.
        const float barW = 120.0f * s, barH = 8.0f * s;
        const float bx = 24.0f * s, by = 24.0f * s + 12.0f * s;
        m_ui.DrawRect(bx - 2.0f * s, by - 2.0f * s, barW + 4.0f * s,
                      barH + 4.0f * s, panelBg);
        m_ui.DrawRect(bx, by, barW, barH, trackBg);
        const float half = barW * 0.5f;
        const float fill = (std::min(100, std::max(-100,
                              m_hudState.factionRespect)) / 100.0f) * half;
        if (fill >= 0.0f) m_ui.DrawRect(bx + half, by, fill, barH, col);
        else m_ui.DrawRect(bx + half + fill, by, -fill, barH, col);
        m_ui.DrawRect(bx + half - 1.0f, by, 2.0f, barH, dim);
    }

    // --- TAB stats screen ---------------------------------------------------
    if (!m_statsOpen) return;
    m_ui.DrawRect(0.0f, 0.0f, W, H, veil);

    const float panelW = 430.0f * s;
    const float rowH   = 26.0f * s;
    const float headH  = 46.0f * s;
    const float footH  = 30.0f * s;
    const float panelH = headH + float(kFactionCount) * rowH + footH;
    const float px = 0.5f * (W - panelW);
    const float py = 0.5f * (H - panelH);
    m_ui.DrawRect(px, py, panelW, panelH, panelBg);
    m_ui.DrawRect(px, py, panelW, 2.0f * s, border);
    m_ui.DrawRect(px, py + panelH - 2.0f * s, panelW, 2.0f * s, border);

    m_ui.DrawText(px + 16.0f * s, py + 12.0f * s, "RESPECT", gold);
    char head[64];
    std::snprintf(head, sizeof(head), "OVERALL %+d   MISSIONS %d",
                  m_hudState.overallRespect, m_hudState.completedMissions);
    m_ui.DrawText(px + panelW - 16.0f * s - m_ui.MeasureText(head),
                  py + 12.0f * s, head, white);

    const float nameW = 78.0f * s;
    const float valW  = 40.0f * s;
    const float barW  = panelW - 32.0f * s - nameW - valW;
    const float barH  = 10.0f * s;
    float y = py + headH;
    for (int i = 0; i < kFactionCount; ++i, y += rowH) {
        const HUDState::FactionRow& row = m_hudState.factionRows[i];
        const uint32_t col = row.stance == 0 ? hostile :
                             row.stance == 2 ? friendly : neutral;
        const float x = px + 16.0f * s;
        m_ui.DrawText(x, y + 1.0f * s, row.name, white);
        const float bx = x + nameW;
        m_ui.DrawRect(bx, y + 1.0f * s, barW, barH, trackBg);
        const float half = barW * 0.5f;
        const float fill = (std::min(100, std::max(-100, row.respect)) /
                            100.0f) * half;
        if (fill >= 0.0f) m_ui.DrawRect(bx + half, y + 1.0f * s, fill, barH, col);
        else m_ui.DrawRect(bx + half + fill, y + 1.0f * s, -fill, barH, col);
        m_ui.DrawRect(bx + half - 1.0f, y + 1.0f * s, 2.0f, barH, dim);
        char vb[12];
        std::snprintf(vb, sizeof(vb), "%+d", row.respect);
        m_ui.DrawText(bx + barW + 6.0f * s, y + 1.0f * s, vb, col);
    }
    m_ui.DrawText(px + 16.0f * s, py + panelH - footH + 8.0f * s,
                  "TAB: CLOSE    F: GIFT    J: JOURNAL", dim);
}

// OPENBULLY_FACTION_DEMO=1: scripted headless verification of the Track 42
// faction system. Phases:
//   t 0-8    walk toward the nearest living NPC (position-direct, like the
//            melee demo) and chain light/light/heavy attacks into it.
//            Expected log: RESPECT <faction> -3 -> ... per hit, then the KO
//            bonus line; the HelpCall drives nearby same-faction NPCs to
//            CHASE (witness mechanic).
//   t 8      gift to the nearest NPC (F path): RESPECT +5 line.
//   t 10     open the stats overlay (TAB path) for the OPENBULLY_SHOT
//            capture; per-second census logs every faction score.
//   t 12+    idle - 2-minute stability soak with the census ticking.
void Engine::DriveFactionDemo(float dt) {
    if (!m_jimmyActive || !m_jimmyId.IsValid() || dt <= 0.0f) return;
    Melee::CombatComponent* mc = m_entities.GetMeleeCombat(m_jimmyId);
    PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
    Transform* pt = m_entities.GetTransform(m_jimmyId);
    if (!mc || !pc || !pt) return;

    // Nearest living NPC.
    EntityID best{};
    float bestDist = 1e9f;
    m_entities.ForEachAI([&](EntityID id, const Transform& t,
                             const AIComponent& ai) {
        if (ai.state == AIState::Ko) return;
        const float d = glm::length(t.position - pt->position);
        if (d < bestDist) { bestDist = d; best = id; }
    });

    // Per-second census: one RESPECT-CENSUS line per faction + overall.
    if (m_time - m_factionCensusLast >= 1.0) {
        m_factionCensusLast = m_time;
        for (int i = 0; i < kFactionCount; ++i) {
            const Faction f = static_cast<Faction>(i);
            Logger::Info("RESPECT-CENSUS: %-9s %+.0f [%s]",
                         FactionName(f), m_factions.Get(f),
                         FactionStanceName(m_factions.Stance(f)));
        }
        Logger::Info("RESPECT-CENSUS: OVERALL %+.0f (missions %d)",
                     m_factions.Overall(), m_factions.CompletedMissions());
    }

    // Phase: stats overlay opens at t=10 and stays open (screenshot target).
    if (!m_statsOpen && m_time >= 10.0) {
        m_statsOpen = true;
        Logger::Info("Faction demo: stats overlay OPEN");
    }

    // Phase: gift once at t=8 (nearest NPC in range).
    static bool s_giftDone = false;
    if (!s_giftDone && m_time >= 8.0) {
        s_giftDone = true;
        TryGiftToNpc();
    }

    if (!best.IsValid() || m_time >= 8.0) return; // fight phase only t<8
    const Transform* nt = m_entities.GetTransform(best);
    if (!nt) return;

    // Walk toward the target (position-direct; shell-plane guard included).
    glm::vec3 pos = pt->position;
    PhysicsComponent* pphys = m_entities.GetPhysics(m_jimmyId);
    if (pphys && pos.y < nt->position.y - 1.0f) {
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

    // Snap the follow camera behind Jimmy (screenshots need the fight).
    {
        const float cy = std::cos(pc->facingYaw), sy = std::sin(pc->facingYaw);
        const glm::vec3 focus = pos + glm::vec3(0.0f, 1.2f, 0.0f);
        m_camera.SetPosition(focus - glm::vec3(sy, 0.0f, cy) * 3.2f +
                             glm::vec3(0.0f, 1.4f, 0.0f));
        m_camera.SetLookAt(focus - m_camera.GetPosition());
    }

    // Light -> light -> heavy chain while in range.
    if (dist < 1.4f && mc->state == Melee::CombatState::Idle &&
        m_time - m_meleeDemoLastAtk > 1.4) {
        m_meleeDemoLastAtk = m_time;
        switch (m_meleeDemoStep++ % 3) {
        case 0: StartMeleeAttack(Melee::AttackType::LightPunch); break;
        case 1: StartMeleeAttack(Melee::AttackType::LightPunch); break;
        case 2: StartMeleeAttack(Melee::AttackType::HeavyPunch); break;
        }
        if (dist > 1e-3f) mc->attackYaw = std::atan2(to.x, to.y);
    }
}

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

    // Track 42: mission completion -> faction respect reward (per-mission
    // JSON "respect_reward"); also journals the standing change.
    m_missions.SetOnComplete([this](const Mission& m) { ApplyMissionReward(&m); });

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

    // Track 34: weather system — initialize state machine + particle buffers.
    WeatherManager::Get().Init();
    m_renderer->InitParticleSystems();
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

    // Phase 10 integration: OPENBULLY_WEATHER_START=<CLEAR|RAIN|SNOW|FOG>
    // overrides the initial weather state (verification hook, mirrors
    // OPENBULLY_TIME_START). Applied after Init so particles/fog reset the
    // same way as a manual W cycle.
    if (const char* ws = std::getenv("OPENBULLY_WEATHER_START")) {
        WeatherType wt = WeatherType::CLEAR;
        bool known = true;
        if      (!std::strcmp(ws, "CLEAR")) wt = WeatherType::CLEAR;
        else if (!std::strcmp(ws, "RAIN"))  wt = WeatherType::RAIN;
        else if (!std::strcmp(ws, "SNOW"))  wt = WeatherType::SNOW;
        else if (!std::strcmp(ws, "FOG"))   wt = WeatherType::FOG;
        else known = false;
        if (known) {
            WeatherManager::Get().SetState(wt);
            Logger::Info("Weather: start override %s", ws);
        } else {
            Logger::Warning("Weather: OPENBULLY_WEATHER_START='%s' is not CLEAR|RAIN|SNOW|FOG, ignored", ws);
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
        m_lastGroundY = jimmySpawn.y; // Track 49: void-guard reference
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

    // Track 49: the boss. Spawns 7u from Jimmy so the approach triggers the
    // engagement; OPENBULLY_BOSS_TYPE picks the roster slot (0 Gary,
    // 1 Russell [default], 2 Johnny, 3 Derby, 4 Ted).
    if (m_jimmyActive && !m_bossSpawned) {
        BossType btype = BossType::Russell;
        bool spawnIt = true;
        if (const char* env = std::getenv("OPENBULLY_BOSS_TYPE")) {
            // "none"/"off" disables the boss (crash bisect + crowd-only runs).
            if (std::strcmp(env, "none") == 0 || std::strcmp(env, "off") == 0) {
                spawnIt = false;
            } else {
                const int idx = std::atoi(env);
                if (idx >= 0 && idx < int(BossType::Count)) btype = BossType(idx);
            }
        }
        m_bossSpawned = spawnIt &&
            SpawnBoss(btype, m_jimmySpawn + glm::vec3(7.0f, 0.0f, 0.0f));
    }

    // Track 26: weapon models (bat/garbage lid/yardstick/slingshot from
    // World.img) + ground pickups around Jimmy so E-collect/1-5 equip are
    // immediately exercisable.
    if (!LoadWeaponModels()) {
        Logger::Warning("Weapons: no models loaded (weapon system disabled)");
    }
    SpawnWeaponPickups(m_jimmySpawn);
    // Track 39: ammo boxes in the world (walk over to reload the slingshot).
    SpawnAmmoBoxes(m_jimmySpawn);
    // Track 47: photo system — load the album + spawn the camera pickup.
    InitPhotoSystem();

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

    // Track 48: cutscene playback (same JSON probing convention; needs the
    // Jimmy spawn + anchor refs, so it runs after the character setup).
    InitCutscenes();

    // Track 41: class system (schedule + location + rewards; JSON probing
    // convention shared with missions/dialogue).
    InitClasses();

    // Track 44: minigame framework — persist per-type progress next to the
    // save slot and bind the class result path (rewards + journal) once.
    m_minigames.SetOnResult([this](const Minigames::MinigameResult& r) {
        OnMinigameResult(r);
    });
    m_minigames.LoadProgress(SDL_GetBasePath());

    // Track 31: vehicles — load 4 drivable models + spawn one of each near
    // Jimmy (E mounts within 2.5u).
    if (LoadVehicleModels())
        SpawnVehicles(m_jimmyActive ? m_jimmySpawn : m_camera.GetPosition());

    // Track 45: arcade cabinets — Nut Shots + Monkey Fling in the same
    // ground-verified ring (E plays within 2.2u, $1 per play).
    SpawnArcadeCabinets();

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

    // Track 43: materialize deferred cop spawns/despawns FIRST (before any
    // entity/rig reference exists this frame - see ServiceCopRequests).
    ServiceCopRequests();

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
    // Track 40: while the quest journal is open, ESC closes it instead of
    // quitting. The quit latch is cleared here (before the engine latches it)
    // so it doesn't carry into the next frame. The cursor belongs to the
    // journal too: its relative deltas are zeroed so the follow camera holds.
    if (m_journalOpen && m_input.QuitRequested()) {
        m_journalOpen = false;
        m_journalScrollPx = 0.0f;
        m_journalLmbDown = false;
        m_input.ClearQuit();
        SDL_SetRelativeMouseMode(SDL_TRUE);
        SDL_ShowCursor(SDL_DISABLE);
        Logger::Info("Journal: closed (ESC)");
    }
    // Track 41: the class schedule overlay and the class session swallow ESC
    // the same way (schedule closes; session aborts without pass/fail).
    if (!m_journalOpen && m_scheduleOpen && m_input.QuitRequested()) {
        m_scheduleOpen = false;
        m_input.ClearQuit();
        Logger::Info("Schedule: closed (ESC)");
    }
    // Track 44: an active minigame owns ESC (forfeit / continue); the quit
    // latch is cleared so main.cpp cannot exit, and the same edge reaches
    // the manager through the raw snapshot UpdateMinigames captures.
    if (!m_journalOpen && !m_scheduleOpen && !m_classSessionActive &&
        m_minigames.IsActive() && m_input.QuitRequested()) {
        m_input.ClearQuit();
    }
    // Track 45: the arcade cabinet view is modal too — ESC exits the game
    // (no refund) instead of quitting. UpdateActive runs BEFORE the button/
    // mouse strip-freeze below so this frame's click edge + deltas reach the
    // mini-game; the world then holds still like the other overlays.
    if (!m_journalOpen && !m_scheduleOpen && !m_classSessionActive &&
        !m_minigames.IsActive() && m_arcade.IsActive() &&
        m_input.QuitRequested()) {
        m_input.ClearQuit();
        m_arcade.ExitGame();
    }
    if (m_arcade.IsActive()) {
        int arcadeMoney = 0;
        if (PlayerController* apc = m_entities.GetPlayer(m_jimmyId))
            arcadeMoney = apc->money;
        m_arcade.UpdateActive(m_input, float(deltaTime), arcadeMoney,
                              m_input.IsPressed(Button::Attack));
        if (PlayerController* apc = m_entities.GetPlayer(m_jimmyId))
            apc->money = arcadeMoney; // coin/prize bookkeeping lands here
    }
    // Track 44: the minigame overlay is modal like the journal/schedule:
    // no camera look, all gameplay buttons held off while it is open.
    // Track 45: the arcade view freezes the world the same way.
    const bool anyModalOpen = m_journalOpen || m_scheduleOpen ||
                              m_classSessionActive || m_minigames.IsActive() ||
                              m_arcade.IsActive();
    if (anyModalOpen)
        m_input.SetMouseDelta(0, 0);
    // Track 40: modal journal — hold the world still for the player. NPC AI
    // and schedules keep ticking (Bully's journal is a full pause; that
    // polish lands with a real pause system), but Jimmy drops all inputs so
    // he does not keep walking into walls while the player reads.
    // Track 41: schedule overlay + class session freeze Jimmy the same way.
    if (anyModalOpen && m_jimmyActive) {
        for (int b = 0; b < int(Button::Count); ++b)
            m_input.SetButton(Button(b), false);
    }
    // Track 48: during a cutscene ESC skips the scene instead of quitting —
    // consumed here, before the quit latch, exactly like the modal overlays
    // above (the button/mouse freeze itself happens in UpdateCutscenes so a
    // scripted demo can never re-inject input after it).
    if (m_cutscenePlaying && m_input.QuitRequested()) {
        m_input.ClearQuit();
        Logger::Info("Cutscenes: skip requested (ESC)");
        m_cutscenes.Stop(); // fires OnCutsceneEnded(true)
    }
    if (m_input.QuitRequested()) m_quitRequested = true;

    // Track 44: scripted minigame verification (OPENBULLY_MINIGAME_DEMO).
    // Runs before the key capture inside UpdateMinigames so injected presses
    // flow through the exact same snapshot/strip path as a physical keyboard.
    static const bool s_minigameDemo =
        std::getenv("OPENBULLY_MINIGAME_DEMO") != nullptr;
    if (s_minigameDemo)
        m_minigames.DriveDemo(m_time, &m_input, SDL_GetBasePath());

    // Track 44: minigame tick — capture the isolated key set, strip it from
    // the shared snapshot, then advance the phase machine. Must run before
    // every gameplay input consumer below.
    UpdateMinigames(float(deltaTime));

    // OPENBULLY_SAVE_DEMO=1: scripted save/load for headless verification.
    // Saves at t=5s, loads at t=15s — exercises the same F5/F9 paths.
    // Latched one-shots (not time windows): a frame hitch can skip a 0.1s
    // window entirely, so the demo must fire on the first frame past t.
    static const bool s_saveDemo = std::getenv("OPENBULLY_SAVE_DEMO") != nullptr;
    static const bool s_loadDemo = std::getenv("OPENBULLY_LOAD_DEMO") != nullptr;
    if (s_saveDemo) {
        static bool s_saved = false;
        static bool s_loaded = false;
        if (!s_saved && m_time >= 5.0) {
            s_saved = true;
            SaveGame(0);
            Logger::Info("Save demo: scripted F5 save at t=%.1f", m_time);
        }
        if (!s_loaded && m_time >= 15.0) {
            s_loaded = true;
            LoadGame(0);
            Logger::Info("Save demo: scripted F9 load at t=%.1f", m_time);
        }
    }
    if (s_loadDemo) {
        // Load-only path: no prior save in this run -> exercises the
        // missing-file error branch (must log, must not crash).
        static bool s_loadedOnly = false;
        if (!s_loadedOnly && m_time >= 5.0) {
            s_loadedOnly = true;
            LoadGame(0);
            Logger::Info("Load demo: scripted F9 load-only at t=%.1f", m_time);
        }
    }

    // Track 42: TAB toggles the faction stats overlay; F gifts the nearest
    // NPC (small respect boost with its faction). Edge-triggered keys.
    HandleFactionKeys();
    static const bool s_factionDemo = std::getenv("OPENBULLY_FACTION_DEMO") != nullptr;

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
    if (s_factionDemo && m_jimmyActive) {
        // Track 42: scripted faction verification (beats a faction member
        // down, exercises gifts + save/load, then opens the stats overlay).
        DriveFactionDemo(float(deltaTime));
    } else if (s_missionDemo && m_jimmyActive) {
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

    // Track 49: scripted boss-fight drive (before UpdateJimmy consumes input,
    // same injection discipline as the other demos).
    static const bool s_bossDemo = std::getenv("OPENBULLY_BOSS_DEMO") != nullptr;
    if (s_bossDemo && m_jimmyActive) DriveBossDemo(float(deltaTime));

    // Track 43: scripted wanted-system drive (position/state-direct, must run
    // before UpdateWanted consumes the state this frame).
    static const bool s_wantedDemo = std::getenv("OPENBULLY_WANTED_DEMO") != nullptr;
    if (s_wantedDemo && m_jimmyActive) DriveWantedDemo(float(deltaTime));

    // Track 48: cutscene tick — Space/ESC skip, gameplay input freeze, then
    // the timeline advance (fires due actions through ExecuteCutsceneAction
    // and OnCutsceneEnded at the end). Runs after the scripted demo hooks so
    // a cutscene always wins the input race, and before the world updates so
    // this frame's actions apply coherently.
    UpdateCutscenes(float(deltaTime));

    m_camera.Update(float(deltaTime), m_input, /*ignoreMove=*/m_jimmyActive);

    // Track 14: entity update (physics integration on gravity carriers).
    UpdateEntities(float(deltaTime));

    // Track 15: Jimmy combat state, then input/physics, then the camera.
    UpdateJimmyCombat(float(deltaTime));
    UpdateJimmy(float(deltaTime));
    // Track 16: NPC AI (after physics so ground/wall state is settled).
    UpdateNpcs(float(deltaTime));
    // Track 49: boss bookkeeping outside the boss entity's own AI (zones,
    // banner, stale handles, player-death retry).
    UpdateBosses(float(deltaTime));

    // Track 48: scripted actor positions win over the AI (last writer), with
    // the tween speed fed back into the physics state for the walk cycle.
    ApplyCutscenePoses(float(deltaTime));

    // Track 29: schedule debug keys (T = +1 hour) + spawn/despawn/behavior
    // ticks driven by the game clock.
    HandleScheduleKeys();
    UpdateSchedules();

    // Track 41: class system — C toggle, prompt detection, session minigame.
    // Runs before UpdateDialogue so the deferred E->attend keeps dialogue
    // priority within the same frame (UpdateClassAttend, below).
    UpdateClasses(float(deltaTime));

    // Track 30: dialogue input + typewriter. Runs before the combat/weapon
    // updates so their input handlers see the conversation-active gate the
    // same frame (E starts/advances the talk instead of attacking/pickup).
    UpdateDialogue(float(deltaTime));

    // Track 41: deferred attend — E was pressed on a class prompt; start the
    // session unless the dialogue started instead (same-frame E arbitration).
    UpdateClassAttend();

    // Track 45: arcade cabinets — near-prompt + E entry (dialogue and class
    // attend keep first claim on the E press; this runs after them).
    UpdateArcade(float(deltaTime));

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
    // Track 39: slingshot aim mode (RMB) + manual reload (R) must run before
    // the combat input consumes LMB so the same frame's aim state is used.
    UpdateAimMode(float(deltaTime));
    UpdateAmmoPickups(float(deltaTime));
    UpdateProjectiles(float(deltaTime));

    // Track 31: vehicles — E toggle, mounted driving, then the camera (the
    // vehicle follow replaces UpdateJimmyCamera while mounted). Track 48: a
    // playing cutscene owns the camera outright (keyframe interpolation),
    // overriding both the follow and the vehicle cameras.
    UpdateVehicles(float(deltaTime));
    if (m_cutscenePlaying)
        UpdateCutsceneCamera(float(deltaTime));
    else if (m_mountedVehicle.IsValid())
        UpdateVehicleCamera(float(deltaTime));
    else if (!UpdateBossCutsceneCamera(float(deltaTime))) // Track 49: victory orbit fallback
        UpdateJimmyCamera(float(deltaTime));

    // Track 47: photo mode. Input gating + shutter live in UpdatePhotoMode,
    // which runs after the camera update so the view/projection cache matches
    // the frame Render will submit. OPENBULLY_PHOTO_DEMO drives everything.
    static const bool s_photoDemo = std::getenv("OPENBULLY_PHOTO_DEMO") != nullptr;
    if (s_photoDemo && m_jimmyActive) DrivePhotoDemo(float(deltaTime));
    UpdatePhotoMode(float(deltaTime));

    // Track 37: M toggles minimap visibility.
    if (m_input.WasKeyPressed(SDL_SCANCODE_M))
        m_hudState.minimapVisible = !m_hudState.minimapVisible;

    // Track 43: wanted system - hide/wardrobe inputs, then the per-frame
    // driver (curfew/trespass crimes, escape decay, authority response, bust
    // cut). Runs after the NPC update so authority positions are current and
    // before the HUD snapshot so the flash/fade draw this frame.
    if (m_jimmyActive && m_jimmyId.IsValid() && m_wanted.bustPhase == 0 &&
        !m_journalOpen) {
        if (m_input.WasKeyPressed(SDL_SCANCODE_B)) {
            m_wanted.hidden = !m_wanted.hidden;
            m_wanted.hideTimer = 0.0f;
            m_wanted.safetyTimer = 0.0f;
            Logger::Info("WANTED: %s", m_wanted.hidden
                ? "hidden (heat decays 1 head / 10 s, cannot be spotted)"
                : "no longer hidden");
        }
        if (m_input.WasKeyPressed(SDL_SCANCODE_V)) {
            if (m_wanted.hidden) {
                Logger::Info("WANTED: cannot change clothes while hidden");
            } else if (AnyAuthorityChasing()) {
                Logger::Info("WANTED: cannot change clothes - being chased!");
            } else {
                if (PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
                    const int before = pc->wanted;
                    pc->wanted = 0;
                    Logger::Info("WANTED: wardrobe reset %d -> 0", before);
                }
            }
        }
    }
    UpdateWanted(float(deltaTime));

    // Track 20 (UI): H/J/K debug keys mutate vitals, then refresh the HUD
    // snapshot the renderer draws from (called again in Render so pauses
    // still present an accurate overlay).
    HandleHudDebugKeys();
    // Track 36: F5 save / F9 load.
    if (m_input.WasKeyPressed(SDL_SCANCODE_F5))
        SaveGame(0);
    if (m_input.WasKeyPressed(SDL_SCANCODE_F9))
        LoadGame(0);
    UpdateHUD(float(deltaTime));

    // Track 20: mission system — fire trigger zones and check objective
    // progress from Jimmy's position every frame. dt feeds the Track 43
    // AVOID_BUST survival timer.
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        if (const Transform* jt = m_entities.GetTransform(m_jimmyId))
            m_missions.Update(jt->position, float(deltaTime));
    }

    // Track 48: mission -> cutscene trigger check (plays authored
    // auto_mission cutscenes when their mission activates) + the scripted
    // OPENBULLY_CUTSCENE_DEMO loop.
    UpdateCutsceneAutoTriggers();

    // Track 40: quest journal — input + snapshot rebuild + transition log.
    UpdateQuestJournal();

    // Track 21: audio listener follows the camera; M toggles mute. IsPressed
    // fires on the down-transition frame only, so holding M doesn't flap.
    if (m_audio) {
        if (m_input.IsPressed(Button::Mute))
            m_audio->ToggleMute();
        m_audio->SetListener(m_camera.GetPosition(), m_camera.GetYaw());
    }

    // Track 34: weather system update — state machine, particles, fog.
    // Also handles W key for manual weather cycling and audio transitions.
    {
        WeatherManager& weather = WeatherManager::Get();
        const glm::vec3 camPos = m_camera.GetPosition();
        const glm::mat4 view = m_camera.GetViewMatrix();
        weather.Update(float(deltaTime), camPos, view);

        // Manual weather cycling: W key.
        if (m_input.WasKeyPressed(SDL_SCANCODE_W)) {
            weather.CycleWeatherManual();
        }

        // Audio transitions: start/stop rain loop on weather change.
        const WeatherType curWeather = weather.GetType();
        if (curWeather != m_prevWeather) {
            if (m_prevWeather == WeatherType::RAIN && m_audio) {
                m_audio->StopSound(SoundId::RainLoop);
            }
            if (curWeather == WeatherType::RAIN && m_audio) {
                m_audio->PlaySound(SoundId::RainLoop, 0.4f, nullptr, /*loop=*/true);
                Logger::Info("Weather: rain loop started");
            }
            m_prevWeather = curWeather;
        }
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
    if (m_arcade.IsActive()) return; // Track 45: the cabinet view owns input
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

// =========================================================================
// Track 48: cutscene playback
// =========================================================================

// Loads data/cutscenes/demo_cutscene.json (camera keyframes + actor actions
// + subtitle lines; positions resolve against the same reference-point map as
// the mission loader) and wires the timeline callbacks the per-frame playback
// consumes. Non-fatal: without the file the game runs cutscene-free.
bool Engine::InitCutscenes() {
    // Reference points for the JSON loader (mission-loader convention).
    std::unordered_map<std::string, glm::vec3> refs;
    refs["spawn"] = m_jimmySpawn;
    if (!m_worldMeshes.empty()) {
        const WorldMesh* anchor = &m_worldMeshes.front();
        for (const WorldMesh& wm : m_worldMeshes)
            if (wm.gpu.vertexCount > anchor->gpu.vertexCount) anchor = &wm;
        refs["anchor_center"] = 0.5f * (glm::make_vec3(anchor->boundsMin) +
                                         glm::make_vec3(anchor->boundsMax));
    }

    const char* paths[] = {
        "data/cutscenes/demo_cutscene.json",
        "../data/cutscenes/demo_cutscene.json",
    };
    bool loaded = false;
    for (const char* p : paths) {
        if (std::ifstream f(p); f.good()) {
            f.close();
            loaded = m_cutscenes.LoadFile(p, refs);
            break;
        }
    }
    if (!loaded) {
        Logger::Warning("Cutscenes: demo_cutscene.json not found or empty "
                        "(cutscene system idle)");
        return false;
    }
    m_cutscenes.SetOnAction([this](const CutsceneAction& a) {
        ExecuteCutsceneAction(a);
    });
    m_cutscenes.SetOnEnd([this](bool skipped) { OnCutsceneEnded(skipped); });
    return true;
}

// Starts a cutscene: resolves the actor bindings ("jimmy" -> the player
// entity, "npc" -> the nearest AI entity to the authored near-point), seeds
// the timeline with their current positions, and snapshots nothing else — the
// gameplay camera simply resumes (lerp) when playback ends.
void Engine::StartCutscene(const Cutscene& cs) {
    if (m_cutscenePlaying) return;
    if (m_mountedVehicle.IsValid()) {
        Logger::Warning("Cutscenes: '%s' skipped while mounted", cs.id.c_str());
        return;
    }
    if (m_journalOpen) { // a cutscene must not fight a modal overlay
        Logger::Warning("Cutscenes: '%s' skipped while the journal is open",
                        cs.id.c_str());
        return;
    }

    m_cutsceneActors.clear();
    std::unordered_map<std::string, glm::vec3> actorStart;
    for (const CutsceneActor& a : cs.actors) {
        CutsceneActorBinding b;
        b.name = a.name;
        if (a.kind == "jimmy") {
            if (m_jimmyActive && m_jimmyId.IsValid()) b.id = m_jimmyId;
        } else {
            b.id = FindNearestNpc(a.npcNear, a.npcRadius);
        }
        if (b.id.IsValid()) {
            if (const Transform* t = m_entities.GetTransform(b.id))
                b.startPos = t->position;
            actorStart[b.name] = b.startPos;
            Logger::Info("Cutscenes: actor '%s' -> entity %u", b.name.c_str(),
                         b.id.index);
        } else {
            Logger::Warning("Cutscenes: actor '%s' unresolved (actions skipped)",
                            b.name.c_str());
        }
        m_cutsceneActors.push_back(b);
    }

    m_cutscenePlaying = true;
    m_cutsceneId = cs.id;
    m_cutscenes.Play(&cs, actorStart);
    Logger::Info("Cutscenes: '%s' playing (%.2fs, %zu action(s), %zu line(s))",
                 cs.id.c_str(), cs.duration, cs.actions.size(), cs.lines.size());
}

// Timeline event: TELEPORT positions were already applied by the manager
// (ActorPosNow), FACE turns the actor toward the target point, ANIMATE plays
// a clip (free-form names are looked up on the rig; walk/idle stay under the
// normal switcher), MOVE_TO needs no event handling (per-frame tween).
void Engine::ExecuteCutsceneAction(const CutsceneAction& a) {
    const CutsceneActorBinding* b = nullptr;
    for (const CutsceneActorBinding& x : m_cutsceneActors)
        if (x.name == a.actor) { b = &x; break; }
    if (!b || !b->id.IsValid() || !m_entities.IsAlive(b->id)) return;
    Transform* t = m_entities.GetTransform(b->id);
    if (!t) return;

    switch (a.type) {
        case CutsceneActionType::Teleport:
            Logger::Info("Cutscenes: TELEPORT '%s' -> (%.1f, %.1f, %.1f)",
                         a.actor.c_str(), a.target.x, a.target.y, a.target.z);
            break;
        case CutsceneActionType::Face: {
            const glm::vec3 to = a.target - t->position;
            if (glm::length(glm::vec2(to.x, to.z)) > 1e-4f) {
                const float yaw = std::atan2(to.x, to.z);
                t->rotation.y = yaw;
                if (PlayerController* pc = m_entities.GetPlayer(b->id)) {
                    pc->facingYaw   = yaw;
                    pc->facingValid = true;
                }
                Logger::Info("Cutscenes: FACE '%s' (yaw %.2f)",
                             a.actor.c_str(), yaw);
            }
            break;
        }
        case CutsceneActionType::Animate: {
            AnimationComponent* ac = m_entities.GetAnimation(b->id);
            if (!ac || ac->rigIndex == 0xFFFFFFFF ||
                ac->rigIndex >= m_animRigs.size()) break;
            AnimRig& rig = m_animRigs[ac->rigIndex];
            int clip = -1;
            bool loop = true;
            if (a.anim == "walk" && rig.walkClip >= 0) {
                clip = rig.walkClip;
            } else if (a.anim == "idle" && rig.idleClip >= 0) {
                clip = rig.idleClip;
            } else if ((clip = rig.controller.FindClip(a.anim)) >= 0) {
                loop = a.duration <= 0.0f; // one-shot when a window is authored
            }
            if (clip >= 0) {
                rig.controller.Play(clip, loop, 0.2f);
                ac->playing = true;
                Logger::Info("Cutscenes: ANIMATE '%s' -> '%s' (clip %d)",
                             a.actor.c_str(), a.anim.c_str(), clip);
            } else {
                Logger::Warning("Cutscenes: ANIMATE '%s': no clip '%s' on this rig",
                                a.actor.c_str(), a.anim.c_str());
            }
            break;
        }
        case CutsceneActionType::MoveTo:
            break; // per-frame tween (ApplyCutscenePoses)
    }
}

// Per-frame cutscene tick: skip handling (Space / ESC), the gameplay input
// freeze, then the timeline advance (fires due actions through
// ExecuteCutsceneAction and OnCutsceneEnded at the end). Runs AFTER the
// scripted demo hooks so a cutscene always wins the input race, and BEFORE
// the world updates so this frame's actions are applied coherently.
void Engine::UpdateCutscenes(float dt) {
    if (!m_cutscenePlaying) return;

    // OPENBULLY_CUTSCENE_SKIP_DEMO=1: scripted skip 5 s into playback. The
    // OS-level key injection cannot reach a headless window (known limitation
    // noted on the other *_DEMO hooks), so the demo drives the same
    // SetButton path a physical Space press produces and the check below
    // consumes it as the skip edge.
    static const bool s_skipDemo = std::getenv("OPENBULLY_CUTSCENE_SKIP_DEMO") != nullptr;
    static bool s_skipDemoFired = false;
    if (s_skipDemo && !s_skipDemoFired && m_time >= 5.0) {
        s_skipDemoFired = true;
        m_input.SetButton(Button::Up, true); // Space edge -> skip below
    }

    // Skip: Space (button edge or raw key edge). ESC is handled in Update()
    // before the quit latch (it must not also quit the game).
    if (m_input.IsPressed(Button::Up) ||
        m_input.WasKeyPressed(SDL_SCANCODE_SPACE)) {
        Logger::Info("Cutscenes: skip requested (Space)");
        m_cutscenes.Stop(); // fires OnCutsceneEnded(true)
        return;
    }

    // Freeze every gameplay button + mouse look (same model as the modal
    // journal). The raw scancode handlers (journal J, save F5, weather W)
    // stay live — debug affordances, harmless during playback.
    for (int b = 0; b < int(Button::Count); ++b)
        m_input.SetButton(Button(b), false);
    m_input.SetMouseDelta(0, 0);

    m_cutscenes.Update(dt); // timeline: actions + end callback
}

// Applies the scripted actor positions after the AI/NPC updates (last writer
// wins), reflects the tween speed into the physics state (the walk cycle
// speed-matches on HorizontalVelocity, and a parked vertical velocity keeps
// gravity from fighting the scripted Y), and faces moving actors along their
// travel. The walk/idle switch itself runs inside UpdateEntityAnimation via
// CutsceneActorMoving.
void Engine::ApplyCutscenePoses(float dt) {
    if (!m_cutscenePlaying) return;
    for (CutsceneActorBinding& b : m_cutsceneActors) {
        if (!b.id.IsValid() || !m_entities.IsAlive(b.id)) continue;
        Transform* t = m_entities.GetTransform(b.id);
        if (!t) continue;
        glm::vec3 target;
        if (!m_cutscenes.ActorPosNow(b.name, target)) continue;
        const glm::vec3 delta = target - t->position;
        const bool movingNow = CutsceneActorMoving(b.id);

        t->position = target;
        if (PhysicsComponent* p = m_entities.GetPhysics(b.id)) {
            // Feed the tween speed back so the walk cycle rates correctly.
            p->velocity.x = (movingNow && dt > 0.0f) ? delta.x / dt : 0.0f;
            p->velocity.z = (movingNow && dt > 0.0f) ? delta.z / dt : 0.0f;
            p->velocity.y = 0.0f;
            p->onGround   = true;
        }
        if (movingNow && glm::length(glm::vec2(delta.x, delta.z)) > 1e-5f) {
            const float yaw = std::atan2(delta.x, delta.z);
            t->rotation.y = yaw;
            if (PlayerController* pc = m_entities.GetPlayer(b.id)) {
                pc->facingYaw   = yaw;
                pc->facingValid = true;
            }
        }
    }
}

// Cutscene end (fires exactly once via CutsceneManager::SetOnEnd, for both
// the natural end and a Space/ESC skip): releases the actor bindings and
// activates the authored on_end_mission (cutscene -> mission-state hook —
// e.g. "spawn the next beat" by starting the chained mission).
void Engine::OnCutsceneEnded(bool skipped) {
    if (!m_cutscenePlaying) return;
    m_cutscenePlaying = false;
    const std::string id = m_cutsceneId;
    m_cutsceneId.clear();
    m_cutsceneActors.clear();
    Logger::Info("Cutscenes: '%s' ended (%s)", id.c_str(),
                 skipped ? "skipped" : "finished");
    // OPENBULLY_CUTSCENE_DEMO re-arm: next scripted play 4 s after the end.
    static const bool s_demo = std::getenv("OPENBULLY_CUTSCENE_DEMO") != nullptr;
    if (s_demo) m_cutsceneDemoNextPlay = m_time + 4.0;

    if (const Cutscene* cs = m_cutscenes.Find(id);
        cs && !cs->onEndMission.empty()) {
        for (uint32_t i = 0; i < m_missions.MissionCount(); ++i) {
            const Mission* m = m_missions.GetMission(i);
            if (m && m->id == cs->onEndMission) {
                if (m_missions.ActivateMission(i))
                    Logger::Info("Cutscenes: '%s' activated mission '%s'",
                                 id.c_str(), cs->onEndMission.c_str());
                break;
            }
        }
    }
}

// Scripted camera override: evaluates the keyframe interpolation (smoothstep
// eases; cut keys hold the previous pose then pop), resolves the position
// against level geometry like the follow cam does, and aims at the
// face-actor's live head position when the active key requests tracking.
void Engine::UpdateCutsceneCamera(float dt) {
    (void)dt;
    if (!m_cutscenePlaying) return;
    const CutsceneManager::CameraPose pose = m_cutscenes.EvalCamera();

    glm::vec3 pos = pose.position;
    if (m_collisionWorld) {
        const float in[3] = { pos.x, pos.y, pos.z };
        float out[3];
        if (m_collisionWorld->ResolveSphere(in, 0.35f, out))
            pos = glm::vec3(out[0], out[1], out[2]);
    }

    glm::vec3 lookAt = pose.lookAt;
    if (!pose.faceActor.empty()) {
        for (const CutsceneActorBinding& b : m_cutsceneActors) {
            if (b.name == pose.faceActor && b.id.IsValid() &&
                m_entities.IsAlive(b.id)) {
                if (const Transform* t = m_entities.GetTransform(b.id))
                    lookAt = t->position + glm::vec3(0.0f, 1.2f, 0.0f);
                break;
            }
        }
    }

    m_camera.SetPosition(pos);
    if (glm::length(lookAt - pos) > 1e-4f)
        m_camera.SetLookAt(lookAt - pos);
}

// Mission -> cutscene hook: plays the first unfired cutscene whose
// auto_mission id matches a mission that is currently Active (trigger zones
// and chains both count as activation). Called after MissionManager::Update
// so a zone fired this frame plays its cutscene this frame.
void Engine::UpdateCutsceneAutoTriggers() {
    // Track 46: OPENBULLY_NO_CUTSCENE=1 disables auto-triggers. The trick
    // demo runs script the player (vehicle-demo mount walk); a 13 s intro
    // cutscene freezes him mid-walk and the script never mounts.
    static const bool s_noCutscene =
        std::getenv("OPENBULLY_NO_CUTSCENE") != nullptr;
    if (s_noCutscene) return;
    for (size_t i = 0; i < m_cutscenes.Count(); ++i) {
        const Cutscene* cs = m_cutscenes.At(i);
        if (!cs || cs->autoMission.empty()) continue;
        const auto it = m_cutsceneAutoFired.find(cs->id);
        if (it != m_cutsceneAutoFired.end() && it->second) continue;
        const Mission* m = m_missions.FindMission(cs->autoMission);
        if (!m || m->state != MissionState::Active) continue;
        m_cutsceneAutoFired[cs->id] = true;
        Logger::Info("Cutscenes: auto-trigger '%s' (mission '%s' active)",
                     cs->id.c_str(), cs->autoMission.c_str());
        StartCutscene(*cs);
        return; // one start per frame
    }

    // OPENBULLY_CUTSCENE_DEMO=1: scripted playback for headless verification
    // (screenshot + stability runs): first cutscene at t=1 s, then every
    // registered cutscene in order, 4 s after each ends (re-armed in
    // OnCutsceneEnded).
    static const bool s_demo = std::getenv("OPENBULLY_CUTSCENE_DEMO") != nullptr;
    if (s_demo && m_jimmyActive && !m_cutscenePlaying &&
        m_time >= m_cutsceneDemoNextPlay) {
        static size_t s_index = 0;
        if (const Cutscene* cs = m_cutscenes.At(s_index)) {
            m_cutsceneAutoFired[cs->id] = true;
            StartCutscene(*cs);
        }
        s_index = (s_index + 1) % std::max<size_t>(1, m_cutscenes.Count());
        m_cutsceneDemoNextPlay = m_time + 1e9; // OnCutsceneEnded re-arms
    }
}

// --- Track 48: actor helpers --------------------------------------------------

bool Engine::IsCutsceneActor(EntityID id) const {
    if (!m_cutscenePlaying || !id.IsValid()) return false;
    for (const CutsceneActorBinding& b : m_cutsceneActors)
        if (b.id == id) return true;
    return false;
}

// True while this entity's scripted MOVE_TO tween is in flight — the
// walk/idle switcher in UpdateEntityAnimation uses this instead of the
// frozen input state.
bool Engine::CutsceneActorMoving(EntityID id) const {
    if (!m_cutscenePlaying || !id.IsValid()) return false;
    for (const CutsceneActorBinding& b : m_cutsceneActors)
        if (b.id == id) return m_cutscenes.ActorMoving(b.name);
    return false;
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
        // Track 47: vehicles photograph too (yearbook objects).
        RegisterPhotoTarget(e, VehicleTypeName(vm.type),
                            Photo::TargetKind::Object, std::max(1.0f, vm.height));
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
    if (m_journalOpen) return;   // Track 40: the journal is modal
    if (m_scheduleOpen || m_classSessionActive) return; // Track 41: class UI
    if (m_cutscenePlaying) return; // Track 48: the scene owns the player
    if (m_arcade.IsActive()) return; // Track 45: the cabinet owns the input
    if (m_mountedVehicle.IsValid()) { DismountVehicle(); return; }
    if (m_arcade.NearCabinet()) return; // Track 45: E on the prompt plays
    if (!m_jimmyActive || !m_jimmyId.IsValid()) return;
    if (m_dialogue.IsActive() || m_dialogueNpc.IsValid()) return; // talk takes E
    if (m_classPromptIdx >= 0) return; // Track 41: attend-class takes E
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
    m_mountTime = m_time;   // Track 46: trick-script clock origin

    // Track 46: fresh trick session — the live trick and combo are cleared,
    // but career totals and unlocked achievements carry across mounts.
    m_tricks.ResetSession();

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
    // Track 46 (DEMO=4): the rooftop deck is ~10 u across and the scripted
    // trick orbit regularly rolls off the rim; a -100 floor means a ~4.5 s
    // fall per reset, which starves the trick phases of grounded time. Snap
    // the demo floor to 3 u below the deck so fall -> reset cycles take
    // ~1 s and the phase machine keeps getting fresh grounded windows.
    if (const char* vehDemo = std::getenv("OPENBULLY_VEHICLE_DEMO");
        vehDemo && std::atoi(vehDemo) == 4)
        m_vehicleVoidFloorY = vt->position.y - 3.0f;
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

    // Track 46: true when the dismount is a crash aftermath (eject) rather
    // than a deliberate step-off — decides bank vs bail below.
    const bool foCrashEject = m_vehicleFeel.crashed;

    // Track 46: end the trick session. A crash-eject dismount is a BAIL
    // (combo lost, pending score forfeited); a deliberate dismount BANKS what
    // the chain already earned. Career totals always survive either way.
    if (foCrashEject)
        m_tricks.Bail();
    else if (m_tricks.active != TrickType::None || m_tricks.pending > 0) {
        if (m_tricks.pending > 0)
            Logger::Info("TRICK: dismount banks +%d (total %d)",
                         m_tricks.pending, m_tricks.totalScore + m_tricks.pending);
        m_tricks.BankAndReset();
    } else {
        m_tricks.ResetSession();
    }

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
        // Track 46 (DEMO=4): the gate stays open for the whole run so the
        // scripted crash-eject is followed by an automatic remount and the
        // trick/crash cycle repeats for the full stability window.
        static const int s_demoModeSel =
            std::atoi(std::getenv("OPENBULLY_VEHICLE_DEMO"));
        if (m_time >= 2.0 && !m_mountedVehicle.IsValid() &&
            (m_time < 5.9 || s_demoModeSel == 4)) {
            // Teleport Jimmy to the nearest vehicle (position-direct, like
            // DIALOGUE_DEMO): the spawn ring can sit across a courtyard the
            // walk can't cross. Y snaps to the vehicle's ground. DEMO=3
            // targets the bike specifically — the wheelie script needs one,
            // and the nearest ring vehicle may be the car (wheelies are
            // bike-only in the feel layer).
            if (Transform* jt = m_entities.GetTransform(m_jimmyId)) {
                EntityID veh{};
                if (s_demoModeSel == 3 || s_demoModeSel == 4) {
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
            } else if (s_demoMode == 4) {
                // Track 46 trick chain (DEMO=4). The rooftop deck is only
                // ~10 u across and the closed orbit at cruise speed slides
                // off the rim (the Track 33 script does the same — see its
                // log), so the script is a STATE MACHINE, not wall-clock
                // windows: phases advance on OBSERVED events (speed reached,
                // trick landed, crash) and simply pause while the bike is
                // airborne; the void guard resets it onto the deck and the
                // chain resumes. One full cycle:
                //   P0 roll to 3 u/s
                //   P1 W+S wheelie ~1.0 s
                //   P2 W coast 0.4 s            -> wheelie #1 LANDS (x1)
                //   P3 W+S wheelie ~1.0 s       (starts inside the window)
                //   P4 W coast 0.4 s            -> wheelie #2 LANDS (x2)
                //   P5 S-only stoppie           -> LANDS (x3)
                //   P6 W + Space tap            -> hop LANDS (x4)
                //   P7 W+S wheelie HELD         -> over-rotate -> CRASH
                //                                  (eject + COMBO LOST)
                //   P8 slow cruise (only if P7 timed out)
                // Coast phases exist because identical W+S patterns across a
                // phase boundary never release the pose — the wheelie would
                // just continue and land once as one long trick.
                // The P7 crash ejects the rider; the mount-walk gate below is
                // widened for mode 4 so the demo re-mounts and repeats the
                // whole cycle — continuous trick/crash coverage for the whole
                // stability window.
                static double s_prevMount = -1e9;
                static int    s_phase     = 0;
                static double s_phaseT    = 0.0;
                static int    s_landedSnap[int(TrickType::Count)] = {};
                if (m_mountTime != s_prevMount) {
                    s_prevMount = m_mountTime;
                    s_phase = 0;
                    s_phaseT = 0.0;
                    for (int& v : s_landedSnap) v = -1; // force re-snapshot
                }
                s_phaseT += dt;
                const bool grounded = m_vehicle.wheelContacts > 0;
                auto nextPhase = [&](int p) {
                    s_phase  = p;
                    s_phaseT = 0.0;
                    for (int t = 0; t < int(TrickType::Count); ++t)
                        s_landedSnap[t] = m_tricks.perType[t].landed;
                };
                auto landedSince = [&](TrickType t) {
                    return m_tricks.perType[int(t)].landed >
                           s_landedSnap[int(t)];
                };
                switch (s_phase) {
                    case 0:
                        if (m_vehicle.speedAlongFacing >= 3.0f || s_phaseT > 2.0)
                            nextPhase(1);
                        break;
                    case 1:
                        if (s_phaseT > 1.0) nextPhase(2);
                        break;
                    case 2:
                        if (landedSince(TrickType::Wheelie) || s_phaseT > 0.8)
                            nextPhase(3);
                        break;
                    case 3:
                        if (s_phaseT > 1.0) nextPhase(4);
                        break;
                    case 4:
                        if (landedSince(TrickType::Wheelie) || s_phaseT > 0.8)
                            nextPhase(5);
                        break;
                    case 5:
                        if (landedSince(TrickType::Stoppie) || s_phaseT > 1.2)
                            nextPhase(6);
                        break;
                    case 6:
                        if (landedSince(TrickType::BunnyHop) || s_phaseT > 2.5)
                            nextPhase(7);
                        break;
                    case 7:
                        // Held wheelie: progressive instability over-rotates
                        // at ~3.1 s -> FeelImpact eject -> CRASH log. The
                        // mounted block stops running on eject; the remount
                        // resets s_phase to 0.
                        if (m_vehicleFeel.crashed || s_phaseT > 6.0)
                            nextPhase(8);
                        break;
                    default:
                        break; // P8: cruise forever
                }

                const glm::vec3 center = m_vehicleDemoCenter;
                if (const Transform* jt2 = m_entities.GetTransform(m_mountedVehicle)) {
                    glm::vec3 toVeh = jt2->position - center;
                    toVeh.y = 0.0f;
                    const float dist = glm::length(toVeh);
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
                    // Edge probe: no ground 3 u ahead -> hard right along the
                    // deck rim instead of launching off the drop.
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
                    if (edgeAhead) steer = 1.0f;
                    m_input.SetButton(Button::Left,  steer < -0.15f);
                    m_input.SetButton(Button::Right, steer >  0.15f);

                    // Stunt inputs by phase. Back is ONLY held in the wheelie
                    // phases (P1/P3) and the stoppie phase (P2): a held brake
                    // anywhere else would read as a stoppie. Cruise caps at
                    // 2.8 u/s (releasing throttle, never braking) to keep the
                    // orbit inside the small deck.
                    bool fwd = false, back = false, up = false;
                    switch (s_phase) {
                        case 0: fwd = true; break;
                        case 1: case 3: case 7:
                            fwd = true; back = true; break;
                        case 2: case 4: fwd = true; break;
                        case 5: back = true; break;
                        case 6:
                            fwd = true;
                            // Tap only when the trick slot is idle (the
                            // stoppie's release decay may still be active)
                            // and the bike is rolling fast enough to hop.
                            up = grounded &&
                                 m_tricks.active == TrickType::None &&
                                 std::fabs(m_vehicle.speedAlongFacing) > 1.5f;
                            break;
                        default:
                            fwd = m_vehicle.speedAlongFacing <= 2.8f;
                            break;
                    }
                    m_input.SetButton(Button::Forward, fwd);
                    m_input.SetButton(Button::Back,    back);
                    m_input.SetButton(Button::Up,      up);
                    m_input.SetButton(Button::Sprint,  false);
                }
            }
        }
    }

    if (m_input.WasKeyPressed(SDL_SCANCODE_E) && !m_dialogue.IsActive() &&
        !m_journalOpen && !m_cutscenePlaying) // Track 40/48: modal owners of E
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

    // Track 46: W + S on the bike = wheelie trigger (rear-brake clutch-up).
    // Raw brake fed into the body while throttling would fight the drive
    // (22 u/s^2 brake vs 9 u/s^2 accel -> stall + reverse), so while the
    // bike rolls with both pedals down the brake FORCE is suppressed for the
    // body; the feel layer still sees the raw inputs and lifts the front.
    const bool wsWheelie = vc->type == VehicleType::Bike && throttle > 0.5f &&
                           brake > 0.5f && m_vehicle.speedAlongFacing > 1.0f;
    const float bodyBrake = wsWheelie ? 0.0f : brake;

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
        m_vehicle.Update(kFixedDt, steer, throttle, bodyBrake);
        m_vehicleAccum -= kFixedDt;
        ++steps;
    }

    // Track 46: bunny hop — Space edge while rolling and grounded hops the
    // body upward; the suspension then launches it and the trick scores on
    // the airtime until the wheels re-contact (StepTricks via UpdateTrickSc).
    // Gated on an idle trick slot so the impulse and the TRICK: BUNNY HOP
    // start always agree (a hop tap during an active wheelie is ignored).
    if (m_vehicle.wheelContacts > 0 &&
        std::fabs(m_vehicle.speedAlongFacing) > 1.5f &&
        m_tricks.active == TrickType::None &&
        m_input.IsPressed(Button::Up)) {
        m_vehicle.velocity.y += kBunnyHopImpulse;
        Logger::Info("TRICK: bunny hop impulse (vy=%.1f)",
                     m_vehicle.velocity.y);
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
// feel layer (per-type drive character, wheelie, stoppie, crash/eject) and
// applies the results on top of the Track 32 body: yaw agility delta, pitch,
// crash tilt, and the eject/dismount path. Track 46: this is also where the
// trick scoring machine is driven (poses in, events out).
void Engine::UpdateVehicleFeel(float dt, VehicleComponent* vc, Transform* vt,
                               float throttle, float brake, float steer) {
    FeelState& feel = m_vehicleFeel;
    feel.type       = vc->type;
    feel.inThrottle = throttle > 0.5f;
    feel.inBrake    = brake > 0.5f;
    feel.inLeanL    = steer < -0.5f;
    feel.inLeanR    = steer > 0.5f;
    // Track 46: the wheelie trigger is now W+S (brake-clutch-up) OR the
    // original Shift modifier (kept for backward compatibility with the
    // Track 33 script/demos). Both feed the same feel pose channel.
    const bool wsTrigger = feel.inThrottle && feel.inBrake;
    feel.inWheelie  = m_input.IsDown(Button::Sprint) || wsTrigger;
    feel.inGrounded = m_vehicle.wheelContacts > 0;

    const float preSpeed = m_vehicle.speedAlongFacing;
    FeelOut fo = StepFeel(feel, m_vehicle.position, m_vehicle.yaw, preSpeed, dt);

    // Feel yaw delta: per-type low-speed agility on top of the Track 32
    // steering (small damped rate, so it composes rather than fights it).
    m_vehicle.yaw += fo.yawDelta;
    vc->yaw = m_vehicle.yaw;

    // Wheelie/stoppie pitch overlays the terrain pitch (cosmetic channel).
    // SIGN NOTE: positive rotation.x tips the nose DOWN (glm Rx(+X) on a
    // +Z-forward body — matches Track 32's weight-transfer convention), and
    // the feel layer reports +deg as "front lifted", so the cosmetic value is
    // NEGATED here: a wheelie must render nose-up, a stoppie nose-down.
    if (feel.wheelieActive || feel.stoppieActive)
        m_vehicle.cosmeticPitch = -fo.pitch;
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

    // Track 46: trick scoring — pose + body + raw-input snapshot in, events
    // (logs, popups, stats, achievements) out. Deliberately LAST: the wall
    // impact block above may have promoted a crash (FeelImpact -> fo), and a
    // crash must reach the trick state machine on the same frame it happens
    // so the bail resets the combo in the same tick.
    UpdateTrickScoring(dt, feel, fo);
}

// Track 46: stunt trick scoring. Runs once per mounted frame, right after
// StepFeel. Inputs are read from the feel state (wheelie/stoppie poses), the
// physics body (grounded/airborne), raw keyboard edges (Space = bunny hop)
// and the crash path (fo.crashBegan / over-rotation ejects); StepTricks owns
// the whole state machine (active/duration/combo/pending/bank/stats).
void Engine::UpdateTrickScoring(float dt, const FeelState& feel,
                                const FeelOut& fo) {
    TrickInputs ti;
    ti.mounted  = true;
    ti.wheelie  = feel.type == VehicleType::Bike && feel.wheelieActive;
    ti.stoppie  = feel.type == VehicleType::Bike && feel.stoppieActive;
    ti.grounded = feel.inGrounded;
    ti.airborne = m_vehicle.Airborne();
    // Space edge, gated on rolling forward + grounded (the impulse itself is
    // applied in UpdateMountedVehiclePhysics with the same gate).
    ti.hopStart = ti.grounded &&
                  std::fabs(m_vehicle.speedAlongFacing) > 1.5f &&
                  m_input.IsPressed(Button::Up);
    // Crash edges: wall impact eject, wheelie over-rotation, stoppie loop.
    ti.crash    = fo.crashBegan || fo.ejectRider || fo.stoppieFail;

    TrickEvents te;
    StepTricks(m_tricks, ti, dt, te);

    if (te.started)
        Logger::Info("TRICK: %s started", TrickName(te.startedType));
    if (te.landed) {
        Logger::Info("TRICK: %s landed %.1fs -> +%d (combo x%d, pending %d)",
                     TrickName(te.landedType), te.landedTime, te.landedPoints,
                     te.landedMult, m_tricks.pending);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s +%d", TrickName(te.landedType),
                      te.landedPoints);
        std::snprintf(m_tricks.popupText, sizeof(m_tricks.popupText), "%s",
                      buf);
        m_tricks.popupTimer = 2.0f;
    }
    if (te.banked)
        Logger::Info("TRICK: combo banked +%d (total %d)", te.bankedPoints,
                     m_tricks.totalScore);
    if (te.comboLost)
        Logger::Info("TRICK: COMBO LOST (-%d pending, multiplier reset)",
                     te.lostPoints);
    if (te.achievement >= 0) {
        const TrickAchievement& a = GetTrickAchievements()[te.achievement];
        Logger::Info("ACHIEVEMENT: %s - %s", a.name, a.desc);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "ACHIEVEMENT: %s", a.name);
        std::snprintf(m_tricks.popupText, sizeof(m_tricks.popupText), "%s",
                      buf);
        m_tricks.popupTimer = 3.0f;
    }
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
    if (s_demo && !m_cutscenePlaying) { // Track 48: the scene blocks demo talks
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

    // Track 40: the quest journal is a modal overlay. Keep the typewriter
    // ticking so a conversation isn't left mid-line, but ignore E / arrow /
    // choice input until the journal closes. Track 48: same while a cutscene
    // plays (conversations cannot start or advance mid-scene).
    if (m_journalOpen || m_cutscenePlaying) { m_dialogue.Update(dt); return; }

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
    } else if (!m_journalOpen && !m_classSessionActive && !m_scheduleOpen &&
               m_classPromptIdx < 0 &&
               m_input.WasKeyPressed(SDL_SCANCODE_E)) {
        // Start: E near an NPC when not in combat and no class UI is up.
        // Track 41: a visible class prompt takes E first (UpdateClassAttend
        // starts the session right after this frame's dialogue check).
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
    if (m_cutscenePlaying) return; // Track 48: no grapple input mid-cutscene
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
    // Track 40: J now toggles the quest journal (see UpdateQuestJournal).
    if (m_input.WasKeyPressed(SDL_SCANCODE_K)) {
        pc->wanted = std::max(0, pc->wanted - 1);
        Logger::Info("HUD debug: wanted -> %d", pc->wanted);
    }
}

// --- Track 40: quest journal ---------------------------------------------------

void Engine::AppendJournalLog(const std::string& line) {
    m_journalLog.push_back(line);
    while (m_journalLog.size() > 6)
        m_journalLog.erase(m_journalLog.begin());
}

// Per-frame quest journal: input (J toggle, arrow/wheel scroll, click select),
// rebuild of the UI snapshot from MissionManager, and the rolling event log
// for activation / objective-completion transitions. Drawing happens in Render
// via DrawQuestJournal, which reads the snapshot rebuilt here.
void Engine::UpdateQuestJournal() {
    // OPENBULLY_JOURNAL_DEMO=1: headless verification. Opens the journal at
    // t=0.5s, then activates the demo missions and completes one objective
    // every 1.5s, so a single run exercises activation, objective completion,
    // mission completion, and chaining without OS-level key injection (same
    // *_DEMO philosophy as the other verification hooks).
    static const bool s_journalDemo = std::getenv("OPENBULLY_JOURNAL_DEMO") != nullptr;
    if (s_journalDemo) {
        if (!m_journalOpen && m_time >= 0.5) {
            m_journalOpen = true;
            m_journalScrollPx = 0.0f;
            SDL_SetRelativeMouseMode(SDL_FALSE);
            SDL_ShowCursor(SDL_ENABLE);
            Logger::Info("Journal demo: opened");
        }
        if (m_journalOpen && m_jimmyActive) {
            static double s_lastStep = -1e9;
            if (m_time >= 1.0 && m_time - s_lastStep >= 1.5) {
                s_lastStep = m_time;
                // Activate the first inactive mission, else complete the first
                // active mission's current objective. Chaining does the rest.
                bool activated = false;
                for (size_t i = 0; i < m_missions.MissionCount(); ++i)
                    if (m_missions.ActivateMission(uint32_t(i))) { activated = true; break; }
                if (!activated) {
                    for (size_t i = 0; i < m_missions.MissionCount(); ++i) {
                        const Mission* m = m_missions.GetMission(uint32_t(i));
                        if (!m || m->state != MissionState::Active) continue;
                        if (m->currentObjective >= m->objectives.size()) continue;
                        m_missions.CompleteObjective(uint32_t(i), m->currentObjective);
                        break;
                    }
                }
            }
            // Cycle the selection so the highlight is visibly moving across
            // the recurring screenshots.
            static double s_lastCycle = 0.0;
            if (!m_journalMissions.empty() && m_time - s_lastCycle >= 2.0) {
                s_lastCycle = m_time;
                m_journalSelected = (m_journalSelected + 1) %
                                    int(m_journalMissions.size());
            }
        }
    }

    // --- input ----------------------------------------------------------------
    // Track 46: OPENBULLY_JOURNAL_AT=<sec> opens the journal once at that
    // time. The trick demo uses it to show the STYLE stats panel (drawn while
    // the journal is open) in the late screenshots — OS-level key injection
    // has no effect on SDL raw scancode state, so scripted verification goes
    // through the engine like every other *_DEMO hook.
    static const bool s_journalAt = std::getenv("OPENBULLY_JOURNAL_AT") != nullptr;
    if (s_journalAt && !m_journalOpen) {
        static const double s_journalAtTime =
            std::atof(std::getenv("OPENBULLY_JOURNAL_AT"));
        if (m_time >= s_journalAtTime) {
            m_journalOpen = true;
            m_journalScrollPx = 0.0f;
            SDL_SetRelativeMouseMode(SDL_FALSE);
            SDL_ShowCursor(SDL_ENABLE);
            Logger::Info("Journal demo: opened at t=%.1f (OPENBULLY_JOURNAL_AT)",
                         m_time);
        }
    }
    // Track 41: the schedule overlay and class session are modal — no journal
    // toggling while class UI is up.
    // Track 45: the arcade view is modal too (J inside a cabinet is inert).
    if (m_input.WasKeyPressed(SDL_SCANCODE_J) &&
        !m_scheduleOpen && !m_classSessionActive &&
        !m_cutscenePlaying && // Track 48: the scene owns the player
        !m_arcade.IsActive()) {
        m_journalOpen = !m_journalOpen;
        m_journalScrollPx = 0.0f;
        // The journal takes the cursor: absolute mode lets a click be
        // hit-tested against the mission blocks. Restored on close.
        SDL_SetRelativeMouseMode(m_journalOpen ? SDL_FALSE : SDL_TRUE);
        SDL_ShowCursor(m_journalOpen ? SDL_ENABLE : SDL_DISABLE);
        m_journalLmbDown = false;
        Logger::Info("Journal: %s", m_journalOpen ? "opened" : "closed");
    }
    // Track 47: TAB flips between the missions page and the photo album.
    if (m_journalOpen && m_input.WasKeyPressed(SDL_SCANCODE_TAB)) {
        m_journalPhotoPage = !m_journalPhotoPage;
        m_journalScrollPx = 0.0f;
        Logger::Info("Journal: %s page",
                     m_journalPhotoPage ? "photo album" : "missions");
    }
    if (m_journalOpen) {
        const int count = int(m_journalMissions.size());
        if (count > 0) {
            if (m_input.WasKeyPressed(SDL_SCANCODE_DOWN) ||
                m_input.WasKeyPressed(SDL_SCANCODE_S))
                m_journalSelected = (m_journalSelected + 1) % count;
            if (m_input.WasKeyPressed(SDL_SCANCODE_UP) ||
                m_input.WasKeyPressed(SDL_SCANCODE_W))
                m_journalSelected = (m_journalSelected + count - 1) % count;
        }
        // Mouse-wheel scroll: main.cpp forwards the delta (SDL_PollEvent
        // drains the events before Input::Poll can see them).
        if (m_journalWheelDelta != 0) {
            m_journalScrollPx -= float(m_journalWheelDelta) * 24.0f;
            m_journalWheelDelta = 0;
        }
        // Click-to-select: in absolute mode SDL_GetMouseState gives real
        // window coordinates for the hit-test.
        int mx = 0, my = 0;
        const Uint32 mb = SDL_GetMouseState(&mx, &my);
        const bool lmb = (mb & SDL_BUTTON_LMASK) != 0;
        if (lmb && !m_journalLmbDown) {
            const int hit = m_ui.JournalHitTest(m_journalMissions,
                                                m_journalScrollPx,
                                                float(mx), float(my));
            if (hit >= 0) m_journalSelected = hit;
        }
        m_journalLmbDown = lmb;
    }
    m_journalScrollPx = m_ui.ClampJournalScroll(m_journalMissions, m_journalScrollPx);

    // --- snapshot: MissionManager -> UI rows (active, complete, failed) --------
    m_journalMissions.clear();
    const size_t missionCount = m_missions.MissionCount();
    m_journalMissions.reserve(missionCount);
    // Three passes: active first, then complete, then failed. Not-yet-started
    // (Inactive) missions are hidden until they activate.
    for (int pass = 0; pass < 3; ++pass) {
        for (size_t i = 0; i < missionCount; ++i) {
            const Mission* m = m_missions.GetMission(uint32_t(i));
            if (!m) continue;
            const int st = m->state == MissionState::Active   ? 0 :
                           m->state == MissionState::Complete ? 1 :
                           m->state == MissionState::Failed   ? 2 : -1;
            if (st != pass) continue;
            JournalMissionEntry e;
            e.name = m->name;
            e.description = m->description.empty()
                               ? m->objectives.front().description
                               : m->description;
            e.state = st;
            e.currentObjectiveIndex =
                (st == 0 && m->currentObjective < m->objectives.size())
                    ? int(m->currentObjective) : -1;
            for (const Objective& o : m->objectives)
                e.objectives.push_back({o.description, o.complete});
            m_journalMissions.push_back(std::move(e));
        }
    }
    if (m_journalSelected >= int(m_journalMissions.size()))
        m_journalSelected = int(m_journalMissions.size()) - 1;
    if (m_journalSelected < 0) m_journalSelected = 0;

    // Track 47: photo album rows for the journal's album page (oldest first,
    // newest at the bottom; the panel clips overflow). '*' marks photos that
    // counted toward a mission objective.
    m_photoAlbumRows.clear();
    {
        const size_t albumN = m_photos.Album().size();
        const size_t start = albumN > 60 ? albumN - 60 : 0;
        for (size_t i = start; i < albumN; ++i) {
            const Photo::PhotoRecord& r = m_photos.Album()[i];
            char row[128];
            std::snprintf(row, sizeof(row), "#%-3zu %-18s @ %02d:%02d%s",
                          i + 1, r.subject.c_str(),
                          (r.clockMinutes / 60) % 24, r.clockMinutes % 60,
                          r.missionId.empty() ? "" : "  *");
            m_photoAlbumRows.emplace_back(row);
        }
    }

    // --- transition log: activation / objective completion -------------------
    // Diff the previous frame's (state, currentObjective) per mission and log
    // every transition. The feed renders in the journal's details pane.
    if (m_missionStatePrev.size() != missionCount) {
        m_missionStatePrev.assign(missionCount, 0xFF);
        m_missionObjPrev.assign(missionCount, 0);
    }
    for (size_t i = 0; i < missionCount; ++i) {
        const Mission* m = m_missions.GetMission(uint32_t(i));
        if (!m) continue;
        const uint8_t  st = uint8_t(m->state);
        const size_t   ob = m->currentObjective;
        if (m_missionStatePrev[i] == 0xFF) {  // first observation: no log
            m_missionStatePrev[i] = st;
            m_missionObjPrev[i]  = ob;
            continue;
        }
        if (st != m_missionStatePrev[i]) {
            if (st == uint8_t(MissionState::Active))
                AppendJournalLog("NEW MISSION: " + m->name);
            else if (st == uint8_t(MissionState::Complete))
                AppendJournalLog("MISSION COMPLETE: " + m->name);
            else if (st == uint8_t(MissionState::Failed))
                AppendJournalLog("MISSION FAILED: " + m->name);
            m_missionStatePrev[i] = st;
        } else if (ob != m_missionObjPrev[i] &&
                   st == uint8_t(MissionState::Active) &&
                   m_missionObjPrev[i] < m->objectives.size()) {
            AppendJournalLog("OBJECTIVE DONE: " +
                             m->objectives[m_missionObjPrev[i]].description);
            m_missionObjPrev[i] = ob;
        }
    }
}

// Track 40: draw the journal overlay on view 10, above every other UI element.
// Track 47: while the album page is TAB-toggled on, the photo album replaces
// the mission list.
void Engine::DrawQuestJournal() {
    if (!m_journalOpen || !m_ui.IsInitialized()) return;
    if (m_journalPhotoPage) {
        char head[96];
        std::snprintf(head, sizeof(head),
                      "%zu photo(s) taken  ('*' = counted for a mission)",
                      m_photos.Album().size());
        m_ui.DrawPhotoAlbum(m_photoAlbumRows, head);
        return;
    }
    m_ui.DrawQuestJournal(m_journalMissions, m_journalSelected,
                          m_journalScrollPx);

    // Track 46: trick stats + achievements. The journal doubles as the stats
    // screen; the panel sits on the journal's full-screen dim layer so no new
    // UIRenderer type is needed. Bottom-left so it clears the mission list
    // and the minimap.
    const uint32_t tGold = 0xff50d2ffu; // #FFD250 (ABGR: alpha|blue|green|red)
    const uint32_t tDim  = 0xffa8a8b0u;
    const uint32_t tOk   = 0xff70dd70u;
    const float sx = 20.0f;
    const float pw = 316.0f;
    const float rowH = 15.0f;
    const float top = float(m_screenHeight) - 208.0f;
    m_ui.DrawRect(sx, top, pw, 196.0f, 0xff0a0a0eu);
    m_ui.DrawRect(sx, top, pw, 2.0f, tGold);

    char line[96];
    float y = top + 12.0f;
    auto row = [&](const char* text, uint32_t color) {
        m_ui.DrawText(sx + 12.0f, y, text, color);
        y += rowH;
    };
    row("STYLE - BIKE TRICKS", tGold);
    std::snprintf(line, sizeof(line), "SCORE          %d", m_tricks.totalScore);
    row(line, 0xffffffffu);
    std::snprintf(line, sizeof(line), "COMBO BEST     x%d", m_tricks.bestMult);
    row(line, 0xffffffffu);
    for (TrickType t = TrickType::Wheelie; t != TrickType::Count;
         t = TrickType(int(t) + 1)) {
        const TrickTypeStats& st = m_tricks.perType[int(t)];
        std::snprintf(line, sizeof(line), "%-9s %d   best %.1fs   pts %d",
                      TrickName(t), st.landed, st.longestTime, st.totalPoints);
        row(line, 0xffffffffu);
    }
    row("ACHIEVEMENTS", tGold);
    const int nAch = TrickAchievementCount();
    for (int i = 0; i < nAch; ++i) {
        const TrickAchievement& a = GetTrickAchievements()[i];
        char goal[64];
        if (a.count > 0) {
            int totalLanded = 0;
            for (const TrickTypeStats& st : m_tricks.perType)
                totalLanded += st.landed;
            const int have = a.type == TrickType::None
                                 ? totalLanded
                                 : m_tricks.perType[int(a.type)].landed;
            std::snprintf(goal, sizeof(goal), "%s %s %d/%d",
                          a.unlocked ? "[x]" : "[ ]",
                          a.name, have, a.count);
        } else if (a.comboMult > 0) {
            std::snprintf(goal, sizeof(goal), "%s %s x%d/%d",
                          a.unlocked ? "[x]" : "[ ]",
                          a.name, m_tricks.bestMult, a.comboMult);
        } else {
            std::snprintf(goal, sizeof(goal), "%s %s %d/%d",
                          a.unlocked ? "[x]" : "[ ]",
                          a.name, m_tricks.totalScore, a.score);
        }
        row(goal, a.unlocked ? tOk : tDim);
    }
}

// --- Track 41: class system ---------------------------------------------------

// Loads data/classes/classes.json. Positions use the same ref map as missions
// ("spawn" / "anchor_center" / "anchor_min" / "anchor_max"); same probing
// convention (CWD first, source tree fallback).
bool Engine::InitClasses() {
    Logger::Info("Classes: init begin"); // Track 41 probe (crash bisect)
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
        "data/classes/classes.json",
        "../data/classes/classes.json",
    };
    bool loaded = false;
    for (const char* p : paths) {
        if (std::ifstream f(p); f.good()) {
            f.close();
            loaded = m_classes.LoadFromJson(p, refs);
            break;
        }
    }
    if (!loaded) {
        Logger::Warning("Classes: classes.json not found or empty "
                        "(class system idle)");
        return false;
    }
    Logger::Info("Classes: %zu definition(s) registered", m_classes.Count());
    return true;
}

// Per-frame class logic: C toggles the schedule overlay, prompt detection
// runs against Jimmy's position + the game clock, and the session minigame
// advances. Runs before UpdateDialogue so UpdateClassAttend can arbitrate a
// same-frame E press.
void Engine::UpdateClasses(float dt) {
    // Track 44: while a real minigame is on screen there is no prompt,
    // schedule toggle or attendance input (its keys were already stripped
    // by UpdateMinigames).
    if (m_minigames.IsActive()) {
        m_classPromptIdx = -1;
        m_classAttendPending = false;
        return;
    }
    // Track 45: the arcade view is modal for the class UI too (C / prompt /
    // attend are all unreachable from inside a cabinet screen).
    if (m_arcade.IsActive()) {
        m_classPromptIdx = -1;
        m_classAttendPending = false;
        return;
    }
    const int dayIndex = m_gameDay % 7;
    const int minuteNow = std::max(0, int(m_gameTimeMinutes));

    // --- C: schedule overlay (modal, no cursor needed) ---------------------
    if (!m_classSessionActive && !m_journalOpen &&
        m_input.WasKeyPressed(SDL_SCANCODE_C)) {
        m_scheduleOpen = !m_scheduleOpen;
        Logger::Info("Schedule: %s", m_scheduleOpen ? "opened" : "closed");
    }

    // --- class session ------------------------------------------------------
    if (m_classSessionActive) {
        m_classPromptIdx = -1;
        m_classPromptDist = 0.0f;

        if (m_sessionResult != 0) {
            // Banner dwell, then close the room.
            m_sessionResolveTimer -= dt;
            if (m_sessionResolveTimer <= 0.0f) {
                m_classSessionActive = false;
                m_classSessionIdx = -1;
                m_sessionResult = 0;
                Logger::Info("Classes: session overlay closed");
            }
        } else {
            m_sessionTime += dt;
            m_sessionSweep += m_sessionSweepDir * kClassSweepSpeed * dt;
            if (m_sessionSweep > 1.0f) { m_sessionSweep = 1.0f; m_sessionSweepDir = -1.0f; }
            if (m_sessionSweep < 0.0f) { m_sessionSweep = 0.0f; m_sessionSweepDir = 1.0f; }

            // E inside the band = correct beat; outside = slip.
            if (m_input.WasKeyPressed(SDL_SCANCODE_E)) {
                const bool inside = m_sessionSweep >= m_sessionBandLo &&
                                    m_sessionSweep <= m_sessionBandHi;
                if (inside) ++m_sessionHits;
                else ++m_sessionMisses;
                Logger::Info("Class session: %s (correct %d/%d, slips %d/%d)",
                             inside ? "CORRECT" : "MISSED BAND",
                             m_sessionHits, 3, m_sessionMisses, 3);
                // Re-roll the band so the next press is not free.
                const float w = kClassBandWidth;
                const float a = 0.08f + nextRandom01() * (0.92f - w);
                m_sessionBandLo = a;
                m_sessionBandHi = std::min(1.0f, a + w);
                if (m_sessionHits >= 3)  EndClassSession(true);
                else if (m_sessionMisses >= 3) EndClassSession(false);
            }
            if (m_sessionResult == 0 && m_sessionTime >= kClassSessionFail)
                EndClassSession(false); // ran out the clock

            // OPENBULLY_CLASS_DEMO=1: auto-focus inside the band so a headless
            // run passes the session without OS-level key injection.
            static const bool s_classDemo =
                std::getenv("OPENBULLY_CLASS_DEMO") != nullptr;
            if (s_classDemo && m_sessionResult == 0 &&
                m_sessionSweep >= m_sessionBandLo &&
                m_sessionSweep <= m_sessionBandHi) {
                ++m_sessionHits;
                Logger::Info("Class demo: auto-focus (correct %d/3)", m_sessionHits);
                const float w = kClassBandWidth;
                const float a = 0.08f + nextRandom01() * (0.92f - w);
                m_sessionBandLo = a;
                m_sessionBandHi = std::min(1.0f, a + w);
                if (m_sessionHits >= 3) EndClassSession(true);
            }
        }
        return; // no prompt while the room is open
    }

    // OPENBULLY_CLASS_DEMO=1: scripted class day for headless verification
    // (same philosophy as the dialogue demo; OS key injection is unreliable).
    // Runs BEFORE the prompt gates so gameplay state cannot starve the demo.
    //   Phase A: walk Jimmy (position-direct) to the nearest class whose
    //            window is live right now and attend once.
    //   Phase B: ~1.5 s after the pass, open the schedule overlay for ~3 s
    //            so screenshots prove completion bookkeeping.
    static const bool s_classDemoWalk =
        std::getenv("OPENBULLY_CLASS_DEMO") != nullptr;
    if (s_classDemoWalk) {
        static bool   s_attended    = false;
        static bool   s_schedOpen   = false;
        static bool   s_schedClosed = false;
        static double s_passAt      = -1.0;
        static double s_closedAt    = -1.0;
        static int    s_lastDone    = 0;

        const int totalDone = m_classes.TotalCompletions();
        if (totalDone > s_lastDone) {
            s_lastDone = totalDone;
            s_passAt = m_time;
        }
        // The session banner dwells 2.5 s after the pass before the room
        // closes; anchor the schedule window on that close so the overlay
        // does not open and close inside the same frame. Latch once.
        if (!m_classSessionActive && s_passAt > 0.0 && s_closedAt < 0.0)
            s_closedAt = m_time;

        // Phase B: schedule screenshot window (3.5 s after the room closes).
        if (!s_schedOpen && !s_schedClosed && s_closedAt > 0.0 &&
            m_time > s_closedAt + 0.5) {
            m_scheduleOpen = true;
            s_schedOpen = true;
            Logger::Info("Class demo: schedule overlay opened (progression proof)");
        }
        if (s_schedOpen && !s_schedClosed && m_time > s_closedAt + 4.0) {
            m_scheduleOpen = false;
            s_schedOpen = false;
            s_schedClosed = true;
            Logger::Info("Class demo: schedule overlay closed");
        }

        // Phase A: attend the live-window class (once per run). The walk
        // itself is Track 18's proof; this demo teleports (position-direct,
        // like the ranged-demo target spawn) so verification is deterministic
        // under any frame rate, including the 1-2 fps debug builds where a
        // real walk races the game clock (60x) and misses the window.
        if (s_attended || m_classSessionActive || m_scheduleOpen ||
            !m_jimmyActive || !m_jimmyId.IsValid()) return;
        const Transform* jtp = m_entities.GetTransform(m_jimmyId);
        const glm::vec3 jp = jtp ? jtp->position : glm::vec3(0.0f);
        const int idx = m_classes.ScheduledIndexNearest(jp, dayIndex, minuteNow);
        if (idx < 0) {
            // Log once per game-minute while the demo waits for a window.
            static int s_lastWaitMinute = -1;
            if (minuteNow != s_lastWaitMinute) {
                s_lastWaitMinute = minuteNow;
                Logger::Info("Class demo: waiting for a live window (day %d, %02d:%02d)",
                             dayIndex, minuteNow / 60, minuteNow % 60);
            }
            return;
        }
        const ClassData* c = m_classes.Get(size_t(idx));
        if (!c) return;
        Transform* pt = m_entities.GetTransform(m_jimmyId);
        if (!pt) return;
        if (PhysicsComponent* pp = m_entities.GetPhysics(m_jimmyId))
            pp->velocity.y = 0.0f;
        pt->position = c->position;
        Logger::Info("Class demo: attending '%s' at game time %02d:%02d",
                     c->displayName.c_str(),
                     GetGameTime().hour, GetGameTime().minute);
        s_attended = true;
        StartClassSession(*c);
        return; // demo drives the class flow; skip the interactive prompt path
    }

    // --- schedule overlay: show the world, still detect the prompt ---------
    if (m_scheduleOpen) {
        m_classPromptIdx = -1;
    } else {
        m_classPromptIdx = -1;
        m_classPromptDist = 0.0f;
        if (!m_jimmyActive || !m_jimmyId.IsValid() || m_classes.Count() == 0) return;
        const Transform* jt = m_entities.GetTransform(m_jimmyId);
        if (!jt) return;
        if (m_dialogue.IsActive() || m_grappleTarget.IsValid()) return;
        if (m_mountedVehicle.IsValid()) return; // dismount first
        if (const PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
            pc && pc->state != DefenseState::Normal) return;
        if (const Melee::CombatComponent* mc = m_entities.GetMeleeCombat(m_jimmyId);
            mc && mc->state != Melee::CombatState::Idle) return;

        const ClassData* c = nullptr;
        float dist = 0.0f;
        const int idx = m_classes.AvailableIndex(jt->position, dayIndex,
                                                    minuteNow, &c, &dist);
        if (idx >= 0 && c) {
            m_classPromptIdx = int(idx);
            m_classPromptDist = dist;
        }
    }

    // --- E on the prompt: defer so dialogue keeps same-frame priority ----
    if (m_classPromptIdx >= 0 && m_input.WasKeyPressed(SDL_SCANCODE_E))
        m_classAttendPending = true;
}

// Deferred attend: E was pressed on a prompt this frame; only start the
// session when dialogue did not take the press.
void Engine::UpdateClassAttend() {
    if (!m_classAttendPending) return;
    m_classAttendPending = false;
    if (m_classSessionActive || m_dialogue.IsActive() || m_journalOpen ||
        m_scheduleOpen || m_classPromptIdx < 0) return;
    const ClassData* c = m_classes.Get(size_t(m_classPromptIdx));
    if (!c) return;
    StartClassSession(*c);
}

// =========================================================================
// Track 45: arcade cabinets (Nut Shots / Monkey Fling)
// =========================================================================

// Init-time cabinet placement: the same ground-verified ring strategy the
// vehicle spawner uses, centered on Jimmy's spawn (or the free camera).
void Engine::SpawnArcadeCabinets() {
    m_arcade.SetAudio(m_audio.get());
    m_arcade.SetDemo(std::getenv("OPENBULLY_ARCADE_DEMO") != nullptr);
    m_arcade.SpawnCabinets(m_entities, m_collisionWorld.get(),
                           m_jimmyActive ? m_jimmySpawn : m_camera.GetPosition());
}

// World-side arcade tick: refresh the near-cabinet prompt, arbitrate the E
// press (dialogue / class attend keep first claim — this runs after both),
// and drive the OPENBULLY_ARCADE_DEMO walk-to-cabinet script.
void Engine::UpdateArcade(float dt) {
    // Demo rotation tracking must observe the open state every frame — the
    // early return below (the cabinet owns the input while open) would
    // otherwise never see the open->closed transition and the demo would
    // re-enter the same cabinet forever.
    static const bool s_demo = std::getenv("OPENBULLY_ARCADE_DEMO") != nullptr;
    static int  s_demoCab     = 0;
    static bool s_demoWasOpen = false;
    if (s_demo) {
        // Rotate to the next cabinet after each exit (the manager demo exits
        // from the round-over banner).
        const bool open = m_arcade.IsActive();
        if (s_demoWasOpen && !open)
            s_demoCab = (s_demoCab + 1) % std::max(1, m_arcade.CabinetCount());
        s_demoWasOpen = open;
    }

    if (m_arcade.IsActive()) return;               // the cabinet owns the input
    if (m_mountedVehicle.IsValid()) return;        // ride first, play after

    glm::vec3 playerPos = m_camera.GetPosition();
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        if (const Transform* jt = m_entities.GetTransform(m_jimmyId))
            playerPos = jt->position;
    }
    m_arcade.UpdateProximity(playerPos);

    // Demo walk: position-direct (same pattern as the dialogue/class demos)
    // toward the current cabinet, then insert a coin when in range.
    if (s_demo && m_jimmyActive && m_jimmyId.IsValid()) {
        static double s_lastTry  = 0.0;
        static bool  s_cashGiven = false;
        PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
        if (pc && !s_cashGiven) {              // headless wallets start empty
            s_cashGiven = true;
            pc->money = std::max(pc->money, 5);
            Logger::Info("Arcade demo: seeded $%d", pc->money);
        }
        if (Transform* pt = m_entities.GetTransform(m_jimmyId)) {
            const glm::vec3 target = m_arcade.CabinetPosition(s_demoCab);
            glm::vec3 to = target - pt->position;
            to.y = 0.0f;
            const float d = glm::length(to);
            if (d > 1.6f) {
                if (PhysicsComponent* pp = m_entities.GetPhysics(m_jimmyId))
                    pp->velocity.y = 0.0f;
                pt->position += (to / d) * (3.0f * dt);
                pt->position.y = target.y;     // cabinets sit on the floor
            } else if (m_time - s_lastTry > 1.0) {
                s_lastTry = m_time;
                if (pc && m_arcade.TryEnterCabinet(s_demoCab, pc->money))
                    Logger::Info("Arcade demo: entered cabinet %d", s_demoCab);
            }
        }
        return;                                // the demo owns entry
    }

    // Real-input entry: E on the near-cabinet prompt (UpdateProximity above
    // already validated the range).
    if (!m_arcade.NearCabinet() ||
        !m_input.WasKeyPressed(SDL_SCANCODE_E)) return;
    if (m_dialogue.IsActive() || m_journalOpen || m_scheduleOpen ||
        m_classSessionActive || m_minigames.IsActive())
        return;
    if (PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
        if (m_arcade.TryEnterCabinet(m_arcade.NearIndex(), pc->money))
            Logger::Info("Arcade: %s", m_arcade.Prompt());
    }
}

// Enters the (placeholder) class minigame. Jimmy is frozen while the
// session runs; NPC AI keeps ticking in the background.
void Engine::StartClassSession(const ClassData& c) {
    // Track 44: subjects with an implemented minigame get the real activity;
    // never two activities at once (framework busy -> skip the legacy room;
    // reserved subjects -> Track 41 placeholder sweep stays the fallback).
    if (m_minigames.IsActive()) return;
    if (StartClassMinigame(c)) return;
    m_classSessionActive = true;
    m_classSessionIdx = int(m_classes.IndexOf(c.id));
    m_sessionHits = 0;
    m_sessionMisses = 0;
    m_sessionTime = 0.0f;
    m_sessionResult = 0;
    m_sessionResolveTimer = 0.0f;
    m_sessionSweep = 0.0f;
    m_sessionSweepDir = 1.0f;
    const float w = kClassBandWidth;
    const float a = 0.08f + nextRandom01() * (0.92f - w);
    m_sessionBandLo = a;
    m_sessionBandHi = std::min(1.0f, a + w);
    Logger::Info("Class: session started '%s' (%s) — %zu beat(s)",
                 c.displayName.c_str(), c.id.c_str(), c.objectives.size());
}

// Resolves a session: pass -> completion + rewards; fail -> retry bookkeeping.
// Both stay on screen as a banner for kSessionResolveDwell seconds.
static constexpr float kClassResolveDwell = 2.5f;
void Engine::EndClassSession(bool passed) {
    m_sessionResult = passed ? 1 : 2;
    m_sessionResolveTimer = kClassResolveDwell;
    const ClassData* c = m_classSessionIdx >= 0
        ? m_classes.Get(size_t(m_classSessionIdx)) : nullptr;
    if (passed) {
        if (c && m_classSessionIdx >= 0) {
            m_classes.MarkComplete(size_t(m_classSessionIdx));
            ApplyClassRewards(*c);
        }
        AppendJournalLog("CLASS PASSED: " +
                         (c ? c->displayName : "?"));
    } else {
        if (c && m_classSessionIdx >= 0)
            m_classes.MarkFailed(size_t(m_classSessionIdx));
        AppendJournalLog("CLASS FAILED: " +
                         (c ? c->displayName : "?") + " (retry available)");
    }
    Logger::Info("Class: session %s (%s)", passed ? "PASSED" : "FAILED",
                 c ? c->displayName.c_str() : "?");
}

// Applies a class's reward list. Weapon grants need a free slot; slingshot
// ammo needs an owned slingshot. Unknown kinds are ignored loudly.
void Engine::ApplyClassRewards(const ClassData& c) {
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc) return;
    std::string summary;
    for (const ClassReward& r : c.rewards) {
        switch (r.kind) {
            case RewardKind::MaxHealth:
                pc->health.max += std::max(0, r.amount);
                pc->health.current += std::max(0, r.amount); // instant full heal
                if (pc->health.current > pc->health.max) pc->health.current = pc->health.max;
                Logger::Info("Reward: max health +%d (now %d/%d)",
                             r.amount, pc->health.current, pc->health.max);
                if (!summary.empty()) summary += ", ";
                summary += "Max Health +" + std::to_string(r.amount);
                break;
            case RewardKind::Money: {
                const int before = pc->money;
                pc->money += std::max(0, r.amount);
                Logger::Info("Reward: money +%d (now %d, was %d)",
                             pc->money - before, pc->money, before);
                if (!summary.empty()) summary += ", ";
                summary += "$" + std::to_string(r.amount);
                break;
            }
            case RewardKind::Weapon: {
                const int wid = r.weaponId;
                if (wid < 0 || wid > 6) {
                    Logger::Warning("Reward: unknown weapon_id %d", wid);
                    break;
                }
                int freeSlot = -1;
                for (int i = 0; i < 5; ++i)
                    if (pc->slots[i].type == WeaponType::Unarmed) { freeSlot = i; break; }
                if (freeSlot < 0) {
                    Logger::Warning("Reward: inventory full, '%s' not granted",
                                    GetWeaponDataById(wid).name);
                    break;
                }
                const WeaponData& wd = GetWeaponDataById(wid);
                pc->slots[freeSlot].type = wd.type;
                pc->slots[freeSlot].weaponId = wid;
                pc->slots[freeSlot].durability = wd.durability;
                pc->slots[freeSlot].ammo = std::max(0, r.amount);
                if (pc->equippedSlot < 0) pc->equippedSlot = freeSlot;
                Logger::Info("Reward: '%s' granted -> slot %d (ammo %d)",
                             wd.name, freeSlot, r.amount);
                if (!summary.empty()) summary += ", ";
                summary += std::string(wd.name) + " x" + std::to_string(std::max(0, r.amount));
                break;
            }
            case RewardKind::Ammo: {
                int slotIdx = -1;
                if (pc->equippedSlot >= 0 &&
                    pc->slots[pc->equippedSlot].type == WeaponType::RangedProjectile)
                    slotIdx = pc->equippedSlot;
                else for (int s = 0; s < 5; ++s)
                    if (pc->slots[s].type == WeaponType::RangedProjectile) { slotIdx = s; break; }
                if (slotIdx < 0) {
                    Logger::Warning("Reward: no slingshot owned, ammo +%d not granted",
                                    r.amount);
                    break;
                }
                InventorySlot& slot = pc->slots[slotIdx];
                const int before = slot.ammo;
                slot.ammo = std::min(SlingshotCapacity(), slot.ammo + std::max(0, r.amount));
                Logger::Info("Reward: ammo +%d (slot %d, %d -> %d)",
                             slot.ammo - before, slotIdx, before, slot.ammo);
                if (!summary.empty()) summary += ", ";
                summary += "+" + std::to_string(slot.ammo - before) + " rocks";
                break;
            }
            case RewardKind::AmmoCap: {
                const int before = m_slingshotCapBonus;
                m_slingshotCapBonus = std::min(kClassMaxCapBonus,
                                                m_slingshotCapBonus + std::max(0, r.amount));
                Logger::Info("Reward: slingshot capacity +%d (now %d)",
                             m_slingshotCapBonus - before, SlingshotCapacity());
                if (!summary.empty()) summary += ", ";
                summary += "Slingshot Cap +" + std::to_string(m_slingshotCapBonus - before);
                break;
            }
            case RewardKind::Respect:
                m_classes.AddRespect(std::max(0, r.amount));
                Logger::Info("Reward: respect +%d (now %d)", r.amount,
                             m_classes.Respect());
                if (!summary.empty()) summary += ", ";
                summary += "Respect +" + std::to_string(r.amount);
                break;
            default:
                Logger::Warning("Reward: unknown kind %d", int(r.kind));
                break;
        }
    }
    Logger::Info("Class '%s': rewards -> %s",
                 c.displayName.c_str(), summary.empty() ? "(none)" : summary.c_str());
}

// Draws the prompt, schedule overlay and session screen on view 10.
void Engine::DrawClassOverlays() {
    if (!m_ui.IsInitialized()) return;

    if (m_classSessionActive) {
        const ClassData* c = m_classSessionIdx >= 0
            ? m_classes.Get(size_t(m_classSessionIdx)) : nullptr;
        ClassSessionView v;
        v.className = c ? c->displayName : "Class";
        v.location = c ? (c->building + " - " + c->room) : std::string();
        v.marker = m_sessionSweep;
        v.bandLo = m_sessionBandLo;
        v.bandHi = m_sessionBandHi;
        v.hits = m_sessionHits;
        v.misses = m_sessionMisses;
        v.hitGoal = 3;
        v.missGoal = 3;
        if (c) v.objectives = c->objectives;
        v.objectivesDone = m_sessionHits;
        v.result = m_sessionResult;
        if (m_sessionResult == 1 && c) {
            std::string rewards;
            for (const ClassReward& r : c->rewards) {
                if (!rewards.empty()) rewards += ", ";
                if (r.kind == RewardKind::Weapon)
                    rewards += std::string(GetWeaponDataById(r.weaponId).name) +
                               " x" + std::to_string(std::max(0, r.amount));
                else
                    rewards += std::string(RewardKindName(r.kind)) + " +" + std::to_string(r.amount);
            }
            v.rewardSummary = rewards;
        }
        m_ui.DrawClassSession(v);
        return;
    }

    if (m_scheduleOpen) {
        std::vector<ClassScheduleEntry> entries;
        const int dayIndex = m_gameDay % 7;
        const int minuteNow = std::max(0, int(m_gameTimeMinutes));
        for (size_t i = 0; i < m_classes.Count(); ++i) {
            const ClassData& c = *m_classes.Get(i);
            ClassScheduleEntry e;
            e.name = c.displayName;
            e.location = c.building + " - " + c.room;
            e.days = c.DaysText();
            e.time = c.TimeText();
            e.completions = m_classes.Completions(i);
            e.sessionsRequired = c.sessionsRequired;
            e.fullyComplete = e.completions >= c.sessionsRequired;
            e.availableNow = c.DayActive(dayIndex) && c.TimeActive(minuteNow);
            entries.push_back(std::move(e));
        }
        static const char* kDayNames[7] = { "Monday", "Tuesday", "Wednesday",
                                            "Thursday", "Friday", "Saturday",
                                            "Sunday" };
        char header[96];
        std::snprintf(header, sizeof(header), "Day %d - %s  %02d:%02d",
                      m_gameDay + 1, kDayNames[dayIndex],
                      GetGameTime().hour, GetGameTime().minute);

        std::string nextLine;
        int lead = -1;
        const ClassData* next = m_classes.NextUpcoming(dayIndex, minuteNow, &lead);
        // Nearest live session (the prompt) outranks the next upcoming one.
        const int availIdx = m_classes.AvailableIndex(
            m_jimmyId.IsValid() ? (m_entities.GetTransform(m_jimmyId)
                                        ? m_entities.GetTransform(m_jimmyId)->position
                                        : glm::vec3(0.0f)) : glm::vec3(0.0f),
            dayIndex, minuteNow);
        if (availIdx >= 0) {
            const ClassData* live = m_classes.Get(size_t(availIdx));
            if (live) nextLine = "NOW: " + live->displayName +
                                 " - " + live->room + " (ends " + live->TimeText() + ")";
        } else if (next) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "NEXT: %s - %s in %dh %02dm",
                          next->displayName.c_str(), next->TimeText().c_str(),
                          lead / 60, lead % 60);
            nextLine = buf;
        } else {
            nextLine = "No more classes scheduled";
        }

        char info[96];
        std::snprintf(info, sizeof(info), "Respect %d    Classes passed %d",
                      m_classes.Respect(), m_classes.TotalCompletions());
        m_ui.DrawClassSchedule(entries, header, nextLine, info);
        return;
    }

    if (m_classPromptIdx >= 0) {
        const ClassData* c = m_classes.Get(size_t(m_classPromptIdx));
        if (c) m_ui.DrawClassPrompt(c->displayName.c_str());
    }
}

// --- Track 44: minigame framework ----------------------------------------------

// Subject -> minigame mapping. Reserved subjects (Rhythm / TargetPractice /
// MemoryMatch) still map to their intended type; CreateMinigame refuses them
// for now, so StartMinigame fails and the caller falls back to the Track 41
// placeholder sweep until those games ship.
bool Engine::StartClassMinigame(const ClassData& c) {
    using MT = Minigames::MinigameType;
    MT type;
    switch (c.type) {
        case ClassType::English:   type = MT::WordScramble;    break;
        case ClassType::Math:      type = MT::MathQuiz;        break;
        case ClassType::Gym:       type = MT::ButtonMash;      break;
        case ClassType::Music:     type = MT::Rhythm;          break;
        case ClassType::Chemistry: type = MT::TargetPractice;  break;
        case ClassType::Biology:   type = MT::MemoryMatch;     break;
        default:
            return false; // Art/Shop/Photography/Geography: placeholder
    }
    if (!m_minigames.StartMinigame(type)) return false; // busy or reserved
    m_minigameClassIdx = int(m_classes.IndexOf(c.id));
    Logger::Info("Class '%s': %s started (%s)",
                 c.displayName.c_str(), Minigames::MinigameTypeName(type),
                 Minigames::MinigameTypeSubject(type));
    return true;
}

// Per-frame minigame tick. Captures the isolated key set straight after
// Input::Poll (demo-injected presses included), then clears those scancodes
// from the shared snapshot for the rest of the frame — gameplay consumers
// (melee Q/E, grapple G/U/I/O, weather W, schedule T, journal J, minimap M,
// reload R, slots 1-4, dialogue digits/arrows) cannot see them. Then the
// manager phase machine advances (Intro -> Playing -> Result).
void Engine::UpdateMinigames(float dt) {
    if (!m_minigames.IsActive()) return;

    m_minigameInput.CaptureFrom(m_input);
    StripMinigameKeys();

    m_minigames.Update(dt, m_minigameInput, /*allowEsc=*/true);

    // Drop the class binding once the overlay closed for good (the player
    // continued out of the Result banner).
    if (!m_minigames.IsActive())
        m_minigameClassIdx = -1;
}

// Clears every scancode the minigame framework consumes from the shared
// snapshot (WasKeyPressed edges go false) after the capture above. F5/F9
// stay live on purpose: saving mid-activity is harmless and still useful.
void Engine::StripMinigameKeys() {
    for (int sc = SDL_SCANCODE_A; sc <= SDL_SCANCODE_Z; ++sc)
        m_input.SetRawKey(SDL_Scancode(sc), false);
    for (int sc = SDL_SCANCODE_1; sc <= SDL_SCANCODE_9; ++sc)
        m_input.SetRawKey(SDL_Scancode(sc), false);
    m_input.SetRawKey(SDL_SCANCODE_0, false);
    m_input.SetRawKey(SDL_SCANCODE_SPACE, false);
    m_input.SetRawKey(SDL_SCANCODE_ESCAPE, false);
    m_input.SetRawKey(SDL_SCANCODE_BACKSPACE, false);
    m_input.SetRawKey(SDL_SCANCODE_RETURN, false);
    m_input.SetRawKey(SDL_SCANCODE_UP, false);
    m_input.SetRawKey(SDL_SCANCODE_DOWN, false);
}

// Full-screen minigame overlay, drawn topmost (above the class overlays;
// the manager renders the activity and its own result banner).
void Engine::DrawMinigameOverlay() {
    if (m_minigames.IsActive())
        m_minigames.Draw(m_ui, float(m_screenWidth), float(m_screenHeight));
}

// Result path for class-backed activities — mirrors EndClassSession's
// bookkeeping (completion + rewards + journal). Fires once per finished
// attempt, so an internal R-retry simply refreshes the outcome; the class
// is only closed out with the final attempt the player continues from.
void Engine::OnMinigameResult(const Minigames::MinigameResult& r) {
    const ClassData* c = m_minigameClassIdx >= 0
        ? m_classes.Get(size_t(m_minigameClassIdx)) : nullptr;
    const std::string who = c ? c->displayName
        : std::string(Minigames::MinigameTypeName(r.type));
    if (r.forfeited) {
        if (c && m_minigameClassIdx >= 0)
            m_classes.MarkFailed(size_t(m_minigameClassIdx));
        AppendJournalLog("CLASS FORFEITED: " + who + " (retry available)");
        Logger::Info("Minigame: forfeited %s", who.c_str());
    } else if (r.passed) {
        if (c && m_minigameClassIdx >= 0) {
            m_classes.MarkComplete(size_t(m_minigameClassIdx));
            ApplyClassRewards(*c);
        }
        AppendJournalLog("CLASS PASSED: " + who);
        Logger::Info("Minigame: PASSED %s (score %d, %.1fs)",
                     who.c_str(), r.score, r.timeTaken);
    } else {
        if (c && m_minigameClassIdx >= 0)
            m_classes.MarkFailed(size_t(m_minigameClassIdx));
        AppendJournalLog("CLASS FAILED: " + who + " (retry available)");
        Logger::Info("Minigame: FAILED %s (score %d)", who.c_str(), r.score);
    }
}

// Track 20 (UI): refresh the plain-data HUD snapshot from live game state.
// Runs after gameplay updates so the overlay always reflects this frame.
void Engine::UpdateHUD(float dt) {
    (void)dt;

    // Game clock: advanced by UpdateGameTime (Track 29, 60x real time);
    // this snapshot only mirrors the accumulator for the HUD.
    m_hudState.clockMinutes = m_clockMinutes;

    // Track 43: wanted system HUD fields (flash, cops banner, hide indicator
    // and the bust cut). Engine-side state; the player wanted level itself is
    // mirrored below from PlayerController.
    m_hudState.wantedFlash = m_wanted.flashTimer;
    m_hudState.copsActive  = !m_copIds.empty();
    m_hudState.hidden      = m_wanted.hidden;
    m_hudState.bustFade    = m_wanted.bustFade;
    m_hudState.busted      = m_wanted.bustPhase != 0;

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
            // Track 39: aim mode + slingshot ammo for the crosshair readout.
            m_hudState.aiming = m_aiming;
            m_hudState.slingshotAmmo = -1;
            if (pc->equippedSlot >= 0 &&
                pc->slots[pc->equippedSlot].type == WeaponType::RangedProjectile) {
                m_hudState.slingshotAmmo = pc->slots[pc->equippedSlot].ammo;
                m_hudState.slingshotCap = SlingshotCapacity();
            }
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
    // happens in UIRenderer::DrawHUD; here we collect world XZ + type).
    m_hudState.npcDotCount = 0;
    m_entities.ForEachAI([&](EntityID, const Transform& t, const AIComponent& ai) {
        if (m_hudState.npcDotCount >= HUDState::kMaxMinimapDots) return;
        m_hudState.npcDots[m_hudState.npcDotCount].x = t.position.x;
        m_hudState.npcDots[m_hudState.npcDotCount].z = t.position.z;
        // Track 37: color coding — red=hostile/prefect, green=friendly civilian,
        // yellow=neutral (transitioning/KO states).
        if (ai.role == AIRole::Prefect || ai.state == AIState::Alert ||
            ai.state == AIState::Chase || ai.state == AIState::Attack)
            m_hudState.npcDots[m_hudState.npcDotCount].type = 1;
        else if (ai.role == AIRole::Civilian &&
                 (ai.state == AIState::Idle || ai.state == AIState::Wander ||
                  ai.state == AIState::Patrol || ai.state == AIState::Watch ||
                  ai.state == AIState::FacePlayer))
            m_hudState.npcDots[m_hudState.npcDotCount].type = 2;
        else
            m_hudState.npcDots[m_hudState.npcDotCount].type = 0;
        ++m_hudState.npcDotCount;
    });

    // Track 37: vehicle dots (cyan squares on the minimap).
    m_hudState.vehicleDotCount = 0;
    for (EntityID vid : m_vehicleIds) {
        if (m_hudState.vehicleDotCount >= HUDState::kMaxMinimapVehicles) break;
        if (const Transform* vt = m_entities.GetTransform(vid)) {
            m_hudState.vehicleDots[m_hudState.vehicleDotCount].x = vt->position.x;
            m_hudState.vehicleDots[m_hudState.vehicleDotCount].z = vt->position.z;
            ++m_hudState.vehicleDotCount;
        }
    }

    // Track 37: active mission objective marker (gold star on the minimap).
    // Track 47: photo progress readout defaults off; set below when the
    // active mission's current objective is a PHOTOGRAPH_TARGET.
    m_hudState.objectiveActive = false;
    m_hudState.photoMissionActive = false;
    m_hudState.photoCount  = 0;
    m_hudState.photoNeeded = 0;
    if (const Mission* m = m_missions.ActiveMission()) {
        if (m->currentObjective < m->objectives.size()) {
            const Objective& obj = m->objectives[m->currentObjective];
            if (!obj.complete) {
                m_hudState.objectiveActive = true;
                m_hudState.objectiveX = obj.targetPosition.x;
                m_hudState.objectiveZ = obj.targetPosition.z;
            }
            // Track 47: photo mission progress readout (HUD line). Uses the
            // photo-specific lookup: several missions can be active at once.
            if (const Mission* pm = m_missions.ActivePhotoMission()) {
                const Objective& pobj = pm->objectives[pm->currentObjective];
                m_hudState.photoMissionActive = true;
                m_hudState.photoCount  = pobj.photoCount;
                m_hudState.photoNeeded = pobj.photoNeeded;
            }
        }
    }

    // Track 42: faction stats rows, overall aggregate + interaction readout.
    UpdateFactionHUD();

    // --- Track 49: boss bar + QTE prompt + banner snapshot ------------------
    {
        m_hudState.bossActive = false;
        m_hudState.bossDefeated = false;
        m_hudState.bossName[0] = '\0';
        m_hudState.bossTitle[0] = '\0';
        m_hudState.bossHealth = 0.0f;
        m_hudState.bossPhase = 1;
        m_hudState.qteActive = false;
        m_hudState.qteKey = ' ';
        m_hudState.qteFrac = 0.0f;
        m_hudState.qteResult = 0;
        m_hudState.qteResultFrac = 0.0f;
        m_hudState.banner[0] = '\0';
        m_hudState.bannerKind = 0;
        m_hudState.bannerFrac = 0.0f;

        if (m_boss.entity.IsValid() && m_entities.IsAlive(m_boss.entity)) {
            if (const AIComponent* ai = m_entities.GetAI(m_boss.entity)) {
                if (const BossComponent* bc = m_entities.GetBoss(m_boss.entity)) {
                    const BossDefinition& def = GetBossDefinition(bc->type);
                    m_hudState.bossDefeated = (ai->state == AIState::Ko);
                    m_hudState.bossActive = !m_hudState.bossDefeated;
                    std::snprintf(m_hudState.bossName,
                                  sizeof(m_hudState.bossName), "%s", def.name);
                    std::snprintf(m_hudState.bossTitle,
                                  sizeof(m_hudState.bossTitle), "%s", def.title);
                    m_hudState.bossHealth = def.maxHealth > 0
                        ? std::clamp(float(ai->health.current) /
                                         float(def.maxHealth), 0.0f, 1.0f)
                        : 0.0f;
                    m_hudState.bossPhase = int(bc->phase);
                    if (bc->qteState == QteState::Waiting) {
                        m_hudState.qteActive = true;
                        m_hudState.qteKey = BossQteKeyChar(bc->qteKey);
                        m_hudState.qteFrac = bc->qteWindow > 0.0f
                            ? std::clamp(bc->qteTimer / bc->qteWindow,
                                         0.0f, 1.0f)
                            : 0.0f;
                    } else if (bc->resultTimer > 0.0f) {
                        m_hudState.qteResult =
                            bc->qteState == QteState::Success ? 1 : 2;
                        m_hudState.qteResultFrac = std::clamp(
                            bc->resultTimer / kQteResultFlash, 0.0f, 1.0f);
                    }
                }
            }
        }
        if (m_bossBannerTimer > 0.0f && m_bossBanner[0]) {
            std::snprintf(m_hudState.banner, sizeof(m_hudState.banner), "%s",
                          m_bossBanner);
            m_hudState.bannerKind = m_bossBannerKind;
            m_hudState.bannerFrac = m_bossBannerDur > 0.0f
                ? std::clamp(m_bossBannerTimer / m_bossBannerDur, 0.0f, 1.0f)
                : 0.0f;
        }
    }
}

// =========================================================================
// Track 49: boss fights (faction leaders: phases, specials, QTEs, HUD)
//
// A boss NPC carries an AIComponent (grapples, projectiles, melee, minimap
// and missions keep working unchanged) PLUS a BossComponent. UpdateNpcs
// routes every boss entity into UpdateBossEntity, a dedicated FSM layered on
// the regular chase/attack/melee plumbing:
//
//   phases   100-66% P1 basic pattern; 66-33% P2 faster + 2nd move;
//            33-0% P3 enraged (2x cadence, 3rd move, finisher).
//   specials per-boss 2-3 move table (charge / stink bomb / reinforcements /
//            sand throw / ground slam / grab / flurry), phase-gated, each a
//            windup -> active -> recover machine.
//   QTEs     one live prompt at a time: a random key inside a shrinking
//            window. Win -> boss damage; miss -> player damage. Fires on
//            phase transitions (shockwave), boss grabs (break free) and
//            player block-counters (punish).
//   victory  boss at 0 HP -> victory cutscene + boss_duel mission complete.
//   defeat   player at 0 HP while engaged -> checkpoint respawn + boss reset.
// =========================================================================

// Phase-gated special pick: expand the boss's move table by phase, drop
// range-inappropriate entries, then round-robin through what remains.
static BossSpecial PickBossSpecial(BossComponent& bc, const BossDefinition& def,
                                   float dist) {
    const int phase = int(bc.phase);
    int count = 1;
    if (phase >= int(def.unlockSecondPhase)) ++count;
    if (phase >= int(def.unlockThirdPhase))  ++count;
    BossSpecial cand[3];
    int cc = 0;
    for (int i = 0; i < count && cc < 3; ++i) {
        const BossSpecial s = def.specials[i];
        if (s == BossSpecial::None) continue;
        const bool ranged = (s == BossSpecial::SandThrow ||
                             s == BossSpecial::StinkBomb);
        const bool close  = (s == BossSpecial::Charge ||
                             s == BossSpecial::GroundSlam ||
                             s == BossSpecial::Grab ||
                             s == BossSpecial::Flurry);
        if (ranged && dist < 4.0f) continue;  // too close to lob
        if (close  && dist > 3.0f) continue;  // too far to lunge
        cand[cc++] = s;
    }
    if (cc == 0) return BossSpecial::None;
    const int pick = int(bc.specialSlot++) % cc;
    return cand[pick];
}

// Spawns a boss NPC: same body as a Track 16 NPC (skinned rig, AI, melee,
// collision) plus the BossComponent and its definition's vitals. Registers
// the fight runtime so the HUD bar and QTE prompts follow this entity.
bool Engine::SpawnBoss(BossType type, const glm::vec3& pos) {
    if (m_npcModels.empty()) {
        Logger::Warning("SpawnBoss: no NPC models loaded");
        return false;
    }
    const BossDefinition& def = GetBossDefinition(type);

    // Ground snap so the boss stands on the surface, like SpawnNpcs.
    glm::vec3 spawn = pos;
    if (m_collisionWorld) {
        Ray down;
        down.origin[0] = spawn.x; down.origin[1] = spawn.y + 10.0f;
        down.origin[2] = spawn.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f;
        down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) spawn.y = hit.point[1];
    }

    // Lazily build a rig for the boss's model (first model = leader look),
    // mirroring the SpawnNpcs lazy-rig path.
    const size_t modelIdx = 0;
    NpcModel& model = m_npcModels[modelIdx];
    uint32_t rigIndex = model.rigIndex;
    if (rigIndex == 0xFFFFFFFF && !m_sharedClips.empty() &&
        !model.sourcePath.empty()) {
        const NifLoadResult nif = LoadNifFile(model.sourcePath);
        if (nif.ok) rigIndex = CreateRig(nif);
        model.rigIndex = rigIndex;
    }
    if (rigIndex != 0xFFFFFFFF) rigIndex = CloneRig(rigIndex);

    EntityID e = m_entities.Create();
    Transform t;
    t.position = spawn;
    t.scale = glm::vec3(model.bodyScale * 1.08f); // leaders stand a bit taller
    m_entities.AddTransform(e, t);

    MeshComponent mc;
    mc.isBox = false;
    mc.isNpc = true;
    mc.npcModelIndex = uint32_t(modelIdx);
    m_entities.AddMesh(e, mc);

    CollisionComponent cc;
    cc.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
    cc.boundsMax = glm::vec3( 0.5f, 1.8f,  0.5f);
    cc.layerMask = 2u;
    m_entities.AddCollision(e, cc);

    PhysicsComponent pc;
    pc.gravity = -9.8f;
    m_entities.AddPhysics(e, pc);

    AIComponent ai;
    ai.homePos = spawn;
    ai.facingYaw = 0.0f;
    ai.lastPos = spawn;
    ai.state = AIState::Idle;
    ai.health.current = def.maxHealth;
    ai.health.max     = def.maxHealth;
    ai.moveSpeed      = def.walkSpeed;
    ai.sightRadius    = 16.0f;   // bosses engage from further out
    ai.attackRange    = 2.0f;
    ai.attackCooldown = def.attackCooldown;
    ai.fleeHealthFrac = -1.0f;   // never flees (UpdateNpcs gate < fraction)
    ai.role           = AIRole::Prefect; // authorities chase the player
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

    BossComponent bc;
    bc.type = type;
    bc.phase = BossPhase::One;
    bc.maxHealth = def.maxHealth;
    bc.specialTimer = def.specialCooldown; // first special after the opener
    bc.specialCd = def.specialCooldown;
    m_entities.AddBoss(e, bc);

    m_boss.entity = e;
    m_boss.type = type;
    m_boss.spawnPos = spawn;
    m_boss.victory = false;
    m_boss.retryCount = 0;
    m_bossCronies.clear();
    Logger::Info("BOSS spawned: %s (%s) HP %d at (%.1f, %.1f, %.1f) entity %u",
                 def.name, def.title, def.maxHealth, spawn.x, spawn.y,
                 spawn.z, e.index);
    return true;
}

// Ground-constrained move step shared by the boss FSM (same ground snap +
// wall sphere as the Track 27 wander walk). Always writes outPos; callers
// compare against pos to detect a blocked step.
void Engine::StepMoveGround(const glm::vec3& pos, const glm::vec3& dir,
                            float speed, float dt, glm::vec3& outPos) {
    outPos = pos;
    glm::vec3 newPos = pos + dir * (speed * dt);
    float groundY = pos.y;
    if (m_collisionWorld) {
        Ray down;
        down.origin[0] = newPos.x; down.origin[1] = newPos.y + 4.0f;
        down.origin[2] = newPos.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f;
        down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) {
            if (std::fabs(hit.point[1] - pos.y) < 3.0f)
                groundY = hit.point[1]; // same-plane guard
        }
        const float in[3] = { newPos.x, newPos.y + 0.5f, newPos.z };
        float out[3];
        if (m_collisionWorld->ResolveSphere(in, 0.5f, out))
            newPos = glm::vec3(out[0], newPos.y, out[2]);
    }
    outPos = glm::vec3(newPos.x, groundY, newPos.z);
}

// --- QTE -----------------------------------------------------------------

// Opens one QTE prompt (never stacks; unless `force`, honors the cooldown).
void Engine::StartBossQTE(BossComponent& bc, float window, bool force) {
    if (bc.qteState == QteState::Waiting) return;
    if (!force && bc.qteCooldown > 0.0f) return;
    const int n = int(sizeof(kQteKeys) / sizeof(kQteKeys[0]));
    bc.qteKey    = kQteKeys[int(nextRandom01() * float(n)) % n];
    bc.qteWindow = window;
    bc.qteTimer  = window;
    bc.qteState  = QteState::Waiting;
    ++bc.qteCount;
    Logger::Info("QTE: PRESS %c within %.2fs (prompt #%d)",
                 BossQteKeyChar(bc.qteKey), window, bc.qteCount);
}

// Per-frame prompt: raw-key edges resolve the press; expiry fails it.
void Engine::UpdateBossQTE(EntityID id, BossComponent& bc, float dt) {
    if (bc.qteCooldown > 0.0f) bc.qteCooldown -= dt;
    if (bc.resultTimer > 0.0f) bc.resultTimer -= dt;
    if (bc.qteState != QteState::Waiting) return;

    bc.qteTimer -= dt;
    if (m_input.WasKeyPressed(SDL_Scancode(bc.qteKey))) {
        ResolveBossQTE(id, bc, true, "pressed in time");
        return;
    }
    if (bc.qteTimer <= 0.0f)
        ResolveBossQTE(id, bc, false, "window expired");
}

// Applies the QTE outcome: success damages the boss, failure hurts Jimmy.
void Engine::ResolveBossQTE(EntityID id, BossComponent& bc, bool success,
                            const char* cause) {
    bc.qteState    = success ? QteState::Success : QteState::Failed;
    bc.resultTimer = kQteResultFlash;
    bc.qteCooldown = kQteCooldown;
    if (success) {
        ++bc.qteWins;
        const Transform* bt = m_entities.GetTransform(id);
        Logger::Info("QTE: SUCCESS (%s) - boss takes %d", cause, kQteDamageToBoss);
        if (bt) DamageNpc(id, m_jimmyId, kQteDamageToBoss, bt->position, 0.35f);
    } else {
        Logger::Info("QTE: FAILED (%s) - player takes %d", cause,
                     kQteDamageToPlayer);
        DamagePlayer(kQteDamageToPlayer, "QTE fail");
    }
}

// --- specials -------------------------------------------------------------

// Kicks off a special: locks the boss into the windup stage and telegraphs.
void Engine::StartBossSpecial(EntityID id, BossComponent& bc,
                              BossSpecial kind) {
    if (bc.special.kind != BossSpecial::None) return;
    bc.special.kind  = kind;
    bc.special.stage = 1;
    bc.special.timer = 0.55f;   // windup telegraph (s)
    bc.special.fired = false;
    bc.special.flurryLeft = (kind == BossSpecial::Flurry) ? 3 : 0;
    Logger::Info("BOSS %s: %s WINDUP", BossTypeName(bc.type),
                 BossSpecialName(kind));
    (void)id;
}

// Advances the active special's windup -> active -> recover machine and
// applies its payload (dash contact, lobbed zone, crony spawn, spray...).
void Engine::UpdateBossSpecial(EntityID id, BossComponent& bc, AIComponent& ai,
                               Transform& t, const glm::vec3& playerPos,
                               bool havePlayer, float dt) {
    const BossDefinition& def = GetBossDefinition(bc.type);
    BossSpecialRun& sp = bc.special;
    const glm::vec3 pos = t.position;
    const float dist = havePlayer
        ? glm::length(glm::vec2(playerPos.x - pos.x, playerPos.z - pos.z))
        : 1e9f;

    // Face the player through every stage (attacks come from the facing).
    if (havePlayer && dist > 0.01f) {
        const float want = std::atan2(playerPos.x - pos.x, playerPos.z - pos.z);
        float d = want - ai.facingYaw;
        while (d >  3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        ai.facingYaw += d * std::min(1.0f, ai.turnSpeed * 1.5f * dt);
        t.rotation.y = ai.facingYaw;
    }

    sp.timer -= dt;
    switch (sp.stage) {
    case 1: // windup: lean back (the flinch lean shares rotation.x - fine)
        t.rotation.x = -0.18f;
        if (sp.timer <= 0.0f) {
            t.rotation.x = 0.0f;
            sp.stage = 2;
            switch (sp.kind) {
            case BossSpecial::Charge:     sp.timer = 0.85f; break;
            case BossSpecial::GroundSlam: sp.timer = 0.45f; break;
            case BossSpecial::Flurry:     sp.timer = 3.2f;  break;
            case BossSpecial::StinkBomb: {
                // Lob at the player's feet: lingering ticking zone.
                const glm::vec3 tgt = havePlayer
                    ? playerPos : pos + glm::vec3(4.0f, 0.0f, 0.0f);
                SpawnBossZone(tgt, 6.0f, 2.4f, 3);
                sp.fired = true;
                sp.timer = 0.35f;
                break; }
            case BossSpecial::SandThrow: {
                if (havePlayer) {
                    const glm::vec3 from = pos + glm::vec3(0.0f, 1.1f, 0.0f);
                    glm::vec3 dir = playerPos - from;
                    dir.y = 0.0f;
                    const float len = glm::length(dir);
                    if (len > 1e-3f) {
                        dir = (dir / len) * 11.0f + glm::vec3(0.0f, 1.6f, 0.0f);
                        SpawnProjectile(from + dir * 0.05f, dir, 6, id, 2.0f);
                    }
                    sp.fired = true;
                }
                sp.timer = 0.4f;
                break; }
            case BossSpecial::Reinforce: {
                // Top the crew back up to 2. DEFERRED: entity creation inside
                // the AI loop would reallocate the component arrays (see the
                // SpawnBossZone note); FlushBossSpawns runs after UpdateNpcs.
                int toSpawn = 2 - int(m_bossCronies.size());
                for (int i = 0; i < toSpawn; ++i) {
                    const float a = float(i) * 1.5708f + 0.8f;
                    m_pendingCronies.push_back(
                        { pos + glm::vec3(std::cos(a) * 2.0f, 0.0f,
                                          std::sin(a) * 2.0f) });
                }
                if (toSpawn > 0)
                    Logger::Info("BOSS %s: called %d reinforcement(s)",
                                 BossTypeName(bc.type), toSpawn);
                sp.fired = true;
                sp.timer = 0.4f;
                break; }
            case BossSpecial::Grab: {
                // Close-range grab -> break-free QTE (dodge by spacing).
                if (dist <= 2.3f) {
                    StartBossQTE(bc, 1.7f, /*force=*/true);
                    sp.fired = true;
                    Logger::Info("BOSS %s: GRAB attempt (break free!)",
                                 BossTypeName(bc.type));
                }
                sp.timer = 0.35f;
                break; }
            default:
                sp.stage = 3; sp.timer = 0.3f; break;
            }
            Logger::Info("BOSS %s: %s ACTIVE", BossTypeName(bc.type),
                         BossSpecialName(sp.kind));
        }
        break;

    case 2: // active payload
        switch (sp.kind) {
        case BossSpecial::Charge: {
            if (havePlayer) {
                glm::vec3 dir = playerPos - pos;
                dir.y = 0.0f;
                const float len = glm::length(dir);
                if (len > 1e-3f) {
                    glm::vec3 np;
                    StepMoveGround(pos, dir / len, def.chargeSpeed, dt, np);
                    if (glm::distance(np, pos) > 1e-4f) t.position = np;
                }
                if (!sp.fired && dist < 1.7f) {
                    sp.fired = true;
                    const int dmg = int(def.chargeDamage + 0.5f);
                    DamagePlayer(dmg, "boss charge");
                    Logger::Info("BOSS %s: charge HIT for %d",
                                 BossTypeName(bc.type), dmg);
                }
            }
            break; }
        case BossSpecial::GroundSlam: {
            // AoE once at the moment of impact; a short shockwave zone lingers.
            if (havePlayer && !sp.fired && dist <= 2.6f) {
                sp.fired = true;
                const int dmg = int(def.chargeDamage * 0.9f + 0.5f);
                DamagePlayer(dmg, "ground slam");
                SpawnBossZone(pos, 2.0f, 2.6f, 2);
                Logger::Info("BOSS %s: slam HIT for %d",
                             BossTypeName(bc.type), dmg);
            }
            break; }
        case BossSpecial::Flurry: {
            // Feed swings through the regular NPC attack path (progressed in
            // UpdateBossEntity) until the window closes.
            if (sp.flurryLeft > 0 && havePlayer && dist <= 2.4f) {
                Melee::CombatComponent* mc = m_entities.GetMeleeCombat(id);
                if (mc && mc->state == Melee::CombatState::Idle) {
                    const float yaw = std::atan2(playerPos.x - pos.x,
                                                 playerPos.z - pos.z);
                    StartNpcAttack(id, Melee::AttackType::HeavyPunch, yaw);
                    --sp.flurryLeft;
                }
            }
            break; }
        default: break;
        }
        if (sp.timer <= 0.0f) { sp.stage = 3; sp.timer = 0.7f; }
        break;

    case 3: // recover
    default:
        if (sp.timer <= 0.0f) {
            sp.kind = BossSpecial::None;
            sp.stage = 0;
            bc.specialTimer = bc.specialCd; // phase-scaled by the phase check
            ai.attackCdTimer = std::min(ai.attackCdTimer,
                                        def.attackCooldown * bc.enrageMult);
        }
        break;
    }
}

// Johnny-style crony: a plain hostile NPC beside the boss.
EntityID Engine::SpawnBossReinforcement(const glm::vec3& pos) {
    if (m_npcModels.empty()) return EntityID{};
    glm::vec3 spawn = pos;
    if (m_collisionWorld) {
        Ray down;
        down.origin[0] = spawn.x; down.origin[1] = spawn.y + 10.0f;
        down.origin[2] = spawn.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f;
        down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) spawn.y = hit.point[1];
    }
    const size_t modelIdx = 1 % m_npcModels.size();
    NpcModel& model = m_npcModels[modelIdx];
    uint32_t rigIndex = model.rigIndex;
    if (rigIndex == 0xFFFFFFFF && !m_sharedClips.empty() &&
        !model.sourcePath.empty()) {
        const NifLoadResult nif = LoadNifFile(model.sourcePath);
        if (nif.ok) rigIndex = CreateRig(nif);
        model.rigIndex = rigIndex;
    }
    if (rigIndex != 0xFFFFFFFF) rigIndex = CloneRig(rigIndex);

    EntityID e = m_entities.Create();
    Transform t;
    t.position = spawn;
    t.scale = glm::vec3(model.bodyScale);
    m_entities.AddTransform(e, t);
    MeshComponent mc;
    mc.isBox = false;
    mc.isNpc = true;
    mc.npcModelIndex = uint32_t(modelIdx);
    m_entities.AddMesh(e, mc);
    CollisionComponent cc;
    cc.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
    cc.boundsMax = glm::vec3( 0.5f, 1.8f,  0.5f);
    cc.layerMask = 2u;
    m_entities.AddCollision(e, cc);
    PhysicsComponent pc;
    pc.gravity = -9.8f;
    m_entities.AddPhysics(e, pc);
    AIComponent ai;
    ai.homePos = spawn;
    ai.lastPos = spawn;
    ai.state = AIState::Chase;
    ai.targetEntity = m_jimmyId;
    ai.health.current = 60;
    ai.health.max = 60;
    ai.moveSpeed = 2.0f;
    ai.sightRadius = 20.0f;
    ai.attackCooldown = 1.6f;
    ai.role = AIRole::Civilian;
    m_entities.AddAI(e, ai);
    m_entities.AddMeleeCombat(e);
    if (rigIndex != 0xFFFFFFFF) {
        AnimationComponent ac;
        ac.rigIndex = rigIndex;
        ac.playing = true;
        m_entities.AddAnimation(e, ac);
        AnimRig& rig = m_animRigs[rigIndex];
        if (rig.idleClip >= 0) rig.controller.Play(rig.idleClip, true, 0.0f);
    }
    Logger::Info("BOSS reinforcement spawned (entity %u)", e.index);
    return e;
}

// --- zones (stink bombs / slam shockwaves) ---------------------------------

// Flat test-box pad marking an AoE; ticks damage to the player inside.
// Track 49: DEFERRED - called from UpdateBossSpecial (inside UpdateNpcs'
// ForEachAI, where entity creation would realloc the dense arrays and dangle
// the loop's references); the actual spawn happens in FlushBossSpawns.
void Engine::SpawnBossZone(const glm::vec3& pos, float life, float radius,
                           int damage) {
    m_pendingZones.push_back({ pos, life, radius, damage });
    Logger::Info("BOSS zone queued: r=%.1f dmg=%d life=%.1fs at (%.1f, %.1f)",
                 radius, damage, life, pos.x, pos.z);
}

// Creates the entities queued by the boss FSM. Runs OUTSIDE the AI loop
// (UpdateBosses, after UpdateNpcs) so component-array reallocation can
// never invalidate the iteration.
void Engine::FlushBossSpawns() {
    for (const PendingZone& pz : m_pendingZones)
        SpawnBossZoneNow(pz.pos, pz.life, pz.radius, pz.damage);
    m_pendingZones.clear();
    for (const PendingCrony& pc : m_pendingCronies) {
        EntityID c = SpawnBossReinforcement(pc.pos);
        if (c.IsValid()) m_bossCronies.push_back(c);
    }
    if (!m_pendingCronies.empty())
        Logger::Info("BOSS: %zu reinforcement(s) spawned", m_pendingCronies.size());
    m_pendingCronies.clear();
}

// The real zone spawn (flush context only).
void Engine::SpawnBossZoneNow(const glm::vec3& pos, float life, float radius,
                              int damage) {
    if (!m_testBoxValid) return;
    glm::vec3 spawn = pos;
    if (m_collisionWorld) {
        Ray down;
        down.origin[0] = spawn.x; down.origin[1] = spawn.y + 6.0f;
        down.origin[2] = spawn.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f;
        down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) spawn.y = hit.point[1];
    }
    EntityID e = m_entities.Create();
    Transform t;
    t.position = spawn + glm::vec3(0.0f, 0.06f, 0.0f);
    t.scale = glm::vec3(radius * 1.6f, 0.12f, radius * 1.6f);
    m_entities.AddTransform(e, t);
    MeshComponent mc;
    mc.isBox = true;
    m_entities.AddMesh(e, mc);
    CollisionComponent cc;
    cc.boundsMin = glm::vec3(-0.5f); cc.boundsMax = glm::vec3(0.5f);
    cc.layerMask = 4u;
    m_entities.AddCollision(e, cc);

    BossZone z;
    z.id = e;
    z.pos = spawn;
    z.life = life;
    z.radius = radius;
    z.damage = damage;
    z.tick = 0.0f; // first tick on entry
    m_bossZones.push_back(z);
    Logger::Info("BOSS zone: r=%.1f dmg=%d life=%.1fs at (%.1f, %.1f)",
                 radius, damage, life, spawn.x, spawn.z);
}

void Engine::UpdateBossZones(float dt) {
    if (m_bossZones.empty()) return;
    const Transform* jt = (m_jimmyActive && m_jimmyId.IsValid())
        ? m_entities.GetTransform(m_jimmyId) : nullptr;
    for (size_t i = 0; i < m_bossZones.size();) {
        BossZone& z = m_bossZones[i];
        z.life -= dt;
        if (z.life <= 0.0f || !m_entities.IsAlive(z.id)) {
            if (z.id.IsValid() && m_entities.IsAlive(z.id))
                m_entities.Destroy(z.id);
            m_bossZones.erase(m_bossZones.begin() + long(i));
            continue;
        }
        z.tick -= dt;
        if (jt && z.tick <= 0.0f) {
            const glm::vec3 d = jt->position - z.pos;
            if (glm::length(glm::vec2(d.x, d.z)) <= z.radius) {
                z.tick = 0.8f;
                DamagePlayer(z.damage, "boss zone");
            }
        }
        ++i;
    }
}

// --- boss FSM ---------------------------------------------------------------

// Per-frame boss brain: QTEs and phase bookkeeping first, then specials, then
// the melee-locked wait, then the chase/attack pattern. Replaces the regular
// Track 27 states for this entity (routed from UpdateNpcs).
void Engine::UpdateBossEntity(EntityID id, Transform& t, AIComponent& ai,
                              float dt) {
    BossComponent* bc = m_entities.GetBoss(id);
    if (!bc) return; // not a boss: regular AI handles it
    const BossDefinition& def = GetBossDefinition(bc->type);

    auto faceYaw = [&](float want) {
        float d = want - ai.facingYaw;
        while (d >  3.14159265f) d -= 6.28318531f;
        while (d < -3.14159265f) d += 6.28318531f;
        ai.facingYaw += d * std::min(1.0f, ai.turnSpeed * dt);
        t.rotation.y = ai.facingYaw;
    };

    // --- QTE + cooldowns (run even while rooted) ---------------------------
    UpdateBossQTE(id, *bc, dt);
    if (bc->specialTimer > 0.0f) bc->specialTimer -= dt;
    if (bc->phaseFlash > 0.0f)   bc->phaseFlash -= dt;

    // --- player reference ---------------------------------------------------
    glm::vec3 playerPos(0.0f);
    bool havePlayer = false;
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        if (const Transform* jt = m_entities.GetTransform(m_jimmyId)) {
            playerPos = jt->position;
            havePlayer = true;
        }
    }
    const glm::vec3 pos = t.position;
    const float dist = havePlayer
        ? glm::length(glm::vec2(playerPos.x - pos.x, playerPos.z - pos.z))
        : 1e9f;

    // --- engagement ----------------------------------------------------------
    if (!bc->engaged) {
        // Sighted from 16u (or once damaged): face + banner, then hunt.
        const bool seen = havePlayer && dist <= ai.sightRadius;
        if (seen || ai.health.current < ai.health.max) {
            bc->engaged = true;
            ai.targetEntity = m_jimmyId;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s - FIGHT!",
                          GetBossDefinition(bc->type).name);
            BossBanner(buf, 1, 2.2f);
            Logger::Info("BOSS %s ENGAGED (dist %.1f)",
                         GetBossDefinition(bc->type).name, dist);
        } else {
            UpdateEntityAnimation(id, false, dt); // idle until sighted
            return;
        }
    }

    // --- phases ---------------------------------------------------------------
    const float frac = def.maxHealth > 0
        ? float(ai.health.current) / float(def.maxHealth) : 0.0f;
    const BossPhase want = frac > 0.66f ? BossPhase::One
                         : frac > 0.33f ? BossPhase::Two
                         : BossPhase::Three;
    if (want != bc->phase) {
        bc->phase = want;
        bc->phaseFlash = 2.0f;
        bc->enrageMult = want == BossPhase::Two ? kBossPhase2Mult
                       : want == BossPhase::Three ? kBossPhase3Mult : 1.0f;
        bc->specialCd = def.specialCooldown *
            (want == BossPhase::Two ? kBossPhase2Cd
             : want == BossPhase::Three ? kBossPhase3Cd : 1.0f);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s - PHASE %s!",
                      GetBossDefinition(bc->type).name,
                      want == BossPhase::Two ? "II" : "III");
        BossBanner(buf, 2, 2.0f);
        Logger::Info("BOSS %s -> PHASE %s (HP %.0f%%)",
                     GetBossDefinition(bc->type).name,
                     want == BossPhase::Two ? "II" : "III", frac * 100.0f);
        // Phase-shock QTE: dodge the transition burst (success = no damage).
        if (havePlayer && dist <= 6.0f)
            StartBossQTE(*bc, bc->phase == BossPhase::Three
                              ? kQteWindowPhase3 : kQteWindowBase,
                         /*force=*/true);
        else if (havePlayer)
            DamagePlayer(8, "phase shockwave"); // out of position: eat it
    }

    // --- melee progress (stun/attack ticks run under everything) ------------
    if (Melee::CombatComponent* mc = m_entities.GetMeleeCombat(id)) {
        if (mc->state == Melee::CombatState::Attacking)
            ProgressNpcAttack(id, mc, dt);
    }

    // --- active special owns the body ----------------------------------------
    if (bc->special.kind != BossSpecial::None) {
        UpdateBossSpecial(id, *bc, ai, t, playerPos, havePlayer, dt);
        UpdateEntityAnimation(id, bc->special.kind == BossSpecial::Charge &&
                                 bc->special.stage == 2, dt);
        return;
    }
    // Stunned/knocked down: rooted until the melee tick restores Idle.
    if (Melee::CombatComponent* mc = m_entities.GetMeleeCombat(id)) {
        if (mc->state != Melee::CombatState::Idle) {
            UpdateEntityAnimation(id, false, dt);
            return;
        }
    }

    // --- pursuit + basic pattern --------------------------------------------
    ai.loseTimer = 0.0f; // bosses never give up
    if (ai.attackCdTimer > 0.0f) ai.attackCdTimer -= dt;

    // One-shot transition into CHASE (from any dormant state).
    if (ai.state != AIState::Chase && ai.state != AIState::Attack) {
        ai.state = AIState::Chase;
        ai.targetEntity = m_jimmyId;
        ai.stuckTimer = 0.0f; ai.sidestepTimer = 0.0f; ai.lastPos = pos;
        Logger::Info("BOSS AI: entity %u -> CHASE (dist %.1f)", id.index, dist);
    }

    // Special trigger: cadence + range + not mid-swing.
    if (havePlayer && bc->specialTimer <= 0.0f && dist <= def.specialRange &&
        dist > 1.2f && ai.attackCdTimer <= 0.0f && bc->qteState != QteState::Waiting) {
        const BossSpecial pick = PickBossSpecial(*bc, def, dist);
        if (pick != BossSpecial::None) {
            StartBossSpecial(id, *bc, pick);
            UpdateEntityAnimation(id, false, dt);
            return;
        }
    }

    if (dist <= ai.attackRange) {
        ai.state = AIState::Attack;
        if (havePlayer) {
            faceYaw(std::atan2(playerPos.x - pos.x, playerPos.z - pos.z));
            if (ai.attackCdTimer <= 0.0f) {
                // Phase-scaled move mix: P1 jabs, P2 heavies, P3 everything.
                Melee::AttackType atk = Melee::AttackType::LightPunch;
                const float r = nextRandom01();
                if (bc->phase == BossPhase::Three)
                    atk = r < 0.45f ? Melee::AttackType::HeavyPunch
                        : r < 0.75f ? Melee::AttackType::HeavyKick
                        : Melee::AttackType::LightPunch;
                else if (bc->phase == BossPhase::Two)
                    atk = r < 0.5f ? Melee::AttackType::HeavyPunch
                        : Melee::AttackType::LightPunch;
                StartNpcAttack(id, atk, ai.facingYaw);
                ai.attackCdTimer = def.attackCooldown * bc->enrageMult;
                Logger::Info("BOSS %s: %s swing (cd %.2fs)",
                             GetBossDefinition(bc->type).name,
                             Melee::GetAttackData(atk).clipName,
                             ai.attackCdTimer);
            }
        }
        UpdateEntityAnimation(id, false, dt);
        return;
    }

    // CHASE: direct steer with the shared stuck/sidestep heuristic.
    ai.state = AIState::Chase;
    const float chaseSpeed = def.walkSpeed * 1.7f;
    if (havePlayer) {
        glm::vec3 dir = playerPos - pos;
        dir.y = 0.0f;
        const float len = glm::length(dir);
        if (len > 1e-4f) {
            dir /= len;
            if (ai.sidestepTimer > 0.0f) {
                ai.sidestepTimer -= dt;
                dir = ai.sidestepDir;
            } else {
                const float expected = chaseSpeed * dt + 1e-5f;
                const float moved = glm::length(pos - ai.lastPos);
                ai.lastPos = pos;
                if (moved < expected * 0.4f) ai.stuckTimer += dt;
                else ai.stuckTimer = std::max(0.0f, ai.stuckTimer - dt * 2.0f);
                if (ai.stuckTimer >= ai.stuckTime) {
                    const float side = nextRandom01() < 0.5f ? -1.0f : 1.0f;
                    ai.sidestepDir = glm::vec3(-dir.z, 0.0f, dir.x) * side;
                    ai.sidestepTimer = 0.7f;
                    ai.stuckTimer = 0.0f;
                }
            }
            glm::vec3 np;
            StepMoveGround(pos, dir, chaseSpeed, dt, np);
            if (glm::distance(np, pos) > 1e-4f) t.position = np;
            faceYaw(std::atan2(dir.x, dir.z));
        }
    }
    UpdateEntityAnimation(id, true, dt);
}

// --- victory / defeat -------------------------------------------------------

void Engine::BossBanner(const char* text, int kind, float dur) {
    std::snprintf(m_bossBanner, sizeof(m_bossBanner), "%s", text ? text : "");
    m_bossBannerKind = kind;
    m_bossBannerTimer = dur;
    m_bossBannerDur = dur;
}

// Boss at 0 HP: victory banner + cutscene + boss_duel mission completion.
// Cronies scatter (they lost their leader).
void Engine::OnBossDefeated(EntityID npc) {
    if (m_boss.victory) return;
    m_boss.victory = true;
    m_boss.victoryAt = m_time;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s DEFEATED!",
                  GetBossDefinition(m_boss.type).name);
    BossBanner(buf, 3, 4.0f);
    Logger::Info("BOSS VICTORY: %s down after %d retries, %d/%d QTEs won",
                 GetBossDefinition(m_boss.type).name, m_boss.retryCount,
                 m_entities.GetBoss(npc) ? m_entities.GetBoss(npc)->qteWins : 0,
                 m_entities.GetBoss(npc) ? m_entities.GetBoss(npc)->qteCount : 0);

    // Cronies: flee the scene.
    const Transform* bt = m_entities.GetTransform(npc);
    for (EntityID c : m_bossCronies) {
        AIComponent* ai = m_entities.GetAI(c);
        if (!ai || !bt) continue;
        ai->state = AIState::Flee;
        ai->threatPos = bt->position;
        ai->targetEntity = EntityID{};
    }
    m_bossCronies.clear();

    // Track 48 integration: play the authored victory scene (camera keys +
    // subtitles). Missing data is non-fatal — the fallback orbit in
    // UpdateBossCutsceneCamera covers the no-JSON case.
    if (const Cutscene* cs = m_cutscenes.Find("boss_victory"))
        StartCutscene(*cs);

    // Mission hook: complete the boss_duel objectives (defeat + report).
    for (uint32_t mi = 0; mi < m_missions.MissionCount(); ++mi) {
        const Mission* mm = m_missions.GetMission(mi);
        if (!mm || mm->id != "boss_duel") continue;
        if (mm->state == MissionState::Inactive) m_missions.ActivateMission(mi);
        for (size_t oi = mm->currentObjective;
             oi < mm->objectives.size(); ++oi) {
            if (mm->objectives[oi].complete) continue;
            m_missions.CompleteObjective(mi, oi);
        }
        Logger::Info("BOSS VICTORY: mission 'boss_duel' completed");
        break;
    }
}

// Player down during the fight: respawn at the checkpoint with full health,
// reset the boss to full HP at its spawn point, clear zones + QTE state.
void Engine::OnPlayerDefeatedByBoss() {
    m_boss.retryCount++;
    BossBanner("DEFEATED - RETRYING", 4, 2.5f);
    Logger::Info("BOSS DEFEAT: player down (retry #%d) - respawn at checkpoint",
                 m_boss.retryCount);

    // Player: checkpoint + full health + brief spawn protection.
    if (m_jimmyId.IsValid()) {
        if (PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
            pc->health.current = pc->health.max;
            pc->state = DefenseState::Normal;
            pc->invincibleTimer = 1.5f;
        }
        if (Transform* jt = m_entities.GetTransform(m_jimmyId))
            jt->position = m_jimmySpawn + glm::vec3(0.0f, 2.0f, 0.0f);
        if (Melee::CombatComponent* mc = m_entities.GetMeleeCombat(m_jimmyId)) {
            mc->state = Melee::CombatState::Idle;
            mc->stunTimer = 0.0f;
            mc->ResetCombo();
        }
    }

    // Zones + QTE reset.
    for (const BossZone& z : m_bossZones)
        if (z.id.IsValid() && m_entities.IsAlive(z.id)) m_entities.Destroy(z.id);
    m_bossZones.clear();

    // Boss: full reset (retry loop must be beatable from scratch).
    if (m_boss.entity.IsValid() && m_entities.IsAlive(m_boss.entity)) {
        if (AIComponent* ai = m_entities.GetAI(m_boss.entity)) {
            const BossDefinition& def = GetBossDefinition(m_boss.type);
            ai->health.current = def.maxHealth;
            ai->state = AIState::Idle;
            ai->targetEntity = EntityID{};
            ai->stuckTimer = 0.0f; ai->sidestepTimer = 0.0f;
            if (Transform* t = m_entities.GetTransform(m_boss.entity)) {
                t->position = m_boss.spawnPos;
                t->rotation.x = 0.0f;
            }
        }
        if (Melee::CombatComponent* mc =
                m_entities.GetMeleeCombat(m_boss.entity)) {
            mc->state = Melee::CombatState::Idle;
            mc->stunTimer = 0.0f;
            mc->downTimer = 0.0f;
        }
        if (BossComponent* bc = m_entities.GetBoss(m_boss.entity)) {
            bc->phase = BossPhase::One;
            bc->enrageMult = 1.0f;
            bc->special = BossSpecialRun{};
            bc->specialTimer = GetBossDefinition(bc->type).specialCooldown;
            bc->qteState = QteState::None;
            bc->qteTimer = 0.0f;
            bc->resultTimer = 0.0f;
            bc->engaged = false;
        }
    }
}

// Victory cutscene camera: 3.5s slow orbit around the fallen boss.
bool Engine::UpdateBossCutsceneCamera(float dt) {
    if (!m_boss.victory || m_time - m_boss.victoryAt > 3.5) return false;
    const Transform* bt = m_boss.entity.IsValid()
        ? m_entities.GetTransform(m_boss.entity) : nullptr;
    if (!bt) return false;
    const double k = (m_time - m_boss.victoryAt) / 3.5;
    const float ang = float(k) * 2.4f + 0.6f; // ~140 deg sweep
    const float r = 4.5f - float(k) * 1.2f;   // slow push-in
    const glm::vec3 center = bt->position + glm::vec3(0.0f, 1.2f, 0.0f);
    const glm::vec3 eye = center + glm::vec3(std::sin(ang) * r, 1.6f,
                                             std::cos(ang) * r);
    m_camera.SetLookAt(glm::normalize(center - eye));
    m_camera.SetPosition(eye);
    return true;
}

// Per-frame bookkeeping that runs outside the boss entity's own AI: zone
// ticks, banner expiry, stale-handle cleanup and the player-death retry.
void Engine::UpdateBosses(float dt) {
    // Deferred boss spawns (zones / cronies): entity creation happens here,
    // safely outside UpdateNpcs' ForEachAI iteration.
    FlushBossSpawns();
    UpdateBossZones(dt);
    if (m_bossBannerTimer > 0.0f) m_bossBannerTimer -= dt;

    if (m_boss.entity.IsValid() && !m_entities.IsAlive(m_boss.entity))
        m_boss.entity = EntityID{}; // destroyed elsewhere: drop the HUD ref

    if (!m_boss.entity.IsValid() || m_boss.victory || !m_jimmyActive ||
        !m_jimmyId.IsValid())
        return;
    BossComponent* bc = m_entities.GetBoss(m_boss.entity);
    PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
    const Transform* bt = m_entities.GetTransform(m_boss.entity);
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!bc || !pc || !bt || !jt) return;
    const float dist = glm::length(glm::vec2(bt->position.x - jt->position.x,
                                             bt->position.z - jt->position.z));
    if (bc->engaged && pc->health.current <= 0 && dist < 30.0f)
        OnPlayerDefeatedByBoss();
}

// OPENBULLY_BOSS_DEMO=1: scripted headless verification of the whole fight
// loop (no OS input needed). The demo clock pauses while a cutscene owns the
// input, so the intro scene does not eat the schedule:
//   t 0-6    walk (sprint) to the boss -> engagement banner must fire.
//   t 6-14   hold block + swing on a 0.6s cadence; scripted -70 shocks at
//            t=8 and t=11 force the phase II/III transitions (and their
//            shockwave QTEs). Specials fire on their own cadence.
//   t 14-16  scripted player death -> checkpoint respawn + boss reset.
//   t 16+    chip 40 HP every 0.8s until the boss drops -> victory banner
//            + cutscene + mission complete.
// QTE answering: the first two prompts are pressed ~0.3s in (success path),
// later ones are deliberately ignored (failure path + player damage).
void Engine::DriveBossDemo(float dt) {
    if (!m_jimmyActive || !m_boss.entity.IsValid() ||
        !m_entities.IsAlive(m_boss.entity)) return;
    const EntityID boss = m_boss.entity;
    const Transform* bt = m_entities.GetTransform(boss);
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!bt || !jt) return;

    // Demo clock: paused while a cutscene owns the input (the intro cutscene
    // auto-plays on the spawn trigger and freezes the player for 13 s -
    // burning the phase gates against m_time would skip the whole fight).
    static double s_t = 0.0;
    if (!m_cutscenePlaying) s_t += dt;
    const double t = s_t;

    // Clear scripted buttons unless a phase below claims them.
    m_input.SetButton(Button::Forward, false);
    m_input.SetButton(Button::Sprint, false);
    m_input.SetButton(Button::Attack, false);
    m_input.SetButton(Button::Block, false);

    static bool s_died = false;
    static bool s_answerArmed = false;
    static double s_answerAt = -1e9;
    static int s_promptSeen = 0;
    static double s_lastSwing = -1e9;
    static double s_shock2 = -1.0, s_shock3 = -1.0;
    static double s_lastDmg = -1e9;

    if (t < 6.0) {
        // Walk to the boss: aim the follow camera at it and hold Forward.
        // Stop the moment Jimmy leaves the ground - the spawn rooftop is
        // small and the demo must never step off the edge (no recovery).
        const float dx = bt->position.x - jt->position.x;
        const float dz = bt->position.z - jt->position.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        const PhysicsComponent* jp = m_entities.GetPhysics(m_jimmyId);
        const bool grounded = jp ? jp->onGround : true;
        if (d > 2.4f && grounded) {
            const float yaw = std::atan2(dx, dz);
            const float pitch = m_camera.GetPitch();
            const float cp = std::cos(pitch), sp = std::sin(pitch);
            m_camera.SetLookAt(glm::vec3(std::sin(yaw) * cp, sp,
                                         std::cos(yaw) * cp));
            m_input.SetButton(Button::Forward, true);
            m_input.SetButton(Button::Sprint, true);
        }
    } else if (t < 14.0) {
        // Block hold + melee cadence + scripted phase shocks.
        m_input.SetButton(Button::Block, true);
        if (t - s_lastSwing >= 0.6) { s_lastSwing = t; m_input.SetButton(Button::Attack, true); }
        if (t >= 8.0 && s_shock2 < 0.0) {
            s_shock2 = t;
            if (AIComponent* ai = m_entities.GetAI(boss)) {
                if (ai->health.current > 0) {
                    DamageNpc(boss, m_jimmyId, 70, bt->position, 0.15f);
                    Logger::Info("BOSS demo: phase shock -70 (HP %d/%d)",
                                 std::max(0, ai->health.current), ai->health.max);
                }
            }
        }
        if (t >= 11.0 && s_shock3 < 0.0) {
            s_shock3 = t;
            if (AIComponent* ai = m_entities.GetAI(boss)) {
                if (ai->health.current > 0) {
                    DamageNpc(boss, m_jimmyId, 70, bt->position, 0.15f);
                    Logger::Info("BOSS demo: phase shock -70 (HP %d/%d)",
                                 std::max(0, ai->health.current), ai->health.max);
                }
            }
        }
    } else if (t < 16.0) {
        if (!s_died) {
            s_died = true;
            DamagePlayer(9999, "demo defeat");
            // Track 49: drive the retry directly (UpdateBosses' live-play
            // retry would also fire, but driving it here keeps the script
            // deterministic and logs the reset before the chip phase).
            OnPlayerDefeatedByBoss();
            Logger::Info("BOSS demo: scripted player defeat -> retry path");
        }
    } else {
        // Chip 20 HP every 0.8s: slow enough that enraged P3 specials fire
        // (charge / grab QTEs) so the QTE-FAILURE path (ignored prompt ->
        // TOO SLOW -> player damage) is exercised before the KO.
        if (t - s_lastDmg >= 0.8) {
            s_lastDmg = t;
            if (AIComponent* ai = m_entities.GetAI(boss)) {
                if (ai->health.current > 0)
                    DamageNpc(boss, m_jimmyId, 20, bt->position, 0.15f);
            }
        }
    }

    // QTE answering (works in every phase above).
    if (BossComponent* bc = m_entities.GetBoss(boss)) {
        if (bc->qteState == QteState::Waiting) {
            if (!s_answerArmed) {
                s_answerArmed = true;
                ++s_promptSeen;
                s_answerAt = t + 0.3;
                Logger::Info("BOSS demo: QTE #%d key '%c' -> %s",
                             s_promptSeen, BossQteKeyChar(bc->qteKey),
                             s_promptSeen <= 2 ? "press (success path)"
                                               : "ignore (failure path)");
            }
            if (s_answerArmed && t >= s_answerAt) {
                s_answerArmed = false;
                if (s_promptSeen <= 2)
                    m_input.SetRawKey(SDL_Scancode(bc->qteKey), true);
                // else: deliberately no press -> window expires -> failure.
            }
        } else {
            s_answerArmed = false;
        }
    }
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
    // Track 49: the boss demo pins the clock at noon - at 60x, a 55 s run
    // would otherwise hit the 23:00 curfew mid-fight and BUST the player out
    // of the scripted verification sequence.
    if (std::getenv("OPENBULLY_BOSS_DEMO"))
        m_gameTimeMinutes = 12.0 * 60.0;
    SyncGameClock();
}

// Derives the integer HUD clock from the fractional game-time accumulator.
// Track 41: wrapping past midnight bumps the day counter (0 = Monday) that
// class schedules read.
void Engine::SyncGameClock() {
    while (m_gameTimeMinutes >= 24.0 * 60.0) {
        m_gameTimeMinutes -= 24.0 * 60.0;
        ++m_gameDay;
    }
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
    // Track 42: faction membership from the model index (deterministic
    // clique round robin; prefects are overridden to Authority below).
    ai.faction = FactionForModel(sn.modelIndex);
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
            ai.faction = Faction::Authority; // Track 42: authorities
            if      (m_prefectIds[0].index == 0xFFFFFFFF) m_prefectIds[0] = e;
            else                                          m_prefectIds[1] = e;
            Logger::Info("NPC AI: entity %u designated PREFECT", e.index);
        }
    }
    m_entities.AddAI(e, ai);
    m_entities.AddMeleeCombat(e);

    // Track 47: scheduled NPCs are photo subjects under their schedule name
    // (student / townie / prefect / teacher) so PHOTOGRAPH_TARGET objectives
    // can require a specific crowd.
    if (const NPCSchedule* psched =
            m_schedules.GetSchedule(size_t(sn.scheduleIndex)))
        RegisterPhotoTarget(e, psched->name, Photo::TargetKind::Person, 1.8f);
    else
        RegisterPhotoTarget(e, "Student", Photo::TargetKind::Person, 1.8f);

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
        // Track 31: mounted vehicles widen the FOV (per-type); Track 47: photo
        // mode narrows it by the zoom. Single source: CurrentFovDeg().
        {
            const float fovDeg = CurrentFovDeg();
            if (fovDeg != m_lastFovDeg) {
                m_renderer->SetProjectionFov(fovDeg);
                m_lastFovDeg = fovDeg;
            }
        }
        const glm::mat4 view = m_camera.GetViewMatrix();
        m_renderer->SetCameraView(view);
        const glm::mat4 proj = m_renderer->GetProjection();

        // Track 35: day/night cycle — compute sky color and lighting from
        // the game clock. SetSkyColor and SetLight are read by DrawMesh3DRange
        // on every 3D draw below, so they must be set before the first submit.
        // Track 34: weather fog is set here too, using the WeatherManager's
        // current density and color.
        {
            const float hour = float(m_gameTimeMinutes) / 60.0f;
            const DayNight::State dn = DayNight::Compute(hour);
            
            // Compute weather-modified sky color (base sky * weather factor).
            WeatherManager& weather = WeatherManager::Get();
            float skyR = dn.sky[0], skyG = dn.sky[1], skyB = dn.sky[2];
            
            // Weather modifies the sky color based on current state and intensity.
            const float intensity = weather.GetIntensity();
            switch (weather.GetType()) {
                case WeatherType::CLEAR:
                    break; // no modification
                case WeatherType::RAIN:
                    skyR = skyR * (1.0f - intensity) + 0.18f * intensity;
                    skyG = skyG * (1.0f - intensity) + 0.22f * intensity;
                    skyB = skyB * (1.0f - intensity) + 0.28f * intensity;
                    break;
                case WeatherType::SNOW:
                    skyR = skyR * (1.0f - intensity) + 0.75f * intensity;
                    skyG = skyG * (1.0f - intensity) + 0.78f * intensity;
                    skyB = skyB * (1.0f - intensity) + 0.82f * intensity;
                    break;
                case WeatherType::FOG:
                    skyR = skyR * (1.0f - intensity) + 0.55f * intensity;
                    skyG = skyG * (1.0f - intensity) + 0.55f * intensity;
                    skyB = skyB * (1.0f - intensity) + 0.55f * intensity;
                    break;
            }
            
            m_renderer->SetSkyColor(skyR, skyG, skyB);
            
            // Track 34: set weather fog density and color.
            m_renderer->SetFog(weather.GetFogDensity(), weather.GetFogColor());
            
            const float ambient[3] = { dn.ambient, dn.ambient, dn.ambient };
            m_renderer->SetLight(dn.lightDir, dn.lightColor, ambient);
        }

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

        // Track 34: weather particles (rain drops, snowflakes, splashes).
        // Drawn after world geometry so they depth-test against the scene.
        WeatherManager::Get().Render(m_renderer.get(), view, proj);

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

        // Track 46: trick popup (top-center, below the melee damage popup)
        // + combo readout / chain-window meter bottom-left. Colors are ABGR
        // literals (alpha in the top byte; see UIRenderer::PackRGBA).
        if (m_tricks.popupTimer > 0.0f && m_tricks.popupText[0]) {
            const float w = m_ui.MeasureText(m_tricks.popupText);
            const float scale = 2.0f; // chunky, same as the melee popup
            const uint32_t trickGold = 0xff50d2ffu; // #FFD250
            m_ui.DrawText(0.5f * (float(m_screenWidth) - w * scale),
                          0.30f * float(m_screenHeight), m_tricks.popupText,
                          m_tricks.popupTimer < 0.4f ? trickGold : white);
        }
        if (m_tricks.active != TrickType::None || m_tricks.pending > 0 ||
            m_tricks.comboMult > 1) {
            const uint32_t trickGold = 0xff50d2ffu;
            const uint32_t trickDim = 0xff989898u;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "COMBO x%d", m_tricks.comboMult);
            m_ui.DrawText(24.0f, float(m_screenHeight) - 70.0f, buf, white);
            std::snprintf(buf, sizeof(buf), "BANKED %d   TOTAL %d",
                          m_tricks.pending, m_tricks.totalScore);
            m_ui.DrawText(24.0f, float(m_screenHeight) - 58.0f, buf, trickDim);
            const float frac = std::min(m_tricks.window, kTrickComboWindow) /
                               kTrickComboWindow;
            m_ui.DrawRect(24.0f, float(m_screenHeight) - 50.0f, 120.0f * frac,
                          6.0f, trickGold);
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

        // Track 48: cinematic subtitles for the playing cutscene (below the
        // modal overlays, above the HUD).
        if (m_cutscenePlaying) {
            if (const CutsceneLine* line = m_cutscenes.CurrentLine())
                m_ui.DrawSubtitle(line->speaker.c_str(), line->text,
                                  /*showSkipHint=*/true);
        }

        // Track 47: photo viewfinder (crosshair / zoom / bracket / flash)
        // above the HUD, below the modal overlays.
        DrawPhotoOverlayUI();

        // Track 40: quest journal overlay — drawn last, above every other
        // UI element (modal full-screen panel).
        DrawQuestJournal();

        // Track 41: class prompt, schedule overlay and session screen. Above
        // the journal so an open schedule is never double-dimmed by a stale
        // journal frame.
        DrawClassOverlays();

        // Track 44: minigame overlay (class activity / demo) — topmost;
        // it owns the whole screen while an attempt is live.
        DrawMinigameOverlay();

        // Track 45: arcade cabinet screen — above the minigame overlay so an
        // open cabinet owns the whole display; the world prompt shows only
        // while the view is closed (and dialogue is not speaking).
        if (m_arcade.IsActive()) {
            m_arcade.Render(m_ui, float(m_screenWidth), float(m_screenHeight));
        } else if (m_arcade.NearCabinet() && !m_dialogue.IsActive() &&
                   !m_cutscenePlaying) {
            const char* prompt = m_arcade.Prompt();
            const float pw = m_ui.MeasureText(prompt);
            const float px = 0.5f * (float(m_screenWidth) - pw);
            const float py = float(m_screenHeight) - 100.0f;
            m_ui.DrawRect(px - 10.0f, py - 6.0f, pw + 20.0f, 22.0f,
                          0xe0101018u);
            m_ui.DrawText(px, py, prompt, 0xff70d2ffu); // #FFD270 gold
        }

        // Track 42: faction interaction readout + TAB stats screen (drawn
        // last so the verification screenshot always captures it).
        DrawFactionStats();
    }

    m_renderer->FlushScreenshot(); // flush pending screenshot before present
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
    for (int wi = 1; wi <= 5; ++wi) { // table order: Bat..Plank (skip Unarmed)
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
        { 3.0f, 0.0f, -0.5f },
    };
    for (size_t i = 0; i < m_weaponModels.size(); ++i) {
        const WeaponModel& wmodel = m_weaponModels[i];
        const WeaponData& data = GetWeaponDataById(wmodel.id);
        glm::vec3 pos = center + kOffsets[i % 5];
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
        // Track 39: ranged demo. Aim for the first seconds (crosshair proof),
        // then walk to the NPC crowd and fire body-facing shots at the nearest
        // target (damage + reaction proof). The spawn sits on a roof shell
        // with no collision beneath, so aimed arcs there never hit anything.
        if (m_time >= 2.0 && pc->equippedSlot >= 0 &&
            pc->slots[pc->equippedSlot].type == WeaponType::RangedProjectile) {
            m_aiming = (m_time < 4.0);
            if (!m_aiming) {
                EntityID tgt{};
                float bd = 60.0f;
                m_entities.ForEachAI([&](EntityID id, const Transform& nt,
                                          const AIComponent& ai) {
                    if (ai.state == AIState::Ko) return;
                    const float d = glm::length(nt.position - jt->position);
                    if (d < bd) { bd = d; tgt = id; }
                });
                // No NPC nearby (schedules rotate the crowd out): spawn a
                // guaranteed static target 8u in front of Jimmy, once.
                static bool s_targetSpawned = false;
                if (!tgt.IsValid() && !s_targetSpawned && m_testBoxValid) {
                    EntityID e = m_entities.Create();
                    Transform t;
                    t.position = jt->position + glm::vec3(0.0f, -1.0f, 10.0f); // roof plane
                    m_entities.AddTransform(e, t);
                    MeshComponent mc;
                    mc.isBox = true;
                    m_entities.AddMesh(e, mc);
                    AIComponent ai;
                    ai.state = AIState::Idle;
                    ai.lockIdle = true;
                    ai.health.current = 60;
                    m_entities.AddAI(e, ai);
                    Melee::CombatComponent mmc;
                    m_entities.AddMeleeCombat(e, mmc);
                    s_targetSpawned = true;
                    Logger::Info("Ranged demo: target box NPC spawned at (%.1f, %.1f, %.1f)",
                                 t.position.x, t.position.y, t.position.z);
                    tgt = e;
                }
                if (tgt.IsValid()) {
                    if (const Transform* ntt = m_entities.GetTransform(tgt)) {
                        if (Transform* pt = m_entities.GetTransform(m_jimmyId)) {
                            const glm::vec3 to = ntt->position - jt->position;
                            const float d = glm::length(to);
                            if (d > 6.0f)
                                pt->position += (to / d) * (4.0f * float(dt));
                            // Hold the spawn-roof problem away: keep Jimmy on
                            // the target's plane (the shell under the spawn has
                            // no collision, so he free-falls without this).
                            pt->position.y = ntt->position.y;
                            if (PhysicsComponent* pp = m_entities.GetPhysics(m_jimmyId))
                                pp->velocity.y = 0.0f;
                            pc->facingYaw   = std::atan2(to.x, to.z);
                            pc->facingValid = true;
                        }
                    } else {
                        Logger::Info("Ranged demo: tgt transform NULL");
                    }
                } else {
                    static double s_noTgtLog = 0.0;
                    if (m_time - s_noTgtLog > 2.0) {
                        s_noTgtLog = m_time;
                        Logger::Info("Ranged demo: no NPC target in 60u yet");
                    }
                }
            }
        }
        if (m_time >= 2.5 && pc->equippedSlot >= 0 &&
            pc->slots[pc->equippedSlot].type == WeaponType::RangedProjectile) {
            // Demo drives its own cadence (the cooldown gate stalls once the
            // rock is stuck in a vehicle); clearing it lets the shot fire.
            static double s_lastShot = -10.0;
            if (m_time - s_lastShot > 0.9) {
                s_lastShot = m_time;
                pc->attackCooldown = 0.0f;
                WeaponTryAttack(pc);
            }
        }
    }

    // --- Pickup: E near a dropped weapon, or walk-over (Track 38) -----------
    // Track 30: E is the dialogue key; a conversation takes precedence.
    // Track 40: the quest journal is modal — no E pickup while it is open.
    // Track 41: class UI (prompt/schedule/session) takes E first.
    // Track 45: the arcade view owns E (and the press that opens a cabinet
    // must not also grab a pickup in the same frame).
    // Track 38: walking over a pickup also collects it (1.2u radius).
    EntityID best{};
    if (m_input.WasKeyPressed(SDL_SCANCODE_E) && !m_dialogue.IsActive() &&
        !m_journalOpen && !m_scheduleOpen && !m_classSessionActive &&
        !m_cutscenePlaying && // Track 48: the cutscene owns E-free input
        !m_arcade.IsActive() && !m_arcade.NearCabinet() && // Track 45
        m_classPromptIdx < 0) {
        float bestDist = 2.2f; // E-key pickup radius
        m_entities.ForEach([&](EntityID id, Transform& t,
                               const EntityManager::EntityRecord&) {
            if (id == m_jimmyId) return;
            if (!m_entities.GetWeapon(id)) return;
            const float d = glm::length(t.position - jt->position);
            if (d < bestDist) { bestDist = d; best = id; }
        });
    } else {
        float bestDist = 1.2f; // walk-over radius
        m_entities.ForEach([&](EntityID id, Transform& t,
                               const EntityManager::EntityRecord&) {
            if (id == m_jimmyId) return;
            if (!m_entities.GetWeapon(id)) return;
            const float d = glm::length(t.position - jt->position);
            if (d < bestDist) { bestDist = d; best = id; }
        });
    }
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

    // Slot selection: 1-5 (deferred during dialogue: 1-4 pick choices;
    // deferred while the quest journal is open — Track 40 modality; also
    // held while a cutscene plays — Track 48).
    static const SDL_Scancode kSlotKeys[5] = {
        SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
    };
    for (int i = 0;
         i < 5 && !m_dialogue.IsActive() && !m_journalOpen && !m_cutscenePlaying;
         ++i) {
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

    // --- Unequip: 0 goes back to fists (Track 38) ---------------------------
    if (!m_dialogue.IsActive() && !m_journalOpen &&
        m_input.WasKeyPressed(SDL_SCANCODE_0) &&
        pc->equippedSlot >= 0) {
        pc->equippedSlot = -1;
        Logger::Info("Unequipped (fists)");
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
    // Track 47: the camera does not attack — its LMB is the shutter, handled
    // by TakePhoto inside photo mode (and a no-op outside it).
    if (data.type == WeaponType::Camera) return;

    const glm::vec3 fwd(std::sin(pc->facingYaw), 0.0f, std::cos(pc->facingYaw));
    int damage = data.damage;
    if (pc->counterArmed && data.type != WeaponType::RangedProjectile) {
        damage = int(float(damage) * 4.0f); // Track 25 counter bonus (melee only)
        pc->counterArmed = false;
        pc->state = DefenseState::Normal;
        Logger::Info("COUNTER ATTACK with %s!", data.name);
    }
    pc->attackCooldown = data.attackCooldown;

    bool hitSomething = false;
    if (data.type == WeaponType::RangedProjectile) {
        // --- Slingshot: ammo gate + projectile spawn --------------------------
        // Track 39: reload (auto on empty, or manual R) before giving up; fire
        // toward the camera aim direction when aiming, body facing otherwise.
        if (slot.ammo <= 0) {
            const int reloaded = std::min(SlingshotCapacity(), kAmmoPerBox);
            slot.ammo = reloaded;
            Logger::Info("%s: auto-reloaded to %d rocks", data.name, slot.ammo);
        }
        slot.ammo--;
        glm::vec3 dir = fwd;
        if (m_aiming) {
            // Camera aim: yaw is live, pitch is clamped (see kAimPitchMin/Max
            // above) so the follow camera's downward view doesn't fire the
            // rock into the ground.
            const float cy = std::cos(m_camera.GetYaw()), sy = std::sin(m_camera.GetYaw());
            const float pitch = glm::clamp(m_camera.GetPitch(), kAimPitchMin, kAimPitchMax);
            const float cp = std::cos(pitch), sp = std::sin(pitch);
            dir = glm::vec3(sy * cp, sp, cy * cp);
        }
        const glm::vec3 origin = jt->position + glm::vec3(0.0f, 1.3f, 0.0f) + dir * 0.5f;
        const glm::vec3 vel = dir * 22.0f;
        const float kb = m_aiming ? kRockKnockback : kRockKnockback * 0.5f;
        SpawnProjectile(origin, vel, damage, m_jimmyId, kb);
        Logger::Info("%s fired (ammo left %d, damage %d%s)",
                     data.name, slot.ammo, damage, m_aiming ? ", aimed" : "");
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
            // Track 38: weapon knockback impulse (attacker-relative), same
            // decay path as the punch knockback in UpdateCombat.
            if (Melee::CombatComponent* tmc = m_entities.GetMeleeCombat(id)) {
                glm::vec3 kdir = t.position - jt->position;
                kdir.y = 0.0f;
                const float klen = glm::length(kdir);
                if (klen > 1e-4f)
                    tmc->knockbackVel = (kdir / klen) * data.knockback +
                                        glm::vec3(0.0f, 0.6f, 0.0f);
            }
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
// Track 39: pool-capped at kMaxProjectiles (oldest rock is recycled first —
// the newest shot wins); each carries an owner + knockback for NPC hits.
void Engine::SpawnProjectile(const glm::vec3& pos, const glm::vec3& vel, int dmg,
                             EntityID owner, float knockback) {
    if (!m_testBoxValid) return;
    // Pool cap: recycle the oldest live projectile (front of m_projectiles).
    if (m_projectiles.size() >= size_t(kMaxProjectiles)) {
        const Projectile& old = m_projectiles.front();
        if (old.id.IsValid() && m_entities.IsAlive(old.id)) m_entities.Destroy(old.id);
        m_projectiles.erase(m_projectiles.begin());
        Logger::Info("Projectile pool full: recycled oldest (cap %d)", kMaxProjectiles);
    }
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
    p.owner = owner;
    p.knockback = knockback;
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

// Track 39: short-lived impact debris puff at a projectile hit point.
void Engine::SpawnImpactFx(const glm::vec3& pos) {
    if (!m_testBoxValid) return;
    for (int i = 0; i < 4; ++i) {
        EntityID e = m_entities.Create();
        Transform t;
        t.position = pos;
        t.scale = glm::vec3(0.05f + 0.02f * float(i % 2));
        m_entities.AddTransform(e, t);
        MeshComponent mc;
        mc.isBox = true;
        m_entities.AddMesh(e, mc);
        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-0.5f); cc.boundsMax = glm::vec3(0.5f);
        cc.layerMask = 4u;
        m_entities.AddCollision(e, cc);
        const float a = float(i) * 1.5708f + 0.4f;
        ImpactFx fx;
        fx.id = e;
        fx.velocity = glm::vec3(std::cos(a) * 1.5f, 2.0f + 0.5f * float(i % 3),
                                std::sin(a) * 1.5f);
        fx.life = 0.35f;
        m_impacts.push_back(fx);
    }
}

// Track 39: spawn kAmmoBoxCount ammo boxes around `center` (ground-snapped).
// Walk-over adds kAmmoPerBox rocks to the equipped/first slingshot slot.
void Engine::SpawnAmmoBoxes(const glm::vec3& center) {
    static const glm::vec3 kOffsets[] = {
        { -4.0f, 0.0f,  3.0f }, {  5.0f, 0.0f,  2.0f }, {  1.0f, 0.0f, -5.0f },
    };
    for (const glm::vec3& off : kOffsets) {
        glm::vec3 pos = center + off;
        if (m_collisionWorld) {
            Ray down;
            down.origin[0] = pos.x; down.origin[1] = pos.y + 6.0f; down.origin[2] = pos.z;
            down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
            RayHit hit;
            if (m_collisionWorld->Raycast(down, hit)) pos.y = hit.point[1];
        }
        EntityID e = m_entities.Create();
        Transform t;
        t.position = pos + glm::vec3(0.0f, 0.3f, 0.0f);
        t.scale = glm::vec3(0.35f);
        m_entities.AddTransform(e, t);
        MeshComponent mc;
        mc.isBox = true;
        m_entities.AddMesh(e, mc);
        CollisionComponent cc;
        cc.boundsMin = glm::vec3(-0.5f); cc.boundsMax = glm::vec3(0.5f);
        cc.layerMask = 2u;
        m_entities.AddCollision(e, cc);
        // No WeaponComponent: an ammo box is ammo, not a weapon, so the
        // E-pickup path in UpdateWeapons must not hand it out as a
        // 0-durability slingshot (walk-over collection handles it instead).
        m_ammoBoxIds.push_back(e);
        Logger::Info("Ammo box spawned at (%.1f, %.1f, %.1f) [%d rocks]",
                     pos.x, pos.y, pos.z, kAmmoPerBox);
    }
}

// Track 39: walk-over collection + pickup bob for ammo boxes.
void Engine::UpdateAmmoPickups(float dt) {
    if (m_ammoBoxIds.empty()) return;
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!pc || !jt) return;
    for (size_t i = 0; i < m_ammoBoxIds.size();) {
        EntityID id = m_ammoBoxIds[i];
        Transform* t = m_entities.GetTransform(id);
        if (!t) { m_ammoBoxIds.erase(m_ammoBoxIds.begin() + long(i)); continue; }
        t->rotation.y += dt * 1.5f;
        t->position.y += std::sin(float(m_time) * 2.0f + float(id.index)) * dt * 0.08f;
        if (glm::length(t->position - jt->position) < 1.2f) {
            // Add rocks to the equipped slingshot, else the first owned one.
            int slotIdx = -1;
            if (pc->equippedSlot >= 0 &&
                pc->slots[pc->equippedSlot].type == WeaponType::RangedProjectile)
                slotIdx = pc->equippedSlot;
            else for (int s = 0; s < 5; ++s)
                if (pc->slots[s].type == WeaponType::RangedProjectile) { slotIdx = s; break; }
            if (slotIdx >= 0) {
                InventorySlot& slot = pc->slots[slotIdx];
                const int before = slot.ammo;
                slot.ammo = std::min(SlingshotCapacity(), slot.ammo + kAmmoPerBox);
                Logger::Info("Ammo box collected: +%d rocks (slot %d, %d -> %d)",
                             slot.ammo - before, slotIdx, before, slot.ammo);
            } else {
                // No slingshot owned yet: leave the box for the E-pickup
                // path in UpdateWeapons (it gives the slingshot itself).
                continue;
            }
            m_entities.Destroy(id);
            m_ammoBoxIds.erase(m_ammoBoxIds.begin() + long(i));
            continue;
        }
        ++i;
    }
}

// Track 39: RMB with the slingshot equipped toggles aim mode (crosshair;
// movement stays free, camera stays player-controlled). R reloads manually.
// Track 47: the camera claims RMB the same way — it toggles the photo
// viewfinder instead (LMB becomes the shutter; see TakePhoto).
void Engine::UpdateAimMode(float dt) {
    (void)dt;
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc) { m_aiming = false; m_photoMode = false; return; }
    const bool slingshotOut = pc->equippedSlot >= 0 &&
        pc->slots[pc->equippedSlot].type == WeaponType::RangedProjectile;
    const bool cameraOut = pc->equippedSlot >= 0 &&
        pc->slots[pc->equippedSlot].type == WeaponType::Camera;
    if (!slingshotOut) {
        if (m_aiming) Logger::Info("Aim mode off (slingshot unequipped)");
        m_aiming = false;
    }
    if (!cameraOut) {
        if (m_photoMode) Logger::Info("Photo mode off (camera unequipped)");
        m_photoMode = false;
    }
    if ((!slingshotOut && !cameraOut) || m_journalOpen) {
        // Track 40: the quest journal is modal — aim/photo toggling and
        // manual reload are player combat inputs, so hold them while it is
        // open (buttons are already masked by the modal clear; raw keys like
        // R need an explicit gate). States themselves are preserved so
        // re-closing resumes mid-aim/mid-viewfinder.
        return;
    }
    // RMB toggles the equipped tool's mode. Block still works when unarmed,
    // so slingshot/camera claim RMB while equipped.
    if (m_input.IsPressed(Button::Block)) {
        if (cameraOut) {
            m_photoMode = !m_photoMode;
            if (m_photoMode) m_aiming = false;
            Logger::Info("Photo mode %s (zoom %.1fx)",
                         m_photoMode ? "ON (viewfinder)" : "OFF", m_photoZoom);
        } else {
            m_aiming = !m_aiming;
            if (m_aiming) m_photoMode = false;
            Logger::Info("Aim mode %s", m_aiming ? "ON (crosshair, camera aim)" : "OFF");
        }
    }
    if (!slingshotOut) return;
    // Manual reload: R.
    InventorySlot& slot = pc->slots[pc->equippedSlot];
    if (m_input.WasKeyPressed(SDL_SCANCODE_R) && slot.ammo < SlingshotCapacity()) {
        slot.ammo = std::min(SlingshotCapacity(), slot.ammo + kAmmoPerBox);
        Logger::Info("Reload: %d rocks", slot.ammo);
    }
}

// Integrates projectiles: gravity arc flight, world raycast per step,
// sphere hit vs NPC torsos (damage + knockback + headshot bonus), vehicle
// bounce, life expiry. Debris (damage 0) gets full gravity and hurts nobody.
void Engine::UpdateProjectiles(float dt) {
    if (m_projectiles.empty() && m_impacts.empty()) return;
    // --- Impact debris first (short-lived, gravity, no interaction) --------
    for (size_t i = 0; i < m_impacts.size();) {
        ImpactFx& fx = m_impacts[i];
        Transform* t = m_entities.GetTransform(fx.id);
        fx.life -= dt;
        if (!t || fx.life <= 0.0f) {
            if (t) m_entities.Destroy(fx.id);
            m_impacts.erase(m_impacts.begin() + long(i));
            continue;
        }
        fx.velocity.y -= 9.8f * dt;
        t->position += fx.velocity * dt;
        ++i;
    }
    for (size_t i = 0; i < m_projectiles.size();) {
        Projectile& p = m_projectiles[i];
        Transform* t = m_entities.GetTransform(p.id);
        if (!t) { // entity destroyed elsewhere: drop the record
            m_projectiles.erase(m_projectiles.begin() + long(i));
            continue;
        }
        p.life -= dt;
        if (p.damage > 0) { // flying rock: arc (gravity)
            p.velocity.y -= 9.8f * dt;
        } else { // debris: full gravity
            p.velocity.y -= 9.8f * dt;
        }
        const glm::vec3 oldPos = t->position;
        const glm::vec3 step = p.velocity * dt;

        bool kill = p.life <= 0.0f;
        bool bounced = false;  // hit something this frame; skip integration
        glm::vec3 fxPos(0.0f); // deferred impact FX spawn (AFTER the loops:
        bool fxKill = false;   //  creating entities mid-iteration reallocates
                               //  the component arrays and dangles references)
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
                fxPos = glm::make_vec3(hit.point); fxKill = true; // rock shatters
                Logger::Info("Projectile hit world at (%.1f, %.1f, %.1f)",
                             fxPos.x, fxPos.y, fxPos.z);
                // Track 43: smashing property with a thrown rock is vandalism
                // (+1 head) when an authority is close enough to see/hear it.
                if (p.owner == m_jimmyId && p.damage > 0)
                    VandalismCrime(fxPos);
                kill = true;
            }
        }
        // NPC hit: sphere vs torso/head (damage projectiles only).
        glm::vec3 hitDir(0.0f);
        if (!kill && p.damage > 0) {
            const glm::vec3 newPos = oldPos + step;
            // Track 38: an equipped garbage lid blocks projectiles — if Jimmy
            // stands in the rock's path, the shot shatters without damage.
            if (m_jimmyId.IsValid()) {
                if (const PlayerController* jpc = m_entities.GetPlayer(m_jimmyId)) {
                    if (jpc->equippedSlot >= 0 && jpc->equippedSlot < 5 &&
                        WeaponBlocksProjectiles(jpc->slots[jpc->equippedSlot].weaponId)) {
                        if (const Transform* jp = m_entities.GetTransform(m_jimmyId)) {
                            if (glm::length(newPos - (jp->position + glm::vec3(0.0f, 1.0f, 0.0f))) < 1.0f) {
                                fxPos = newPos; fxKill = true;
                                Logger::Info("Garbage lid blocked a projectile!");
                                kill = true;
                            }
                        }
                    }
                }
            }
            if (!kill) m_entities.ForEachAI([&](EntityID id, const Transform& nt, AIComponent& ai) {
                if (kill || id == m_jimmyId) return;
                if (ai.state == AIState::Ko) return; // downed: no re-trigger
                const glm::vec3 torso = nt.position + glm::vec3(0.0f, 1.0f, 0.0f);
                const glm::vec3 head  = nt.position + glm::vec3(0.0f, 1.55f, 0.0f);
                const bool headshot = glm::length(newPos - head) < 0.35f;
                const float hitR = headshot ? 0.35f : 0.8f;
                if (headshot || glm::length(newPos - torso) < hitR) {
                    // Track 39: headshot x2; knockback impulse along the
                    // projectile velocity (XZ) via the melee knockback field.
                    const int dmg = headshot ? p.damage * 2 : p.damage;
                    // No entity create/destroy in here: DamageNpc only queues
                    // events; FX spawn is deferred to after this loop.
                    DamageNpc(id, p.owner.IsValid() ? p.owner : m_jimmyId,
                              dmg, nt.position, 0.4f);
                    if (Melee::CombatComponent* tmc = m_entities.GetMeleeCombat(id)) {
                        glm::vec3 dir(p.velocity.x, 0.0f, p.velocity.z);
                        const float len = glm::length(dir);
                        if (len > 1e-4f)
                            tmc->knockbackVel = (dir / len) * p.knockback +
                                                glm::vec3(0.0f, 0.4f, 0.0f);
                    }
                    fxPos = newPos; fxKill = true;
                    hitDir = nt.position; // for the log below
                    Logger::Info("Projectile hit NPC idx=%u for %d%s",
                                 id.index, dmg, headshot ? " HEADSHOT" : "");
                    kill = true;
                }
            });
        }
        // Track 39: vehicle hit — bounce off (simple elastic kick up).
        if (!kill && p.damage > 0) {
            const glm::vec3 newPos = oldPos + step;
            for (EntityID vid : m_vehicleIds) {
                glm::vec3 bmin, bmax;
                if (!m_entities.GetWorldAABB(vid, bmin, bmax)) continue;
                if (glm::any(glm::lessThan(newPos, bmin)) ||
                    glm::any(glm::greaterThan(newPos, bmax))) continue;
                glm::vec3 n = newPos - 0.5f * (bmin + bmax);
                if (glm::length(n) < 1e-3f) n = glm::vec3(0.0f, 1.0f, 0.0f);
                n = glm::normalize(n);
                n.y = std::max(n.y, 0.5f);
                p.velocity = glm::reflect(p.velocity, n) * 0.5f;
                // Push the rock out of the hull along the normal, otherwise
                // the end-of-loop integration puts it back inside and the
                // bounce repeats every frame forever.
                const float push = 0.5f * glm::length(bmax - bmin) + 0.15f;
                t->position = 0.5f * (bmin + bmax) + n * push;
                fxPos = newPos; fxKill = false; // bounce keeps the rock alive
                p.bounces++;
                if (p.bounces >= 3) kill = true; // spent: don't jitter forever
                Logger::Info("Projectile bounced off vehicle idx=%u%s",
                             vid.index, kill ? " (spent)" : "");
                bounced = !kill;
                break;
            }
        }
        if (fxKill) SpawnImpactFx(fxPos);   // deferred: safe to create now
        else if (bounced) SpawnImpactFx(fxPos);
        (void)hitDir;
        if (bounced) { ++i; continue; } // skip integration this frame

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
    // Track 38: swing arc while the attack cooldown is fresh — the prop
    // sweeps down/forward over ~0.25s, then returns to the carry pose.
    float swing = 0.0f;
    if (pc->attackCooldown > 0.0f) {
        const WeaponData& wd = GetWeaponDataById(pc->slots[pc->equippedSlot].weaponId);
        const float t01 = 1.0f - pc->attackCooldown / wd.attackCooldown;
        swing = std::sin(glm::clamp(t01, 0.0f, 1.0f) * 3.14159f); // 0->1->0
    }
    const glm::vec3 handPos = jt->position + fwd * (0.30f + 0.25f * swing) +
                              right * (0.30f - 0.15f * swing) +
                              glm::vec3(0.0f, 1.20f - 0.25f * swing, 0.0f);
    const glm::mat4 modelM =
        glm::translate(glm::mat4(1.0f), handPos) *
        glm::rotate(glm::mat4(1.0f), pc->facingYaw, glm::vec3(0.0f, 1.0f, 0.0f)) *
        glm::rotate(glm::mat4(1.0f), -0.5f + 1.8f * swing, glm::vec3(1.0f, 0.0f, 0.0f)) *
        glm::scale(glm::mat4(1.0f), glm::vec3(model->drawScale));
    const glm::mat4 mvp = proj * view * modelM;
    for (const WorldMesh::SubRange& sr : model->subs) {
        m_renderer->BindTexture(sr.texture);
        m_renderer->DrawMesh3DSub(model->gpu, sr.indexOffset, sr.indexCount,
                                  glm::value_ptr(mvp));
    }
}

// =========================================================================
// Track 36: Save/Load system
// =========================================================================

// Captures the current runtime state into a SaveData struct. Called by
// SaveGame(); reads from the live engine (entities, game clock, missions).
SaveData Engine::CaptureSaveState() {
    SaveData data;
    data.version = 1;
    data.slot    = 0;

    // ISO-8601 UTC timestamp.
    {
        std::time_t now = std::time(nullptr);
        std::tm tm_buf = *std::gmtime(&now);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        data.savedAt = ts;
    }

    // --- Player ---
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        if (const Transform* jt = m_entities.GetTransform(m_jimmyId)) {
            data.playerPos[0] = jt->position.x;
            data.playerPos[1] = jt->position.y;
            data.playerPos[2] = jt->position.z;
            data.playerRot[0] = jt->rotation.x;
            data.playerRot[1] = jt->rotation.y;
            data.playerRot[2] = jt->rotation.z;
        }
        if (const PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
            data.healthCurrent  = pc->health.current;
            data.healthMax      = pc->health.max;
            data.staminaCurrent = pc->stamina.current;
            data.staminaMax     = pc->stamina.max;
            data.money          = pc->money;
            data.wanted         = pc->wanted;
            data.facingYaw      = pc->facingYaw;
            data.facingValid    = pc->facingValid;
            data.equippedSlot   = pc->equippedSlot;
            for (int i = 0; i < 5; ++i) {
                data.slots[i].type       = static_cast<int>(pc->slots[i].type);
                data.slots[i].weaponId   = pc->slots[i].weaponId;
                data.slots[i].durability = pc->slots[i].durability;
                data.slots[i].ammo       = pc->slots[i].ammo;
            }
        }
    }

    // --- Mission ---
    data.missions  = m_missions.SnapshotMissions();
    data.triggers  = m_missions.SnapshotTriggers();

    // --- Class (Track 41) ---
    data.classes            = m_classes.Snapshot();
    data.classRespect       = m_classes.Respect();
    data.slingshotCapBonus  = m_slingshotCapBonus;

    // --- Arcade (Track 45): high scores / plays / wins / achievement ---
    m_arcade.WriteSave(data);

    // --- Reputation (Track 42) ---
    for (int i = 0; i < ReputationSave::kCount && i < kFactionCount; ++i)
        data.reputation.respect[i] =
            m_factions.Get(static_cast<Faction>(i));
    data.reputation.completedMissions = m_factions.CompletedMissions();

    // --- World ---
    data.gameTimeMinutes = m_gameTimeMinutes;
    data.gameDay         = m_gameDay;
    // Phase 10 integration: persist the weather state (Track 34 + 36).
    // Previously a placeholder 0; the W-cycled / auto-cycled state now
    // round-trips through the save file.
    data.weather = static_cast<int>(WeatherManager::Get().GetType());

    return data;
}

// Applies a SaveData to the engine: restores player position/vitals,
// game time, mission state, and respawns dynamic entities.
void Engine::ApplySaveState(const SaveData& data) {
    // --- Restore player ---
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        Transform* jt = m_entities.GetTransform(m_jimmyId);
        if (jt) {
            jt->position = glm::vec3(data.playerPos[0], data.playerPos[1], data.playerPos[2]);
            jt->rotation = glm::vec3(data.playerRot[0], data.playerRot[1], data.playerRot[2]);
        }
        PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
        if (pc) {
            pc->health.current = data.healthCurrent;
            pc->health.max     = data.healthMax;
            pc->stamina.current = data.staminaCurrent;
            pc->stamina.max     = data.staminaMax;
            pc->money          = data.money;
            pc->wanted         = data.wanted;
            pc->facingYaw      = data.facingYaw;
            pc->facingValid    = data.facingValid;
            pc->equippedSlot   = data.equippedSlot;
            for (int i = 0; i < 5; ++i) {
                pc->slots[i].type       = static_cast<WeaponType>(data.slots[i].type);
                pc->slots[i].weaponId   = data.slots[i].weaponId;
                pc->slots[i].durability = data.slots[i].durability;
                pc->slots[i].ammo       = data.slots[i].ammo;
            }
            // Reset combat/grapple state on load.
            pc->state = DefenseState::Normal;
            pc->stateTimer = 0.0f;
            pc->invincibleTimer = 0.0f;
            pc->dodgeCooldown = 0.0f;
            pc->counterArmed = false;
            pc->blockStartTime = -1e9f;
            pc->attackCooldown = 0.0f;
        }
        // Reset the camera to follow the new player position.
        m_camDesired = jt ? jt->position : m_jimmySpawn;
    }

    // --- Restore game time ---
    m_gameTimeMinutes = data.gameTimeMinutes;
    m_gameDay = std::max(0, data.gameDay);
    SyncGameClock();

    // Phase 10 integration: restore the weather state (Track 34 + 36).
    // Bounds-checked against the enum range; unknown values fall back to CLEAR
    // via TransitionTo (which no-ops when already CLEAR).
    {
        const int w = data.weather;
        if (w >= 0 && w < static_cast<int>(WeatherType::Count))
            WeatherManager::Get().SetState(static_cast<WeatherType>(w));
        else
            WeatherManager::Get().SetState(WeatherType::CLEAR);
    }

    // --- Restore mission state ---
    m_missions.RestoreMissions(data.missions);
    m_missions.RestoreTriggers(data.triggers);

    // --- Restore class progression (Track 41) + abort any live session ---
    if (m_classSessionActive) {
        m_classSessionActive = false;
        m_classSessionIdx    = -1;
        Logger::Info("Classes: session aborted by load");
    }
    m_scheduleOpen = false;
    m_classPromptIdx = -1;
    m_classes.Restore(data.classes);
    m_classes.AddRespect(data.classRespect - m_classes.Respect()); // absolute set
    m_slingshotCapBonus = std::clamp(data.slingshotCapBonus, 0, kClassMaxCapBonus);

    // --- Restore arcade progression (Track 45); close any open cabinet view ---
    if (m_arcade.IsActive()) {
        m_arcade.ExitGame();
        Logger::Info("Arcade: cabinet view closed by load");
    }
    m_arcade.ReadSave(data);

    // --- Restore reputation (Track 42) ---
    for (int i = 0; i < ReputationSave::kCount && i < kFactionCount; ++i)
        m_factions.SetRespect(static_cast<Faction>(i),
                              data.reputation.respect[i]);
    m_factions.SetCompletedMissions(data.reputation.completedMissions);
    Logger::Info("Reputation: restored faction scores, %d mission(s) done",
                 m_factions.CompletedMissions());

    // --- Clear combat/grapple state ---
    m_grappleTarget = EntityID{};
    m_mountedVehicle = EntityID{};
    m_projectiles.clear();
    // Track 39: clear impact debris + ammo boxes (respawned below) + aim mode.
    for (const ImpactFx& fx : m_impacts)
        if (fx.id.IsValid() && m_entities.IsAlive(fx.id)) m_entities.Destroy(fx.id);
    m_impacts.clear();
    for (EntityID id : m_ammoBoxIds)
        if (id.IsValid() && m_entities.IsAlive(id)) m_entities.Destroy(id);
    m_ammoBoxIds.clear();
    m_aiming = false;
    m_meleePopup[0] = '\0';
    m_meleePopupTimer = 0.0f;

    // --- Clear dialogue ---
    m_dialogue.EndDialogue();
    m_dialogueNpc = EntityID{};

    // --- Clear AI event queue + fight history ---
    m_aiEvents.clear();
    m_fights.clear();

    // Track 43: cops are runtime pursuers (never persisted); reset the whole
    // wanted system state. The meter itself restores from the save (player).
    DespawnCops();
    m_copsPending = false;
    m_copsStandDown = false;
    m_wanted = WantedSystem{};
    m_trespassZones.clear(); // rebuilt from the (new) spawn on next update

    // --- Clear and respawn scheduled NPCs ---
    for (auto& sn : m_scheduledNpcs) {
        if (sn.entity.IsValid() && m_entities.IsAlive(sn.entity))
            m_entities.Destroy(sn.entity);
        sn.entity = EntityID{};
        sn.lastWindowIndex = -1;
    }

    // --- Destroy weapon pickups ---
    m_entities.ForEach([&](EntityID eid, const Transform&, const EntityManager::EntityRecord& rec) {
        if (rec.mesh == 0xFFFFFFFF) return;
        MeshComponent* mc = m_entities.GetMesh(eid);
        if (mc && mc->isWeaponPickup) m_entities.Destroy(eid);
    });

    // --- Destroy vehicles ---
    for (EntityID vid : m_vehicleIds) {
        if (vid.IsValid() && m_entities.IsAlive(vid))
            m_entities.Destroy(vid);
    }
    m_vehicleIds.clear();
    m_vehiclePhysSeeded = false;

    // --- Respawn dynamic entities at the player's position ---
    RespawnDynamicEntities();

    // --- Update schedules immediately ---
    UpdateSchedules();

    // --- Refresh HUD ---
    UpdateHUD(0.0f);
}

// Destroy all dynamic entities (NPCs, projectiles, pickups, vehicles)
// that are not part of the static level geometry.
void Engine::DestroyDynamicEntities() {
    // Track 43: cops are dynamic pursuers.
    DespawnCops();

    // Destroy scheduled NPCs.
    for (auto& sn : m_scheduledNpcs) {
        if (sn.entity.IsValid() && m_entities.IsAlive(sn.entity))
            m_entities.Destroy(sn.entity);
        sn.entity = EntityID{};
    }

    // Destroy projectiles.
    for (const Projectile& p : m_projectiles) {
        if (p.id.IsValid() && m_entities.IsAlive(p.id))
            m_entities.Destroy(p.id);
    }
    m_projectiles.clear();

    // Track 39: destroy impact debris + ammo boxes.
    for (const ImpactFx& fx : m_impacts)
        if (fx.id.IsValid() && m_entities.IsAlive(fx.id)) m_entities.Destroy(fx.id);
    m_impacts.clear();
    for (EntityID id : m_ammoBoxIds)
        if (id.IsValid() && m_entities.IsAlive(id)) m_entities.Destroy(id);
    m_ammoBoxIds.clear();
    m_aiming = false;

    // Destroy weapon pickups.
    // Collect IDs first to avoid modifying the entity list during iteration.
    std::vector<EntityID> toDestroy;
    m_entities.ForEach([&](EntityID eid, const Transform&, const EntityManager::EntityRecord& rec) {
        if (rec.mesh == 0xFFFFFFFF) return;
        MeshComponent* mc = m_entities.GetMesh(eid);
        if (mc && mc->isWeaponPickup) toDestroy.push_back(eid);
    });
    for (EntityID eid : toDestroy) m_entities.Destroy(eid);

    // Destroy vehicles.
    for (EntityID vid : m_vehicleIds) {
        if (vid.IsValid() && m_entities.IsAlive(vid))
            m_entities.Destroy(vid);
    }
    m_vehicleIds.clear();
}

// Respawn dynamic entities (weapons, vehicles) at the player's current
// position. NPCs are handled by UpdateSchedules() called separately.
void Engine::RespawnDynamicEntities() {
    glm::vec3 center = m_jimmyActive && m_jimmyId.IsValid()
        ? (m_entities.GetTransform(m_jimmyId) ? m_entities.GetTransform(m_jimmyId)->position : m_jimmySpawn)
        : m_jimmySpawn;

    // Respawn weapon pickups.
    SpawnWeaponPickups(center);
    // Track 39: respawn ammo boxes.
    SpawnAmmoBoxes(center);

    // Respawn vehicles.
    SpawnVehicles(center);
}

// F5: save current state to slot 0.
bool Engine::SaveGame(int slot) {
    const SaveData data = CaptureSaveState();
    const char* baseDir = SDL_GetBasePath();
    const std::string path = SaveSystem::SlotPath(baseDir ? baseDir : ".", slot);
    if (SaveSystem::WriteToFile(path, data)) {
        Logger::Info("Game saved to slot %d (%s)", slot, path.c_str());
        return true;
    }
    return false;
}

// F9: load state from slot 0. Restores all saved state; NPCs respawn
// from schedules. If the file doesn't exist, logs an error and does not crash.
bool Engine::LoadGame(int slot) {
    const char* baseDir = SDL_GetBasePath();
    const std::string base = baseDir ? baseDir : ".";
    if (!SaveSystem::Exists(base, slot)) {
        Logger::Error("LoadGame: no save file found at slot %d (use F5 to save first)", slot);
        return false;
    }
    SaveData data;
    const std::string path = SaveSystem::SlotPath(base, slot);
    if (!SaveSystem::ReadFromFile(path, data)) {
        Logger::Error("LoadGame: failed to read/parse '%s'", path.c_str());
        return false;
    }

    Logger::Info("Loading game from slot %d (saved %s, pos (%.1f,%.1f,%.1f), hp %d/%d, time %02d:%02d)",
                 slot, data.savedAt.c_str(),
                 data.playerPos[0], data.playerPos[1], data.playerPos[2],
                 data.healthCurrent, data.healthMax,
                 (int)(data.gameTimeMinutes / 60.0),
                 (int)(data.gameTimeMinutes) % 60);

    ApplySaveState(data);
    Logger::Info("Game loaded from slot %d", slot);
    return true;
}

// =========================================================================
// Track 47: photo challenge system
// =========================================================================

// Album load (saves/photo_album.json, persisted across runs) + a camera
// pickup next to Jimmy so the item is collectible without the demo drive.
bool Engine::InitPhotoSystem() {
    // The album lives next to the save slots (same directory convention as
    // SaveSystem::SlotPath: <base>/saves/...). Probe CWD then the source tree
    // like every other data file; missing file = empty album.
    const char* albumPaths[] = { "saves/photo_album.json",
                                 "../saves/photo_album.json" };
    for (const char* p : albumPaths) {
        if (std::ifstream f(p); f.good()) {
            f.close();
            m_photos.LoadAlbum(p);
            break;
        }
    }
    Logger::Info("Photo system: %zu target(s) registered at init",
                 m_photos.Targets().size());

    // Camera pickup near Jimmy (walk-over or E). No NIF in the extracted
    // set, so it draws as a small box prop instead of a model.
    if (!m_jimmyActive || !m_testBoxValid) return true;
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return true;
    glm::vec3 pos = jt->position + glm::vec3(-3.5f, 0.0f, 1.5f);
    if (m_collisionWorld) {
        Ray down;
        down.origin[0] = pos.x; down.origin[1] = pos.y + 6.0f; down.origin[2] = pos.z;
        down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit)) pos.y = hit.point[1];
    }
    EntityID e = m_entities.Create();
    Transform t;
    t.position = pos + glm::vec3(0.0f, 0.35f, 0.0f);
    t.scale    = glm::vec3(0.22f);
    m_entities.AddTransform(e, t);
    MeshComponent mc;
    mc.isBox = true;
    m_entities.AddMesh(e, mc);
    CollisionComponent cc;
    cc.boundsMin = glm::vec3(-0.5f); cc.boundsMax = glm::vec3(0.5f);
    cc.layerMask = 2u;
    m_entities.AddCollision(e, cc);
    WeaponComponent w;
    w.type = WeaponType::Camera;
    w.weaponId = kCameraWeaponId;
    w.durability = 0;
    w.ammo = 0;
    m_entities.AddWeapon(e, w);
    Logger::Info("Photo system: camera pickup spawned at (%.1f, %.1f, %.1f)",
                 pos.x, pos.y, pos.z);
    return true;
}

// Subjects are labeled at spawn: schedule name for scheduled NPCs (student /
// townie / prefect / teacher), model file name for the legacy crowd.
void Engine::RegisterPhotoTarget(EntityID npc, const std::string& label,
                                 Photo::TargetKind kind, float height) {
    m_photos.RegisterTarget(npc, label, kind, height);
}

// Drop subjects whose entity handle died (scheduled NPCs despawn through the
// day cycle; KO'd corpses stay until cleanup but their handles stay valid).
void Engine::PrunePhotoTargets() {
    auto& targets = m_photos.Targets();
    for (size_t i = 0; i < targets.size();) {
        if (!m_entities.IsAlive(targets[i].entity)) {
            targets.erase(targets.begin() + long(i));
        } else {
            ++i;
        }
    }
}

// Per-frame viewfinder upkeep: wheel zoom, projection cache (zoomed FOV) and
// the highlighted-subject bracket the UI draws. Runs after the camera update
// so the cached matrices match what Render will submit this frame.
void Engine::UpdatePhotoMode(float dt) {
    if (m_photoFlash  > 0.0f) m_photoFlash  -= dt;
    if (m_photoPopupTimer > 0.0f) m_photoPopupTimer -= dt;
    PrunePhotoTargets();
    m_photoSubject.has = false;
    if (!m_photoMode) return;

    // Zoom: mouse wheel (forwarded by main.cpp before Input::Poll). The
    // journal consumes the same delta when it is open, so the wheel only
    // zooms while the journal is closed (journal keeps scroll priority).
    if (m_journalWheelDelta != 0 && !m_journalOpen) {
        m_photoZoom = glm::clamp(m_photoZoom + float(m_journalWheelDelta) * 0.25f,
                                 kPhotoZoomMin, kPhotoZoomMax);
        m_journalWheelDelta = 0;
        Logger::Info("Photo zoom: %.2fx", m_photoZoom);
    }

    // Projection cache: build the same view*proj Render will use (zoomed
    // FOV) so shutter validation and the bracket match the final image.
    if (m_renderer) {
        const float fov = CurrentFovDeg();
        if (fov != m_lastFovDeg) {
            m_renderer->SetProjectionFov(fov);
            m_lastFovDeg = fov;
        }
        m_photoViewProj = m_renderer->GetProjection() * m_camera.GetViewMatrix();
    }

    // Highlight the best subject: shutter-valid ones win over merely visible
    // ones; among equals the most centered, then the largest fill.
    const float W = float(m_screenWidth), H = float(m_screenHeight);
    if (W <= 0.0f || H <= 0.0f) return;
    const glm::vec3 camPos = m_camera.GetPosition();
    float bestScore = 1e9f;
    bool  bestValid = false;
    for (const Photo::PhotoTarget& tgt : m_photos.Targets()) {
        const Transform* tt = m_entities.GetTransform(tgt.entity);
        if (!tt) continue;
        const Photo::AimAssessment a = m_photos.Assess(
            tgt, tt->position, camPos, m_photoViewProj, W, H);
        if (!a.inFrustum) continue;

        float xMid = 0.0f, yMid = 0.0f;
        Photo::PhotoSystem::ProjectPoint(tt->position + glm::vec3(0.0f, 0.5f * tgt.height, 0.0f),
                            m_photoViewProj, W, H, &xMid, &yMid);
        const float dx = (xMid - 0.5f * W) / W;
        const float dy = (yMid - 0.5f * H) / H;
        const float score = dx * dx + dy * dy;
        if (bestValid && !a.Valid()) continue;         // valid beats invalid
        if (!bestValid && a.Valid()) { /* upgrade below */ }
        else if (score >= bestScore) continue;
        bestScore  = score;
        bestValid  = a.Valid();
        m_photoSubject.has    = true;
        m_photoSubject.valid  = a.Valid();
        m_photoSubject.entity = tgt.entity;
        // Bracket: width from the fill ratio (bodies are ~2.4x taller than
        // wide), height from the projected feet..head span.
        float xFeet = 0.0f, yFeet = 0.0f, xHead = 0.0f, yHead = 0.0f;
        Photo::PhotoSystem::ProjectPoint(tt->position, m_photoViewProj, W, H, &xFeet, &yFeet);
        Photo::PhotoSystem::ProjectPoint(tt->position + glm::vec3(0.0f, tgt.height, 0.0f),
                            m_photoViewProj, W, H, &xHead, &yHead);
        m_photoSubject.bh = std::fabs(yFeet - yHead);
        m_photoSubject.bw = m_photoSubject.bh * 0.5f;
        m_photoSubject.bx = xMid - 0.5f * m_photoSubject.bw;
        m_photoSubject.by = yFeet - m_photoSubject.bh;
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "%s  %.0fm%s", tgt.name.c_str(),
                      a.dist, a.Valid() ? "" : "  (not framed)");
        m_photoSubject.label = lbl;
    }
}

// LMB in photo mode. Validates every subject, picks the best one, records
// the album entry (metadata only) and advances photo missions. Returns true
// when the shutter counted a subject toward an objective.
bool Engine::TakePhoto() {
    if (!m_photoMode) return false;

    m_photoFlash = 0.12f; // viewfinder flash (UI)

    const float W = float(m_screenWidth), H = float(m_screenHeight);
    const glm::vec3 camPos = m_camera.GetPosition();

    // Pick the shutter subject with the same scoring as the bracket.
    const Photo::PhotoTarget* best = nullptr;
    Photo::AimAssessment bestAssessment;
    float bestScore = 1e9f;
    bool  bestValid = false;
    for (const Photo::PhotoTarget& tgt : m_photos.Targets()) {
        const Transform* tt = m_entities.GetTransform(tgt.entity);
        if (!tt) continue;
        const Photo::AimAssessment a = m_photos.Assess(
            tgt, tt->position, camPos, m_photoViewProj, W, H);
        if (!a.inFrustum) continue;
        // Track 47 debug: why did a visible subject fail the shutter?
        Logger::Info("Photo check: '%s' dist=%.1f fill=%.2f frustum=%d range=%d framed=%d centered=%d",
                     tgt.name.c_str(), a.dist, a.fill, int(a.inFrustum),
                     int(a.inRange), int(a.framed), int(a.centered));
        float xMid = 0.0f, yMid = 0.0f;
        Photo::PhotoSystem::ProjectPoint(tt->position + glm::vec3(0.0f, 0.5f * tgt.height, 0.0f),
                            m_photoViewProj, W, H, &xMid, &yMid);
        const float dx = (xMid - 0.5f * W) / W;
        const float dy = (yMid - 0.5f * H) / H;
        const float score = dx * dx + dy * dy;
        if (bestValid && !a.Valid()) continue;
        if (!bestValid && !a.Valid() && best) continue; // keep first visible
        if (bestValid == a.Valid() && score >= bestScore) continue;
        bestScore = score;
        bestValid = a.Valid();
        best = &tgt;
        bestAssessment = a;
    }

    Photo::PhotoRecord rec;
    rec.clockMinutes = m_clockMinutes;
    rec.frame        = int64_t(m_frameCount);
    if (best) {
        rec.subject     = best->name;
        rec.entityIndex = best->entity.index;
    } else {
        rec.subject = "(no subject)";
    }

    // Which mission (if any) does this photo count toward?
    std::string missionId;
    if (best && bestValid) {
        if (const Mission* am = m_missions.ActivePhotoMission()) {
            const Objective& o = am->objectives[am->currentObjective];
            if (o.photoTarget.empty() || o.photoTarget == best->name)
                missionId = am->id;
        }
    }
    rec.missionId = missionId;
    m_photos.AddPhoto(rec);

    // Persist the album next to the save slots (small file, one write/shot).
    std::error_code fsErr;
    std::filesystem::create_directories("saves", fsErr);
    m_photos.SaveAlbum("saves/photo_album.json");

    // Feedback + mission wiring.
    if (best && bestValid) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "PHOTO TAKEN: %s", best->name.c_str());
        m_photoPopup = buf;
        m_photoPopupTimer = 2.0f;
        m_missions.NotifyPhotoTaken(best->name);
        Logger::Info("Photo #%zu: '%s' counted for mission '%s' (zoom %.1fx, %02d:%02d)",
                     m_photos.Album().size(), best->name.c_str(),
                     missionId.c_str(), m_photoZoom,
                     (m_clockMinutes / 60) % 24, m_clockMinutes % 60);
    } else {
        m_photoPopup = best ? "PHOTO TAKEN (subject not framed)"
                            : "PHOTO TAKEN (no subject in frame)";
        m_photoPopupTimer = 2.0f;
        Logger::Info("Photo #%zu: no valid subject (zoom %.1fx)",
                     m_photos.Album().size(), m_photoZoom);
    }
    return !missionId.empty();
}

// FOV source of truth shared by Update (projection cache) and Render (the
// actual renderer projection): photo zoom narrows the view like a lens.
float Engine::CurrentFovDeg() const {
    if (m_photoMode) return 60.0f / std::max(1.0f, m_photoZoom);
    if (m_mountedVehicle.IsValid()) {
        if (const MeshComponent* vmc = m_entities.GetMesh(m_mountedVehicle))
            if (vmc->isVehicle &&
                vmc->vehicleModelIndex < m_vehicleModels.size())
                return m_vehicleModels[vmc->vehicleModelIndex].fovDeg;
    }
    return 60.0f;
}

// Viewfinder widgets (view 10). Drawn after the HUD, before the journal.
void Engine::DrawPhotoOverlayUI() {
    if (!m_photoMode || !m_ui.IsInitialized()) return;
    PhotoOverlay o;
    o.active   = true;
    o.zoomPct  = int(m_photoZoom * 100.0f + 0.5f);
    o.flash    = m_photoFlash > 0.0f;
    o.popupText = m_photoPopup;
    o.popupTimer = m_photoPopupTimer;
    o.statusLine = "LMB: SHUTTER   RMB: EXIT   WHEEL: ZOOM";
    if (m_photoSubject.has) {
        o.hasBracket   = true;
        o.bracketValid = m_photoSubject.valid;
        o.bx = m_photoSubject.bx; o.by = m_photoSubject.by;
        o.bw = m_photoSubject.bw; o.bh = m_photoSubject.bh;
        o.bracketLabel = m_photoSubject.label;
    }
    m_ui.DrawPhotoOverlay(o);
}

// OPENBULLY_PHOTO_DEMO=1: scripted photography run for headless
// verification. t=1.0 grants + equips the camera, t=1.5 activates the
// photography mission, t=2.0 spawns 3 "Jock" subjects and opens the
// viewfinder (3x zoom), then the shutter fires every 1.2s from t=4.0 — the
// third valid photo completes the mission (same cadence style as
// WEAPON/MISSION demos).
void Engine::DrivePhotoDemo(float dt) {
    (void)dt;
    PlayerController* pc = m_jimmyId.IsValid()
        ? m_entities.GetPlayer(m_jimmyId) : nullptr;
    if (!pc || !m_jimmyActive) return;

    static bool s_gave = false;
    if (!s_gave && m_time >= 1.0) {
        s_gave = true;
        for (int i = 0; i < 5; ++i) {
            if (pc->slots[i].type != WeaponType::Unarmed) continue;
            pc->slots[i].type = WeaponType::Camera;
            pc->slots[i].weaponId = kCameraWeaponId;
            pc->slots[i].durability = 0;
            pc->equippedSlot = i;
            Logger::Info("Photo demo: camera equipped (slot %d)", i);
            break;
        }
    }
    static bool s_activated = false;
    if (!s_activated && m_time >= 1.5) {
        s_activated = true;
        for (size_t i = 0; i < m_missions.MissionCount(); ++i) {
            const Mission* m = m_missions.GetMission(uint32_t(i));
            if (m && m->id == "ch1_photography") {
                m_missions.ActivateMission(uint32_t(i));
                break;
            }
        }
    }
    static bool s_started = false;
    if (!s_started && m_time >= 2.0) {
        s_started = true;
        SpawnPhotoDemoTargets();
        m_photoMode = true;
        m_photoZoom = 3.0f;
        Logger::Info("Photo demo: viewfinder ON (zoom 3.0x)");
    }

    // Keep the view aimed at the subject group: the follow camera would
    // otherwise keep orbiting Jimmy; the photo demo owns the camera while
    // the viewfinder is up (same override idea as the ranged demo's clamp).
    // The stage sits 16u ahead of Jimmy so the vehicle pickup ring (radius
    // <= 6u around the spawn, one Police car pulled in to r=3 during
    // verification) stays behind the camera and out of the sight line.
    if (m_photoMode && !m_photoDemoTargets.empty()) {
        glm::vec3 sum(0.0f);
        int n = 0;
        for (EntityID id : m_photoDemoTargets) {
            if (const Transform* tt = m_entities.GetTransform(id)) {
                sum += tt->position;
                ++n;
            }
        }
        if (n > 0) {
            const glm::vec3 center = sum / float(n) + glm::vec3(0.0f, 1.1f, 0.0f);
            const glm::vec3 camPos = center + glm::vec3(0.0f, 2.2f, -10.0f);
            m_camera.SetPosition(camPos);
            m_camera.SetLookAt(center - camPos);
        }
    }

    // Shutter cadence: 3 photos complete the 3/3 objective. Stops once no
    // active mission still has a live PHOTOGRAPH_TARGET (the album keeps
    // exactly the three counted shots); the viewfinder stays up for the
    // verification capture.
    if (m_photoMode && m_time >= 4.0 && m_missions.ActivePhotoMission()) {
        static double s_lastShot = -1e9;
        if (m_time - s_lastShot >= 1.2) {
            s_lastShot = m_time;
            TakePhoto();
        }
    }

    // Album reveal: after the 3rd photo completes the objective, open the
    // journal on its photo-album page so the recurring screenshot captures
    // the album UI with the three rows (verification proof).
    if (m_time >= 9.0 && !m_journalOpen) {
        m_journalOpen = true;
        m_journalPhotoPage = true;
        m_journalScrollPx = 0.0f;
        m_photoMode = false; // viewfinder yields to the modal journal
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_ShowCursor(SDL_ENABLE);
        Logger::Info("Photo demo: journal opened on album page (%zu photo(s))",
                     m_photos.Album().size());
    }
}

// Spawns the demo's 3 photographable subjects ~5.5u ahead of Jimmy on his
// ground plane (the ranged demo learned the spawn roof has no collision
// beneath, so the group sits on Jimmy's own plane). Real NPC models when the
// extracted set loaded, boxes otherwise; both photograph identically.
void Engine::SpawnPhotoDemoTargets() {
    m_photoDemoTargets.clear();
    const Transform* jt = m_entities.GetTransform(m_jimmyId);
    if (!jt) return;

    static const glm::vec3 kOffsets[3] = {
        { -2.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.4f }, { 2.0f, 0.0f, 0.0f },
    };
    const bool useNpc = !m_npcModels.empty();
    for (int i = 0; i < 3; ++i) {
        EntityID e = m_entities.Create();
        Transform t;
        // 16u ahead of Jimmy: clears the weapon/vehicle/NPC spawn clutter
        // around the spawn point (vehicles ring at <= 6u) so the demo sight
        // line photographs the jocks, not a parked Police car.
        t.position = jt->position + kOffsets[i] + glm::vec3(0.0f, 0.0f, 16.0f);
        m_entities.AddTransform(e, t);
        if (useNpc) {
            const uint32_t mi = uint32_t(i % m_npcModels.size());
            t.scale = glm::vec3(m_npcModels[mi].bodyScale);
            m_entities.AddTransform(e, t);
            MeshComponent mc;
            mc.isNpc = true;
            mc.npcModelIndex = mi;
            m_entities.AddMesh(e, mc);
        } else {
            t.scale = glm::vec3(0.8f, 1.8f, 0.8f);
            m_entities.AddTransform(e, t);
            MeshComponent mc;
            mc.isBox = true;
            m_entities.AddMesh(e, mc);
        }
        // Static subject: lockIdle keeps the AI out of the movement code.
        AIComponent ai;
        ai.state = AIState::Idle;
        ai.lockIdle = true;
        ai.homePos = t.position;
        ai.health.current = 60;
        m_entities.AddAI(e, ai);
        m_entities.AddMeleeCombat(e);
        RegisterPhotoTarget(e, "Jock", Photo::TargetKind::Person, 1.8f);
        m_photoDemoTargets.push_back(e);
        Logger::Info("Photo demo: subject %d spawned at (%.1f, %.1f, %.1f)",
                     i, t.position.x, t.position.y, t.position.z);
    }
}

} // namespace OpenBully
