#include "save_system.h"

#include "../platform/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iomanip>
#include <ctime>

namespace OpenBully {

// =========================================================================
// JSON escape / format helpers
// =========================================================================

namespace {

// Escape a string for JSON output (quotes, backslashes, control chars).
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// Format a float with minimal digits, avoiding "-0" and trailing ".0" noise.
std::string FormatFloat(float v) {
    if (std::abs(v - std::round(v)) < 1e-6f) {
        std::ostringstream os;
        os << std::fixed << std::setprecision(1) << v;
        return os.str();
    }
    std::ostringstream os;
    os << std::setprecision(6) << v;
    return os.str();
}

std::string FormatDouble(double v) {
    std::ostringstream os;
    os << std::setprecision(6) << v;
    return os.str();
}

} // namespace

// =========================================================================
// Serialize: SaveData -> pretty JSON
// =========================================================================

std::string SaveSystem::Serialize(const SaveData& d) {
    std::ostringstream os;
    const char* ind = "  ";
    const char* ind2 = "    ";
    const char* ind3 = "      ";
    const char* ind4 = "        ";

    os << "{\n";
    os << ind << "\"version\": " << d.version << ",\n";
    os << ind << "\"slot\": " << d.slot << ",\n";
    os << ind << "\"saved_at\": \"" << JsonEscape(d.savedAt) << "\",\n";

    // --- Player ---
    os << ind << "\"player\": {\n";
    os << ind2 << "\"position\": [" << FormatFloat(d.playerPos[0]) << ", "
       << FormatFloat(d.playerPos[1]) << ", " << FormatFloat(d.playerPos[2]) << "],\n";
    os << ind2 << "\"rotation\": [" << FormatFloat(d.playerRot[0]) << ", "
       << FormatFloat(d.playerRot[1]) << ", " << FormatFloat(d.playerRot[2]) << "],\n";
    os << ind2 << "\"health\": [" << d.healthCurrent << ", " << d.healthMax << "],\n";
    os << ind2 << "\"stamina\": [" << FormatFloat(d.staminaCurrent) << ", "
       << FormatFloat(d.staminaMax) << "],\n";
    os << ind2 << "\"money\": " << d.money << ",\n";
    os << ind2 << "\"wanted\": " << d.wanted << ",\n";
    os << ind2 << "\"facing_yaw\": " << FormatFloat(d.facingYaw) << ",\n";
    os << ind2 << "\"facing_valid\": " << (d.facingValid ? "true" : "false") << ",\n";
    os << ind2 << "\"equipped_slot\": " << d.equippedSlot << ",\n";
    os << ind2 << "\"slots\": [\n";
    for (int i = 0; i < 5; ++i) {
        const SlotSave& s = d.slots[i];
        os << ind3 << "{\"type\":" << s.type << ",\"weapon_id\":" << s.weaponId
           << ",\"durability\":" << s.durability << ",\"ammo\":" << s.ammo << "}"
           << (i < 4 ? "," : "") << "\n";
    }
    os << ind2 << "]\n";
    os << ind << "},\n";

    // --- Mission ---
    os << ind << "\"mission\": {\n";
    os << ind2 << "\"missions\": [\n";
    for (size_t i = 0; i < d.missions.size(); ++i) {
        const MissionSave& m = d.missions[i];
        os << ind3 << "{\"id\":\"" << JsonEscape(m.id) << "\""
           << ",\"state\":" << m.state
           << ",\"current_objective\":" << m.currentObjective
           << ",\"objectives\":[";
        for (size_t j = 0; j < m.objectives.size(); ++j)
            os << (j > 0 ? "," : "") << (m.objectives[j] ? "true" : "false");
        os << "]}";
        os << (i + 1 < d.missions.size() ? "," : "") << "\n";
    }
    if (d.missions.empty()) os << ind3;
    os << ind2 << "],\n";
    os << ind2 << "\"triggers\": [\n";
    for (size_t i = 0; i < d.triggers.size(); ++i) {
        const TriggerSave& t = d.triggers[i];
        os << ind3 << "{\"id\":\"" << JsonEscape(t.id) << "\""
           << ",\"fired\":" << (t.fired ? "true" : "false") << "}";
        os << (i + 1 < d.triggers.size() ? "," : "") << "\n";
    }
    if (d.triggers.empty()) os << ind3;
    os << ind2 << "]\n";
    os << ind << "},\n";

    // --- Class (Track 41) ---
    os << ind << "\"class\": {\n";
    os << ind2 << "\"respect\": " << d.classRespect << ",\n";
    os << ind2 << "\"slingshot_cap_bonus\": " << d.slingshotCapBonus << ",\n";
    os << ind2 << "\"classes\": [\n";
    for (size_t i = 0; i < d.classes.size(); ++i) {
        const ClassSave& c = d.classes[i];
        os << ind3 << "{\"id\":\"" << JsonEscape(c.id) << "\""
           << ",\"completions\":" << c.completions
           << ",\"fails\":" << c.fails << "}";
        os << (i + 1 < d.classes.size() ? "," : "") << "\n";
    }
    if (d.classes.empty()) os << ind3;
    os << ind2 << "]\n";
    os << ind << "},\n";

    // --- Reputation (Track 42): per-faction respect + mission tally ---------
    os << ind << "\"reputation\": {\n";
    os << ind2 << "\"respect\": [";
    for (int i = 0; i < ReputationSave::kCount; ++i)
        os << (i > 0 ? ", " : "") << FormatDouble(d.reputation.respect[i]);
    os << "],\n";
    os << ind2 << "\"completed_missions\": " << d.reputation.completedMissions << "\n";
    os << ind << "},\n";

    // --- World ---
    os << ind << "\"world\": {\n";
    os << ind2 << "\"game_time_minutes\": " << FormatDouble(d.gameTimeMinutes) << ",\n";
    os << ind2 << "\"game_day\": " << d.gameDay << ",\n";
    os << ind2 << "\"weather\": " << d.weather << "\n";
    os << ind << "},\n";

    // --- Arcade (Track 45): high scores, plays, wins, achievement ---
    os << ind << "\"arcade\": {\n";
    os << ind2 << "\"high_scores\": [" << d.arcadeHigh[0] << ", "
       << d.arcadeHigh[1] << "],\n";
    os << ind2 << "\"plays\": " << d.arcadePlays << ",\n";
    os << ind2 << "\"wins\": " << d.arcadeWins << ",\n";
    os << ind2 << "\"achievement\": " << d.arcadeAchievement << "\n";
    os << ind << "}\n";
    os << "}\n";

    return os.str();
}

// =========================================================================
// Minimal JSON parser (same pattern as mission.cpp — no external deps)
// =========================================================================

namespace {

struct JValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JValue> items;
    std::vector<std::pair<std::string, JValue>> members;

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
    bool ParseValue(JValue& v) {
        SkipWs();
        if (m_pos >= m_text.size()) return false;
        const char c = m_text[m_pos];
        if (c == '{') return ParseObject(v);
        if (c == '[') return ParseArray(v);
        if (c == '"') return ParseString(v);
        if (c == 't' || c == 'f') return ParseBool(v);
        if (c == 'n') return ParseNull(v);
        return ParseNumber(v);
    }
    bool ParseObject(JValue& v) {
        v.type = JValue::Type::Object;
        if (!Consume('{')) return false;
        SkipWs();
        if (Peek('}')) { ++m_pos; return true; }
        while (true) {
            SkipWs();
            JValue key;
            if (!ParseString(key)) return false;
            if (!Consume(':')) return false;
            JValue val;
            if (!ParseValue(val)) return false;
            v.members.emplace_back(std::move(key.str), std::move(val));
            SkipWs();
            if (Consume(',')) continue;
            if (Consume('}')) return true;
            return false;
        }
    }
    bool ParseArray(JValue& v) {
        v.type = JValue::Type::Array;
        if (!Consume('[')) return false;
        SkipWs();
        if (Peek(']')) { ++m_pos; return true; }
        while (true) {
            JValue val;
            if (!ParseValue(val)) return false;
            v.items.push_back(std::move(val));
            SkipWs();
            if (Consume(',')) continue;
            if (Consume(']')) return true;
            return false;
        }
    }
    bool ParseString(JValue& v) {
        SkipWs();
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') return false;
        ++m_pos;
        v.type = JValue::Type::String;
        v.str.clear();
        while (m_pos < m_text.size() && m_text[m_pos] != '"') {
            char c = m_text[m_pos++];
            if (c == '\\' && m_pos < m_text.size()) {
                char e = m_text[m_pos++];
                switch (e) {
                    case '"': v.str += '"'; break;
                    case '\\': v.str += '\\'; break;
                    case '/': v.str += '/'; break;
                    case 'n': v.str += '\n'; break;
                    case 'r': v.str += '\r'; break;
                    case 't': v.str += '\t'; break;
                    case 'u': // skip 4 hex digits
                        for (int i = 0; i < 4 && m_pos < m_text.size(); ++i) ++m_pos;
                        v.str += '?';
                        break;
                    default: v.str += e; break;
                }
            } else {
                if (c == '\n') ++m_line;
                v.str += c;
            }
        }
        if (m_pos >= m_text.size()) return false;
        ++m_pos; // closing quote
        return true;
    }
    bool ParseNumber(JValue& v) {
        SkipWs();
        size_t start = m_pos;
        if (m_pos < m_text.size() && (m_text[m_pos] == '-' || m_text[m_pos] == '+')) ++m_pos;
        while (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) ++m_pos;
        if (m_pos < m_text.size() && m_text[m_pos] == '.') {
            ++m_pos;
            while (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) ++m_pos;
        }
        if (m_pos < m_text.size() && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_text.size() && (m_text[m_pos] == '-' || m_text[m_pos] == '+')) ++m_pos;
            while (m_pos < m_text.size() && std::isdigit(static_cast<unsigned char>(m_text[m_pos]))) ++m_pos;
        }
        if (start == m_pos) return false;
        v.type = JValue::Type::Number;
        v.number = std::atof(m_text.c_str() + start);
        return true;
    }
    bool ParseBool(JValue& v) {
        if (m_pos + 4 <= m_text.size() && m_text.compare(m_pos, 4, "true") == 0) {
            v.type = JValue::Type::Bool; v.boolean = true;  m_pos += 4; return true;
        }
        if (m_pos + 5 <= m_text.size() && m_text.compare(m_pos, 5, "false") == 0) {
            v.type = JValue::Type::Bool; v.boolean = false; m_pos += 5; return true;
        }
        return false;
    }
    bool ParseNull(JValue& v) {
        if (m_pos + 4 <= m_text.size() && m_text.compare(m_pos, 4, "null") == 0) {
            v.type = JValue::Type::Null; m_pos += 4; return true;
        }
        return false;
    }
    bool Peek(char c) {
        SkipWs();
        return m_pos < m_text.size() && m_text[m_pos] == c;
    }
};

// Helper: read a float array [x, y, z] from a JValue array.
void ReadVec3(const JValue* arr, float out[3]) {
    if (arr && arr->type == JValue::Type::Array && arr->items.size() >= 3) {
        out[0] = static_cast<float>(arr->items[0].number);
        out[1] = static_cast<float>(arr->items[1].number);
        out[2] = static_cast<float>(arr->items[2].number);
    }
}

} // namespace

// =========================================================================
// Deserialize: JSON -> SaveData
// =========================================================================

bool SaveSystem::Deserialize(const std::string& json, SaveData& out) {
    JsonParser parser(json);
    JValue root;
    std::string err;
    if (!parser.Parse(root, err)) {
        Logger::Error("SaveSystem::Deserialize parse error: %s", err.c_str());
        return false;
    }
    if (root.type != JValue::Type::Object) return false;

    // version, slot
    if (const JValue* v = root.Find("version")) out.version = static_cast<int>(v->number);
    if (const JValue* v = root.Find("slot"))    out.slot    = static_cast<int>(v->number);
    if (const JValue* v = root.Find("saved_at")) out.savedAt = v->str;

    // --- Player ---
    if (const JValue* p = root.Find("player")) {
        if (p->type == JValue::Type::Object) {
            if (const JValue* v = p->Find("position"))    ReadVec3(v, out.playerPos);
            if (const JValue* v = p->Find("rotation"))    ReadVec3(v, out.playerRot);
            if (const JValue* v = p->Find("health"); v && v->type == JValue::Type::Array && v->items.size() >= 2) {
                out.healthCurrent = static_cast<int>(v->items[0].number);
                out.healthMax     = static_cast<int>(v->items[1].number);
            }
            if (const JValue* v = p->Find("stamina"); v && v->type == JValue::Type::Array && v->items.size() >= 2) {
                out.staminaCurrent = static_cast<float>(v->items[0].number);
                out.staminaMax     = static_cast<float>(v->items[1].number);
            }
            if (const JValue* v = p->Find("money"))       out.money       = static_cast<int>(v->number);
            if (const JValue* v = p->Find("wanted"))      out.wanted      = static_cast<int>(v->number);
            if (const JValue* v = p->Find("facing_yaw"))  out.facingYaw   = static_cast<float>(v->number);
            if (const JValue* v = p->Find("facing_valid")) out.facingValid = v->boolean;
            if (const JValue* v = p->Find("equipped_slot")) out.equippedSlot = static_cast<int>(v->number);

            if (const JValue* slots = p->Find("slots"); slots && slots->type == JValue::Type::Array) {
                int n = static_cast<int>(slots->items.size());
                for (int i = 0; i < 5; ++i) {
                    out.slots[i] = SlotSave{};
                    if (i >= n) break;
                    const JValue& s = slots->items[i];
                    if (const JValue* sv = s.Find("type"))       out.slots[i].type       = static_cast<int>(sv->number);
                    if (const JValue* sv = s.Find("weapon_id"))  out.slots[i].weaponId   = static_cast<int>(sv->number);
                    if (const JValue* sv = s.Find("durability")) out.slots[i].durability = static_cast<int>(sv->number);
                    if (const JValue* sv = s.Find("ammo"))       out.slots[i].ammo       = static_cast<int>(sv->number);
                }
            }
        }
    }

    // --- Mission ---
    if (const JValue* m = root.Find("mission")) {
        if (m->type == JValue::Type::Object) {
            if (const JValue* ms = m->Find("missions"); ms && ms->type == JValue::Type::Array) {
                out.missions.clear();
                for (const JValue& item : ms->items) {
                    MissionSave ms_save;
                    if (const JValue* v = item.Find("id"))               ms_save.id = v->str;
                    if (const JValue* v = item.Find("state"))            ms_save.state = static_cast<int>(v->number);
                    if (const JValue* v = item.Find("current_objective")) ms_save.currentObjective = static_cast<int>(v->number);
                    if (const JValue* v = item.Find("objectives"); v && v->type == JValue::Type::Array) {
                        for (const JValue& o : v->items)
                            ms_save.objectives.push_back(o.boolean);
                    }
                    out.missions.push_back(std::move(ms_save));
                }
            }
            if (const JValue* ts = m->Find("triggers"); ts && ts->type == JValue::Type::Array) {
                out.triggers.clear();
                for (const JValue& item : ts->items) {
                    TriggerSave ts_save;
                    if (const JValue* v = item.Find("id"))    ts_save.id = v->str;
                    if (const JValue* v = item.Find("fired")) ts_save.fired = v->boolean;
                    out.triggers.push_back(std::move(ts_save));
                }
            }
        }
    }

    // --- Class (Track 41) ---
    if (const JValue* cl = root.Find("class")) {
        if (cl->type == JValue::Type::Object) {
            if (const JValue* v = cl->Find("respect")) out.classRespect = static_cast<int>(v->number);
            if (const JValue* v = cl->Find("slingshot_cap_bonus")) out.slingshotCapBonus = static_cast<int>(v->number);
            if (const JValue* arr = cl->Find("classes"); arr && arr->type == JValue::Type::Array) {
                out.classes.clear();
                for (const JValue& item : arr->items) {
                    ClassSave c;
                    if (const JValue* v = item.Find("id"))         c.id = v->str;
                    if (const JValue* v = item.Find("completions")) c.completions = static_cast<int>(v->number);
                    if (const JValue* v = item.Find("fails"))       c.fails = static_cast<int>(v->number);
                    out.classes.push_back(std::move(c));
                }
            }
        }
    }

    // --- Reputation (Track 42) ---------------------------------------------
    if (const JValue* rp = root.Find("reputation")) {
        if (rp->type == JValue::Type::Object) {
            if (const JValue* arr = rp->Find("respect"); arr &&
                arr->type == JValue::Type::Array) {
                for (int i = 0; i < ReputationSave::kCount &&
                                i < static_cast<int>(arr->items.size()); ++i)
                    out.reputation.respect[i] =
                        static_cast<float>(arr->items[i].number);
            }
            if (const JValue* v = rp->Find("completed_missions"))
                out.reputation.completedMissions = static_cast<int>(v->number);
        }
    }

    // --- World ---
    if (const JValue* w = root.Find("world")) {
        if (w->type == JValue::Type::Object) {
            if (const JValue* v = w->Find("game_time_minutes")) out.gameTimeMinutes = v->number;
            if (const JValue* v = w->Find("game_day"))          out.gameDay = static_cast<int>(v->number);
            if (const JValue* v = w->Find("weather"))           out.weather = static_cast<int>(v->number);
        }
    }

    // --- Arcade (Track 45) ---
    if (const JValue* a = root.Find("arcade")) {
        if (a->type == JValue::Type::Object) {
            if (const JValue* hs = a->Find("high_scores")) {
                if (hs->type == JValue::Type::Array && hs->items.size() >= 2) {
                    out.arcadeHigh[0] = static_cast<int>(hs->items[0].number);
                    out.arcadeHigh[1] = static_cast<int>(hs->items[1].number);
                }
            }
            if (const JValue* v = a->Find("plays"))       out.arcadePlays       = static_cast<int>(v->number);
            if (const JValue* v = a->Find("wins"))        out.arcadeWins        = static_cast<int>(v->number);
            if (const JValue* v = a->Find("achievement")) out.arcadeAchievement = static_cast<int>(v->number);
        }
    }

    return true;
}

// =========================================================================
// File I/O
// =========================================================================

std::string SaveSystem::SlotPath(const std::string& baseDir, int slot) {
    std::string dir = baseDir;
    // Ensure trailing slash on Windows
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\')
        dir += '\\';
    return dir + "saves\\slot_" + std::to_string(slot) + ".json";
}

bool SaveSystem::Exists(const std::string& baseDir, int slot) {
    const std::string path = SlotPath(baseDir, slot);
    std::ifstream f(path);
    return f.good();
}

bool SaveSystem::WriteToFile(const std::string& path, const SaveData& data) {
    // Create parent directory
    namespace fs = std::filesystem;
    fs::path p(path);
    try {
        fs::create_directories(p.parent_path());
    } catch (const fs::filesystem_error& e) {
        Logger::Error("SaveSystem: failed to create directory '%s': %s",
                      p.parent_path().string().c_str(), e.what());
        return false;
    }

    const std::string json = Serialize(data);
    std::ofstream f(path);
    if (!f.is_open()) {
        Logger::Error("SaveSystem: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f << json;
    f.close();
    if (!f.good()) {
        Logger::Error("SaveSystem: write failed for '%s'", path.c_str());
        return false;
    }
    Logger::Info("SaveSystem: wrote %zu bytes to %s", json.size(), path.c_str());
    return true;
}

bool SaveSystem::ReadFromFile(const std::string& path, SaveData& out) {
    std::ifstream f(path);
    if (!f.is_open()) {
        Logger::Error("SaveSystem: cannot open '%s' for reading (file may not exist)", path.c_str());
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    f.close();
    return Deserialize(ss.str(), out);
}

} // namespace OpenBully
