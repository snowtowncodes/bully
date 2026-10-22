#pragma once

// Track 20: mission system.
//
// The retail game compiles mission logic to Lua 5.0 bytecode (.lur), which we
// deliberately do not parse. Missions are instead hand-authored as JSON (see
// data/missions/demo_missions.json) and loaded into plain structs; the engine
// registers trigger zones around the demo level's world meshes and drives the
// manager with Jimmy's position every frame.
//
// Objective semantics:
//   GO_TO_LOCATION / TALK_TO_NPC / COLLECT_ITEM — completed by proximity to
//     targetPosition (walking within completionRadius). TALK/COLLECT demo
//     content places targets at known NPC/prop spots.
//   DEFEAT_NPC / CUSTOM — completed externally: NotifyNpcDefeated(entityId)
//     or CompleteObjective(mission, objective). For hand-authored demo
//     content an objective may set "auto_complete_on_reach" so proximity
//     stands in for the scripted beat it replaces.

#include "../entities/entity.h"
#include "../engine/save_system.h" // Track 36: MissionSave / TriggerSave
#include "../game/faction.h" // Track 42: Faction enum + ParseFactionName

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenBully {

// Kinds of objective (Task spec enum, C++-cased).
enum class ObjectiveType : uint8_t {
    GoToLocation,   // reach targetPosition within completionRadius
    TalkToNpc,      // reach the NPC at targetPosition (proximity talk)
    DefeatNpc,      // completed externally via NotifyNpcDefeated()
    CollectItem,    // reach the item at targetPosition
    PhotographTarget, // Track 47: take N photos of a subject (NotifyPhotoTaken)
    Custom,           // completed externally via CompleteObjective()
    AvoidBust        // Track 43: survive `surviveTime` s; Busted() fails it
};

// Lifecycle of a mission.
enum class MissionState : uint8_t {
    Inactive,       // registered, not yet started
    Active,         // objectives in progress
    Complete,       // all objectives done
    Failed          // terminal failure (no fail conditions yet; reserved)
};

// One step of a mission.
struct Objective {
    ObjectiveType type   = ObjectiveType::GoToLocation;
    glm::vec3     targetPosition{0.0f};  // marker + proximity check point
    EntityID      targetEntity{};        // TALK/DEFEAT/COLLECT target (optional)
    float         completionRadius = 2.5f;
    bool          completeOnReach  = false; // proximity completes DEFEAT/CUSTOM too
    std::string   description;              // log/UI text
    // Track 47: PHOTOGRAPH_TARGET counters. photoTarget matches the subject
    // label the photo engine registered (empty = any valid photo counts).
    std::string   photoTarget;
    int           photoNeeded = 1;   // photos required to complete
    int           photoCount  = 0;   // valid photos so far
    // Track 43: AVOID_BUST timing. `surviveTime` is the required survival
    // duration (seconds of engine time); `timer` accumulates while the
    // objective is current and the wanted meter is untouched.
    float         surviveTime     = 0.0f;
    float         timer           = 0.0f;
    bool          complete = false;
};

// A mission: an ordered objective list driven by MissionManager.
struct Mission {
    std::string          id;
    std::string          name;
    std::string          description; // short blurb shown in the quest journal
    std::vector<Objective> objectives;
    size_t               currentObjective = 0;
    MissionState         state = MissionState::Inactive;
    std::string          startAfter; // activation chain: complete this id first

    // --- Track 42: faction respect reward -----------------------------------
    // Granted once when the mission completes (JSON "respect_reward":
    // { "faction": "NERDS", "amount": 20 }); respectFaction < 0 = none.
    int  respectFaction = -1;              // Faction enum value
    int  respectReward  = 20;              // delta applied to that faction
};

// World-space AABB that starts a mission when the player enters it (once).
struct TriggerZone {
    std::string id;         // e.g. "tz_spawn"
    std::string missionId;  // mission activated on entry
    glm::vec3   boundsMin{0.0f};
    glm::vec3   boundsMax{0.0f};
    bool        fired = false;
};

// Owns missions + trigger zones; advances their state from player position.
class MissionManager {
public:
    // --- registration -------------------------------------------------------

    // Appends a mission; returns its index.
    uint32_t RegisterMission(const Mission& mission);
    // Appends a trigger zone; returns its index.
    uint32_t RegisterTriggerZone(const TriggerZone& zone);

    // --- JSON loading ---------------------------------------------------------

    // Parses { "missions": [...], "trigger_zones": [...] } from `path`.
    // Positions/bounds may be absolute ("position": [x,y,z]) or relative:
    // { "ref": "<name>", "offset": [x,y,z] } resolved against `refs`
    // (runtime points: Jimmy spawn, anchor-mesh center/corners). Logs each
    // registered mission and zone. Returns false (with an error log) when the
    // file is missing or malformed.
    bool LoadFromJson(const char* path,
                      const std::unordered_map<std::string, glm::vec3>& refs);

    // --- per-frame update -----------------------------------------------------

    // Fires trigger zones the player entered, checks the active mission's
    // current objective, and starts chained missions. Logs every transition.
    // `dt` feeds the Track 43 AVOID_BUST survival timer (0 = no tick).
    void Update(const glm::vec3& playerPos, float dt = 0.0f);

    // --- external completion (DEFEAT_NPC / CUSTOM) ------------------------------

    // Completes the active mission's current objective when it targets `npc`.
    bool NotifyNpcDefeated(EntityID npc);
    // Track 43: fail every active mission whose current objective is
    // AVOID_BUST ("don't get busted"). Called from Engine::FinishBust;
    // returns the number of missions failed (0 = none applied).
    int  NotifyBust();
    // Track 47: a valid photo of `subject` was taken. Advances every active
    // PHOTOGRAPH_TARGET objective whose subject matches (empty photoTarget
    // accepts any). Returns true when at least one counter moved.
    bool NotifyPhotoTaken(const std::string& subject);
    // Track 47: first mission whose current objective is a live (incomplete)
    // PHOTOGRAPH_TARGET, or nullptr. Unlike ActiveMission() this skips
    // non-photo missions that are active at the same time (chained demo
    // content keeps ch1_welcome active indefinitely).
    const Mission* ActivePhotoMission() const;
    // Force-completes a specific objective (script hooks).
    bool CompleteObjective(uint32_t missionIndex, size_t objectiveIndex);
    // Activates an Inactive mission, resetting its objective cursor (script
    // hooks / the OPENBULLY_JOURNAL_DEMO verification drive). Chained missions
    // still activate through Update() when their prerequisite completes.
    bool ActivateMission(uint32_t missionIndex);

    // --- accessors --------------------------------------------------------------

    size_t             MissionCount() const { return m_missions.size(); }
    size_t             TriggerCount() const { return m_triggers.size(); }
    const Mission*     GetMission(uint32_t index) const;
    const Mission*     FindMission(const std::string& id) const;
    // First mission in Active state (nullptr when none).
    const Mission*     ActiveMission() const;
    const TriggerZone* GetTrigger(size_t index) const;

    // --- Track 36: save/load ----------------------------------------------------
    // Collects all mission state (id, state, currentObjective, per-objective
    // complete flags) into a vector for serialization.
    std::vector<MissionSave> SnapshotMissions() const;
    // Restores mission state from a save: sets state, currentObjective, and
    // per-objective complete flags for every registered mission.
    void RestoreMissions(const std::vector<MissionSave>& saved);
    // Collects trigger zone fired states.
    std::vector<TriggerSave> SnapshotTriggers() const;
    // Restores trigger zone fired states from a save.
    void RestoreTriggers(const std::vector<TriggerSave>& saved);

    // --- Track 42: completion hook ---------------------------------------------
    // Fired exactly once when a mission completes (the Engine applies the
    // mission's faction respect reward). Bound in Engine::InitMissions.
    void SetOnComplete(std::function<void(const Mission&)> fn)
        { m_onComplete = std::move(fn); }

private:
    void UpdateTriggerZones(const glm::vec3& playerPos);
    void UpdateActiveMissions(const glm::vec3& playerPos, float dt);
    void CheckObjective(uint32_t missionIndex, const glm::vec3& playerPos, float dt);
    void CompleteMission(uint32_t missionIndex);
    bool ReachedObjective(const Objective& o, const glm::vec3& playerPos) const;

    std::vector<Mission>     m_missions;
    std::vector<TriggerZone> m_triggers;
    std::function<void(const Mission&)> m_onComplete; // Track 42
};

} // namespace OpenBully
