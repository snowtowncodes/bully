#include "engine.h"
#include "formats/nif_loader.h"
#include "formats/nft_loader.h"
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
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <unordered_set>
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
        //    check stale handles fail.
        EntityID stale = hit;
        if (stale.IsValid() && m_entities.IsAlive(stale)) {
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
    static const char* kPlayerNifs[] = { "Player_Mascot.nif", "GOG_Player.nif" };
    for (const char* playerNif : kPlayerNifs) {
        for (const char* dir : { "data/extracted/world", "../data/extracted/world" }) {
            const std::string path = std::string(dir) + "/" + playerNif;
            NifLoadResult nif = LoadNifFile(path);
            if (!nif.ok || nif.totalVertices < 10) continue;
            Mesh mesh;
            if (!MeshFromNif(nif, mesh)) continue;
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

// Track 15: integrate Jimmy's physics + input each frame.
//   - Gravity always applies (task: falls if spawned above ground).
//   - On the ground (ground raycast within the snap window): WASD moves the
//     character in camera-yaw space at moveSpeed; vertical velocity resets.
//   - In the air: gravity only, no horizontal control.
//   - Wall collision: horizontal sphere resolve against level AABBs (slide).
void Engine::UpdateJimmy(float dt) {
    if (!m_jimmyActive || !m_jimmyId.IsValid() || dt <= 0.0f) return;
    Transform* t = m_entities.GetTransform(m_jimmyId);
    PhysicsComponent* p = m_entities.GetPhysics(m_jimmyId);
    PlayerController* pc = m_entities.GetPlayer(m_jimmyId);
    if (!t || !p || !pc) return;

    // --- Gravity -------------------------------------------------------------
    p->velocity.y += p->gravity * dt;
    glm::vec3 pos = t->position + p->velocity * glm::vec3(dt);

    // --- Ground detection: raycast straight down from the chest -------------
    // Cast from well above (meshes can be hollow AABBs; starting inside one
    // would report t=0) so we always hit the topmost surface below.
    float groundY = -FLT_MAX;
    if (m_collisionWorld) {
        Ray down;
        down.origin[0] = pos.x;
        down.origin[1] = pos.y + 4.0f;
        down.origin[2] = pos.z;
        down.direction[0] = 0.0f;
        down.direction[1] = -1.0f;
        down.direction[2] = 0.0f;
        RayHit hit;
        if (m_collisionWorld->Raycast(down, hit))
            groundY = hit.point[1];
    }

    const float feetY = pos.y; // transform origin sits at the feet
    const bool landed = feetY <= groundY + pc->groundSnapMax;
    if (landed && p->velocity.y <= 0.0f) {
        pos.y = groundY;             // stand exactly on the floor
        p->velocity.y = 0.0f;
        p->onGround = true;
    } else {
        p->onGround = false;
    }

    // --- Horizontal control (ground only; air keeps momentum) ---------------
    if (p->onGround) {
        glm::vec3 move(0.0f);
        const float cy = std::cos(m_camera.GetYaw()), sy = std::sin(m_camera.GetYaw());
        if (m_input.IsDown(Button::Forward)) move += glm::vec3(sy, 0.0f, cy);
        if (m_input.IsDown(Button::Back))    move -= glm::vec3(sy, 0.0f, cy);
        if (m_input.IsDown(Button::Left))    move -= glm::vec3(cy, 0.0f, -sy);
        if (m_input.IsDown(Button::Right))   move += glm::vec3(cy, 0.0f, -sy);
        if (move.x != 0.0f || move.z != 0.0f) {
            move = glm::normalize(move);
            const float speed = pc->moveSpeed *
                (m_input.IsDown(Button::Sprint) ? 2.0f : 1.0f);
            pos += move * (speed * dt);
            // Face the movement direction (shortest-arc turn).
            const float targetYaw = std::atan2(move.x, move.z);
            float dyaw = targetYaw - pc->facingYaw;
            while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
            while (dyaw < -3.14159265f) dyaw += 6.28318531f;
            pc->facingYaw += dyaw * std::min(1.0f, pc->turnSpeed * dt);
            pc->facingValid = true;
            t->rotation.y = pc->facingYaw;
        }
    }

    // --- Wall collision: resolve the 0.5-radius sphere against level AABBs ---
    if (m_collisionWorld) {
        const float in[3]  = { pos.x, pos.y + pc->radius, pos.z };
        float out[3];
        if (m_collisionWorld->ResolveSphere(in, pc->radius, out))
            pos = glm::vec3(out[0], pos.y, out[2]);
    }

    t->position = pos;
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

// Track 14: integrate entity physics. Only entities with a PhysicsComponent
// move; the world floor (y = 0) grounds them, setting onGround and zeroing
// the vertical velocity.
void Engine::UpdateEntities(float dt) {
    if (dt <= 0.0f) return;
    m_entities.ForEachPhysics([this, dt](EntityID id, Transform& t, PhysicsComponent& p) {
        // Track 15: Jimmy integrates his own physics (raycast ground + wall
        // resolve); the y=0 world-floor clamp below would fight it.
        if (m_entities.GetPlayer(id)) return;
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
        if (mc->isJimmy) {
            if (!m_jimmyMeshValid) return;
            er.model = t.Matrix(); // player mesh is model-space: use transform
            er.gpu = &m_jimmyMesh;
            er.subs = m_jimmySubs;
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

    if (!m_worldMeshes.empty()) {
        const WorldMesh* anchor = &m_worldMeshes.front();
        for (const WorldMesh& wm : m_worldMeshes)
            if (wm.gpu.vertexCount > anchor->gpu.vertexCount) anchor = &wm;
        SpawnTestEntities(0.5f * (glm::make_vec3(anchor->boundsMin) +
                                  glm::make_vec3(anchor->boundsMax)));
    } else {
        SpawnTestEntities(glm::vec3(0.0f, 0.5f, 0.0f));
    }

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
    static const bool s_jimmyDemo = std::getenv("OPENBULLY_JIMMY_DEMO") != nullptr;
    if (s_jimmyDemo && m_jimmyActive) {
        const double t = m_time;
        // Phase (seconds): 0-0.5 W (short walk), 0.5-1.0 S (walk back),
        // 1.0-1.5 A (strafe), then idle — short bursts keep Jimmy on the
        // small demo platforms.
        m_input.SetButton(Button::Forward, t < 0.5);
        m_input.SetButton(Button::Back,    t >= 0.5 && t < 1.0);
        m_input.SetButton(Button::Left,    t >= 1.0 && t < 1.5);
    }

    m_camera.Update(float(deltaTime), m_input, /*ignoreMove=*/m_jimmyActive);

    // Track 14: entity update (physics integration on gravity carriers).
    UpdateEntities(float(deltaTime));

    // Track 15: Jimmy input/physics, then the 3rd-person follow camera.
    UpdateJimmy(float(deltaTime));
    UpdateJimmyCamera(float(deltaTime));

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

        // Keep the reference triangle as a small 2D overlay in view 0 so the
        // Phase-2 regression baseline stays visibly green.
        m_renderer->DrawMesh(0, 3, 0.6f, -0.6f, 0.15f, 0.15f);
    } else {
        // Triangle-only mode (pre-extraction): full-size reference triangle.
        m_renderer->DrawMesh(0, 3, 0.0f, 0.0f, 1.0f, 1.0f);
    }

    m_renderer->Frame(); // present (VSync via BGFX_RESET_VSYNC)

    // One-shot verification capture (see OPENBULLY_SHOT in Init).
    if (!m_screenshotName.empty() && !m_screenshotPending && m_time >= 1.0) {
        m_screenshotPending = true;
        m_renderer->RequestScreenshot(m_screenshotName.c_str());
    }
    ++m_frameCount;
}

void Engine::Shutdown() {
    if (!m_initialized) return;

    Logger::Info("Shutting down engine...");
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
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    m_initialized = false;
    Logger::Info("Engine shutdown complete");
}

} // namespace OpenBully
