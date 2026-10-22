#include "mission.h"

#include "../platform/logger.h"
#include "../engine/save_system.h" // Track 36: MissionSave / TriggerSave

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace OpenBully {

// --- Minimal JSON value parser (no external dependency; the project vendors
// no JSON library and retail .lur bytecode is deliberately not parsed) -------

namespace {

// Recursive-descent JSON parser producing a lightweight DOM.
struct JValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JValue> items;                       // Array
    std::vector<std::pair<std::string, JValue>> members; // Object

    const JValue* Find(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : members)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : m_text(text) {}

    bool Parse(JValue& out, std::string& err) {
        SkipWs();
        if (!ParseValue(out)) { err = Error("value"); return false; }
        SkipWs();
        if (m_pos != m_text.size()) { err = Error("trailing characters"); return false; }
        return true;
    }

private:
    const std::string& m_text;
    size_t m_pos = 0;
    int m_line = 1;

    void SkipWs() {
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == '\n') { ++m_line; ++m_pos; }
            else if (c == ' ' || c == '\t' || c == '\r') ++m_pos;
            else break;
        }
    }
    std::string Error(const char* what) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "line %d (byte %zu): expected %s near \"%.24s\"",
                      m_line, m_pos, what, m_text.c_str() + std::min(m_pos, m_text.size()));
        return buf;
    }
    bool Consume(char c) {
        SkipWs();
        if (m_pos < m_text.size() && m_text[m_pos] == c) { ++m_pos; return true; }
        return false;
    }
    bool Peek(char c) {
        SkipWs();
        return m_pos < m_text.size() && m_text[m_pos] == c;
    }

    bool ParseValue(JValue& v) {
        SkipWs();
        if (m_pos >= m_text.size()) return false;
        const char c = m_text[m_pos];
        switch (c) {
            case '{':  return ParseObject(v);
            case '[':  return ParseArray(v);
            case '"':  v.type = JValue::Type::String; return ParseString(v.str);
            case 't':  return ParseLit("true", v, true);
            case 'f':  return ParseLit("false", v, false);
            case 'n':  return ParseLit("null", v, true);
            default:   return ParseNumber(v);
        }
    }
    bool ParseLit(const char* lit, JValue& v, bool flag) {
        const size_t n = std::strlen(lit);
        if (m_text.compare(m_pos, n, lit) != 0) return false;
        m_pos += n;
        v.type = flag && lit[0] != 'n' ? JValue::Type::Bool : JValue::Type::Null;
        v.boolean = flag;
        return true;
    }
    bool ParseNumber(JValue& v) {
        const char* begin = m_text.c_str() + m_pos;
        char* end = nullptr;
        const double d = std::strtod(begin, &end);
        if (end == begin) return false;
        m_pos += size_t(end - begin);
        v.type = JValue::Type::Number;
        v.number = d;
        return true;
    }
    bool ParseString(std::string& out) {
        if (m_text[m_pos] != '"') return false;
        ++m_pos;
        out.clear();
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos++];
            if (c == '"') return true;
            if (c == '\\' && m_pos < m_text.size()) {
                const char e = m_text[m_pos++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    default:   out += e;    break;
                }
            } else {
                out += c;
            }
        }
        return false; // unterminated
    }
    bool ParseArray(JValue& v) {
        v.type = JValue::Type::Array;
        ++m_pos; // [
        if (Peek(']')) { ++m_pos; return true; }
        while (true) {
            v.items.emplace_back();
            if (!ParseValue(v.items.back())) return false;
            if (Consume(',')) continue;
            if (Consume(']')) return true;
            return false;
        }
    }
    bool ParseObject(JValue& v) {
        v.type = JValue::Type::Object;
        ++m_pos; // {
        if (Peek('}')) { ++m_pos; return true; }
        while (true) {
            SkipWs(); // whitespace before a key is legal ("{ \"a\": 1, \"b\": 2 }")
            std::string key;
            if (!ParseString(key)) return false;
            if (!Consume(':')) return false;
            v.members.emplace_back(std::move(key), JValue{});
            if (!ParseValue(v.members.back().second)) return false;
            if (Consume(',')) continue;
            if (Consume('}')) return true;
            return false;
        }
    }
};

// Helpers over the parsed DOM -------------------------------------------------

bool GetNumber(const JValue& obj, const char* key, double& out) {
    if (const JValue* v = obj.Find(key); v && v->type == JValue::Type::Number) {
        out = v->number;
        return true;
    }
    return false;
}

bool GetVec3(const JValue& obj, const char* key, glm::vec3& out) {
    if (const JValue* v = obj.Find(key); v && v->type == JValue::Type::Array &&
                                         v->items.size() >= 3) {
        out = glm::vec3(float(v->items[0].number),
                        float(v->items[1].number),
                        float(v->items[2].number));
        return true;
    }
    return false;
}

ObjectiveType ParseObjectiveType(const std::string& s) {
    if (s == "TALK_TO_NPC")   return ObjectiveType::TalkToNpc;
    if (s == "DEFEAT_NPC")    return ObjectiveType::DefeatNpc;
    if (s == "COLLECT_ITEM")  return ObjectiveType::CollectItem;
    if (s == "CUSTOM")        return ObjectiveType::Custom;
    return ObjectiveType::GoToLocation; // default / GO_TO_LOCATION
}

const char* MissionStateName(MissionState s) {
    switch (s) {
        case MissionState::Inactive: return "INACTIVE";
        case MissionState::Active:   return "ACTIVE";
        case MissionState::Complete: return "COMPLETE";
        case MissionState::Failed:   return "FAILED";
    }
    return "?";
}

const char* ObjectiveTypeName(ObjectiveType t) {
    switch (t) {
        case ObjectiveType::GoToLocation: return "GO_TO_LOCATION";
        case ObjectiveType::TalkToNpc:    return "TALK_TO_NPC";
        case ObjectiveType::DefeatNpc:    return "DEFEAT_NPC";
        case ObjectiveType::CollectItem:  return "COLLECT_ITEM";
        case ObjectiveType::Custom:       return "CUSTOM";
    }
    return "?";
}

} // namespace

// --- MissionManager -----------------------------------------------------------

uint32_t MissionManager::RegisterMission(const Mission& mission) {
    const uint32_t idx = uint32_t(m_missions.size());
    m_missions.push_back(mission);
    return idx;
}

uint32_t MissionManager::RegisterTriggerZone(const TriggerZone& zone) {
    const uint32_t idx = uint32_t(m_triggers.size());
    m_triggers.push_back(zone);
    return idx;
}

const Mission* MissionManager::GetMission(uint32_t index) const {
    return index < m_missions.size() ? &m_missions[index] : nullptr;
}

const Mission* MissionManager::FindMission(const std::string& id) const {
    for (const Mission& m : m_missions)
        if (m.id == id) return &m;
    return nullptr;
}

const Mission* MissionManager::ActiveMission() const {
    for (const Mission& m : m_missions)
        if (m.state == MissionState::Active) return &m;
    return nullptr;
}

const TriggerZone* MissionManager::GetTrigger(size_t index) const {
    return index < m_triggers.size() ? &m_triggers[index] : nullptr;
}

bool MissionManager::LoadFromJson(
        const char* path,
        const std::unordered_map<std::string, glm::vec3>& refs) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Logger::Warning("Missions: cannot open %s (no missions loaded)", path);
        return false;
    }
    const std::string raw((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
    // Tolerate a UTF-8 BOM (common from PowerShell-written files).
    const std::string text =
        (raw.size() >= 3 && uint8_t(raw[0]) == 0xEF && uint8_t(raw[1]) == 0xBB &&
         uint8_t(raw[2]) == 0xBF)
            ? raw.substr(3) : raw;

    JValue root;
    std::string err;
    if (!JsonParser(text).Parse(root, err)) {
        Logger::Error("Missions: JSON parse failed in %s (%s)", path, err.c_str());
        return false;
    }
    if (root.type != JValue::Type::Object) {
        Logger::Error("Missions: %s root is not an object", path);
        return false;
    }

    // Point-reference resolver: objective "ref": "<name>", "offset": [x,y,z]
    // (absolute arrays also accepted). `offsetKey` lets trigger bounds use
    // min_offset / max_offset under one "ref".
    auto resolvePos = [&](const JValue& v, glm::vec3& out,
                          const char* offsetKey = "offset") -> bool {
        if (v.type == JValue::Type::Array && v.items.size() >= 3) {
            out = glm::vec3(float(v.items[0].number), float(v.items[1].number),
                            float(v.items[2].number));
            return true;
        }
        if (v.type == JValue::Type::Object) {
            if (const JValue* ref = v.Find("ref"); ref && ref->type == JValue::Type::String) {
                auto it = refs.find(ref->str);
                if (it == refs.end()) {
                    Logger::Warning("Missions: unknown point ref '%s'", ref->str.c_str());
                    return false;
                }
                out = it->second;
                glm::vec3 offset(0.0f);
                GetVec3(v, offsetKey, offset);
                out += offset;
                return true;
            }
        }
        return false;
    };

    size_t missionCount = 0, zoneCount = 0;
    if (const JValue* arr = root.Find("missions"); arr && arr->type == JValue::Type::Array) {
        for (const JValue& jm : arr->items) {
            Mission m;
            if (const JValue* s = jm.Find("id"); s) m.id = s->str;
            if (const JValue* s = jm.Find("name"); s) m.name = s->str;
            if (const JValue* s = jm.Find("start_after"); s) m.startAfter = s->str;
            if (const JValue* objs = jm.Find("objectives"); objs &&
                                                          objs->type == JValue::Type::Array) {
                for (const JValue& jo : objs->items) {
                    Objective o;
                    if (const JValue* s = jo.Find("type"); s)
                        o.type = ParseObjectiveType(s->str);
                    if (const JValue* s = jo.Find("description"); s) o.description = s->str;
                    GetVec3(jo, "position", o.targetPosition) ||
                        resolvePos(jo, o.targetPosition);
                    double d = 0.0;
                    if (GetNumber(jo, "radius", d) && d > 0.0)
                        o.completionRadius = float(d);
                    if (const JValue* b = jo.Find("auto_complete_on_reach"); b)
                        o.completeOnReach = (b->type == JValue::Type::Bool && b->boolean);
                    m.objectives.push_back(o);
                }
            }
            if (m.id.empty() || m.objectives.empty()) {
                Logger::Warning("Missions: skipping mission with no id or no objectives");
                continue;
            }
            Logger::Info("Missions: registered '%s' (%s) with %zu objective(s)",
                         m.name.c_str(), m.id.c_str(), m.objectives.size());
            RegisterMission(m);
            ++missionCount;
        }
    }

    if (const JValue* arr = root.Find("trigger_zones"); arr &&
                                                          arr->type == JValue::Type::Array) {
        for (const JValue& jz : arr->items) {
            TriggerZone z;
            if (const JValue* s = jz.Find("id"); s) z.id = s->str;
            if (const JValue* s = jz.Find("mission"); s) z.missionId = s->str;
            const JValue* b = jz.Find("bounds");
            if (!b || b->type != JValue::Type::Object) {
                Logger::Warning("Missions: trigger zone '%s' missing bounds", z.id.c_str());
                continue;
            }
            const bool okMin = GetVec3(*b, "min", z.boundsMin) ||
                               resolvePos(*b, z.boundsMin, "min_offset");
            const bool okMax = GetVec3(*b, "max", z.boundsMax) ||
                               resolvePos(*b, z.boundsMax, "max_offset");
            if (!okMin || !okMax) {
                Logger::Warning("Missions: trigger zone '%s' has invalid bounds",
                                z.id.c_str());
                continue;
            }
            Logger::Info("Missions: trigger '%s' -> mission '%s' "
                         "(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)",
                         z.id.c_str(), z.missionId.c_str(),
                         z.boundsMin.x, z.boundsMin.y, z.boundsMin.z,
                         z.boundsMax.x, z.boundsMax.y, z.boundsMax.z);
            RegisterTriggerZone(z);
            ++zoneCount;
        }
    }

    Logger::Info("Missions: loaded %zu mission(s), %zu trigger zone(s) from %s",
                 missionCount, zoneCount, path);
    return missionCount > 0 || zoneCount > 0;
}

void MissionManager::Update(const glm::vec3& playerPos) {
    UpdateTriggerZones(playerPos);
    UpdateActiveMissions(playerPos);
}

void MissionManager::UpdateTriggerZones(const glm::vec3& playerPos) {
    for (TriggerZone& z : m_triggers) {
        if (z.fired) continue;
        const bool inside =
            playerPos.x >= z.boundsMin.x && playerPos.x <= z.boundsMax.x &&
            playerPos.y >= z.boundsMin.y && playerPos.y <= z.boundsMax.y &&
            playerPos.z >= z.boundsMin.z && playerPos.z <= z.boundsMax.z;
        if (!inside) continue;

        z.fired = true;
        Mission* m = const_cast<Mission*>(FindMission(z.missionId));
        if (!m) {
            Logger::Warning("Missions: trigger '%s' names unknown mission '%s'",
                            z.id.c_str(), z.missionId.c_str());
            continue;
        }
        if (m->state == MissionState::Inactive) {
            m->state = MissionState::Active;
            m->currentObjective = 0;
            Logger::Info("MISSION ACTIVATED: '%s' (%s) — objective 1/%zu: %s",
                         m->name.c_str(), m->id.c_str(), m->objectives.size(),
                         m->objectives.front().description.c_str());
        } else {
            Logger::Info("Missions: trigger '%s' entered but '%s' already %s",
                         z.id.c_str(), m->id.c_str(),
                         MissionStateName(m->state));
        }
    }
}

void MissionManager::UpdateActiveMissions(const glm::vec3& playerPos) {
    for (size_t i = 0; i < m_missions.size(); ++i) {
        Mission& m = m_missions[i];
        if (m.state != MissionState::Active) continue;
        if (m.currentObjective >= m.objectives.size()) continue;

        CheckObjective(uint32_t(i), playerPos);

        // Activation chain: any mission that just completed starts the first
        // Inactive mission waiting on it (Bully-style mission flow). Note the
        // gate is on `next.startAfter == m.id` only — the ROOT mission (e.g.
        // ch1_welcome) has an empty startAfter of its own and must still fire
        // the chain.
        if (m.state == MissionState::Complete) {
            for (Mission& next : m_missions) {
                if (next.state == MissionState::Inactive &&
                    next.startAfter == m.id) {
                    next.state = MissionState::Active;
                    next.currentObjective = 0;
                    Logger::Info("MISSION ACTIVATED: '%s' (%s) — chained after '%s'",
                                 next.name.c_str(), next.id.c_str(), m.id.c_str());
                }
            }
        }
    }
}

bool MissionManager::ReachedObjective(const Objective& o,
                                      const glm::vec3& playerPos) const {
    if (o.complete) return false;
    switch (o.type) {
        case ObjectiveType::GoToLocation:
        case ObjectiveType::TalkToNpc:
        case ObjectiveType::CollectItem:
            // Horizontal distance only: targets are floor points and the
            // player may stand slightly above/below the authored Y (roofs,
            // slopes); vertical separation must not block completion.
            return glm::length(glm::vec2(playerPos.x - o.targetPosition.x,
                                         playerPos.z - o.targetPosition.z)) <=
                   o.completionRadius;
        case ObjectiveType::DefeatNpc:
        case ObjectiveType::Custom:
            // Proximity stands in for the scripted beat when hand-authored
            // demo content set auto_complete_on_reach; otherwise these wait
            // for external completion (NotifyNpcDefeated / CompleteObjective).
            return o.completeOnReach &&
                   glm::length(glm::vec2(playerPos.x - o.targetPosition.x,
                                          playerPos.z - o.targetPosition.z)) <=
                          o.completionRadius;
    }
    return false;
}

void MissionManager::CheckObjective(uint32_t missionIndex, const glm::vec3& playerPos) {
    Mission& m = m_missions[missionIndex];
    if (m.currentObjective >= m.objectives.size()) return;
    Objective& o = m.objectives[m.currentObjective];

    // Proximity types complete on reach; DEFEAT/CUSTOM complete on reach only
    // when hand-authored content set auto_complete_on_reach.
    const bool reachCompletes =
        o.type == ObjectiveType::GoToLocation ||
        o.type == ObjectiveType::TalkToNpc   ||
        o.type == ObjectiveType::CollectItem ||
        o.completeOnReach;
    if (!reachCompletes || !ReachedObjective(o, playerPos)) return;

    o.complete = true;
    Logger::Info("OBJECTIVE COMPLETE: %s (mission '%s', %zu/%zu)",
                 o.description.c_str(), m.id.c_str(),
                 m.currentObjective + 1, m.objectives.size());
    ++m.currentObjective;

    if (m.currentObjective >= m.objectives.size()) {
        CompleteMission(missionIndex);
    } else {
        const Objective& next = m.objectives[m.currentObjective];
        Logger::Info("MISSION ADVANCED: '%s' — objective %zu/%zu: %s",
                     m.name.c_str(), m.currentObjective + 1, m.objectives.size(),
                     next.description.c_str());
    }
}

bool MissionManager::NotifyNpcDefeated(EntityID npc) {
    for (size_t i = 0; i < m_missions.size(); ++i) {
        Mission& m = m_missions[i];
        if (m.state != MissionState::Active ||
            m.currentObjective >= m.objectives.size()) continue;
        Objective& o = m.objectives[m.currentObjective];
        if (o.type != ObjectiveType::DefeatNpc || o.complete) continue;
        if (o.targetEntity.IsValid() && !(o.targetEntity == npc)) continue;
        return CompleteObjective(i, m.currentObjective);
    }
    return false;
}

bool MissionManager::CompleteObjective(uint32_t missionIndex, size_t objectiveIndex) {
    if (missionIndex >= m_missions.size()) return false;
    Mission& m = m_missions[missionIndex];
    if (m.state != MissionState::Active || objectiveIndex >= m.objectives.size())
        return false;
    Objective& o = m.objectives[objectiveIndex];
    if (o.complete) return false;

    o.complete = true;
    Logger::Info("OBJECTIVE COMPLETE: %s (mission '%s', %zu/%zu)",
                 o.description.c_str(), m.id.c_str(), objectiveIndex + 1,
                 m.objectives.size());
    // Only the current objective gates progression.
    if (objectiveIndex == m.currentObjective) {
        ++m.currentObjective;
        if (m.currentObjective >= m.objectives.size()) {
            CompleteMission(missionIndex);
        } else {
            const Objective& next = m.objectives[m.currentObjective];
            Logger::Info("MISSION ADVANCED: '%s' — objective %zu/%zu: %s",
                         m.name.c_str(), m.currentObjective + 1,
                         m.objectives.size(), next.description.c_str());
        }
    }
    return true;
}

void MissionManager::CompleteMission(uint32_t missionIndex) {
    Mission& m = m_missions[missionIndex];
    m.state = MissionState::Complete;
    Logger::Info("MISSION COMPLETE: '%s' (%s) — all %zu objective(s) done",
                 m.name.c_str(), m.id.c_str(), m.objectives.size());
}

// =========================================================================
// Track 36: Save/Load — snapshot and restore mission state
// =========================================================================

std::vector<MissionSave> MissionManager::SnapshotMissions() const {
    std::vector<MissionSave> out;
    out.reserve(m_missions.size());
    for (const Mission& m : m_missions) {
        MissionSave ms;
        ms.id               = m.id;
        ms.state            = static_cast<int>(m.state);
        ms.currentObjective = static_cast<int>(m.currentObjective);
        ms.objectives.reserve(m.objectives.size());
        for (const Objective& o : m.objectives)
            ms.objectives.push_back(o.complete);
        out.push_back(std::move(ms));
    }
    return out;
}

void MissionManager::RestoreMissions(const std::vector<MissionSave>& saved) {
    for (const MissionSave& ms : saved) {
        // Find by ID (save may not align with runtime registration order).
        const Mission* found = FindMission(ms.id);
        if (!found) continue;
        // Find the writable index.
        size_t idx = 0;
        for (; idx < m_missions.size(); ++idx)
            if (m_missions[idx].id == ms.id) break;
        if (idx >= m_missions.size()) continue;

        Mission& m = m_missions[idx];
        m.state            = static_cast<MissionState>(ms.state);
        m.currentObjective = static_cast<size_t>(ms.currentObjective);
        // Apply per-objective completion flags.
        for (size_t j = 0; j < ms.objectives.size() && j < m.objectives.size(); ++j)
            m.objectives[j].complete = ms.objectives[j];
    }
    Logger::Info("RestoreMissions: restored %zu mission(s)", saved.size());
}

std::vector<TriggerSave> MissionManager::SnapshotTriggers() const {
    std::vector<TriggerSave> out;
    out.reserve(m_triggers.size());
    for (const TriggerZone& tz : m_triggers) {
        TriggerSave ts;
        ts.id    = tz.id;
        ts.fired = tz.fired;
        out.push_back(std::move(ts));
    }
    return out;
}

void MissionManager::RestoreTriggers(const std::vector<TriggerSave>& saved) {
    for (const TriggerSave& ts : saved) {
        for (auto& tz : m_triggers)
            if (tz.id == ts.id) { tz.fired = ts.fired; break; }
    }
    Logger::Info("RestoreTriggers: restored %zu trigger(s)", saved.size());
}

} // namespace OpenBully
