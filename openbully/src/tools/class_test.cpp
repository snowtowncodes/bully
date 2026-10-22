// Track 41: class system self-test (headless; no GPU/SDL).
//
// Exercises the pure logic the engine drives per frame:
//   - JSON class definitions load (data/classes/classes.json)
//   - schedule queries (day mask + time window + proximity)
//   - NextUpcoming countdown math across day wrap
//   - progression (completions/fails/respect) + save snapshot/restore
//   - SaveSystem serialize/deserialize roundtrip of the class section
//
// Exit code 0 = all checks passed; 1 = at least one failed.

#include "../src/game/classes.h"
#include "../src/engine/save_system.h"
#include "../src/platform/logger.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace OpenBully;

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL: %s\n", what);
    } else {
        std::printf("ok:   %s\n", what);
    }
}

std::unordered_map<std::string, glm::vec3> MakeRefs() {
    std::unordered_map<std::string, glm::vec3> refs;
    refs["anchor_center"] = glm::vec3(0.0f, 0.0f, 0.0f);
    refs["spawn"] = glm::vec3(0.0f, 0.0f, 0.0f);
    return refs;
}

} // namespace

int main() {
    Logger::Init();

    // --- 1. JSON load --------------------------------------------------------
    ClassManager cm;
    const bool loaded = cm.LoadFromJson("data/classes/classes.json", MakeRefs()) ||
                        cm.LoadFromJson("../data/classes/classes.json", MakeRefs());
    Check(loaded, "classes.json loads");
    Check(cm.Count() == 10, "10 class definitions registered");

    const ClassData* english = cm.Find("english_1");
    Check(english != nullptr, "english_1 exists");
    if (english) {
        Check(english->type == ClassType::English, "english_1 type ENGLISH");
        Check(english->schedule.dayMask == ((1u << 0) | (1u << 2) | (1u << 4)),
              "english_1 days MON/WED/FRI");
        Check(english->schedule.startMinute == 9 * 60 &&
              english->schedule.endMinute == 10 * 60, "english_1 09:00-10:00");
        Check(english->position.x == 6.0f && english->position.z == -1.0f,
              "english_1 top-level ref+offset position (6,0,-1)");
        Check(english->objectives.size() == 3, "english_1 has 3 objectives");
        Check(english->rewards.size() == 2, "english_1 has 2 rewards");
        Check(english->DaysText() == "MON/WED/FRI", "DaysText MON/WED/FRI");
        Check(english->TimeText() == "09:00-10:00", "TimeText 09:00-10:00");
        Check(english->DayActive(0) && !english->DayActive(1) &&
              english->DayActive(4) && !english->DayActive(6),
              "DayActive MON/WED/FRI true, TUE/SUN false");
        Check(english->TimeActive(9 * 60 + 30) && !english->TimeActive(10 * 60 + 1),
              "TimeActive window bounds");
    }
    const ClassData* chemistry = cm.Find("chemistry_1");
    Check(chemistry != nullptr && chemistry->type == ClassType::Chemistry,
          "chemistry_1 type CHEMISTRY");
    if (chemistry && !chemistry->rewards.empty())
        Check(chemistry->rewards[0].kind == RewardKind::Weapon &&
              chemistry->rewards[0].weaponId == 6 && chemistry->rewards[0].amount == 5,
              "chemistry_1 firecracker weapon reward (id 6, x5)");
    const ClassData* gym = cm.Find("gym_1");
    if (gym && !gym->rewards.empty())
        Check(gym->rewards[0].kind == RewardKind::MaxHealth && gym->rewards[0].amount == 10,
              "gym_1 max_health +10 reward");

    // --- 2. AvailableIndex (schedule + proximity) -----------------------------
    // Monday (day 0) 09:30, standing at english_1's marker -> English 1.
    const int idx = cm.AvailableIndex(english ? english->position : glm::vec3(0.0f),
                                      0, 9 * 60 + 30);
    Check(idx == cm.IndexOf("english_1"), "Monday 09:30 at English 1 -> prompt class");

    // Monday 12:00 (no class in that window anywhere near) -> none.
    const int noneIdx = cm.AvailableIndex(glm::vec3(0.0f), 0, 12 * 60);
    Check(noneIdx < 0, "Monday 12:00 -> no class available");

    // Monday 09:30 but far away -> none (proximity gate).
    const int farIdx = cm.AvailableIndex(glm::vec3(500.0f, 0.0f, 500.0f), 0, 9 * 60 + 30);
    Check(farIdx < 0, "Monday 09:30 far away -> no class");

    // Monday 13:30: chemistry (offset 0,4) and shop (offset 4,5) both live;
    // standing at chemistry's marker must pick Chemistry (nearest).
    {
        const ClassData* chem = cm.Find("chemistry_1");
        const int chemIdx = chem ? cm.AvailableIndex(chem->position, 0, 13 * 60 + 30) : -2;
        Check(chemIdx == cm.IndexOf("chemistry_1"), "Monday 13:30 at Chemistry -> prompt");
    }

    // --- 3. NextUpcoming (cross-day countdown) --------------------------------
    {
        int lead = -1;
        const ClassData* next = cm.NextUpcoming(0, 8 * 60, &lead); // Monday 08:00
        Check(next != nullptr && next->id == "english_1", "Monday 08:00 -> next is English 1");
        Check(lead == 60, "English 1 starts in 60 min");
    }
    {
        int lead = -1;
        const ClassData* next = cm.NextUpcoming(0, 16 * 60, &lead); // Monday 16:00
        Check(next != nullptr && next->id == "art_1",
              "Monday 16:00 -> next is Art 1 (TUE 09:00)");
        Check(lead == 17 * 60, "Art 1 in 17h (1020 min)");
    }
    {
        // Saturday 20:00 -> nothing until Monday 09:00 English = 1d13h = 2220 min.
        int lead = -1;
        const ClassData* next = cm.NextUpcoming(5, 20 * 60, &lead);
        Check(next != nullptr && next->id == "english_1", "Saturday 20:00 -> next is English 1");
        Check(lead == 1440 + 13 * 60, "English 1 in 1d 13h (2220 min)");
    }

    // --- 4. Progression -------------------------------------------------------
    const size_t eIdx = cm.IndexOf("english_1");
    Check(eIdx != SIZE_MAX, "IndexOf english_1");
    cm.MarkComplete(eIdx);
    cm.MarkComplete(eIdx);
    cm.MarkComplete(eIdx);
    cm.MarkFailed(eIdx);
    Check(cm.Completions(eIdx) == 3, "3 completions recorded");
    Check(cm.Fails(eIdx) == 1, "1 fail recorded");
    Check(cm.TotalCompletions() == 3, "total completions 3");
    cm.AddRespect(4);
    Check(cm.Respect() == 4, "respect 4");

    // --- 5. Save snapshot / restore ------------------------------------------
    std::vector<ClassSave> snap = cm.Snapshot();
    Check(snap.size() == 10, "snapshot has 10 entries");
    bool snapOk = false;
    for (const ClassSave& s : snap)
        if (s.id == "english_1") snapOk = (s.completions == 3 && s.fails == 1);
    Check(snapOk, "english_1 snapshot (3 completions, 1 fail)");

    ClassManager cm2;
    cm2.LoadFromJson("data/classes/classes.json", MakeRefs()) ||
        cm2.LoadFromJson("../data/classes/classes.json", MakeRefs());
    cm2.Restore(snap);
    const size_t e2 = cm2.IndexOf("english_1");
    Check(cm2.Completions(e2) == 3 && cm2.Fails(e2) == 1,
          "restored manager shows 3/1 for english_1");

    // --- 6. SaveSystem roundtrip (class section) ------------------------------
    SaveData out;
    out.version = 1;
    out.slot = 0;
    out.classes = snap;
    out.classRespect = 4;
    out.slingshotCapBonus = 10;
    out.gameDay = 2;
    out.gameTimeMinutes = 9 * 60.0 + 30.0;
    const std::string json = SaveSystem::Serialize(out);

    SaveData in;
    const bool parsed = SaveSystem::Deserialize(json, in);
    Check(parsed, "save JSON parses");
    Check(in.classes.size() == 10, "roundtrip: 10 class entries");
    bool rtOk = false;
    for (const ClassSave& s : in.classes)
        if (s.id == "english_1") rtOk = (s.completions == 3 && s.fails == 1);
    Check(rtOk, "roundtrip: english_1 3/1");
    Check(in.classRespect == 4, "roundtrip: respect 4");
    Check(in.slingshotCapBonus == 10, "roundtrip: slingshot cap bonus 10");
    Check(in.gameDay == 2, "roundtrip: game day 2");

    // --- 7. Enum helpers ------------------------------------------------------
    Check(std::string(ClassName(ClassType::Photography)) == "Photography",
          "ClassName(Photography)");
    Check(ParseClassType("BIOLOGY") == ClassType::Biology, "ParseClassType BIOLOGY");
    Check(ParseRewardKind("ammo_cap") == RewardKind::AmmoCap, "ParseRewardKind ammo_cap");
    Check(std::string(RewardKindName(RewardKind::Respect)) == "RESPECT",
          "RewardKindName(Respect)");

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
