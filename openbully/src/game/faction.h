#pragma once

// Track 42: faction respect system.
//
// Bully's social sim centers on factions: the player keeps a running respect
// score with each clique, and actions (fights, missions, gifts) move it. The
// score bands drive faction-wide NPC behavior:
//
//   respect < -50   HOSTILE   - faction members attack on sight
//   -50 .. +50      NEUTRAL   - passive; fights back when provoked
//   respect > +50   FRIENDLY  - never attacks the player; greets on sight
//
// The player also has an OVERALL respect (mean of all faction scores plus a
// per-completed-mission bonus, clamped to [-100, 100]) shown on the stats
// screen; high overall respect calms ambient behavior (fewer random attacks,
// more greetings).
//
// Faction membership lives on AIComponent::faction, assigned at spawn from
// the NPC's model index (Engine::FactionForModel; AIRole::Prefect maps to
// Faction::Authority). This header is plain data + logic (no entity/SDL
// types) so npc.h can include it without an entity.h cycle.

#include <cstdint>
#include <string>

namespace OpenBully {

// Task-spec faction enum. Order doubles as the HUD/stats row order.
enum class Faction : uint8_t {
    Nerds = 0,
    Jocks,
    Greasers,
    Preppies,
    Bullies,
    Townies,
    Authority,   // prefects / cops (Track 28 AIRole::Prefect)
    Count
};

constexpr int kFactionCount = static_cast<int>(Faction::Count);

// Respect band for one faction.
enum class FactionStance : uint8_t { Hostile = 0, Neutral = 1, Friendly = 2 };

// Stable display names (logs / HUD / JSON parsing).
inline const char* FactionName(Faction f) {
    switch (f) {
        case Faction::Nerds:     return "Nerds";
        case Faction::Jocks:     return "Jocks";
        case Faction::Greasers:  return "Greasers";
        case Faction::Preppies:  return "Preppies";
        case Faction::Bullies:   return "Bullies";
        case Faction::Townies:   return "Townies";
        case Faction::Authority: return "Authority";
        default:                 return "?";
    }
}

inline const char* FactionStanceName(FactionStance s) {
    switch (s) {
        case FactionStance::Hostile:  return "HOSTILE";
        case FactionStance::Neutral:  return "NEUTRAL";
        case FactionStance::Friendly: return "FRIENDLY";
        default:                      return "?";
    }
}

// JSON "NERDS"/"Nerds" -> enum. Unknown strings read as Faction::Count.
inline Faction ParseFactionName(const std::string& s) {
    for (int i = 0; i < kFactionCount; ++i) {
        const char* n = FactionName(static_cast<Faction>(i));
        if (s.size() == std::string(n).size()) {
            size_t j = 0;
            while (j < s.size()) {
                char a = s[j], b = n[j];
                if (a >= 'a' && a <= 'z') a = char(a - 'a' + 'A');
                if (b >= 'a' && b <= 'z') b = char(b - 'a' + 'A');
                if (a != b) break;
                ++j;
            }
            if (j == s.size()) return static_cast<Faction>(i);
        }
    }
    return Faction::Count;
}

// Respect deltas per action (one source of truth for logs and JSON defaults).
struct FactionTuning {
    static constexpr float kHit        = -3.0f;   // landed melee/weapon hit
    static constexpr float kKo         = -8.0f;   // KO (stacks on the last hit)
    static constexpr float kGrapple    = -2.0f;   // grapple move on an NPC
    static constexpr float kBust       = -10.0f;  // caught by an authority NPC
    static constexpr float kGift       =  5.0f;   // gift (F) given to an NPC
    static constexpr float kMission    =  20.0f;  // default mission reward
    static constexpr float kOverallPerMission = 5.0f;  // overall bonus/mission
    static constexpr float kOverallMin = -100.0f;
    static constexpr float kOverallMax =  100.0f;
    static constexpr float kStanceHostileBelow  = -50.0f;
    static constexpr float kStanceFriendlyAbove =  50.0f;
};

// Per-faction respect pool + completed-mission tally. One instance, owned by
// the Engine; clamps every value to [-100, 100].
class FactionSystem {
public:
    float Get(Faction f) const {
        return (int(f) >= 0 && int(f) < kFactionCount) ? m_respect[int(f)]
                                                       : 0.0f;
    }
    int GetRounded(Faction f) const {
        const float v = Get(f);
        return int(v >= 0.0f ? v + 0.5f : v - 0.5f);
    }

    FactionStance Stance(Faction f) const {
        const float v = Get(f);
        if (v < FactionTuning::kStanceHostileBelow)  return FactionStance::Hostile;
        if (v > FactionTuning::kStanceFriendlyAbove) return FactionStance::Friendly;
        return FactionStance::Neutral;
    }

    // Applies a clamped delta; returns the value after the change.
    float Modify(Faction f, float delta, const char* reason = "") {
        if (int(f) < 0 || int(f) >= kFactionCount) return 0.0f;
        float v = m_respect[int(f)] + delta;
        if (v < -100.0f) v = -100.0f;
        if (v >  100.0f) v =  100.0f;
        m_respect[int(f)] = v;
        (void)reason;
        return v;
    }

    // Save/load restore (silent, clamped).
    void SetRespect(Faction f, float value) {
        if (int(f) < 0 || int(f) >= kFactionCount) return;
        float v = value;
        if (v < -100.0f) v = -100.0f;
        if (v >  100.0f) v =  100.0f;
        m_respect[int(f)] = v;
    }

    // Overall respect: mean across factions + bonus per completed mission,
    // clamped. High overall = calmer crowds (fewer random attacks).
    float Overall() const {
        float sum = 0.0f;
        for (int i = 0; i < kFactionCount; ++i) sum += m_respect[i];
        float v = sum / float(kFactionCount) +
                  float(m_completedMissions) * FactionTuning::kOverallPerMission;
        if (v < FactionTuning::kOverallMin) v = FactionTuning::kOverallMin;
        if (v > FactionTuning::kOverallMax) v = FactionTuning::kOverallMax;
        return v;
    }
    int GetRoundedOverall() const {
        const float v = Overall();
        return int(v >= 0.0f ? v + 0.5f : v - 0.5f);
    }

    int  CompletedMissions() const { return m_completedMissions; }
    void AddCompletedMission()     { ++m_completedMissions; }
    void SetCompletedMissions(int n) { m_completedMissions = n > 0 ? n : 0; }

    // True when at least one faction is Hostile (ambient hostility check).
    bool AnyHostile() const {
        for (int i = 0; i < kFactionCount; ++i)
            if (m_respect[i] < FactionTuning::kStanceHostileBelow) return true;
        return false;
    }

private:
    float m_respect[kFactionCount] = {};  // new kid: neutral everywhere
    int   m_completedMissions = 0;
};

} // namespace OpenBully
