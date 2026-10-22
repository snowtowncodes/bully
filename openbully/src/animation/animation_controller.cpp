#include "animation_controller.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/gtc/type_ptr.hpp>

namespace OpenBully {

// Local transform of NifNode `index` as a Bone (bind pose from the file).
static bool ReadNifNode(const NifLoadResult& nif, uint32_t index,
                        std::string& name, float t[3], float r[9], float* s) {
    if (index >= nif.nodes.size()) return false;
    const NifNode& n = nif.nodes[index];
    name = n.name;
    t[0] = n.translation.x; t[1] = n.translation.y; t[2] = n.translation.z;
    std::memcpy(r, n.rotation, sizeof(float) * 9);
    *s = n.scale;
    return true;
}

bool AnimationController::BuildSkeleton(const NifLoadResult& nif,
                                        uint32_t skeletonRootBlock) {
    m_bones.clear();
    // The skin's skeleton root is a raw NiBlock reference; map it to the
    // nodes[] position (non-node blocks interleave with nodes in the file).
    uint32_t rootPos = skeletonRootBlock;
    if (skeletonRootBlock < nif.blockToNode.size() &&
        nif.blockToNode[skeletonRootBlock] >= 0)
        rootPos = static_cast<uint32_t>(nif.blockToNode[skeletonRootBlock]);
    if (rootPos >= nif.nodes.size()) return false;

    // .agr tracks count bones from "Dummy" downward (docs/agr.md: "the 36
    // bones ... in file order under Dummy"). Exporters wrap the skeleton in
    // Scene Root / model nodes; re-root on the first "Dummy" descendant so
    // the DFS reproduces the track order exactly.
    //
    // Model-space composition must still pass through the FULL ancestor
    // chain (skinned vertices live in the file root's space, and the skin
    // root may be Scene Root, not Dummy), so the world matrix of the root
    // bone gets the constant ancestor offset m_rootOffset applied.
    m_rootOffset = glm::mat4(1.0f);
    {
        std::vector<uint32_t> q{rootPos};
        bool found = false;
        for (size_t qi = 0; qi < q.size() && !found; qi++) {
            if (q[qi] >= nif.nodes.size()) continue;
            if (nif.nodes[q[qi]].name == "Dummy") {
                // Compose the world matrix of everything ABOVE the new root
                // (file-root side), which the DFS composition below drops.
                glm::mat4 anc(1.0f);
                std::vector<int32_t> chain;
                int32_t p = nif.nodes[q[qi]].parent;
                int guard = 0;
                while (p >= 0 && size_t(p) < nif.nodes.size() && guard++ < 64) {
                    chain.push_back(p);
                    p = nif.nodes[size_t(p)].parent;
                }
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    const NifNode& nn = nif.nodes[size_t(*it)];
                    glm::mat4 lm(1.0f);
                    for (int c = 0; c < 3; c++)
                        for (int row = 0; row < 3; row++)
                            lm[c][row] = nn.rotation[row * 3 + c] * nn.scale;
                    lm[3][0] = nn.translation.x;
                    lm[3][1] = nn.translation.y;
                    lm[3][2] = nn.translation.z;
                    anc = anc * lm;
                }
                m_rootOffset = anc;
                rootPos = q[qi];
                found = true;
                break;
            }
            for (uint32_t c : nif.nodes[q[qi]].children) q.push_back(c);
        }
    }

    // DFS under the skeleton root, BONE NODES ONLY (shapes are leaves and
    // exporters attach meshes inside the bone tree; including them would
    // shift the track order). This reproduces the .agr track order exactly
    // (verified: Player_Mascot's Dummy subtree = docs/agr.md's 36 bones).
    // Cap depth defensively (NIFs are trees; 64 is well past any rig).
    std::vector<std::pair<uint32_t, int32_t>> work; // (nodes[] pos, parent bone)
    work.push_back({rootPos, -1});
    std::vector<Bone> bones;
    while (!work.empty()) {
        auto [node, parentBone] = work.back();
        work.pop_back();
        if (node >= nif.nodes.size()) continue;
        const NifNode& n = nif.nodes[node];
        const int32_t myIndex = static_cast<int32_t>(bones.size());
        if (!n.isShape) {
            if (myIndex >= kMaxSkinBones) break; // palette budget
            Bone& b = bones.emplace_back();
            b.parent = parentBone;
            if (!ReadNifNode(nif, node, b.name, b.bindT, b.bindR, &b.bindS))
                b.name.clear();
            parentBone = myIndex;
        }
        // Push children reversed so the DFS visits them in file order.
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it)
            work.push_back({*it, parentBone});
    }

    m_bones = std::move(bones);
    m_localA.resize(m_bones.size());
    m_localB.resize(m_bones.size());
    m_skin.resize(m_bones.size());
    m_model.resize(m_bones.size());
    m_shapeBinds.clear();
    BuildBindPose();
    return !m_bones.empty();
}

// Compose local T*R*S (column-major, GLM). Convention note (verified by
// probe on Player_Mascot + docs/agr.md): bully-re composes stored row-major
// matrices as v' = v * R (R applied on the right), i.e. the GLM matrix is
// the TRANSPOSE of the stored row-major read: glm[c][r] = stored[r*3+c].
// AgrMatrix's standard quaternion matrix matches the same storage, so both
// the bind and the animation paths use this one helper.
static glm::mat4 LocalMatrix(const float t[3], const float r[9], float s) {
    glm::mat4 m(1.0f);
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++)
            m[c][row] = r[row * 3 + c] * s;
    m[3][0] = t[0]; m[3][1] = t[1]; m[3][2] = t[2];
    return m;
}

// Column-major multiply: o = a * b.
static void MatMul(const glm::mat4& a, const glm::mat4& b, glm::mat4& o) {
    o = a * b;
}

void AnimationController::BuildBindPose() {
    // Model-space bind matrix per bone (child-to-root composition, starting
    // at the track root's ancestor offset: skinned vertices live in the
    // file root's space; see BuildSkeleton's Dummy re-rooting).
    std::vector<glm::mat4> model(m_bones.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < m_bones.size(); i++) {
        const Bone& b = m_bones[i];
        const glm::mat4 local = LocalMatrix(b.bindT, b.bindR, b.bindS);
        model[i] = (b.parent >= 0)
            ? model[static_cast<size_t>(b.parent)] * local
            : m_rootOffset * local;
        m_bones[i].bindModel = model[i];
    }
    // Default skin matrices = identity (bind pose renders unchanged).
    for (size_t i = 0; i < m_bones.size(); i++) {
        m_skin[i] = glm::mat4(1.0f);
        m_model[i] = model[i]; // valid palette source before any clip plays
    }
}

// --- Clip management -----------------------------------------------------------

int AnimationController::AddClip(const std::string& name, AgrAnim anim,
                                 float speed) {
    AnimClip c;
    c.name = name;
    c.anim = std::move(anim);
    c.speed = speed;
    const int idx = static_cast<int>(m_clips.size());
    m_clips.push_back(std::move(c));
    m_clipByName[name] = idx;
    return idx;
}

int AnimationController::FindClip(const std::string& name) const {
    auto it = m_clipByName.find(name);
    return it == m_clipByName.end() ? -1 : it->second;
}

void AnimationController::Play(int clipIndex, bool loop, float blendSecs) {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_clips.size())) return;
    if (clipIndex == m_current && m_playing) return; // already playing it
    m_previous = m_playing ? m_current : -1;
    m_prevTime = m_time;
    m_current = clipIndex;
    m_time = 0.0f;
    m_loop = loop;
    m_playing = true;
    m_blendTotal = blendSecs;
    m_blend = blendSecs;
    // Seed the ARROW tracker at the new clip's start.
    float arrow[3] = {0, 0, 0};
    AgrPosition(m_clips[clipIndex].anim, kAgrBones - 1, 0.0f, arrow);
    m_prevTravelPos = glm::vec3(arrow[0], arrow[1], arrow[2]);
}

void AnimationController::Play(const std::string& name, bool loop, float blendSecs) {
    Play(FindClip(name), loop, blendSecs);
}

void AnimationController::Stop() {
    m_playing = false;
    m_previous = -1;
    m_blend = 0.0f;
}

const std::string& AnimationController::CurrentName() const {
    static const std::string kEmpty;
    if (m_current < 0 || m_current >= static_cast<int>(m_clips.size()))
        return kEmpty;
    return m_clips[m_current].name;
}

float AnimationController::NormalizedTime() const {
    if (m_current < 0 || m_current >= static_cast<int>(m_clips.size())) return 0.0f;
    const float d = m_clips[m_current].anim.duration;
    return d > 0.0f ? m_time / d : 0.0f;
}

// --- Posing --------------------------------------------------------------------

void AnimationController::PoseFromClip(const AnimClip& clip, float t,
                                       std::vector<BonePose>& outLocal) const {
    const size_t n = m_bones.size();
    outLocal.resize(n);
    for (size_t i = 0; i < n; i++) {
        BonePose& p = outLocal[i];
        const Bone& b = m_bones[i];
        // Rotation: the animation REPLACES the node's rotation when the bone
        // has a track; otherwise keep the file pose.
        float q[4];
        if (AgrRotation(clip.anim, static_cast<int>(i), t, q)) {
            std::memcpy(p.q, q, sizeof q);
        } else {
            // Quaternion from the bind rotation matrix (Shepperd's method).
            const float* m = b.bindR;
            const float trace = m[0] + m[4] + m[8];
            if (trace > 0.0f) {
                const float s = std::sqrt(trace + 1.0f) * 2.0f;
                p.q[0] = 0.25f * s;
                p.q[1] = (m[7] - m[5]) / s;
                p.q[2] = (m[2] - m[6]) / s;
                p.q[3] = (m[3] - m[1]) / s;
            } else if (m[0] > m[4] && m[0] > m[8]) {
                const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
                p.q[0] = (m[7] - m[5]) / s;
                p.q[1] = 0.25f * s;
                p.q[2] = (m[1] + m[3]) / s;
                p.q[3] = (m[2] + m[6]) / s;
            } else if (m[4] > m[8]) {
                const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
                p.q[0] = (m[2] - m[6]) / s;
                p.q[1] = (m[1] + m[3]) / s;
                p.q[2] = 0.25f * s;
                p.q[3] = (m[5] + m[7]) / s;
            } else {
                const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
                p.q[0] = (m[3] - m[1]) / s;
                p.q[1] = (m[2] + m[6]) / s;
                p.q[2] = (m[5] + m[7]) / s;
                p.q[3] = 0.25f * s;
            }
        }
        // Translation: keep the file value; only Root (track 1) uses the
        // animation's position track (pelvis bob). ARROW (track 35) drives
        // the entity, not the mesh.
        std::memcpy(p.t, b.bindT, sizeof p.t);
        if (i == 1) {
            float pos[3];
            if (AgrPosition(clip.anim, 1, t, pos))
                std::memcpy(p.t, pos, sizeof p.t);
        }
        p.s = b.bindS;
    }
}

void AnimationController::PoseAt(int clipIndex, float normalizedT,
                                 std::vector<BonePose>& outLocal) const {
    if (clipIndex < 0 || clipIndex >= static_cast<int>(m_clips.size())) return;
    const float d = m_clips[clipIndex].anim.duration;
    PoseFromClip(m_clips[clipIndex], normalizedT * d, outLocal);
}

void AnimationController::ComputeSkin(const std::vector<BonePose>& local) {
    const size_t n = m_bones.size();
    // Quat -> row-major 3x3 (AgrMatrix convention).
    auto quatMat = [](const float q[4], float m[9]) { AgrMatrix(q, m); };
    m_model.resize(n);
    m_skin.resize(n);
    for (size_t i = 0; i < n; i++) {
        float r[9];
        quatMat(local[i].q, r);
        const glm::mat4 lm = LocalMatrix(local[i].t, r, local[i].s);
        m_model[i] = (m_bones[i].parent >= 0)
            ? m_model[static_cast<size_t>(m_bones[i].parent)] * lm
            : m_rootOffset * lm;
        // Skin matrix: current model pose * inverse(bind model pose).
        m_skin[i] = m_model[i] * glm::inverse(m_bones[i].bindModel);
    }
}

// --- Per-shape skin palettes (Track 17) ---------------------------------------

void AnimationController::AddShapeBind(ShapeBind bind) {
    bind.bind.resize(m_bones.size(), glm::mat4(1.0f)); // missing bones: identity
    m_shapeBinds.push_back(std::move(bind));
}

void AnimationController::AddShapeBindPacked(const float* matrices) {
    ShapeBind sb;
    sb.bind.resize(m_bones.size(), glm::mat4(1.0f));
    if (matrices) {
        for (size_t i = 0; i < m_bones.size() && i < size_t(kMaxSkinBones); i++) {
            glm::mat4& m = sb.bind[i];
            std::memcpy(glm::value_ptr(m), matrices + i * 16, 16 * sizeof(float));
        }
    }
    m_shapeBinds.push_back(std::move(sb));
}

void AnimationController::ShapePalette(size_t index,
                                       std::vector<glm::mat4>& out) const {
    out.resize(m_bones.size());
    if (index >= m_shapeBinds.size()) {
        for (size_t i = 0; i < m_bones.size(); i++) out[i] = m_skin[i];
        return;
    }
    const ShapeBind& sb = m_shapeBinds[index];
    for (size_t i = 0; i < m_bones.size(); i++)
        out[i] = m_model[i] * sb.bind[i]; // M_os * S_os (verified formula)
}

void AnimationController::Update(float dt) {
    m_frameTravel = glm::vec3(0.0f);
    if (m_current < 0 || m_current >= static_cast<int>(m_clips.size())) return;
    AnimClip& clip = m_clips[m_current];
    const float d = clip.anim.duration;
    if (!m_playing || d <= 0.0f) return;

    m_time += dt * clip.speed;
    if (m_time >= d) {
        if (m_loop) m_time = std::fmod(m_time, d);
        else { m_time = d; m_playing = false; }
    }
    if (m_blend > 0.0f) m_blend = std::max(0.0f, m_blend - dt);

    // Ground travel (ARROW track): body-space offset since the last frame.
    float arrow[3] = {0, 0, 0};
    AgrPosition(clip.anim, kAgrBones - 1, m_time, arrow);
    const glm::vec3 now(arrow[0], arrow[1], arrow[2]);
    m_frameTravel = now - m_prevTravelPos;
    // On loop wrap, travel jumps forward instead of backward.
    if (m_loop && m_time < dt * clip.speed && glm::length(m_frameTravel) > d)
        m_frameTravel = glm::vec3(0.0f);
    m_prevTravelPos = now;

    // Evaluate the pose: current clip, blended with the fading previous one.
    const bool blending = m_blend > 0.0f && m_previous >= 0 &&
                          m_previous < static_cast<int>(m_clips.size());
    if (!blending) {
        PoseFromClip(clip, m_time, m_localA);
        ComputeSkin(m_localA);
        return;
    }

    const AnimClip& prev = m_clips[m_previous];
    const float prevDur = prev.anim.duration;
    float pt = m_prevTime + dt * prev.speed;
    if (prevDur > 0.0f) pt = std::fmod(pt, prevDur);
    PoseFromClip(clip, m_time, m_localA);
    PoseFromClip(prev, pt, m_localB);

    const float u = 1.0f - m_blend / std::max(m_blendTotal, 1e-4f);
    std::vector<BonePose> blended(m_bones.size());
    for (size_t i = 0; i < m_bones.size(); i++) {
        float qa[4], qb[4], qo[4];
        std::memcpy(qa, m_localA[i].q, sizeof qa);
        std::memcpy(qb, m_localB[i].q, sizeof qb);
        AgrSlerp(qb, qa, u, qo); // prev -> current
        std::memcpy(blended[i].q, qo, sizeof qo);
        for (int k = 0; k < 3; k++)
            blended[i].t[k] = m_localB[i].t[k] +
                              u * (m_localA[i].t[k] - m_localB[i].t[k]);
        blended[i].s = m_localA[i].s;
    }
    ComputeSkin(blended);
}

} // namespace OpenBully
