#include "agr_loader.h"

#include <cmath>
#include <cstring>
#include <array>

// Port of the byte-verified reference decoder (bully-re/src/anim/Agr.cpp);
// see docs there for the bully.exe decoder addresses backing each branch.

namespace OpenBully {
namespace {

constexpr uint32_t kKeySize[6] = {32, 20, 12, 8, 20, 12}; // types 999..1004

uint32_t Rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t Rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
int16_t  RdI16(const uint8_t* p) { int16_t v; std::memcpy(&v, p, 2); return v; }
float    RdF(const uint8_t* p)  { float v; std::memcpy(&v, p, 4); return v; }

// 1002/1004 packed quaternion (w, x, y, z), 10-bit magnitude each, 1/1023.
void DecodePackedQuat(uint32_t w0, uint32_t w1, float q[4]) {
    const float e = 1.0f / 1023.0f;
    q[1] = ((w0 >> 21) & 0x3ff) * e * ((w0 >> 20 & 1) ? -1.0f : 1.0f);
    q[2] = (w1 & 0x3ff)       * e * ((w0 >> 31 & 1) ? -1.0f : 1.0f);
    q[3] = ((w1 >> 11) & 0x3ff) * e * ((w1 >> 10 & 1) ? -1.0f : 1.0f);
    q[0] = ((w1 >> 22))       * e * ((w1 >> 21 & 1) ? -1.0f : 1.0f);
}

// One key of any type: previous-key index, time fraction, quaternion, and
// translation for the types that carry it (999, 1003, 1004).
void DecodeKey(int32_t type, const uint8_t* k, uint32_t* prev, float* t,
               float q[4], float tr[3]) {
    if (type == 1002 || type == 1004) {
        const uint32_t w0 = Rd32(k), w1 = Rd32(k + 4);
        *prev = w0 & 0x7ff;
        *t = ((w0 >> 11) & 0x1ff) / 511.0f;
        DecodePackedQuat(w0, w1, q);
        if (tr) {
            const uint32_t v = Rd32(k + 8); // centimeters, packed
            tr[0] = (v & 0x3ff)        * 0.01f * ((v >> 10 & 1) ? -1.0f : 1.0f);
            tr[1] = ((v >> 11) & 0x3ff) * 0.01f * ((v >> 21 & 1) ? -1.0f : 1.0f);
            tr[2] = ((v >> 22) & 0x1ff) * 0.01f * ((v >> 31 & 1) ? -1.0f : 1.0f);
        }
        return;
    }
    *prev = Rd16(k);
    *t = Rd16(k + 2) / 65535.0f;
    if (type == 1001 || type == 1003) {
        const float e = 1.0f / 32767.0f;
        q[1] = RdI16(k + 4) * e; q[2] = RdI16(k + 6) * e;
        q[3] = RdI16(k + 8) * e; q[0] = RdI16(k + 10) * e; // x y z w order
        if (tr) { // millimeters
            tr[0] = RdI16(k + 12) * 0.001f;
            tr[1] = RdI16(k + 14) * 0.001f;
            tr[2] = RdI16(k + 16) * 0.001f;
        }
    } else { // 1000 and 999: floats, w x y z order; 999 translation in meters
        q[0] = RdF(k + 4); q[1] = RdF(k + 8); q[2] = RdF(k + 12); q[3] = RdF(k + 16);
        if (tr) { tr[0] = RdF(k + 20); tr[1] = RdF(k + 24); tr[2] = RdF(k + 28); }
    }
}

// Header sanity: magic 0x100, type 999..1004, plausible count/duration.
bool LooksLikeHeader(const uint8_t* p, const uint8_t* end) {
    if (p + 20 > end) return false;
    const uint32_t type = Rd32(p + 4), n = Rd32(p + 8);
    const float d = RdF(p + 16);
    return Rd32(p) == 0x100 && type >= 999 && type <= 1004 &&
           Rd32(p + 12) == 0 && n > 0 && n < 20000 && d > 0 && d < 1000;
}

} // namespace

bool AgrLoadGroup(const uint8_t* data, size_t size, std::vector<AgrAnim>& out) {
    const uint8_t* p = data;
    const uint8_t* realEnd = data + size;
    // Sectors are zero-padded. Strip the padding to find the end of position
    // records, but keep it for keys: a 1003 key ends with two zero padding
    // bytes that stripping would truncate into.
    const uint8_t* end = realEnd;
    while (end > data && end[-1] == 0) end--;

    while (p + 20 <= end && LooksLikeHeader(p, end)) {
        AgrAnim a;
        a.type = static_cast<int32_t>(Rd32(p + 4));
        const int32_t numKeys = static_cast<int32_t>(Rd32(p + 8));
        a.duration = RdF(p + 16);
        const uint8_t* keys = p + 20;
        const uint8_t* afterKeys = keys + static_cast<size_t>(numKeys) * kKeySize[a.type - 999];
        if (numKeys <= 0 || afterKeys > realEnd) return !out.empty();

        // End of the position section: the next header, scanned 4 bytes at a
        // time (alignment drifts because key blocks are not always aligned).
        const uint8_t* next = afterKeys;
        while (next + 20 <= end && !LooksLikeHeader(next, end)) next += 4;
        if (next + 20 > end) next = (end > afterKeys) ? end : afterKeys;

        const size_t kc = static_cast<size_t>(kKeySize[a.type - 999]);
        std::vector<int32_t> bone(numKeys, -1);
        std::vector<float> t(numKeys);
        std::vector<std::array<float, 4>> q(numKeys);
        std::vector<uint32_t> prev(numKeys);
        std::vector<float> tr(numKeys * 3);
        const bool hasTrans = (a.type == 999 || a.type == 1003 || a.type == 1004);

        bool ok = true;
        for (int32_t i = 0; i < numKeys; i++) {
            DecodeKey(a.type, keys + kc * i, &prev[i], &t[i], q[i].data(),
                      hasTrans ? &tr[i * 3] : nullptr);
            t[i] *= a.duration;
        }

        // Track heads open the animation: prev==0 AND time==0. ("prev 0" also
        // means "key 0": the second key of bone 0 carries the same field but a
        // nonzero time.) 36 bones for a ped, 1..33 for objects.
        int32_t heads = 0;
        while (heads < numKeys && heads < kAgrBones &&
               prev[heads] == 0 && t[heads] == 0.0f) heads++;
        a.numBones = heads;
        ok = heads > 0;

        // Every other key references the previous key of the same bone in
        // time order, so one pass propagates the bone index.
        for (int32_t i = 0; i < heads; i++) bone[i] = i;
        for (int32_t i = heads; i < numKeys && ok; i++) {
            if (static_cast<int32_t>(prev[i]) >= i || bone[prev[i]] < 0) { ok = false; break; }
            bone[i] = bone[prev[i]];
        }

        if (ok) {
            for (int32_t i = 0; i < numKeys; i++) {
                // A zero-norm key (~500 across 63 type-1002 animations, on
                // arm/hand bones) means "no data for this bone": skip it.
                const float n2 = q[i][0]*q[i][0] + q[i][1]*q[i][1] +
                                 q[i][2]*q[i][2] + q[i][3]*q[i][3];
                if (n2 < 0.25f) continue;
                AgrKey c;
                c.t = t[i];
                std::memcpy(c.q, q[i].data(), sizeof c.q);
                if (bone[i] < kAgrBones) a.rot[bone[i]].push_back(c);
                if (hasTrans && bone[i] < kAgrBones) {
                    AgrPosKey ps;
                    ps.t = t[i];
                    std::memcpy(ps.p, &tr[i * 3], sizeof ps.p);
                    a.pos[bone[i]].push_back(ps);
                }
            }
            // Time ordering sanity per track.
            for (int b = 0; b < kAgrBones && ok; b++)
                for (size_t j = 1; j < a.rot[b].size(); j++)
                    if (a.rot[b][j].t < a.rot[b][j - 1].t) { ok = false; break; }
            a.decoded = ok;

            // Separate position records (types without in-key translation):
            // u16 key index, i16 x/y/z in millimeters. The referenced key
            // gives both the instant and the bone.
            if (!hasTrans) {
                for (const uint8_t* r = afterKeys; r + 8 <= next; r += 8) {
                    const uint16_t k = Rd16(r);
                    if (k >= static_cast<uint16_t>(numKeys)) break;
                    if (bone[k] < 0 || bone[k] >= kAgrBones) continue;
                    AgrPosKey ps;
                    ps.t = t[k];
                    ps.p[0] = RdI16(r + 2) * 0.001f;
                    ps.p[1] = RdI16(r + 4) * 0.001f;
                    ps.p[2] = RdI16(r + 6) * 0.001f;
                    a.pos[bone[k]].push_back(ps);
                }
            }
        }

        out.push_back(std::move(a));
        p = next;
    }
    return !out.empty();
}

void AgrSlerp(const float a[4], const float b0[4], float u, float out[4]) {
    float b[4] = { b0[0], b0[1], b0[2], b0[3] };
    float c = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    if (c < 0) { for (float& x : b) x = -x; c = -c; }
    float ka, kb;
    if (c > 0.9995f) { ka = 1 - u; kb = u; }
    else {
        const float th = std::acos(c), s = std::sin(th);
        ka = std::sin((1 - u) * th) / s;
        kb = std::sin(u * th) / s;
    }
    float n = 0;
    for (int i = 0; i < 4; i++) { out[i] = ka * a[i] + kb * b[i]; n += out[i] * out[i]; }
    n = std::sqrt(n);
    if (n > 1e-6f) for (int i = 0; i < 4; i++) out[i] /= n;
}

bool AgrRotation(const AgrAnim& a, int bone, float t, float q[4]) {
    if (!a.decoded || bone < 0 || bone >= kAgrBones || a.rot[bone].empty())
        return false;
    const std::vector<AgrKey>& p = a.rot[bone];
    if (a.duration > 0) { t = std::fmod(t, a.duration); if (t < 0) t += a.duration; }
    if (t <= p.front().t) { std::memcpy(q, p.front().q, 16); return true; }
    if (t >= p.back().t)  { std::memcpy(q, p.back().q, 16);  return true; }
    size_t j = 1; while (j < p.size() && p[j].t < t) j++;
    const float dt = p[j].t - p[j - 1].t;
    AgrSlerp(p[j - 1].q, p[j].q, dt > 1e-6f ? (t - p[j - 1].t) / dt : 0.0f, q);
    return true;
}

bool AgrPosition(const AgrAnim& a, int bone, float t, float p[3]) {
    if (!a.decoded || bone < 0 || bone >= kAgrBones || a.pos[bone].empty())
        return false;
    const std::vector<AgrPosKey>& v = a.pos[bone];
    if (a.duration > 0) { t = std::fmod(t, a.duration); if (t < 0) t += a.duration; }
    if (t <= v.front().t) { std::memcpy(p, v.front().p, 12); return true; }
    if (t >= v.back().t)  { std::memcpy(p, v.back().p, 12);  return true; }
    size_t j = 1; while (j < v.size() && v[j].t < t) j++;
    const float dt = v[j].t - v[j - 1].t;
    const float u = dt > 1e-6f ? (t - v[j - 1].t) / dt : 0.0f;
    for (int k = 0; k < 3; k++) p[k] = v[j - 1].p[k] + u * (v[j].p[k] - v[j - 1].p[k]);
    return true;
}

void AgrTravel(const AgrAnim& a, float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    const std::vector<AgrPosKey>& v = a.pos[kAgrBones - 1]; // ARROW
    if (!a.decoded || v.size() < 2) return;
    for (int k = 0; k < 3; k++) out[k] = v.back().p[k] - v.front().p[k];
}

void AgrMatrix(const float q[4], float m[9]) {
    const float w = q[0], x = q[1], y = q[2], z = q[3];
    m[0] = 1 - 2*(y*y + z*z); m[1] = 2*(x*y - z*w);     m[2] = 2*(x*z + y*w);
    m[3] = 2*(x*y + z*w);     m[4] = 1 - 2*(x*x + z*z); m[5] = 2*(y*z - x*w);
    m[6] = 2*(x*z - y*w);     m[7] = 2*(y*z + x*w);     m[8] = 1 - 2*(x*x + y*y);
}

} // namespace OpenBully
