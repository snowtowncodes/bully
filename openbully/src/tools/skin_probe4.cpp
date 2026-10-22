// Numeric glitch check: walk a full cycle and assert every palette entry
// stays a proper rigid-ish transform (no NaN, sane translation magnitude,
// orthonormal-ish rotation). Exploding vertices == some entry blowing up.
#include <cstdio>
#include <cmath>
#include <glm/glm.hpp>
#include "../engine/formats/nif_loader.h"
#include "../engine/formats/agr_loader.h"
#include "../animation/animation_controller.h"
#include "../rendering/mesh.h"

using namespace OpenBully;

int main(int argc, char** argv) {
    const std::string animDir = argc > 1 ? argv[1] : "data/extracted/anim";
    const std::string worldDir = argc > 2 ? argv[2] : "data/extracted/world";

    std::vector<AgrAnim> clips;
    {
        const std::string path = animDir + "/F_Jocks.agr";
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { std::printf("no agr\n"); return 1; }
        std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) { std::fclose(f); return 1; }
        std::fclose(f);
        AgrLoadGroup(buf.data(), buf.size(), clips);
    }
    int walkIdx = -1;
    for (size_t i = 0; i < clips.size(); i++) {
        if (!clips[i].decoded) continue;
        float tr[3]; AgrTravel(clips[i], tr);
        if (std::sqrt(tr[0]*tr[0]+tr[1]*tr[1]+tr[2]*tr[2]) > 0.5f) { walkIdx = int(i); break; }
    }
    if (walkIdx < 0) { std::printf("no walk\n"); return 1; }

    NifLoadResult nif = LoadNifFile(worldDir + "/PLAYER.nif");
    if (!nif.ok) { std::printf("nif fail\n"); return 1; }
    uint32_t root = 0xFFFFFFFF;
    for (const NifGeometryData& g : nif.geometries)
        if (g.skinIndex != 0xFFFFFFFF && g.skinIndex < nif.skins.size() &&
            nif.skins[g.skinIndex].skeletonRoot != 0xFFFFFFFF) {
            root = nif.skins[g.skinIndex].skeletonRoot; break;
        }

    AnimationController c;
    if (!c.BuildSkeleton(nif, root)) { std::printf("skel fail\n"); return 1; }
    c.AddClip("walk", clips[size_t(walkIdx)]);
    c.Play("walk", true, 0.0f);

    int fails = 0;
    const int steps = 120; // 2 s at 60 Hz: two full cycles
    for (int s = 0; s <= steps; s++) {
        c.Update(1.0f / 60.0f);
        std::vector<glm::mat4> pal;
        for (size_t shape = 0; shape < c.ShapeCount(); shape++) {
            c.ShapePalette(shape, pal);
            for (size_t i = 0; i < pal.size(); i++) {
                const glm::mat4& m = pal[i];
                for (int cc = 0; cc < 4; cc++)
                    for (int rr = 0; rr < 4; rr++) {
                        const float v = m[cc][rr];
                        if (std::isnan(v) || std::isinf(v) || std::fabs(v) > 100.0f) {
                            std::printf("GLITCH shape=%zu bone=%zu [%d][%d]=%.3f at step %d\n",
                                        shape, i, cc, rr, v, s);
                            fails++;
                            if (fails > 5) { std::printf("FAILURES\n"); return 1; }
                        }
                    }
            }
        }
    }
    std::printf("palette integrity over %d steps: %s\n", steps,
                fails == 0 ? "OK (no glitches)" : "FAIL");
    return fails == 0 ? 0 : 1;
}
