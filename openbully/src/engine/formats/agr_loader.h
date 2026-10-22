#pragma once

// OpenBully: .agr animation group loader (Track 17).
//
// Bully stores skeletal animation in ".agr" groups (Stream/World.img and the
// loose Anim/*.agr files). Format reverse-engineered and byte-verified across
// all 550 retail groups (bully-re/docs/agr.md); this is a C++17 port of the
// reference decoder (bully-re/src/anim/Agr.cpp).
//
// A file is a concatenation of animations. Each:
//   u32 0x100 | u32 type (999..1004, key encoding) | u32 key count |
//   u32 0 | f32 duration (s) | keys | position records (8 B each)
// Key size by type: 999->32, 1000->20, 1001->12, 1002->8, 1003->20, 1004->12.
//
// Type 1002 (2 772 of 3 446 animations, ALL ped idles/walks) packs each key
// into 8 bytes: w0 bits 0-10 previous key index of the same bone, bits 11-19
// time in 1/511ths, bits 20-31 packed quaternion x/y; w1 carries y/z/w.
// The first "numBones" keys are the starting pose of each bone, in skeleton
// DFS order under "Dummy" (peds: 36 bones, Dummy..ARROW - see docs/agr.md).
//
// Rotation replaces the node's rotation (translation comes from the node);
// separate position tracks exist for Root (pelvis bob) and ARROW (ground
// travel). Body space faces +y; walk cycles advance toward +y.

#include <cstdint>
#include <vector>

namespace OpenBully {

constexpr int kAgrBones = 36;

// One quaternion key of one bone. q = (w, x, y, z), normalized.
struct AgrKey {
    float t = 0.0f;            // seconds from animation start
    float q[4] = {0, 0, 0, 1}; // w, x, y, z
};

// One position key of one bone (parent-relative, meters).
struct AgrPosKey {
    float t = 0.0f;
    float p[3] = {0, 0, 0};
};

// One animation: per-bone rotation tracks (+ optional position tracks).
struct AgrAnim {
    int32_t type = 0;
    int32_t numBones = 0;
    float   duration = 0.0f;
    bool    decoded = false;
    std::vector<AgrKey>    rot[kAgrBones];
    std::vector<AgrPosKey> pos[kAgrBones];
};

// Decodes every animation in a .agr buffer. Returns false when nothing
// decoded (bad header, no valid animation found).
bool AgrLoadGroup(const uint8_t* data, size_t size, std::vector<AgrAnim>& out);

// Rotation of `bone` at time `t` (wrapped into the duration), slerped between
// the two surrounding keys. False when the bone has no track.
bool AgrRotation(const AgrAnim& a, int bone, float t, float q[4]);

// Position of `bone` at time `t`, linearly interpolated. False when the bone
// has no position track (in ped animations: only Root and ARROW do).
bool AgrPosition(const AgrAnim& a, int bone, float t, float p[3]);

// Ground travel (ARROW, bone kAgrBones-1) over the whole animation: what the
// character advances per cycle. Zero-length for in-place animations.
void AgrTravel(const AgrAnim& a, float out[3]);

// Slerp between quaternions (w, x, y, z), shortest arc; u=0 -> a, u=1 -> b.
void AgrSlerp(const float a[4], const float b0[4], float u, float out[4]);

// Quaternion (w, x, y, z) -> row-major 3x3, NiAVObject convention (the matrix
// read row by line reproduces the quaternion).
void AgrMatrix(const float q[4], float m[9]);

} // namespace OpenBully
