// OpenBully: Mesh assembly from NIF data (Track 3 side of the mesh contract).
// Converts NifLoadResult geometries into a GPU-uploadable Mesh:
//   - all sub-mesh vertices concatenated into one vertex pool
//   - indices rebased per sub-mesh, stored as a 32-bit triangle list
//   - normals normalized (contract requirement), UVs from the first UV set
//   - one MeshSubMesh per NIF geometry, with its index range recorded

#include "mesh.h"
#include "../engine/formats/nif_loader.h"
#include "../physics/collision.h"

#include <glm/glm.hpp>

// Track 17: bone palette budget (kept in sync with animation_controller.h,
// whose header cannot be included here — nif_test builds mesh.cpp).
namespace { constexpr int kMaxSkinBonesLocal = 64; }

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace OpenBully {

namespace {

void Normalize(float& x, float& y, float& z) {
    const float len = std::sqrt(x * x + y * y + z * z);
    if (len > 1e-12f) {
        x /= len;
        y /= len;
        z /= len;
    } else {
        x = 0.0f;
        y = 1.0f;
        z = 0.0f;
    }
}

} // namespace

bool MeshFromNif(const NifLoadResult& nif, Mesh& out) {
    if (!nif.ok || nif.geometries.empty()) return false;
    if (nif.totalVertices == 0 || nif.totalTriangles == 0) return false;

    out = Mesh{};
    out.vertices.reserve(nif.totalVertices);
    out.indices.reserve(nif.totalTriangles * 3);

    // Debug label: node name of the first geometry. Static so `const char*
    // name` stays valid for the mesh lifetime.
    static std::string nameStorage;
    nameStorage = nif.geometries.front().nodeName;

    for (const NifGeometryData& geo : nif.geometries) {
        MeshSubMesh sub;
        sub.name = geo.nodeName;
        sub.textureName = geo.textureName;
        sub.indexOffset = static_cast<uint32_t>(out.indices.size());

        const size_t base = out.vertices.size();
        const size_t n = geo.vertices.size();
        const bool hasNormals = geo.normals.size() == n;
        const bool hasUV = !geo.uvSets.empty() && geo.uvSets[0].size() == n;

        for (size_t i = 0; i < n; i++) {
            MeshVertex v{};
            v.x = geo.vertices[i].x;
            v.y = geo.vertices[i].y;
            v.z = geo.vertices[i].z;
            if (hasNormals) {
                v.nx = geo.normals[i].x;
                v.ny = geo.normals[i].y;
                v.nz = geo.normals[i].z;
                Normalize(v.nx, v.ny, v.nz);
            }
            if (hasUV) {
                v.u = geo.uvSets[0][i].u;
                v.v = geo.uvSets[0][i].v;
            }
            out.vertices.push_back(v);
        }

        for (const NifTriangle& t : geo.triangles) {
            out.indices.push_back(static_cast<uint32_t>(base + t.a));
            out.indices.push_back(static_cast<uint32_t>(base + t.b));
            out.indices.push_back(static_cast<uint32_t>(base + t.c));
        }

        sub.indexCount =
            static_cast<uint32_t>(out.indices.size()) - sub.indexOffset;
        out.subMeshes.push_back(std::move(sub));
    }

    out.name = nameStorage.c_str();
    out.ComputeCollisionBounds();
    return out.IsValid();
}

// --- Track 17: skinned mesh -------------------------------------------------

bool SkinnedMeshFromNif(const NifLoadResult& nif, SkinnedMesh& out,
                        std::vector<std::vector<float>>* outShapeBinds) {
    if (!nif.ok || nif.geometries.empty()) return false;

    // The rig: the first valid skin's skeleton root defines the bone DFS
    // order (all skinned shapes in a ped NIF share it).
    uint32_t skeletonRootBlock = 0xFFFFFFFF;
    for (const NifGeometryData& geo : nif.geometries) {
        if (geo.skinIndex != 0xFFFFFFFF && geo.skinIndex < nif.skins.size()) {
            skeletonRootBlock = nif.skins[geo.skinIndex].skeletonRoot;
            if (skeletonRootBlock != 0xFFFFFFFF) break;
        }
    }
    if (skeletonRootBlock == 0xFFFFFFFF) return false; // no skinned geometry

    // Map the skeleton root block -> nodes[] position (blocks interleave).
    uint32_t skeletonRoot = skeletonRootBlock;
    if (skeletonRootBlock < nif.blockToNode.size() &&
        nif.blockToNode[skeletonRootBlock] >= 0)
        skeletonRoot = static_cast<uint32_t>(nif.blockToNode[skeletonRootBlock]);
    if (skeletonRoot >= nif.nodes.size()) return false;

    // Re-root on "Dummy" when present (same rule as BuildSkeleton: .agr
    // tracks count from Dummy downward; palette slots must match).
    {
        std::vector<uint32_t> q{skeletonRoot};
        for (size_t qi = 0; qi < q.size(); qi++) {
            if (q[qi] >= nif.nodes.size()) continue;
            if (nif.nodes[q[qi]].name == "Dummy") { skeletonRoot = q[qi]; break; }
            for (uint32_t c : nif.nodes[q[qi]].children) q.push_back(c);
        }
    }

    // Bone slot per nodes[] position: DFS under the skeleton root, BONE
    // NODES ONLY (== .agr track order, see animation_controller.h). Nodes
    // outside the subtree map to -1 (their vertices fall back to the
    // nearest ancestor bone).
    std::vector<int32_t> nodeToBone(nif.nodes.size(), -1);
    std::vector<std::pair<uint32_t, int32_t>> work;
    work.push_back({skeletonRoot, -1});
    int32_t boneCount = 0;
    while (!work.empty()) {
        auto [node, parentBone] = work.back();
        work.pop_back();
        if (node >= nif.nodes.size()) continue;
        const NifNode& n = nif.nodes[node];
        if (!n.isShape) {
            const int32_t myBone = boneCount;
            if (myBone >= kMaxSkinBonesLocal) break; // palette budget
            nodeToBone[node] = myBone;
            boneCount++;
            parentBone = myBone;
        }
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it)
            work.push_back({*it, parentBone});
    }
    if (boneCount == 0) return false;

    // Nearest skinned ancestor for rigid shapes (by node -> parent chain).
    auto ancestorBone = [&](uint32_t nodePos) -> int32_t {
        int32_t p = (nodePos < nif.nodes.size())
            ? nif.nodes[nodePos].parent : -1;
        int depth = 0;
        while (p >= 0 && static_cast<uint32_t>(p) < nif.nodes.size() && depth++ < 64) {
            if (nodeToBone[static_cast<size_t>(p)] >= 0)
                return nodeToBone[static_cast<size_t>(p)];
            p = nif.nodes[static_cast<size_t>(p)].parent;
        }
        return 0; // root bone
    };

    // Track 17: palette remap per skinned geometry. Vertex weights index the
    // NiSkinInstance bone list (skin bones array), whose entries are raw
    // block refs; the shader palette is the DFS bone order, so translate:
    // skin bone b -> boneNodes[b] block -> nodes[] pos -> DFS slot.
    auto paletteRemap = [&](const NifGeometryData& geo) {
        std::vector<int32_t> remap;
        if (geo.skinIndex == 0xFFFFFFFF || geo.skinIndex >= nif.skins.size())
            return remap;
        const NifSkinData& skin = nif.skins[geo.skinIndex];
        remap.resize(skin.bones.size(), -1);
        for (size_t b = 0; b < skin.bones.size(); b++) {
            const uint32_t block = skin.boneNodes[b];
            const int32_t pos = (block < nif.blockToNode.size())
                ? nif.blockToNode[block] : -1;
            remap[b] = (pos >= 0 && static_cast<size_t>(pos) < nodeToBone.size())
                ? nodeToBone[static_cast<size_t>(pos)] : -1;
        }
        return remap;
    };

    out = SkinnedMesh{};
    if (outShapeBinds) outShapeBinds->clear();
    // Shape index per skin block: skinned geometries sharing a NiSkinInstance
    // share the palette; uniques get sequential indices as they appear.
    std::unordered_map<uint32_t, int> skinBlockToShape;
    static std::string skinNameStorage;
    skinNameStorage = nif.geometries.front().nodeName;
    out.name = skinNameStorage.c_str();

    // Compose the bind (S_os) matrix of one skin bone: NiSkinData transform
    // composed with the node's bind-pose world transform (verified formula in
    // docs/nif.md; same convention as bully-re NifSkin.cpp DeLiaison/Composer).
    auto bindMatrix = [&](uint32_t boneBlock, const NifSkinBone& sb) -> glm::mat4 {
        // Node world bind transform (child-to-root, nodes[] positions).
        glm::mat4 world(1.0f);
        int32_t chain[64];
        int depth = 0;
        int32_t pos = (boneBlock < nif.blockToNode.size())
            ? nif.blockToNode[boneBlock] : -1;
        while (pos >= 0 && static_cast<uint32_t>(pos) < nif.nodes.size() &&
               depth < 64) {
            chain[depth++] = pos;
            pos = nif.nodes[static_cast<size_t>(pos)].parent;
        }
        for (int d = depth - 1; d >= 0; d--) {
            const NifNode& nn = nif.nodes[static_cast<size_t>(chain[d])];
            glm::mat4 lm(1.0f);
            for (int c = 0; c < 3; c++)
                for (int row = 0; row < 3; row++)
                    lm[c][row] = nn.rotation[row * 3 + c] * nn.scale;
            lm[3][0] = nn.translation.x;
            lm[3][1] = nn.translation.y;
            lm[3][2] = nn.translation.z;
            world = world * lm;
        }
        // S_os: bind transform from NiSkinData (row-major 3x3 + t + scale),
        // same v*R composition convention as the node chain above.
        glm::mat4 S(1.0f);
        for (int c = 0; c < 3; c++)
            for (int row = 0; row < 3; row++)
                S[c][row] = sb.rotation[row * 3 + c] * sb.scale;
        S[3][0] = sb.translation[0];
        S[3][1] = sb.translation[1];
        S[3][2] = sb.translation[2];
        return world * S;
    };

    // One bind palette per skinned shape (first-appearance order); submeshes
    // reference theirs via shapeIndex. Vertices of a shape carry only that
    // shape's influences, so per-shape palettes are required.
    auto ensureShapeBind = [&](uint32_t skinBlock) -> int {
        auto it = skinBlockToShape.find(skinBlock);
        if (it != skinBlockToShape.end()) return it->second;
        const NifSkinData& skin = nif.skins[skinBlock];
        std::vector<glm::mat4> bind(kMaxSkinBonesLocal, glm::mat4(1.0f));
        for (size_t b = 0; b < skin.bones.size() && b < skin.boneNodes.size(); b++) {
            const uint32_t block = skin.boneNodes[b];
            const int32_t pos = (block < nif.blockToNode.size())
                ? nif.blockToNode[block] : -1;
            const int32_t slot = (pos >= 0 && static_cast<size_t>(pos) < nodeToBone.size())
                ? nodeToBone[static_cast<size_t>(pos)] : -1;
            if (slot < 0) continue;
            bind[static_cast<size_t>(slot)] = bindMatrix(block, skin.bones[b]);
        }
        if (outShapeBinds) {
            outShapeBinds->emplace_back();
            std::vector<float>& packed = outShapeBinds->back();
            packed.reserve(size_t(kMaxSkinBonesLocal) * 16);
            for (int s = 0; s < kMaxSkinBonesLocal; s++) {
                const glm::mat4& m = bind[static_cast<size_t>(s)];
                for (int c = 0; c < 4; c++)
                    for (int r = 0; r < 4; r++)
                        packed.push_back(m[c][r]);
            }
        }
        const int idx = outShapeBinds ? int(outShapeBinds->size()) - 1 : 0;
        skinBlockToShape[skinBlock] = idx;
        return idx;
    };

    for (const NifGeometryData& geo : nif.geometries) {
        if (geo.vertices.empty() || geo.triangles.empty()) continue;

        MeshSubMesh sub;
        sub.name = geo.nodeName;
        sub.textureName = geo.textureName;
        sub.indexOffset = static_cast<uint32_t>(out.indices.size());

        const size_t base = out.vertices.size();
        const size_t n = geo.vertices.size();
        const bool hasNormals = geo.normals.size() == n;
        const bool hasUV = !geo.uvSets.empty() && geo.uvSets[0].size() == n;
        const bool skinned = geo.skinIndex != 0xFFFFFFFF &&
                             !geo.weights.empty() && geo.weights.size() == n;
        // geo.nodeIndex is a raw block ref; ancestorBone walks nodes[].
        int32_t rigidBone = 0;
        {
            uint32_t nodePos = geo.nodeIndex;
            if (nodePos < nif.blockToNode.size() && nif.blockToNode[nodePos] >= 0)
                nodePos = static_cast<uint32_t>(nif.blockToNode[nodePos]);
            rigidBone = skinned ? 0 : ancestorBone(nodePos);
        }
        const std::vector<int32_t> remap = skinned ? paletteRemap(geo)
                                                   : std::vector<int32_t>();
        // Per-shape palette index for the draw call (-1 for rigid shapes:
        // they use the identity/bind palette of the rig root).
        sub.shapeIndex = skinned ? ensureShapeBind(geo.skinIndex) : -1;

        // NOTE: vertices stay in their SHAPE-LOCAL space. The bind transform
        // S_os in NiSkinData already encodes the shape-node offset (verified:
        // bully-re NifSkinVertices applies the formula to raw NiGeometryData
        // vertices and gets entity-space positions), so no shape bake here.
        const glm::mat3 shapeRot(1.0f); (void)shapeRot;

        for (size_t i = 0; i < n; i++) {
            SkinnedMeshVertex v{};
            v.x = geo.vertices[i].x;
            v.y = geo.vertices[i].y;
            v.z = geo.vertices[i].z;
            if (hasNormals) {
                v.nx = geo.normals[i].x;
                v.ny = geo.normals[i].y;
                v.nz = geo.normals[i].z;
                Normalize(v.nx, v.ny, v.nz);
            }
            if (hasUV) {
                v.u = geo.uvSets[0][i].u;
                v.v = geo.uvSets[0][i].v;
            }
            if (skinned) {
                const NifVertexWeights& w = geo.weights[i];
                float wsum = 0.0f;
                int out_n = 0;
                for (int k = 0; k < 4 && k < w.count; k++) {
                    const int32_t slot = (size_t(w.index[k]) < remap.size())
                        ? remap[w.index[k]] : -1;
                    if (slot < 0 || w.weight[k] <= 0.0f) continue;
                    v.bone[out_n] = static_cast<uint8_t>(slot);
                    v.weight[out_n] = w.weight[k];
                    wsum += w.weight[k];
                    out_n++;
                }
                if (out_n == 0) {
                    // No usable influence: bind rigidly to the shape's bone.
                    v.bone[0] = static_cast<uint8_t>(rigidBone);
                    v.weight[0] = 1.0f;
                } else if (std::fabs(wsum - 1.0f) > 1e-4f && wsum > 1e-4f) {
                    for (int k = 0; k < out_n; k++) v.weight[k] /= wsum;
                }
            } else {
                // Rigid sub-shape: 100% bound to its nearest skeleton bone.
                v.bone[0] = static_cast<uint8_t>(rigidBone);
                v.weight[0] = 1.0f;
            }
            out.vertices.push_back(v);
        }

        for (const NifTriangle& t : geo.triangles) {
            out.indices.push_back(static_cast<uint32_t>(base + t.a));
            out.indices.push_back(static_cast<uint32_t>(base + t.b));
            out.indices.push_back(static_cast<uint32_t>(base + t.c));
        }

        sub.indexCount =
            static_cast<uint32_t>(out.indices.size()) - sub.indexOffset;
        out.subMeshes.push_back(std::move(sub));
    }

    return out.IsValid();
}

void Mesh::ComputeCollisionBounds() {
    constexpr float kMax = std::numeric_limits<float>::max();
    bounds.min[0] = kMax; bounds.min[1] = kMax; bounds.min[2] = kMax;
    bounds.max[0] = -kMax; bounds.max[1] = -kMax; bounds.max[2] = -kMax;
    for (const MeshVertex& v : vertices) {
        bounds.min[0] = std::min(bounds.min[0], v.x); bounds.max[0] = std::max(bounds.max[0], v.x);
        bounds.min[1] = std::min(bounds.min[1], v.y); bounds.max[1] = std::max(bounds.max[1], v.y);
        bounds.min[2] = std::min(bounds.min[2], v.z); bounds.max[2] = std::max(bounds.max[2], v.z);
    }
    for (MeshSubMesh& sub : subMeshes) {
        sub.bounds.min[0] = kMax; sub.bounds.min[1] = kMax; sub.bounds.min[2] = kMax;
        sub.bounds.max[0] = -kMax; sub.bounds.max[1] = -kMax; sub.bounds.max[2] = -kMax;
        for (uint32_t i = sub.indexOffset; i < sub.indexOffset + sub.indexCount; i++) {
            const MeshVertex& v = vertices[indices[i]];
            sub.bounds.min[0] = std::min(sub.bounds.min[0], v.x); sub.bounds.max[0] = std::max(sub.bounds.max[0], v.x);
            sub.bounds.min[1] = std::min(sub.bounds.min[1], v.y); sub.bounds.max[1] = std::max(sub.bounds.max[1], v.y);
            sub.bounds.min[2] = std::min(sub.bounds.min[2], v.z); sub.bounds.max[2] = std::max(sub.bounds.max[2], v.z);
        }
    }
}

} // namespace OpenBully
