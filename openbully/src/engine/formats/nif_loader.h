#pragma once

// OpenBully: NIF (Gamebryo/NetImmerse) mesh loader.
// NIF v20.3.0.9, Bully SE (PC). Parses:
//   - header (version check, little/big endianness)
//   - block table (types, sizes, strings) so unknown blocks are skipped
//   - NiNode / NiTriShape / NiTriStrips hierarchy: per-node transforms,
//     parent/children links (used for world-space positioning)
//   - NiTriShape / NiTriStrips geometry: vertex positions, normals, UV sets,
//     triangle indices (strips converted to triangles)
//
// Format notes (verified against bully-re/docs/nif.md and hex dumps of
// retail assets):
//   - header is a newline-terminated text line, NOT length-prefixed
//   - hex version 0x14030009 ("20.3.0.9"), user version 0
//   - block count is always little-endian; endianness applies from the type
//     count onward (247 CS_* cutscene files are big-endian)
//   - NiTriShape/NiTriStrips carry an NiObjectNET header; their data blocks
//     (NiTriShapeData/NiTriStripsData) start directly at NiGeometryData
//   - every block's declared size lets unknown blocks be skipped exactly
//
// Animations, controllers, skins, materials and textures are skipped, not
// interpreted (Track 3 scope).

#include <cstdint>
#include <string>
#include <vector>

namespace OpenBully {

// 0x14030009 == "20.3.0.9"
constexpr uint32_t NIF_VERSION_BULLY = 0x14030009u;

struct NifVector3 {
    float x, y, z;
};

struct NifVector2 {
    float u, v;
};

struct NifTriangle {
    uint16_t a, b, c;
};

// One geometry piece extracted from the file (one per NiTriShape/NiTriStrips
// data block, paired with its owning shape node).
// Vertex skin weights: up to 4 influences per vertex (indices into the
// owning shape's NifSkinData::bones). Short entries pad with weight 0.
// Defined before NifGeometryData, which stores one per vertex.
struct NifVertexWeights {
    uint8_t index[4] = {0, 0, 0, 0};
    float   weight[4] = {1, 0, 0, 0}; // default: rigid to bone 0
    uint8_t count = 1;
};

struct NifGeometryData {
    std::string nodeName;        // name of the owning NiTriShape/NiTriStrips
    std::vector<NifVector3> vertices;
    std::vector<NifVector3> normals;
    std::vector<std::vector<NifVector2>> uvSets; // one inner vector per UV set
    std::vector<NifTriangle> triangles;          // strips already converted
    uint32_t nodeIndex = 0;      // index into NifLoadResult::nodes (owner)
    // Base-map texture name from the shape's NiTexturingProperty ->
    // NiSourceTexture, stripped to a bare file name ("wall01"). Empty when
    // the shape carries no texturing property (Track 11).
    std::string textureName;
    // Track 17: skin block (NiSkinInstance) referenced by the owning shape.
    // 0xFFFFFFFF = unskinned (rigid) geometry. Per-vertex weights resolved
    // into `weights` after the whole file parses.
    uint32_t skinIndex = 0xFFFFFFFF;
    std::vector<NifVertexWeights> weights; // parallel to vertices
};

// One scene-graph node (NiNode / NiTriShape / NiTriStrips): local transform
// + hierarchy links. World transforms compose child-to-root.
// NOTE (Track 17): after LoadNifFromMemory's post-pass, `children` hold
// positions in NifLoadResult::nodes (NOT raw block indices) and `parent` is
// a nodes[] position too, so hierarchy walks stay index-consistent.
struct NifNode {
    std::string name;
    NifVector3 translation = {0, 0, 0};
    float rotation[9] = {
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
    };                           // row-major 3x3 (NIF file order)
    float scale = 1.0f;
    int32_t parent = -1;         // index into the same nodes array
    std::vector<uint32_t> children;
    bool isShape = false;        // NiTriShape/NiTriStrips (leaf, not a bone)
};

// Track 17: one skinned shape's skinning data.
//
// NiSkinInstance: skinData ref, skinPartition ref, skeleton root ref, bone
// count + bone node refs. NiSkinData: overall transform, then per bone a bind
// transform (row-major 3x3, translation, scale), bounding sphere, and the
// weighted vertex list (vertex index + weight, weight 0 when absent).
// Weights are present in all 2 614 retail blocks (docs/nif.md).
struct NifSkinBone {
    float rotation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // bind pose, row-major
    float translation[3] = {0, 0, 0};
    float scale = 1.0f;
    std::vector<uint32_t> vertexIndices;
    std::vector<float>    vertexWeights;
};

struct NifSkinData {
    uint32_t skinDataBlock  = 0xFFFFFFFF; // NiSkinData block index
    uint32_t skeletonRoot   = 0xFFFFFFFF; // skeleton root NiNode block index
    std::vector<uint32_t>   boneNodes;    // NiNode block index per bone
    std::vector<NifSkinBone> bones;       // parallel to boneNodes
};

// Vertex skin weights: up to 4 influences per vertex (indices into the
// owning shape's NifSkinData::bones). Short entries pad with weight 0.
struct NifLoadResult {
    bool ok = false;
    std::string error;                 // empty when ok
    uint32_t version = 0;              // hex NIF version, e.g. 0x14030009
    uint32_t numBlocks = 0;
    bool bigEndian = false;            // CS_* cutscene exports are big-endian
    std::vector<NifGeometryData> geometries;
    std::vector<NifNode> nodes;        // full node hierarchy (block order)
    // Track 17: raw NiBlock index -> nodes[] position (-1 when the block is
    // not a node/shape). Skin instance bone refs and skeleton roots are
    // block indices; map them through this before touching nodes[].
    std::vector<int32_t> blockToNode;
    // Track 17: parsed NiSkinInstance blocks (index = block index).
    std::vector<NifSkinData> skins;
    size_t totalVertices = 0;
    size_t totalTriangles = 0;
};

// Parse a NIF file from memory. `data` must remain valid only for the call.
NifLoadResult LoadNifFromMemory(const uint8_t* data, size_t size);

// Convenience: read the file at `path` and parse it.
NifLoadResult LoadNifFile(const std::string& path);

// Compose the world transform of node `index` (child-to-root) into a
// column-major 4x4 matrix (bgfx convention). Returns false for out-of-range
// index; cycles are broken at 64 levels (defensive; NIFs are trees).
bool NifWorldTransform(const NifLoadResult& nif, uint32_t index, float out[16]);

} // namespace OpenBully
