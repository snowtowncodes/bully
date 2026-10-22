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
//   - uint16_t indices: meshes are split before exceeding 65,535 vertices

#include <cstdint>
#include <string>
#include <vector>

namespace OpenBully {

struct NifLoadResult;
struct Mesh;

// Assembles a Mesh from a successful NIF load result. Returns false when the
// result carries no geometry or would exceed the 16-bit index limit.
bool MeshFromNif(const NifLoadResult& nif, Mesh& out);

struct Vertex {
    float position[3];  // XYZ, left-handed, Y-up (Gamebryo convention)
    float normal[3];    // unit normal
    float uv[2];        // texture coords (first UV set)
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;  // triangle list
    const char* name = nullptr;     // debug label (points at NIF-derived name)

    // -- Convenience accessors (do not break the contract layout above) --
    bool IsValid() const { return !vertices.empty() && !indices.empty(); }
    size_t TriangleCount() const { return indices.size() / 3; }
    size_t VertexCount() const { return vertices.size(); }
};

} // namespace OpenBully
