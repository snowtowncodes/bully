#include "engine.h"
#include "audio/audio.h"
#include "formats/nif_loader.h"
#include "formats/nft_loader.h"
#include "formats/agr_loader.h"
#include "../entities/entity.h"
#include "../platform/logger.h"
#include "../platform/window.h"
#include "../rendering/mesh.h"
#include "../rendering/renderer.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
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
void Engine::UpdateJimmy(float dt) {
    if (!m_jimmyActive || !m_jimmyId.IsValid() || dt <= 0.0f) return;
    Transform* t = m_entities.GetTransform(m_jimmyId);
    PhysicsComponent* p = m_entities.GetPhysics(m_jimmyId);
    PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
    if (!t || !p || !pc) return;

    glm::vec3 pos = t->position;

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
    auto next = [&rng]() {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return rng;
    };
    auto unit = [&next]() { return float(next() % 10000u) / 10000.0f; }; // [0,1)

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
        // Mix: ~1/3 linger idle, 2/3 head out after a short pause (targets
        // are picked through the normal IDLE path so ground checks apply).
        ai.state = (i % 3 == 0) ? AIState::Idle : AIState::Wander;
        ai.timer = 0.2f + unit() * 2.8f;
        ai.moveSpeed = 1.0f + unit() * 1.0f; // 1.0-2.0 u/s
        ai.wanderRadius = 3.0f;              // stay close on uneven geometry
        m_entities.AddAI(e, ai);

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
    Logger::Info("NPCs: %u spawned around (%.1f, %.1f, %.1f) from %zu models",
                 m_npcCount, center.x, center.y, center.z, m_npcModels.size());
}

// Track 16: NPC AI update. States:
//   IDLE        stand still; timer expires -> pick a WANDER target.
//   WANDER      walk toward targetPos (ground raycast + wall resolve like
//               Jimmy); arrive -> IDLE for a random duration.
//   FACE_PLAYER player within faceRadius: turn to face them; movement
//               pauses while they are close.
void Engine::UpdateNpcs(float dt) {
    if (dt <= 0.0f) return;

    // Player position for FACE_PLAYER (Jimmy's feet transform).
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

    m_entities.ForEachAI([&](EntityID id, Transform& t, AIComponent& ai) {
        (void)id;
        const glm::vec3 pos = t.position;
        bool moving = false; // Track 17: drive the walk/idle state machine

        // Player proximity overrides wandering: pause and face the player.
        if (havePlayer) {
            const glm::vec3 toP = playerPos - pos;
            const float distP = glm::length(glm::vec3(toP.x, 0.0f, toP.z));
            if (distP < ai.faceRadius) {
                const float targetYaw = std::atan2(toP.x, toP.z);
                ai.facingYaw += shortestArc(ai.facingYaw, targetYaw) *
                                std::min(1.0f, ai.turnSpeed * dt);
                t.rotation.y = ai.facingYaw;
                if (ai.state == AIState::Wander) ai.hasTarget = false; // re-decide later
                ai.state = AIState::FacePlayer;
                UpdateEntityAnimation(id, false, dt); // Track 17: idle in place
                return;
            }
        }

        switch (ai.state) {
        case AIState::Idle:
            ai.timer -= dt;
            if (ai.timer <= 0.0f) {
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
                    // DEBUG(Track 16): log why the first target was rejected.
                    static int dbgLogged = 0;
                    if (dbgLogged < 3) {
                        dbgLogged++;
                        Ray down;
                        down.origin[0] = ai.targetPos.x;
                        down.origin[1] = pos.y + 10.0f;
                        down.origin[2] = ai.targetPos.z;
                        down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
                        RayHit hit;
                        const bool hitOk = m_collisionWorld && m_collisionWorld->Raycast(down, hit);
                        Logger::Info("NPC wander-reject: pos.y=%.2f target=(%.1f,%.1f) hit=%s hitY=%.2f",
                                     pos.y, ai.targetPos.x, ai.targetPos.z,
                                     hitOk ? "y" : "n", hitOk ? hit.point[1] : 0.0f);
                    }
                    ai.hasTarget = false; ai.timer = 1.0f; break;
                }
                ai.hasTarget = true;
                ai.state = AIState::Wander;
            }
            break;

        case AIState::Wander: {
            if (!ai.hasTarget) { ai.state = AIState::Idle; ai.timer = 1.0f; break; }
            glm::vec3 to = ai.targetPos - pos;
            to.y = 0.0f;
            const float dist = glm::length(to);
            if (dist < 0.25f) {
                ai.state = AIState::Idle;
                ai.timer = ai.idleTimeMin + float(nextRandom01()) *
                           (ai.idleTimeMax - ai.idleTimeMin);
                break;
            }
            glm::vec3 dir = to / dist;
            glm::vec3 newPos = pos + dir * (ai.moveSpeed * dt);

            // Ground: raycast down from above, clamp feet to the surface.
            // A missed ray or a big level jump (walk-off edge) aborts the trip.
            float groundY = pos.y;
            bool walkable = false;
            if (m_collisionWorld) {
                Ray down;
                down.origin[0] = newPos.x; down.origin[1] = newPos.y + 4.0f; down.origin[2] = newPos.z;
                down.direction[0] = 0.0f; down.direction[1] = -1.0f; down.direction[2] = 0.0f;
                RayHit hit;
                if (m_collisionWorld->Raycast(down, hit)) {
                    if (std::fabs(hit.point[1] - pos.y) < 3.0f) {
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
            if (!walkable) { // walk-off rejected: idle here, pick again later
                ai.state = AIState::Idle;
                ai.timer = 1.0f;
                break;
            }
            newPos.y = groundY;
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
            // Held only while the player is inside faceRadius (checked above
            // at the top of the loop); falling through here means the player
            // left the radius -> resume idling.
            ai.state = AIState::Idle;
            ai.timer = 0.5f;
            break;
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

    if (moving != ac->wasMoving) {
        if (moving && rig.walkClip >= 0) {
            ctrl.Play(rig.walkClip, true, 0.2f);
            ac->playing = true;
        } else if (!moving && rig.idleClip >= 0) {
            ctrl.Play(rig.idleClip, true, 0.2f);
            ac->playing = true;
        }
        ac->wasMoving = moving;
    }
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
        SpawnNpcs(npcCenter);
    }

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

    // Track 20 (UI): HUD renderer. Runs on bgfx view 10 with the same
    // shaderRoot convention as Renderer::Init. Non-fatal on failure: the
    // world keeps rendering, the HUD stays off.
    if (!m_ui.Init(shaderRoot))
        Logger::Warning("UI renderer init failed; HUD disabled");
    else
        m_ui.SetScreenSize(unsigned(window.GetWidth()), unsigned(window.GetHeight()));

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
    }

    m_camera.Update(float(deltaTime), m_input, /*ignoreMove=*/m_jimmyActive);

    // Track 14: entity update (physics integration on gravity carriers).
    UpdateEntities(float(deltaTime));

    // Track 15: Jimmy input/physics, then the 3rd-person follow camera.
    UpdateJimmy(float(deltaTime));
    // Track 16: NPC AI (after physics so ground/wall state is settled).
    UpdateNpcs(float(deltaTime));
    UpdateJimmyCamera(float(deltaTime));

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
            pc->health.Damage(10);
            Logger::Info("HUD demo: damage 10 -> health %d/%d",
                         pc->health.current, pc->health.max);
        }
    }

    if (m_input.WasKeyPressed(SDL_SCANCODE_H)) {
        pc->health.Damage(10);
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

    // Game clock: 1 in-game minute per real second (single-player pacing).
    m_clockMinutes = (m_clockMinutes + 1) % (24 * 60);
    m_hudState.clockMinutes = m_clockMinutes;

    // Player vitals + minimap anchor.
    if (m_jimmyActive && m_jimmyId.IsValid()) {
        if (const PlayerController* pc = m_entities.GetPlayer(m_jimmyId)) {
            m_hudState.health = pc->health.current;
            m_hudState.maxHealth = pc->health.max;
            m_hudState.wanted = pc->wanted;
            m_hudState.money = pc->money;
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
    // Track 16: NPC AI census (idle/wander/face counts + one wander sample).
    {
        unsigned idle = 0, wander = 0, face = 0; bool first = true;
        char sample[128] = {};
        m_entities.ForEachAI([&](EntityID, const Transform& t, const AIComponent& ai) {
            switch (ai.state) {
            case AIState::Idle:        idle++;   break;
            case AIState::Wander:      wander++; break;
            case AIState::FacePlayer:  face++;   break;
            }
            if (first && ai.state == AIState::Wander) {
                std::snprintf(sample, sizeof(sample),
                              "sample: (%.1f, %.1f, %.1f) -> target (%.1f, %.1f)",
                              t.position.x, t.position.y, t.position.z,
                              ai.targetPos.x, ai.targetPos.z);
                first = false;
            }
        });
        Logger::Info("NPC AI: idle=%u wander=%u face=%u %s",
                     idle, wander, face, sample);
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

} // namespace OpenBully
