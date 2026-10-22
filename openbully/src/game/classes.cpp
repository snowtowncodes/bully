#include "classes.h"

#include "../platform/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace OpenBully {

// --- Minimal JSON value parser (same pattern as mission.cpp/save_system.cpp —
// the project vendors no JSON library; each data-owned translation unit keeps
// its own copy) ---------------------------------------------------------------

namespace {

struct JValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JValue> items;                           // Array
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
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') return false;
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
            SkipWs(); // whitespace before a key is legal
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

// --- DOM helpers -----------------------------------------------------------

bool GetString(const JValue& obj, const char* key, std::string& out) {
    if (const JValue* v = obj.Find(key); v && v->type == JValue::Type::String) {
        out = v->str;
        return true;
    }
    return false;
}

bool GetNumber(const JValue& obj, const char* key, double& out) {
    if (const JValue* v = obj.Find(key); v && v->type == JValue::Type::Number) {
        out = v->number;
        return true;
    }
    return false;
}

// Position: absolute [x,y,z] or { "ref": "<name>", "offset": [x,y,z] }.
bool ResolvePos(const JValue& v, const std::unordered_map<std::string, glm::vec3>& refs,
                glm::vec3& out) {
    if (v.type == JValue::Type::Array && v.items.size() >= 3) {
        out = glm::vec3(float(v.items[0].number), float(v.items[1].number),
                        float(v.items[2].number));
        return true;
    }
    if (v.type == JValue::Type::Object) {
        if (const JValue* ref = v.Find("ref"); ref && ref->type == JValue::Type::String) {
            const auto it = refs.find(ref->str);
            if (it == refs.end()) {
                Logger::Warning("Classes: unknown point ref '%s'", ref->str.c_str());
                return false;
            }
            out = it->second;
            if (const JValue* off = v.Find("offset");
                    off && off->type == JValue::Type::Array && off->items.size() >= 3) {
                out += glm::vec3(float(off->items[0].number), float(off->items[1].number),
                                 float(off->items[2].number));
            }
            return true;
        }
    }
    return false;
}

// "09:30" -> 570; plain numbers are minutes already.
bool ParseClock(const std::string& s, int& outMinutes) {
    int h = -1, m = -1;
    if (std::sscanf(s.c_str(), "%d:%d", &h, &m) == 2 &&
        h >= 0 && h < 24 && m >= 0 && m < 60) {
        outMinutes = h * 60 + m;
        return true;
    }
    if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos) {
        outMinutes = std::atoi(s.c_str());
        return true;
    }
    return false;
}

constexpr const char* kDayNames[7] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };

} // namespace

// --- Name tables --------------------------------------------------------------

const char* ClassName(ClassType t) {
    switch (t) {
        case ClassType::English:    return "English";
        case ClassType::Math:       return "Math";
        case ClassType::Gym:        return "Gym";
        case ClassType::Art:        return "Art";
        case ClassType::Chemistry:  return "Chemistry";
        case ClassType::Shop:       return "Shop";
        case ClassType::Photography:return "Photography";
        case ClassType::Music:      return "Music";
        case ClassType::Geography:  return "Geography";
        case ClassType::Biology:    return "Biology";
        default:                    return "?";
    }
}

ClassType ParseClassType(const std::string& s) {
    if (s == "MATH")         return ClassType::Math;
    if (s == "GYM")          return ClassType::Gym;
    if (s == "ART")          return ClassType::Art;
    if (s == "CHEMISTRY")    return ClassType::Chemistry;
    if (s == "SHOP")         return ClassType::Shop;
    if (s == "PHOTOGRAPHY")  return ClassType::Photography;
    if (s == "MUSIC")        return ClassType::Music;
    if (s == "GEOGRAPHY")    return ClassType::Geography;
    if (s == "BIOLOGY")      return ClassType::Biology;
    return ClassType::English; // "ENGLISH" + unknown fallback
}

RewardKind ParseRewardKind(const std::string& s) {
    if (s == "max_health") return RewardKind::MaxHealth;
    if (s == "money")      return RewardKind::Money;
    if (s == "weapon")     return RewardKind::Weapon;
    if (s == "ammo")       return RewardKind::Ammo;
    if (s == "ammo_cap")   return RewardKind::AmmoCap;
    if (s == "respect")    return RewardKind::Respect;
    return RewardKind::Money; // unknown -> harmless default
}

const char* RewardKindName(RewardKind k) {
    switch (k) {
        case RewardKind::MaxHealth: return "MAX_HEALTH";
        case RewardKind::Money:     return "MONEY";
        case RewardKind::Weapon:    return "WEAPON";
        case RewardKind::Ammo:      return "AMMO";
        case RewardKind::AmmoCap:   return "AMMO_CAP";
        case RewardKind::Respect:   return "RESPECT";
        default:                    return "?";
    }
}

std::string ClassData::DaysText() const {
    std::string out;
    for (int d = 0; d < 7; ++d) {
        if (!DayActive(d)) continue;
        if (!out.empty()) out += '/';
        out += kDayNames[d];
    }
    return out.empty() ? std::string("NONE") : out;
}

std::string ClassData::TimeText() const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d-%02d:%02d",
                  schedule.startMinute / 60, schedule.startMinute % 60,
                  schedule.endMinute / 60, schedule.endMinute % 60);
    return buf;
}

// --- ClassManager ---------------------------------------------------------------

const ClassData* ClassManager::Get(size_t index) const {
    return index < m_classes.size() ? &m_classes[index] : nullptr;
}

const ClassData* ClassManager::Find(const std::string& id) const {
    for (const ClassData& c : m_classes)
        if (c.id == id) return &c;
    return nullptr;
}

size_t ClassManager::IndexOf(const std::string& id) const {
    for (size_t i = 0; i < m_classes.size(); ++i)
        if (m_classes[i].id == id) return i;
    return SIZE_MAX;
}

int ClassManager::AvailableIndex(const glm::vec3& playerPos, int dayIndex,
                                 int minuteNow, const ClassData** out,
                                 float* distOut) const {
    int best = -1;
    float bestDist = 1e9f;
    for (size_t i = 0; i < m_classes.size(); ++i) {
        const ClassData& c = m_classes[i];
        if (!c.DayActive(dayIndex) || !c.TimeActive(minuteNow)) continue;
        const float d = glm::length(c.position - playerPos);
        if (d <= c.radius && d < bestDist) {
            bestDist = d;
            best = int(i);
        }
    }
    if (out)     *out     = best >= 0 ? &m_classes[size_t(best)] : nullptr;
    if (distOut) *distOut = bestDist;
    return best;
}

// Nearest live-window class regardless of distance (demo automation).
int ClassManager::ScheduledIndexNearest(const glm::vec3& playerPos, int dayIndex,
                                        int minuteNow) const {
    int best = -1;
    float bestDist = 1e9f;
    for (size_t i = 0; i < m_classes.size(); ++i) {
        const ClassData& c = m_classes[i];
        if (!c.DayActive(dayIndex) || !c.TimeActive(minuteNow)) continue;
        const float d = glm::length(c.position - playerPos);
        if (d < bestDist) {
            bestDist = d;
            best = int(i);
        }
    }
    return best;
}

const ClassData* ClassManager::NextUpcoming(int dayIndex, int minuteNow,
                                            int* minutesUntilOut) const {    const ClassData* best = nullptr;
    int bestLead = 7 * 1440 + 1;
    const int idx = dayIndex < 0 ? 0 : dayIndex % 7;
    for (const ClassData& c : m_classes) {
        for (int d = 0; d < 7; ++d) {
            if (!c.DayActive((idx + d) % 7)) continue;
            const int startAbs = d * 1440 + c.schedule.startMinute;
            // Already started at/inside today's window: skip today's start.
            if (d == 0 && c.schedule.startMinute <= minuteNow) continue;
            if (startAbs < bestLead) {
                bestLead = startAbs;
                best = &c;
            }
        }
    }
    if (minutesUntilOut) *minutesUntilOut = best ? bestLead - minuteNow : -1;
    return best;
}

void ClassManager::MarkComplete(size_t index) {
    if (index >= m_completions.size()) return;
    ++m_completions[index];
    Logger::Info("ClassManager: '%s' completed (%d/%d)",
                 m_classes[index].displayName.c_str(),
                 m_completions[index], m_classes[index].sessionsRequired);
}

void ClassManager::MarkFailed(size_t index) {
    if (index >= m_fails.size()) return;
    ++m_fails[index];
    Logger::Info("ClassManager: '%s' failed (%d attempt(s)) — retry available",
                 m_classes[index].displayName.c_str(), m_fails[index]);
}

int ClassManager::TotalCompletions() const {
    int total = 0;
    for (int c : m_completions) total += std::max(0, c);
    return total;
}

bool ClassManager::LoadFromJson(
        const char* path,
        const std::unordered_map<std::string, glm::vec3>& refs) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Logger::Warning("Classes: cannot open %s (no classes loaded)", path);
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
        Logger::Error("Classes: JSON parse failed in %s (%s)", path, err.c_str());
        return false;
    }
    if (root.type != JValue::Type::Object) {
        Logger::Error("Classes: %s root is not an object", path);
        return false;
    }

    const JValue* arr = root.Find("classes");
    if (!arr || arr->type != JValue::Type::Array) {
        Logger::Error("Classes: %s has no 'classes' array", path);
        return false;
    }

    size_t count = 0;
    for (const JValue& jc : arr->items) {
        ClassData c;
        GetString(jc, "id", c.id);
        std::string typeStr = "ENGLISH";
        GetString(jc, "type", typeStr);
        c.type = ParseClassType(typeStr);
        GetString(jc, "name", c.displayName);
        GetString(jc, "building", c.building);
        GetString(jc, "room", c.room);

        // Schedule: "days": ["MON", ...] | "ALL" | "WEEKDAYS" | "WEEKEND";
        // "start"/"end" "HH:MM" or minutes.
        if (const JValue* days = jc.Find("days")) {
            if (days->type == JValue::Type::String) {
                const std::string& s = days->str;
                if      (s == "ALL")       c.schedule.dayMask = 0x7F;
                else if (s == "WEEKDAYS")  c.schedule.dayMask = 0x1F;
                else if (s == "WEEKEND")   c.schedule.dayMask = 0x60;
                else for (int d = 0; d < 7; ++d)
                    if (s == kDayNames[d]) c.schedule.dayMask = 1u << d;
            } else if (days->type == JValue::Type::Array) {
                c.schedule.dayMask = 0;
                for (const JValue& dv : days->items) {
                    if (dv.type != JValue::Type::String) continue;
                    for (int d = 0; d < 7; ++d)
                        if (dv.str == kDayNames[d])
                            c.schedule.dayMask |= (1u << d);
                }
            }
        }
        std::string startStr, endStr;
        if (GetString(jc, "start", startStr)) ParseClock(startStr, c.schedule.startMinute);
        if (GetString(jc, "end",   endStr))   ParseClock(endStr,   c.schedule.endMinute);

        // Position (absolute [x,y,z], nested {position:{ref,offset}}, or
        // top-level "ref"+"offset" — the mission-file convention).
        if (const JValue* pos = jc.Find("position")) ResolvePos(*pos, refs, c.position);
        if (const JValue* refv = jc.Find("ref"); refv && refv->type == JValue::Type::String) {
            const auto it = refs.find(refv->str);
            if (it != refs.end()) {
                c.position = it->second;
                if (const JValue* off = jc.Find("offset");
                    off && off->type == JValue::Type::Array && off->items.size() >= 3) {
                    c.position += glm::vec3(float(off->items[0].number),
                                            float(off->items[1].number),
                                            float(off->items[2].number));
                }
            } else {
                Logger::Warning("Classes: unknown top-level point ref '%s'",
                                refv->str.c_str());
            }
        }

        double radius = 0.0;
        if (GetNumber(jc, "radius", radius) && radius > 0.0) c.radius = float(radius);

        double sessions = 0.0;
        if (GetNumber(jc, "sessions_required", sessions) && sessions > 0.0)
            c.sessionsRequired = int(sessions);

        if (const JValue* objs = jc.Find("objectives"); objs &&
                                                         objs->type == JValue::Type::Array) {
            for (const JValue& jo : objs->items)
                if (jo.type == JValue::Type::String) c.objectives.push_back(jo.str);
        }
        if (const JValue* rws = jc.Find("rewards"); rws && rws->type == JValue::Type::Array) {
            for (const JValue& jr : rws->items) {
                ClassReward r;
                std::string kind = "money";
                GetString(jr, "kind", kind);
                r.kind = ParseRewardKind(kind);
                double amount = 0.0, wid = 0.0;
                GetNumber(jr, "amount", amount);
                r.amount = int(amount);
                if (GetNumber(jr, "weapon_id", wid)) r.weaponId = int(wid);
                c.rewards.push_back(r);
            }
        }

        if (c.id.empty() || c.displayName.empty()) {
            Logger::Warning("Classes: skipping entry with no id/name");
            continue;
        }
        if (c.objectives.empty()) c.objectives = { "Attend the lesson" };

        Logger::Info("Classes: registered '%s' (%s, %s %s %s) with %zu objective(s)",
                     c.displayName.c_str(), c.id.c_str(),
                     c.DaysText().c_str(), c.TimeText().c_str(),
                     c.room.c_str(), c.objectives.size());
        const size_t index = m_classes.size();
        m_classes.push_back(c);
        m_completions.push_back(0);
        m_fails.push_back(0);
        (void)index;
        ++count;
    }

    Logger::Info("Classes: loaded %zu class definition(s) from %s", count, path);
    return count > 0;
}

// --- Track 36 save/load ----------------------------------------------------------

std::vector<ClassSave> ClassManager::Snapshot() const {
    std::vector<ClassSave> out;
    out.reserve(m_classes.size());
    for (size_t i = 0; i < m_classes.size(); ++i) {
        ClassSave s;
        s.id          = m_classes[i].id;
        s.completions = std::max(0, m_completions[i]);
        s.fails       = std::max(0, m_fails[i]);
        out.push_back(std::move(s));
    }
    return out;
}

void ClassManager::Restore(const std::vector<ClassSave>& saved) {
    for (const ClassSave& s : saved) {
        const size_t idx = IndexOf(s.id);
        if (idx == SIZE_MAX) continue;
        m_completions[idx] = std::max(0, s.completions);
        m_fails[idx]       = std::max(0, s.fails);
    }
    Logger::Info("RestoreClasses: restored %zu class progress entr(ies)", saved.size());
}

} // namespace OpenBully
