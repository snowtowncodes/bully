#include <cstdio>
#include <glm/glm.hpp>
#include "../engine/formats/nif_loader.h"

using namespace OpenBully;

static glm::mat4 NodeLocal(const NifNode& n) {
    glm::mat4 m(1.0f);
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++)
            m[c][row] = n.rotation[c * 3 + row] * n.scale;
    m[3][0] = n.translation.x; m[3][1] = n.translation.y; m[3][2] = n.translation.z;
    return m;
}
static void PrintM(const char* tag, const glm::mat4& m) {
    std::printf("%s:\n  %.3f %.3f %.3f %.3f\n  %.3f %.3f %.3f %.3f\n  %.3f %.3f %.3f %.3f\n  %.3f %.3f %.3f %.3f\n",
        tag, m[0][0],m[1][0],m[2][0],m[3][0], m[0][1],m[1][1],m[2][1],m[3][1],
        m[0][2],m[1][2],m[2][2],m[3][2], m[0][3],m[1][3],m[2][3],m[3][3]);
}

int main(int argc, char** argv) {
    const std::string worldDir = argc > 1 ? argv[1] : "data/extracted/world";
    NifLoadResult nif = LoadNifFile(worldDir + "/Player_Mascot.nif");
    if (!nif.ok) { std::printf("load fail\n"); return 1; }

    // Skin block 12: bones Root Head (block 15) and Root Spine2 (block 49).
    const NifSkinData& s = nif.skins[12];
    for (size_t b = 0; b < s.bones.size(); b++) {
        const uint32_t blk = s.boneNodes[b];
        const int32_t pos = nif.blockToNode[blk];
        // world = chain compose
        glm::mat4 w(1.0f);
        int32_t chain[64]; int depth = 0;
        int32_t p = pos;
        while (p >= 0 && size_t(p) < nif.nodes.size() && depth < 64) {
            chain[depth++] = p;
            p = nif.nodes[size_t(p)].parent;
        }
        for (int d = depth - 1; d >= 0; d--) w = w * NodeLocal(nif.nodes[size_t(chain[d])]);
        const NifSkinBone& sb = s.bones[b];
        glm::mat4 S(1.0f);
        for (int c = 0; c < 3; c++)
            for (int row = 0; row < 3; row++)
                S[c][row] = sb.rotation[c * 3 + row] * sb.scale;
        S[3][0] = sb.translation[0]; S[3][1] = sb.translation[1]; S[3][2] = sb.translation[2];
        char tag[64];
        std::snprintf(tag, sizeof(tag), "W(%s)", nif.nodes[size_t(pos)].name.c_str());
        PrintM(tag, w);
        std::snprintf(tag, sizeof(tag), "S(bone %zu)", b);
        PrintM(tag, S);
        std::snprintf(tag, sizeof(tag), "W*S(bone %zu)", b);
        PrintM(tag, w * S);
    }
    return 0;
}
