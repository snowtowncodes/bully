// Track 29: NPC schedule system — JSON loading (see schedule.h).

#include "schedule.h"

#include "../platform/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace OpenBully {

namespace {

// --- Minimal JSON value parser (same vendored-free approach as mission.cpp;
// the project carries no external JSON dependency) ----------------------------

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
            SkipWs();
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

// --- Field helpers -----------------------------------------------------------

bool GetString(const JValue& obj, const char* key, std::string& out) {
    if (const JValue* v = obj.Find(key); v && v->type == JValue::Type::String) {
        out = v->str;
        return true;
    }
    return false;
}

// "HH:MM" string or raw minutes-since-midnight number -> minutes [0,1440).
bool ParseClock(const JValue& v, int& outMinutes) {
    if (v.type == JValue::Type::Number) {
        const int m = int(v.number);
        if (m >= 0 && m < 24 * 60) { outMinutes = m; return true; }
        return false;
    }
    if (v.type == JValue::Type::String) {
        int h = 0, m = 0;
        if (std::sscanf(v.str.c_str(), "%d:%d", &h, &m) == 2 &&
            h >= 0 && h < 24 && m >= 0 && m < 60) {
            outMinutes = h * 60 + m;
            return true;
        }
    }
    return false;
}

// Window "spawn_point": [x,y,z] absolute, or {"ref": "<name>",
// "offset": [x,y,z]} resolved against `refs` (mission-JSON convention).
bool ParseSpawnPoint(const JValue& v,
                     const std::unordered_map<std::string, glm::vec3>& refs,
                     glm::vec3& out) {
    auto vecFrom = [](const JValue& a, glm::vec3& o) {
        if (a.type != JValue::Type::Array || a.items.size() < 3) return false;
        o = glm::vec3(float(a.items[0].number), float(a.items[1].number),
                      float(a.items[2].number));
        return true;
    };
    if (vecFrom(v, out)) return true;
    if (v.type != JValue::Type::Object) return false;
    const JValue* ref = v.Find("ref");
    if (!ref || ref->type != JValue::Type::String) return false;
    const auto it = refs.find(ref->str);
    if (it == refs.end()) return false;
    out = it->second;
    if (const JValue* off = v.Find("offset")) {
        glm::vec3 o;
        if (vecFrom(*off, o)) out += o;
    }
    return true;
}

ScheduleBehavior ParseBehavior(const std::string& s) {
    if (s == "idle") return ScheduleBehavior::Idle;
    return ScheduleBehavior::Wander;
}

const char* BehaviorName(ScheduleBehavior b) {
    return b == ScheduleBehavior::Idle ? "idle" : "wander";
}

} // namespace

// --- ScheduleManager -----------------------------------------------------------

const NPCSchedule* ScheduleManager::GetSchedule(size_t index) const {
    return index < m_schedules.size() ? &m_schedules[index] : nullptr;
}

const NPCSchedule* ScheduleManager::Find(const std::string& name) const {
    for (const NPCSchedule& s : m_schedules)
        if (s.name == name) return &s;
    return nullptr;
}

bool ScheduleManager::LoadFromJson(
        const char* path,
        const std::unordered_map<std::string, glm::vec3>& refs) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Logger::Warning("Schedules: cannot open %s (no schedules loaded)", path);
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
        Logger::Error("Schedules: JSON parse failed in %s (%s)", path, err.c_str());
        return false;
    }
    const JValue* schedules = root.Find("schedules");
    if (!schedules || schedules->type != JValue::Type::Array) {
        Logger::Error("Schedules: %s has no \"schedules\" array", path);
        return false;
    }

    size_t parsed = 0;
    for (const JValue& sv : schedules->items) {
        NPCSchedule sched;
        if (!GetString(sv, "name", sched.name)) {
            Logger::Warning("Schedules: entry without \"name\" skipped");
            continue;
        }
        const JValue* windows = sv.Find("windows");
        if (!windows || windows->type != JValue::Type::Array) {
            Logger::Warning("Schedules: '%s' has no windows, skipped",
                            sched.name.c_str());
            continue;
        }
        bool ok = true;
        for (const JValue& wv : windows->items) {
            ScheduleWindow w;
            const JValue* startV = wv.Find("start");
            if (!startV || !ParseClock(*startV, w.startMinute)) {
                Logger::Warning("Schedules: '%s' window start invalid, skipped",
                                sched.name.c_str());
                ok = false; break;
            }
            const JValue* endV = wv.Find("end");
            if (!endV || !ParseClock(*endV, w.endMinute)) {
                Logger::Warning("Schedules: '%s' window end invalid, skipped",
                                sched.name.c_str());
                ok = false; break;
            }
            std::string state;
            w.state = (GetString(wv, "state", state) && state == "spawned")
                          ? SpawnState::Spawned : SpawnState::Despawned;
            std::string behavior;
            if (GetString(wv, "behavior", behavior))
                w.behavior = ParseBehavior(behavior);
            if (const JValue* sp = wv.Find("spawn_point"))
                w.hasSpawnPoint = ParseSpawnPoint(*sp, refs, w.spawnPoint);
            sched.windows.push_back(w);
        }
        if (!ok || sched.windows.empty()) continue;
        Logger::Info("Schedules: '%s' registered (%zu windows)",
                     sched.name.c_str(), sched.windows.size());
        for (const ScheduleWindow& w : sched.windows)
            Logger::Info("Schedules:   %02d:%02d-%02d:%02d %s (%s%s)",
                         w.startMinute / 60, w.startMinute % 60,
                         w.endMinute / 60, w.endMinute % 60,
                         w.state == SpawnState::Spawned ? "SPAWNED" : "DESPAWNED",
                         BehaviorName(w.behavior),
                         w.hasSpawnPoint ? ", spawn_point" : "");
        m_schedules.push_back(std::move(sched));
        ++parsed;
    }

    if (parsed == 0) {
        Logger::Warning("Schedules: no usable schedules in %s", path);
        return false;
    }
    return true;
}

} // namespace OpenBully
