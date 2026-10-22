// OpenBully: Mesh assembly from NIF data (Track 3 side of the mesh contract).
// Converts NifLoadResult geometries into a GPU-uploadable Mesh:
//   - all sub-mesh vertices concatenated into one vertex pool
//   - indices rebased per sub-mesh, widened to uint16_t triangle list
//   - normals normalized (contract requirement), UVs from the first UV set
//   - meshes exceeding 65,535 vertices are rejected (contract: split first)

#include "mesh.h"
#include "../engine/formats/nif_loader.h"

#include <cmath>

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
    if (nif.totalVertices > 0xFFFF) {
        // Contract: Track 3 must split over-large meshes; until splitting
        // exists, reject rather than silently truncate.
        return false;
    }

    out = Mesh{};
    out.vertices.reserve(nif.totalVertices);
    out.indices.reserve(nif.totalTriangles * 3);

    // Debug label: node name of the first geometry, else the file version
    // string. Static so `const char* name` stays valid for the mesh lifetime.
    static std::string nameStorage;
    nameStorage = nif.geometries.front().nodeName;

    for (const NifGeometryData& geo : nif.geometries) {
        const size_t base = out.vertices.size();
        const size_t n = geo.vertices.size();
        const bool hasNormals = geo.normals.size() == n;
        const bool hasUV = !geo.uvSets.empty() && geo.uvSets[0].size() == n;

        for (size_t i = 0; i < n; i++) {
            Vertex v{};
            v.position[0] = geo.vertices[i].x;
            v.position[1] = geo.vertices[i].y;
            v.position[2] = geo.vertices[i].z;
            if (hasNormals) {
                v.normal[0] = geo.normals[i].x;
                v.normal[1] = geo.normals[i].y;
                v.normal[2] = geo.normals[i].z;
                Normalize(v.normal[0], v.normal[1], v.normal[2]);
            }
            if (hasUV) {
                v.uv[0] = geo.uvSets[0][i].u;
                v.uv[1] = geo.uvSets[0][i].v;
            }
            out.vertices.push_back(v);
        }

        for (const NifTriangle& t : geo.triangles) {
            out.indices.push_back(static_cast<uint16_t>(base + t.a));
            out.indices.push_back(static_cast<uint16_t>(base + t.b));
            out.indices.push_back(static_cast<uint16_t>(base + t.c));
        }
    }

    out.name = nameStorage.c_str();
    return out.IsValid();
}

} // namespace OpenBully
