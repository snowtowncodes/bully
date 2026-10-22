#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <glm/glm.hpp>

#include "../engine/formats/nif_loader.h"

using namespace OpenBully;

int main(int argc, char** argv) {
    const std::string worldDir = argc > 1 ? argv[1] : "data/extracted/world";
    NifLoadResult nif = LoadNifFile(worldDir + "/Player_Mascot.nif");
    if (!nif.ok) { std::printf("load fail: %s\n", nif.error.c_str()); return 1; }
    // Node name table (block -> nodes pos -> name)
    for (size_t i = 0; i < nif.skins.size(); i++) {
        const NifSkinData& s = nif.skins[i];
        if (s.bones.empty()) continue;
        std::printf("skin block %zu: root=%u bones=%zu\n", i, s.skeletonRoot, s.bones.size());
        for (size_t b = 0; b < s.bones.size() && b < 8; b++) {
            const uint32_t blk = s.boneNodes[b];
            const int32_t pos = blk < nif.blockToNode.size() ? nif.blockToNode[blk] : -1;
            const char* nm = pos >= 0 ? nif.nodes[size_t(pos)].name.c_str() : "?";
            const NifSkinBone& sb = s.bones[b];
            std::printf("  bone %zu '%s' block=%u S.t=(%.3f,%.3f,%.3f) scale=%.2f\n",
                        b, nm, blk, sb.translation[0], sb.translation[1],
                        sb.translation[2], sb.scale);
        }
    }
    // Print local transforms of the chain from root to 'Root' and 'Root Pelvis'.
    for (const char* want : {"Scene Root", "Player_Mascot", "Dummy", "Root", "Root Pelvis", "Root L Thigh"}) {
        for (size_t i = 0; i < nif.nodes.size(); i++) {
            if (nif.nodes[i].name != want) continue;
            const NifNode& n = nif.nodes[i];
            std::printf("%-14s pos=%zu parent=%d t=(%.3f,%.3f,%.3f) rot=[%.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f] s=%.2f\n",
                        want, i, n.parent, n.translation.x, n.translation.y, n.translation.z,
                        n.rotation[0], n.rotation[1], n.rotation[2],
                        n.rotation[3], n.rotation[4], n.rotation[5],
                        n.rotation[6], n.rotation[7], n.rotation[8], n.scale);
            break;
        }
    }
    return 0;
}
