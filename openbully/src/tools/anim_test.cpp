// Track 17 verification harness: .agr decode + NIF skin extraction + skeleton
// + pose evaluation, no GPU required. Run from openbully/ with the extracted
// data present:
//   build\Release\anim_test.exe data/extracted/anim data/extracted/world
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../engine/formats/agr_loader.h"
#include "../engine/formats/nif_loader.h"
#include "../animation/animation_controller.h"
#include "../rendering/mesh.h"

using namespace OpenBully;

int main(int argc, char** argv) {
    const std::string animDir = argc > 1 ? argv[1] : "data/extracted/anim";
    const std::string worldDir = argc > 2 ? argv[2] : "data/extracted/world";
    int failures = 0;

    // --- 1. .agr decode -------------------------------------------------------
    std::vector<AgrAnim> clips;
    {
        const std::string path = animDir + "/F_Jocks.agr";
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { std::printf("FAIL: cannot open %s\n", path.c_str()); return 1; }
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) { std::fclose(f); return 1; }
        std::fclose(f);
        if (!AgrLoadGroup(buf.data(), buf.size(), clips)) {
            std::printf("FAIL: AgrLoadGroup decoded nothing\n");
            return 1;
        }
        int decoded = 0, walks = 0;
        for (const AgrAnim& a : clips) {
            if (!a.decoded) continue;
            decoded++;
            float travel[3];
            AgrTravel(a, travel);
            const float len = std::sqrt(travel[0]*travel[0] + travel[1]*travel[1] +
                                        travel[2]*travel[2]);
            if (len > 0.5f) walks++;
        }
        std::printf("AGR: %zu anims, %d decoded, %d walks\n", clips.size(), decoded, walks);
        if (decoded == 0) { std::printf("FAIL: no decoded animations\n"); failures++; }
        if (walks == 0)   { std::printf("FAIL: no walk cycles found\n"); failures++; }
    }

    // --- 2. NIF skin + skeleton ------------------------------------------------
    NifLoadResult nif = LoadNifFile(worldDir + "/Player_Mascot.nif");
    if (!nif.ok) { std::printf("FAIL: Player_Mascot.nif: %s\n", nif.error.c_str()); return 1; }
    int skinned = 0;
    uint32_t skeletonRoot = 0xFFFFFFFF;
    for (const NifGeometryData& g : nif.geometries)
        if (g.skinIndex != 0xFFFFFFFF && g.skinIndex < nif.skins.size()) {
            skinned++;
            if (skeletonRoot == 0xFFFFFFFF)
                skeletonRoot = nif.skins[g.skinIndex].skeletonRoot;
        }
    std::printf("NIF: %zu geometries, %d skinned, %zu skin blocks, root=%u\n",
                nif.geometries.size(), skinned, nif.skins.size(), skeletonRoot);
    if (skinned == 0 || skeletonRoot == 0xFFFFFFFF) {
        std::printf("FAIL: no skinned geometry / skeleton root\n");
        return 1;
    }

    AnimationController ctrl;
    if (!ctrl.BuildSkeleton(nif, skeletonRoot)) {
        std::printf("FAIL: BuildSkeleton\n");
        return 1;
    }
    std::printf("Skeleton: %d bones\n", ctrl.BoneCount());
    if (ctrl.BoneCount() < 30 || ctrl.BoneCount() > kMaxSkinBones) {
        std::printf("FAIL: unexpected bone count\n");
        failures++;
    }

    // --- 3. Skinned mesh --------------------------------------------------------
    SkinnedMesh skin;
    if (!SkinnedMeshFromNif(nif, skin)) {
        std::printf("FAIL: SkinnedMeshFromNif\n");
        failures++;
    } else {
        // Count weighted vs rigid vertices.
        size_t skinnedVerts = 0;
        for (const SkinnedMeshVertex& v : skin.vertices)
            if (v.weight[1] > 0.0f) skinnedVerts++;
        std::printf("SkinnedMesh: %zu verts (%zu influenced), %zu indices, %zu submeshes\n",
                    skin.vertices.size(), skinnedVerts, skin.indices.size(),
                    skin.subMeshes.size());
        if (skinnedVerts == 0) { std::printf("FAIL: no vertex weights\n"); failures++; }
    }

    // --- 4. Pose evaluation: find walk + idle, evaluate at t=0 and t=end --------
    int walkIdx = -1, idleIdx = -1;
    for (size_t i = 0; i < clips.size(); i++) {
        if (!clips[i].decoded) continue;
        float travel[3];
        AgrTravel(clips[i], travel);
        const float len = std::sqrt(travel[0]*travel[0] + travel[1]*travel[1] +
                                    travel[2]*travel[2]);
        if (len > 0.5f && walkIdx < 0) walkIdx = int(i);
        if (len <= 0.5f && clips[i].duration > 1.0f && idleIdx < 0) idleIdx = int(i);
    }
    std::printf("clips: walk=%d idle=%d\n", walkIdx, idleIdx);
    if (walkIdx < 0 || idleIdx < 0) { std::printf("FAIL: clip pick\n"); failures++; }
    else {
        ctrl.AddClip("walk", clips[size_t(walkIdx)]);
        ctrl.AddClip("idle", clips[size_t(idleIdx)]);
        ctrl.Play("walk", true, 0.0f);
        for (int step = 0; step < 30; step++) ctrl.Update(1.0f / 60.0f);
        const std::vector<glm::mat4>& m = ctrl.SkinMatrices();
        float maxOff = 0.0f;
        for (const glm::mat4& mm : m)
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    maxOff = std::max(maxOff, std::fabs(mm[c][r] - (c == r ? 1.0f : 0.0f)));
        std::printf("Pose after 0.5s of walk: max |M - I| = %.4f\n", maxOff);
        if (maxOff < 0.01f) {
            std::printf("FAIL: pose did not move (walk animation not applied)\n");
            failures++;
        }
        ctrl.Play("idle", true, 0.2f); // crossfade exercise
        for (int step = 0; step < 30; step++) ctrl.Update(1.0f / 60.0f);
        std::printf("Crossfade to idle OK (t=%.3f, %s)\n", ctrl.CurrentTime(),
                    ctrl.CurrentName().c_str());
    }

    // --- 4b. Per-shape palette validation -----------------------------------
    {
        std::vector<std::vector<float>> binds;
        SkinnedMesh skin2;
        SkinnedMeshFromNif(nif, skin2, &binds);
        AnimationController c2;
        c2.BuildSkeleton(nif, skeletonRoot);
        for (const auto& b : binds) c2.AddShapeBindPacked(b.data());
        // No Play(): pose = file transforms, palette must be ~identity.
        c2.Update(0.0f);
        std::vector<glm::mat4> pal;
        c2.ShapePalette(0, pal);
        float maxDev = 0.0f;
        for (size_t i = 0; i < pal.size(); i++)
            for (int cc = 0; cc < 4; cc++)
                for (int rr = 0; rr < 4; rr++)
                    maxDev = std::max(maxDev,
                        std::fabs(pal[i][cc][rr] - (cc == rr ? 1.0f : 0.0f)));
        std::printf("bind palette deviation from identity: %.4f\n", maxDev);

        // Reproject a weighted vertex through the palette; the result must
        // land back on the stored bind position (rest pose == bind pose).
        const SkinnedMeshVertex* probe = nullptr;
        size_t probeShape = 0;
        for (size_t si = 0; si < skin2.subMeshes.size() && !probe; si++) {
            if (skin2.subMeshes[si].shapeIndex < 0) continue;
            for (uint32_t idx : skin2.indices) {
                const SkinnedMeshVertex& v = skin2.vertices[idx];
                if (v.weight[0] > 0.5f && v.weight[1] == 0.0f) { probe = &v; probeShape = size_t(skin2.subMeshes[si].shapeIndex); break; }
            }
        }
        if (probe) {
            c2.ShapePalette(probeShape, pal);
            const glm::vec4 p(probe->x, probe->y, probe->z, 1.0f);
            glm::vec4 out(0.0f);
            for (int k = 0; k < 4; k++)
                if (probe->weight[k] > 0.0f)
                    out += probe->weight[k] * pal[probe->bone[k]] * p;
            const float err = glm::length(out - p);
            std::printf("bind reproject: vertex (%.3f, %.3f, %.3f) -> (%.3f, %.3f, %.3f) err=%.4f\n",
                        probe->x, probe->y, probe->z, out.x, out.y, out.z, err);
        }
    }

    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
