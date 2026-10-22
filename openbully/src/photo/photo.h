#pragma once

// Track 47: photo challenge system.
//
// Photography-class missions (Bully's yearbook side-quests) require the
// player to frame and photograph specific subjects. This module is the
// engine-facing core: a target registry (which entities count as subjects),
// a shutter-time assessment (frustum / range / framing / centering) and the
// album (metadata only — no bitmap capture; the retail game composites the
// shot at the end, which is out of scope here).
//
// The viewfinder itself (crosshair, zoom, LMB shutter, mission wiring) lives
// in the Engine because it needs the live camera + inventory + HUD state;
// this file stays free of engine/entity-manager includes so it can be unit
// tested with hand-built matrices.

#include "../entities/entity_id.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace OpenBully {
namespace Photo {

// Kind of photographable subject (task spec: people, objects, activities).
enum class TargetKind : uint8_t {
    Person   = 0,   // students, prefects, teachers
    Object   = 1,   // bikes, cars, props
    Activity = 2,   // fights, dances (an NPC pair acting as one subject)
};

// A registered photographable subject. Registered by the Engine when an NPC
// or vehicle spawns (label comes from the NPC's schedule / model name so a
// mission can require e.g. "Jock" or "Prefect"). Dead entity handles are
// pruned every frame (scheduled NPCs despawn through the day cycle).
struct PhotoTarget {
    EntityID    entity{};
    std::string name;           // subject label ("Jock", "Prefect", "Bike")
    TargetKind  kind    = TargetKind::Person;
    float       height  = 1.8f; // world-space height used by the framing check
};

// Result of validating one subject against the camera view. Every flag is
// independent so the UI can report which check failed (the shutter only
// counts a photo when all of them pass).
struct AimAssessment {
    bool  inFrustum = false;  // subject projects inside the NDC box
    bool  inRange   = false;  // within kMaxSubjectDist
    bool  framed    = false;  // spans >= kMinFill of the view height
    bool  centered  = false;  // center pixel within kCenterX/kCenterY margins
    float fill      = 0.0f;   // fraction of the view height the subject spans
    float dist      = 0.0f;   // camera -> subject (units)

    bool Valid() const { return inFrustum && inRange && framed && centered; }
};

// Album entry: what was photographed and when. No screenshot is stored — the
// metadata is enough to list "what I've taken" in the album UI.
struct PhotoRecord {
    std::string subject;    // "(no subject)" when the shutter missed everything
    uint32_t    entityIndex = 0xFFFFFFFFu; // subject entity (0xFFFFFFFF = none)
    std::string missionId;  // mission the photo counted toward ("" = free shot)
    int         clockMinutes = 0;          // game clock at shutter time
    int64_t     frame   = 0;               // engine frame tag
};

// Owns the subject registry + album. Pure state + math: no engine, no SDL.
class PhotoSystem {
public:
    // --- target registry --------------------------------------------------

    void RegisterTarget(EntityID entity, const std::string& name,
                        TargetKind kind, float height);
    std::vector<PhotoTarget>& Targets() { return m_targets; }
    const std::vector<PhotoTarget>& Targets() const { return m_targets; }
    void ClearTargets() { m_targets.clear(); }

    // --- validation -------------------------------------------------------

    // Projects a world point with `viewProj`; false when it is behind the
    // camera (clip.w <= 0). Outputs are pixels, origin top-left.
    static bool ProjectPoint(const glm::vec3& world, const glm::mat4& viewProj,
                             float screenW, float screenH,
                             float* outX, float* outY);

    // Full shutter assessment of one subject at `pos` (entity origin, feet).
    AimAssessment Assess(const PhotoTarget& t, const glm::vec3& pos,
                         const glm::vec3& camPos, const glm::mat4& viewProj,
                         float screenW, float screenH) const;

    // --- album ------------------------------------------------------------

    const std::vector<PhotoRecord>& Album() const { return m_album; }
    void AddPhoto(const PhotoRecord& rec) { m_album.push_back(rec); }
    void ClearAlbum() { m_album.clear(); }

    // Album persistence: <base>/photo_album.json (same dir as the save slots).
    bool SaveAlbum(const std::string& path) const;
    bool LoadAlbum(const std::string& path);

    // Tunables (task spec: distance + framing thresholds). kMinFill 0.12:
    // at the demo sight line a 1.8u body 10u away spans ~15% of a 20-deg
    // zoomed view; 0.20 demanded belly-flop closeups that pushed the shutter
    // out of comfortable yearbook framing.
    static constexpr float kMaxSubjectDist = 30.0f; // u
    static constexpr float kMinFill        = 0.12f; // >=12% of view height
    static constexpr float kCenterX        = 0.30f; // +-30% of width from center
    static constexpr float kCenterY        = 0.35f; // +-35% of height from center

private:
    std::vector<PhotoTarget>  m_targets;
    std::vector<PhotoRecord>  m_album;
};

} // namespace Photo
} // namespace OpenBully
