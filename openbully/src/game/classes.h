#pragma once

// Track 41: class system (attendance + completion).
//
// Bully's progression centers on classes (English, Math, Gym, ...). Retail
// compiles class logic into .lur scripts, which we deliberately do not parse;
// classes are hand-authored as JSON (data/classes/classes.json) following the
// mission/schedule loader conventions: positions relative to runtime ref
// points, "HH:MM" clock strings, day names (MON..SUN).
//
// A class pairs a world location + weekly schedule with a placeholder
// minigame (Phase 12 swaps in real minigames) and a reward list applied on
// pass. Per-class progression (completions/fails) + total respect snapshot
// into the save file (Track 36 pattern, see ClassSave in save_system.h).

#include "../engine/save_system.h" // Track 41: ClassSave

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenBully {

// Discipline of a class (Track 41 spec enums, C++-cased).
enum class ClassType : uint8_t {
    English = 0,
    Math,
    Gym,
    Art,
    Chemistry,
    Shop,
    Photography,
    Music,
    Geography,
    Biology,
    Count
};

// "English" etc. for UI/logs; unknown values read as "?".
const char* ClassName(ClassType t);
// "ENGLISH" -> ClassType::English (unknown falls back to English).
ClassType ParseClassType(const std::string& s);

// Reward kinds applied once per passed session.
enum class RewardKind : uint8_t {
    MaxHealth = 0,   // +amount permanent max health (GYM)
    Money,           // +amount cash
    Weapon,          // grant weaponId with amount ammo into a free slot (CHEMISTRY)
    Ammo,            // +amount rocks into an owned slingshot
    AmmoCap,         // +amount slingshot ammo capacity (SHOP upgrade)
    Respect,         // +amount respect (progression counter)
    Count
};
RewardKind ParseRewardKind(const std::string& s);
const char* RewardKindName(RewardKind k);

struct ClassReward {
    RewardKind kind = RewardKind::Money;
    int amount   = 0;
    int weaponId = 0;    // kTable index, Weapon kind only
};

// Weekly availability: bitmask, bit 0 = Monday .. bit 6 = Sunday.
struct ClassSchedule {
    uint32_t dayMask    = 0x7F;
    int startMinute = 0; // minutes since midnight, inclusive
    int endMinute   = 0; // exclusive (start > end wraps midnight)
};

// One attendable class definition.
struct ClassData {
    std::string id;                      // "english_1"
    ClassType   type = ClassType::English;
    std::string displayName;             // "English 1"
    std::string building;                // "Main School Building"
    std::string room;                    // "Room 101"
    ClassSchedule schedule;
    glm::vec3   position{0.0f};          // attendance marker (world)
    float       radius = 3.0f;           // prompt + attendance radius
    std::vector<std::string> objectives; // minigame beats (1 hit completes one)
    std::vector<ClassReward> rewards;
    int sessionsRequired = 5;            // completions that "finish" the class

    bool DayActive(int dayIndex) const {
        return dayIndex >= 0 && dayIndex < 7 &&
               (schedule.dayMask & (1u << dayIndex)) != 0;
    }
    bool TimeActive(int minuteNow) const {
        return schedule.startMinute <= schedule.endMinute
            ? (minuteNow >= schedule.startMinute && minuteNow < schedule.endMinute)
            : (minuteNow >= schedule.startMinute || minuteNow < schedule.endMinute);
    }
    // "MON/WED/FRI" for the schedule UI.
    std::string DaysText() const;
    // "09:00-10:00" for the schedule UI.
    std::string TimeText() const;
};

// Owns class definitions + progression; answers the engine's per-frame
// queries (prompt detection, schedule overlay, next-up countdown).
class ClassManager {
public:
    // Parses { "classes": [ ... ] }. Class "position" accepts an absolute
    // [x,y,z] or { "ref": "<name>", "offset": [x,y,z] } resolved against
    // `refs` (same convention as the mission/schedule JSON). Returns false
    // (with an error log) when the file is missing or malformed.
    bool LoadFromJson(const char* path,
                      const std::unordered_map<std::string, glm::vec3>& refs);

    size_t           Count() const { return m_classes.size(); }
    const ClassData* Get(size_t index) const;
    const ClassData* Find(const std::string& id) const;
    // Index by identity (SIZE_MAX when absent); completion/fail accessors
    // key off this index.
    size_t           IndexOf(const std::string& id) const;

    // Nearest class covering (dayIndex, minuteNow) within its radius of
    // playerPos. Returns the manager index (-1 = none); `out`/`distOut`
    // optional.
    int  AvailableIndex(const glm::vec3& playerPos, int dayIndex,
                        int minuteNow, const ClassData** out = nullptr,
                        float* distOut = nullptr) const;
    // Nearest class whose schedule covers (dayIndex, minuteNow) regardless of
    // distance (demo automation walks to it; the schedule overlay highlights
    // by window, not by proximity).
    int  ScheduledIndexNearest(const glm::vec3& playerPos, int dayIndex,
                               int minuteNow) const;
    // Next not-yet-started session across all classes from
    // (dayIndex, minuteNow); lead time (minutes) via minutesUntilOut
    // (-1 when no class is scheduled in the next 7 days).
    const ClassData* NextUpcoming(int dayIndex, int minuteNow,
                                  int* minutesUntilOut) const;

    // --- progression -------------------------------------------------------
    int  Completions(size_t index) const { return m_completions[index]; }
    int  Fails(size_t index)       const { return m_fails[index]; }
    void MarkComplete(size_t index);
    void MarkFailed(size_t index);
    int  TotalCompletions() const;
    int  Respect() const { return m_respect; }
    void AddRespect(int amount) { m_respect += amount; }

    // --- Track 36 save/load -------------------------------------------------
    std::vector<ClassSave> Snapshot() const;
    void Restore(const std::vector<ClassSave>& saved);

private:
    std::vector<ClassData> m_classes;
    std::vector<int>       m_completions;
    std::vector<int>       m_fails;
    int                    m_respect = 0;
};

} // namespace OpenBully
