#pragma once

// OpenBully: GPU-uploadable mesh container — the Track 3 <-> Track 4 contract
// (docs/checkpoints/mesh-contract.md).
//
// Track 3 (Data Loader) creates Mesh from NIF and owns CPU-side data.
// Track 4 (Rendering) reads Mesh, uploads to GPU, owns GPU buffers.
// This file is renderer-agnostic: no bgfx/GL types may appear here.
//
// Contract points:
//   - left-handed coordinates (Gamebryo), Y-up world space
//   - normals are unit vectors (loader normalizes)
//   - NOTE: index width deviates from the original Hour-24 contract doc:
//     Track 4's uploader uses 32-bit indices (BGFX_BUFFER_INDEX32), so Mesh
//     stores uint32_t. The 65,535-vertex split rule is lifted accordingly.

#include "../physics/collision.h"

#include <cstdint>
#include <string>
#include <vector>
namespace OpenBully {

struct NifLoadResult;
struct Mesh;

// Assembles a Mesh from a successful NIF load result. Returns false when the
// result carries no geometry.
bool MeshFromNif(const NifLoadResult& nif, Mesh& out);

// Interleaved vertex: position + unit normal + first UV set. Member order is
// the GPU layout (Track 4 uploads the vector blob directly).
struct MeshVertex {
    float x, y, z;      // position, left-handed, Y-up (Gamebryo convention)
    float nx, ny, nz;   // unit normal
    float u, v;         // texture coords (first UV set)
};

// Contract-doc spelling kept as an alias so either name compiles.
using Vertex = MeshVertex;

// Track 17: bone palette budget shared by the skinned mesh builder and the
// animation controller (character rigs are 36 bones).
constexpr int kMaxSkinBones = 64;

// Track 17: skinned vertex = MeshVertex + 4 bone indices + 4 weights.
// Member order is the GPU layout. Indices are uint8 bone palette slots
// (weight 0 entries pad); weights are normalized [0,1] floats.
struct SkinnedMeshVertex {
    float x, y, z;            // position, bind pose (model space)
    float nx, ny, nz;         // unit normal
    float u, v;               // texture coords (first UV set)
    uint8_t bone[4] = {0, 0, 0, 0}; // bone indices into the rig's palette
    float weight[4] = {1, 0, 0, 0}; // weights (sum 1)
};

#include "../physics/collision.h"

struct MeshSubMesh {
    std::string name;          // owning NiTriShape/NiTriStrips node
    uint32_t indexOffset = 0;  // into Mesh::indices
    uint32_t indexCount = 0;
    // Base-map texture name resolved from the NIF's NiTexturingProperty
    // (bare file name, e.g. "wall01"). Empty = untextured (loader will bind
    // a placeholder). Track 11.
    std::string textureName;
    AABB bounds; // world-space bounds over this sub-mesh's vertices (Track 13)
    // Track 17: skinned-shape index for the draw-time bone palette
    // (-1 = static geometry / rigid). Indexes the AnimationController's
    // per-shape bind list (see animation_controller.h ShapeBind).
    int shapeIndex = -1;
};

struct Mesh {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;  // triangle list (32-bit; see note above)
    std::vector<MeshSubMesh> subMeshes;
    const char* name = nullptr;     // debug label (NIF-derived, static storage)

    // -- Convenience accessors --
    bool IsValid() const { return !vertices.empty() && !indices.empty(); }
    size_t TriangleCount() const { return indices.size() / 3; }
    size_t VertexCount() const { return vertices.size(); }

    // Recomputes Mesh::bounds over all vertices and per-submesh bounds over
    // each submesh's index range (Track 13 collision contract).
    void ComputeCollisionBounds();

    AABB bounds; // whole-mesh bounds over vertices
};

// Track 17: skinned mesh assembled from a NIF with skin data. Bone indices
// refer to the rig's skeleton (DFS under the skin's skeleton root); rigid
// sub-shapes are 100%-weighted to their nearest skinned-ancestor bone.
struct SkinnedMesh {
    std::vector<SkinnedMeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshSubMesh> subMeshes;
    const char* name = nullptr;   // debug label

    bool IsValid() const { return !vertices.empty() && !indices.empty(); }
    size_t VertexCount() const { return vertices.size(); }
};

// Assembles a SkinnedMesh from a NIF load result. Requires at least one
// geometry with resolved skin weights (NifLoadResult::skins) or rigid
// geometries under a bone; returns false when the result carries no skin.
//
// `outShapeBinds` (optional, 16 floats per matrix, column-major, kMaxSkinBones
// matrices per shape) receives one bind-matrix palette per skinned shape
// (NiSkinInstance): slot s = the skin's bone composed through the DFS
// hierarchy (the S_os of the Gamebryo formula). Feed each palette to
// AnimationController::AddShapeBind and draw each submesh with the palette
// of MeshSubMesh::shapeIndex.
bool SkinnedMeshFromNif(const NifLoadResult& nif, SkinnedMesh& out,
                        std::vector<std::vector<float>>* outShapeBinds = nullptr);

} // namespace OpenBully
