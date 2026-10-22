#include "photo.h"

#include "../platform/logger.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace OpenBully {
namespace Photo {

namespace {

// --- Album JSON helpers (same hand-rolled style as the mission loader) -----

std::string JEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20u) out += '?';
        else out += c;
    }
    return out;
}

// Locates `"key":` after `from` and returns the index of the value token's
// first character ('"' or a digit / '-' when it is a number).
std::string::size_type FindValue(const std::string& t,
                                 std::string::size_type from,
                                 std::string::size_type end,
                                 const char* key) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    std::string::size_type p = t.find(needle, from);
    while (p != std::string::size_type(-1) && p < end) {
        const std::string::size_type c = t.find(':', p + needle.size());
        if (c != std::string::size_type(-1) && c < end) {
            std::string::size_type v = c + 1;
            while (v < end && (t[v] == ' ' || t[v] == '\t' ||
                                t[v] == '\n' || t[v] == '\r'))
                ++v;
            if (v < end && (t[v] == '"' || t[v] == '-' ||
                            (t[v] >= '0' && t[v] <= '9')))
                return v;
        }
        p = t.find(needle, p + needle.size());
    }
    return std::string::size_type(-1);
}

bool ReadString(const std::string& t, std::string::size_type from,
                std::string::size_type end, const char* key, std::string& out) {
    const std::string::size_type v = FindValue(t, from, end, key);
    if (v == std::string::size_type(-1) || t[v] != '"') return false;
    out.clear();
    std::string::size_type i = v + 1;
    while (i < end && t[i] != '"') {
        if (t[i] == '\\' && i + 1 < end) {
            const char e = t[i + 1];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 't':  out += '\t'; break;
                case 'r':  out += '\r'; break;
                default:   out += e;    break;
            }
            i += 2;
        } else {
            out += t[i++];
        }
    }
    return true;
}

bool ReadInt(const std::string& t, std::string::size_type from,
             std::string::size_type end, const char* key, int* out) {
    const std::string::size_type v = FindValue(t, from, end, key);
    if (v == std::string::size_type(-1)) return false;
    char* e = nullptr;
    const long n = std::strtol(t.c_str() + v, &e, 10);
    if (!e || e == t.c_str() + v) return false;
    *out = static_cast<int>(n);
    return true;
}

bool ReadInt64(const std::string& t, std::string::size_type from,
               std::string::size_type end, const char* key, int64_t* out) {
    const std::string::size_type v = FindValue(t, from, end, key);
    if (v == std::string::size_type(-1)) return false;
    char* e = nullptr;
    const int64_t n = std::strtoll(t.c_str() + v, &e, 10);
    if (!e || e == t.c_str() + v) return false;
    *out = n;
    return true;
}

} // namespace

// --- Registry --------------------------------------------------------------

void PhotoSystem::RegisterTarget(EntityID entity, const std::string& name,
                                 TargetKind kind, float height) {
    if (!entity.IsValid()) return;
    PhotoTarget t;
    t.entity = entity;
    t.name   = name;
    t.kind   = kind;
    t.height = height > 0.0f ? height : 1.8f;
    m_targets.push_back(std::move(t));
}

// --- Projection + assessment ----------------------------------------------

bool PhotoSystem::ProjectPoint(const glm::vec3& world, const glm::mat4& viewProj,
                               float screenW, float screenH,
                               float* outX, float* outY) {
    const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-3f) return false;   // behind (or on) the camera plane
    const float ndcX = clip.x / clip.w;
    const float ndcY = clip.y / clip.w;
    if (outX) *outX = (ndcX * 0.5f + 0.5f) * screenW;
    if (outY) *outY = (1.0f - (ndcY * 0.5f + 0.5f)) * screenH;
    return true;
}

AimAssessment PhotoSystem::Assess(const PhotoTarget& t, const glm::vec3& pos,
                                  const glm::vec3& camPos,
                                  const glm::mat4& viewProj,
                                  float screenW, float screenH) const {
    AimAssessment a;
    if (screenW <= 0.0f || screenH <= 0.0f) return a;

    // Distance is measured to the mid body (feet + height/2), the point the
    // player actually aims at.
    const glm::vec3 mid = pos + glm::vec3(0.0f, 0.5f * t.height, 0.0f);
    a.dist    = glm::length(mid - camPos);
    a.inRange = a.dist <= kMaxSubjectDist;

    float xMid = 0.0f, yMid = 0.0f;
    const bool midVisible =
        ProjectPoint(mid, viewProj, screenW, screenH, &xMid, &yMid);
    a.inFrustum = midVisible &&
                  xMid >= 0.0f && xMid <= screenW &&
                  yMid >= 0.0f && yMid <= screenH;
    if (midVisible)
        a.centered =
            std::fabs(xMid - 0.5f * screenW) <= kCenterX * screenW &&
            std::fabs(yMid - 0.5f * screenH) <= kCenterY * screenH;

    // Framing: both the feet and the head must project to a real depth,
    // otherwise the span is meaningless (the subject straddles the eye).
    float xFeet = 0.0f, yFeet = 0.0f, xHead = 0.0f, yHead = 0.0f;
    const bool feetVisible =
        ProjectPoint(pos, viewProj, screenW, screenH, &xFeet, &yFeet);
    const bool headVisible =
        ProjectPoint(pos + glm::vec3(0.0f, t.height, 0.0f), viewProj,
                     screenW, screenH, &xHead, &yHead);
    if (feetVisible && headVisible) {
        a.fill = std::fabs(yFeet - yHead) / screenH;
        a.framed = a.fill >= kMinFill;
    }
    return a;
}

// --- Album persistence -----------------------------------------------------

bool PhotoSystem::SaveAlbum(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        Logger::Error("Photo album: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f << "{\n  \"photos\": [\n";
    const size_t n = m_album.size();
    for (size_t i = 0; i < n; ++i) {
        const PhotoRecord& r = m_album[i];
        char buf[640];
        std::snprintf(buf, sizeof(buf),
                      "    { \"subject\": \"%s\", \"entity\": %u, "
                      "\"mission\": \"%s\", \"clock\": %d, \"frame\": %lld }",
                      JEscape(r.subject).c_str(),
                      static_cast<unsigned int>(r.entityIndex),
                      JEscape(r.missionId).c_str(),
                      r.clockMinutes, static_cast<long long>(r.frame));
        f << buf;
        if (i + 1 < n) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
    f.close();
    if (!f) {
        Logger::Error("Photo album: write failed for '%s'", path.c_str());
        return false;
    }
    Logger::Info("Photo album: saved %zu photo(s) to %s", n, path.c_str());
    return true;
}

bool PhotoSystem::LoadAlbum(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;   // missing album = empty album, not an error
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    m_album.clear();
    // Each record is one JSON object; the "subject" key is the anchor so a
    // malformed file degrades to however many complete records it holds.
    static const std::string kAnchor = "\"subject\"";
    std::string::size_type from = 0;
    while (true) {
        const std::string::size_type p = text.find(kAnchor, from);
        if (p == std::string::size_type(-1)) break;
        const std::string::size_type nxt = text.find(kAnchor, p + kAnchor.size());
        const std::string::size_type end =
            (nxt == std::string::size_type(-1)) ? text.size() : nxt;
        PhotoRecord r;
        ReadString(text, p, end, "subject", r.subject);
        ReadString(text, p, end, "mission", r.missionId);
        ReadInt(text, p, end, "entity",
                reinterpret_cast<int*>(
                    reinterpret_cast<unsigned int*>(&r.entityIndex)));
        ReadInt(text, p, end, "clock", &r.clockMinutes);
        ReadInt64(text, p, end, "frame", &r.frame);
        m_album.push_back(std::move(r));
        from = end;
    }
    Logger::Info("Photo album: loaded %zu photo(s) from %s", m_album.size(),
                 path.c_str());
    return true;
}

} // namespace Photo
} // namespace OpenBully
