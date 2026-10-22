// Exhaustive convention search: which (node-compose, S-compose, order,
// vertex-space) reproduces stored vertices at rest?
#include <cstdio>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>
#include "../engine/formats/nif_loader.h"

using namespace OpenBully;

// conv: 0 = glm[c][r]=stored[c*3+r] (column=stored row, v*R reading)
//       1 = glm[c][r]=stored[r*3+c] (column=stored col, R*v reading)
static glm::mat4 FromRM(const float r[9], float s, float tx, float ty, float tz, int conv) {
    glm::mat4 m(1.0f);
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++)
            m[c][row] = (conv == 0 ? r[c * 3 + row] : r[row * 3 + c]) * s;
    m[3][0] = tx; m[3][1] = ty; m[3][2] = tz;
    return m;
}

int main(int argc, char** argv) {
    const std::string worldDir = argc > 1 ? argv[1] : "data/extracted/world";
    NifLoadResult nif = LoadNifFile(worldDir + "/Player_Mascot.nif");
    if (!nif.ok) { std::printf("load fail\n"); return 1; }

    const NifGeometryData* geo = nullptr;
    const NifSkinData* skin = nullptr;
    for (const NifGeometryData& g : nif.geometries)
        if (g.skinIndex != 0xFFFFFFFF && g.skinIndex < nif.skins.size() &&
            g.weights.size() == g.vertices.size() && g.vertices.size() > 100) {
            geo = &g; skin = &nif.skins[g.skinIndex]; break;
        }
    if (!geo || !skin) { std::printf("no skinned geo\n"); return 1; }

    // Shape-node world (both conventions).
    auto chainWorld = [&](uint32_t block, int conv) -> glm::mat4 {
        glm::mat4 w(1.0f);
        int32_t chain[64]; int depth = 0;
        int32_t pos = block < nif.blockToNode.size() ? nif.blockToNode[block] : -1;
        while (pos >= 0 && size_t(pos) < nif.nodes.size() && depth < 64) {
            chain[depth++] = pos;
            pos = nif.nodes[size_t(pos)].parent;
        }
        for (int d = depth - 1; d >= 0; d--) {
            const NifNode& nn = nif.nodes[size_t(chain[d])];
            w = w * FromRM(nn.rotation, nn.scale, nn.translation.x,
                           nn.translation.y, nn.translation.z, conv);
        }
        return w;
    };

    for (int vc = 0; vc < 2; vc++) {          // vertex bake convention
        const glm::mat4 shapeW = chainWorld(geo->nodeIndex, vc);
        for (int bc = 0; bc < 2; bc++) {      // bone world convention
            for (int sc = 0; sc < 2; sc++) {  // S convention
                for (int order = 0; order < 2; order++) {
                    double err = 0; int count = 0;
                    for (size_t v = 0; v < geo->vertices.size(); v++) {
                        const NifVertexWeights& w = geo->weights[v];
                        if (w.count == 0) continue;
                        const glm::vec4 raw(geo->vertices[v].x, geo->vertices[v].y,
                                            geo->vertices[v].z, 1.0f);
                        const glm::vec3 want = glm::vec3(shapeW * raw);
                        glm::vec4 out(0.0f);
                        for (int k = 0; k < 4 && k < w.count; k++) {
                            if (w.weight[k] <= 0.0f) continue;
                            const uint32_t b = w.index[k];
                            if (b >= skin->bones.size()) continue;
                            const glm::mat4 W = chainWorld(skin->boneNodes[b], bc);
                            const NifSkinBone& sb = skin->bones[b];
                            const glm::mat4 S = FromRM(sb.rotation, sb.scale,
                                sb.translation[0], sb.translation[1],
                                sb.translation[2], sc);
                            const glm::mat4 M = order == 0 ? W * S : S * W;
                            out += w.weight[k] * (M * raw); // raw in shape space
                        }
                        err += glm::length(glm::vec3(out) - want);
                        count++;
                    }
                    if (count && err / count < 0.05)
                        std::printf("MATCH vc=%d bc=%d sc=%d order=%s err=%.4f\n",
                                    vc, bc, sc, order == 0 ? "W*S" : "S*W",
                                    err / count);
                }
            }
        }
    }
    std::printf("search done\n");
    return 0;
}
