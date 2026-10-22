#pragma once

// Track 29: NPC schedule system.
//
// A schedule maps time-of-day windows to a spawn state (SPAWNED /
// DESPAWNED) plus a behavior override (WANDER / IDLE). Schedules are
// hand-authored as JSON (data/schedules/npc_schedules.json) — the same
// approach the mission system uses for retail-.lur content we deliberately
// do not parse.
//
// Window semantics:
//   - startMinute inclusive, endMinute exclusive (minutes since midnight).
//   - A window may wrap past midnight (start > end), e.g. 22:00-07:00.
//   - Minutes not covered by any window are treated as DESPAWNED (safe
//     default: an unscheduled hour never leaves a schedule-ghost around).
//
// The Engine pairs every scheduled NPC slot with one schedule and re-evaluates
// the active window each frame; spawn/despawn/behavior transitions are driven
// by Engine::UpdateSchedules.

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenBully {

// Should the NPC exist during this window?
enum class SpawnState : uint8_t {
    Despawned = 0,
    Spawned   = 1,
};

// Behavior override while the window is active. Wander = normal AI
// (idle -> pick wander target); Idle = pinned in place (no wandering).
enum class ScheduleBehavior : uint8_t {
    Wander = 0,
    Idle   = 1,
};

// One time-of-day range within a schedule.
struct ScheduleWindow {
    int startMinute = 0;          // minutes since midnight, inclusive
    int endMinute   = 0;          // exclusive; start > end wraps midnight
    SpawnState state          = SpawnState::Despawned;
    ScheduleBehavior behavior = ScheduleBehavior::Wander;
    glm::vec3 spawnPoint{0.0f};   // (re)spawn location + despawn walk target
    bool hasSpawnPoint = false;   // false = engine fallback location
};

// A named day-cycle (student / townie / prefect / ...).
struct NPCSchedule {
    std::string name;
    std::vector<ScheduleWindow> windows;

    // Index of the window covering `minute` (minutes since midnight), or -1.
    // Handles windows that wrap past midnight (start > end).
    int WindowIndexAt(int minute) const {
        for (size_t i = 0; i < windows.size(); ++i) {
            const ScheduleWindow& w = windows[i];
            const bool covered =
                w.startMinute <= w.endMinute
                    ? (minute >= w.startMinute && minute < w.endMinute)
                    : (minute >= w.startMinute || minute < w.endMinute);
            if (covered) return int(i);
        }
        return -1;
    }
};

// Loads and owns the schedule set.
class ScheduleManager {
public:
    // Parses { "schedules": [ { "name": ..., "windows": [...] }, ... ] }.
    // Window "start"/"end" accept "HH:MM" strings or minute counts; a
    // window's "spawn_point" is { "ref": "<name>", "offset": [x,y,z] } or an
    // absolute [x,y,z], resolved against `refs` (same convention as the
    // mission JSON). Logs each schedule. Returns false when nothing could be
    // loaded (missing/malformed file).
    bool LoadFromJson(const char* path,
                      const std::unordered_map<std::string, glm::vec3>& refs);

    size_t ScheduleCount() const { return m_schedules.size(); }
    const NPCSchedule* GetSchedule(size_t index) const;
    const NPCSchedule* Find(const std::string& name) const;

private:
    std::vector<NPCSchedule> m_schedules;
};

} // namespace OpenBully
