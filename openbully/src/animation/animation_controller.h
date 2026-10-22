#pragma once

// OpenBully: skeletal animation controller (Track 17).
//
// An AnimationController drives one skinned character:
//   - a 36-bone skeleton posed from .agr animation tracks (see agr_loader.h),
//   - per-bone skin matrices handed to the vertex shader (up to 64 bones),
//   - state machine: play an animation by name, loop or one-shot, with a
//     crossfade blend (~0.2 s) when switching.
//
// Track mapping: .agr track N poses the Nth skeleton node under "Dummy" in
// NIF DFS order (verified: Player_Mascot's Dummy subtree matches docs/agr.md
// exactly). Rotation REPLACES the node's local rotation; translation keeps
// the node's file value except for Root (pelvis bob) and ARROW (ground
// travel, which drives the entity instead of the mesh).

#include "../engine/formats/nif_loader.h"
#include "../engine/formats/agr_loader.h"
#include "../rendering/mesh.h" // kMaxSkinBones palette budget

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenBully {

// kMaxSkinBones lives in rendering/mesh.h (shared with the skinned-mesh
// builder); re-imported here via that header for controller-side checks.

// One entry in a named animation library.
struct AnimClip {
    std::string name;    // engine-side name ("walk", "idle")
    AgrAnim     anim;    // decoded tracks
    float       speed = 1.0f; // playback rate multiplier
};

// Runtime pose of one bone: local transform (translation from the NIF file
// or the animation's position track, rotation from the animation).
struct BonePose {
    float q[4] = {1, 0, 0, 0}; // local rotation (w, x, y, z)
    float t[3] = {0, 0, 0};    // local translation
    float s = 1.0f;
};

class AnimationController {
public:
    // Builds the bone list from the NIF's skin skeleton root: DFS order under
    // the root (matching the .agr track order for peds). Returns false when
    // the root block is missing. Must be called before Play/Evaluate.
    bool BuildSkeleton(const struct NifLoadResult& nif, uint32_t skeletonRootBlock);

    // Registers a decoded clip. Returns the clip index.
    int  AddClip(const std::string& name, AgrAnim anim, float speed = 1.0f);
    int  FindClip(const std::string& name) const;

    // Direct clip access for playback-rate tuning (speed matching). Returns
    // nullptr for an out-of-range index.
    const AnimClip* ClipAt(int index) const { return ClipAtImpl(index); }
    AnimClip*       ClipAt(int index)       { return ClipAtImpl(index); }

    // Plays `clip` (by index or name). loop=false plays once and stops on the
    // last frame. Blending: the previous pose crossfades out over `blendSecs`.
    void Play(int clipIndex, bool loop = true, float blendSecs = 0.2f);
    void Play(const std::string& name, bool loop = true, float blendSecs = 0.2f);
    void Stop();

    bool IsPlaying() const { return m_playing; }
    const std::string& CurrentName() const;
    float CurrentTime() const { return m_time; }
    float NormalizedTime() const; // 0..1 within the current clip

    // Advances playback and re-poses the skeleton. `dt` in seconds.
    void Update(float dt);

    // --- Per-shape skin palettes (Track 17) ---------------------------------
    //
    // The verified Gamebryo skin formula is vertex' = sum w * (M_os * S_os)(v)
    // (bully-re/docs/nif.md): M_os = the bone's CURRENT model transform, S_os =
    // the per-shape bind transform stored in NiSkinData. Each skinned shape
    // carries its own NiSkinInstance, so palettes are per shape, not per rig.
    // Register one ShapeBind per skinned submesh (bind matrices per DFS slot,
    // built from that shape's NiSkinData); ShapePalette() then produces the
    // matrix palette to upload before the submesh's draw.
    struct ShapeBind {
        // Per DFS slot: bind matrix S_os (bind pose composed through the
        // hierarchy), or identity when the shape's skin lacks that bone.
        std::vector<glm::mat4> bind;
    };
    void AddShapeBind(ShapeBind bind);
    // Packed variant: `matrices` = kMaxSkinBones * 16 column-major floats
    // (the SkinnedMeshFromNif out-param format). Missing bones must already
    // be identity in the packed data.
    void AddShapeBindPacked(const float* matrices);
    size_t ShapeCount() const { return m_shapeBinds.size(); }
    // Fills `out` (BoneCount matrices) for shape `index`: M_os * S_os.
    void ShapePalette(size_t index, std::vector<glm::mat4>& out) const;

    // Current model-space bone matrices (recomputed by Update()). Exposed for
    // palette recomputation and debugging.
    const std::vector<glm::mat4>& ModelMatrices() const { return m_model; }

    // Evaluates the pose at a specific normalized time (0..1) of `clip` into
    // `outLocal` (size = bone count) WITHOUT advancing state. Used by the
    // blender and by tests.
    void PoseAt(int clipIndex, float normalizedT,
                std::vector<BonePose>& outLocal) const;

    // Skin matrices for the GPU: skinMatrix[i] maps bind-pose bone i to its
    // current model-space pose (skinning = sum w_i * skinMatrix[bone_i] * v,
    // with v in bind-pose model space). Size = bone count (<= kMaxSkinBones),
    // column-major 4x4 (bgfx convention). Updated by Update().
    const std::vector<glm::mat4>& SkinMatrices() const { return m_skin; }
    int BoneCount() const { return static_cast<int>(m_bones.size()); }

    // Ground-travel offset of the current clip this frame (ARROW track):
    // how far the pose "walks" in body space during the last dt. The engine
    // adds this (rotated by entity yaw) to the entity position so feet stick
    // to the ground. Zero for in-place clips.
    glm::vec3 FrameTravel() const { return m_frameTravel; }

private:
    AnimClip*       ClipAtImpl(int index) {
        return (index >= 0 && index < static_cast<int>(m_clips.size()))
            ? &m_clips[index] : nullptr;
    }
    const AnimClip* ClipAtImpl(int index) const {
        return (index >= 0 && index < static_cast<int>(m_clips.size()))
            ? &m_clips[index] : nullptr;
    }

    struct Bone {
        std::string name;
        int32_t     parent = -1;  // index into m_bones
        // Bind local transform from the NIF file.
        float bindT[3] = {0, 0, 0};
        float bindR[9] = {1,0,0,0,1,0,0,0,1}; // row-major
        float bindS = 1.0f;
        // Bind model-space matrix (bind pose, column-major glm).
        glm::mat4 bindModel{1.0f};
    };

    void BuildBindPose();
    void PoseFromClip(const AnimClip& clip, float t,
                      std::vector<BonePose>& outLocal) const;
    void ComputeSkin(const std::vector<BonePose>& local);

    std::vector<Bone>   m_bones;
    std::vector<glm::mat4> m_skin;
    std::vector<glm::mat4> m_model;      // current model-space bone matrices
    std::vector<ShapeBind> m_shapeBinds; // per skinned shape bind matrices
    glm::mat4 m_rootOffset{1.0f};        // file-root -> track-root transform

    std::vector<AnimClip> m_clips;
    std::unordered_map<std::string, int> m_clipByName;

    // Current + previous (fading-out) playback state.
    int   m_current = -1;
    int   m_previous = -1;
    float m_time = 0.0f;
    float m_prevTime = 0.0f;
    bool  m_loop = true;
    bool  m_playing = false;
    float m_blend = 0.0f;      // seconds remaining in the crossfade
    float m_blendTotal = 0.0f; // crossfade length

    // Scratch buffers (bone count sized).
    mutable std::vector<BonePose> m_localA;
    mutable std::vector<BonePose> m_localB;

    glm::vec3 m_frameTravel{0.0f};
    glm::vec3 m_prevTravelPos{0.0f}; // ARROW position at m_time
};

} // namespace OpenBully
