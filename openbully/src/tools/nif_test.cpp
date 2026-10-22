// OpenBully: NIF loader command-line test harness.
//
//   nif_test <file.nif> [<file.nif> ...]
//
// Loads each file, prints vertex/triangle counts and the first vertices,
// and verifies all triangle indices are in range. Exit code 0 = every file
// loaded; 1 = at least one failure (each file is still attempted).

#include "engine/formats/nif_loader.h"
#include "rendering/mesh.h"

#include <cstdio>

using namespace OpenBully;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: nif_test <file.nif> [...]\n");
        return 1;
    }

    int failures = 0;
    for (int a = 1; a < argc; a++) {
        const char* path = argv[a];
        const NifLoadResult nif = LoadNifFile(path);
        if (!nif.ok) {
            std::printf("FAIL %s: %s\n", path, nif.error.c_str());
            failures++;
            continue;
        }

        Mesh mesh;
        const bool built = MeshFromNif(nif, mesh);

        std::printf("OK   %s\n", path);
        std::printf("     version 0x%08x, %u blocks, %s-endian, %zu geometries\n",
                    nif.version, nif.numBlocks,
                    nif.bigEndian ? "big" : "little", nif.geometries.size());
        std::printf("     vertices %zu, triangles %zu\n",
                    nif.totalVertices, nif.totalTriangles);
        if (built) {
            std::printf("     mesh: %zu verts, %zu indices, %zu sub-meshes, label '%s'\n",
                        mesh.VertexCount(), mesh.indices.size(),
                        mesh.subMeshes.size(),
                        mesh.name ? mesh.name : "");
            // First 3 vertices for eyeball verification against NifSkope.
            for (size_t i = 0; i < mesh.vertices.size() && i < 3; i++) {
                const MeshVertex& v = mesh.vertices[i];
                std::printf("       v[%zu] pos(%.4f, %.4f, %.4f) nrm(%.3f, %.3f, %.3f) uv(%.3f, %.3f)\n",
                            i, v.x, v.y, v.z, v.nx, v.ny, v.nz, v.u, v.v);
            }
        }

        // Index sanity: every index must be < vertex count of its geometry.
        bool indicesOk = true;
        for (const NifGeometryData& geo : nif.geometries) {
            for (const NifTriangle& t : geo.triangles) {
                if (t.a >= geo.vertices.size() || t.b >= geo.vertices.size() ||
                    t.c >= geo.vertices.size()) {
                    indicesOk = false;
                    break;
                }
            }
            if (!indicesOk) break;
        }
        if (!indicesOk) {
            std::printf("     FAIL: out-of-range triangle index\n");
            failures++;
        }
    }

    return failures == 0 ? 0 : 1;
}
