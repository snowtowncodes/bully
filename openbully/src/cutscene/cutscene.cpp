#include "cutscene.h"

#include "../platform/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace OpenBully {

// --- Minimal JSON value parser (same model as mission.cpp / dialogue.cpp:
// the project vendors no JSON library; this is a private copy for cutscene
// content) -----------------------------------------------------------------

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
            case 't':  return ParseLit("true", v);
            case 'f':  return ParseLit("false", v);
            case 'n':  return ParseLit("null", v);
            default:   return ParseNumber(v);
        }
    }
    bool ParseLit(const char* lit, JValue& v) {
        const size_t n = std::strlen(lit);
        if (m_text.compare(m_pos, n, lit) != 0) return false;
        m_pos += n;
        if (lit[0] == 't') { v.type = JValue::Type::Bool; v.boolean = true; }
        else if (lit[0] == 'f') { v.type = JValue::Type::Bool; v.boolean = false; }
        else v.type = JValue::Type::Null;
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

bool GetNumber(const JValue& o, const char* key, double& out) {
    if (const JValue* n = o.Find(key); n && n->type == JValue::Type::Number) {
        out = n->number;
        return true;
    }
    return false;
}
bool GetBool(const JValue& o, const char* key) {
    if (const JValue* b = o.Find(key); b && b->type == JValue::Type::Bool)
        return b->boolean;
    return false;
}
bool GetVec3(const JValue& o, const char* key, glm::vec3& out) {
    const JValue* v = o.Find(key);
    if (!v) return false;
    if (v->type == JValue::Type::Array && v->items.size() >= 3) {
        out = glm::vec3(float(v->items[0].number), float(v->items[1].number),
                        float(v->items[2].number));
        return true;
    }
    return false;
}

// Absolute arrays OR {"ref": "<name>", "offset": [x,y,z]} resolved against
// the reference-point map (mission-loader convention).
bool ResolvePos(const JValue& o, const char* key,
                const std::unordered_map<std::string, glm::vec3>& refs,
                glm::vec3& out) {
    const JValue* v = o.Find(key);
    if (!v) return false;
    if (v->type == JValue::Type::Array && v->items.size() >= 3) {
        out = glm::vec3(float(v->items[0].number), float(v->items[1].number),
                        float(v->items[2].number));
        return true;
    }
    if (v->type == JValue::Type::Object) {
        const JValue* ref = v->Find("ref");
        if (!ref || ref->type != JValue::Type::String) return false;
        const auto it = refs.find(ref->str);
        if (it == refs.end()) {
            Logger::Warning("Cutscenes: unknown point ref '%s'", ref->str.c_str());
            return false;
        }
        out = it->second;
        glm::vec3 offset(0.0f);
        if (GetVec3(*v, "offset", offset)) out += offset;
        return true;
    }
    return false;
}

CutsceneActionType ParseActionType(const std::string& s) {
    if (s == "MOVE_TO" || s == "move_to" || s == "MoveTo") return CutsceneActionType::MoveTo;
    if (s == "TELEPORT" || s == "teleport" || s == "Teleport") return CutsceneActionType::Teleport;
    if (s == "FACE" || s == "face" || s == "Face") return CutsceneActionType::Face;
    if (s == "ANIMATE" || s == "animate" || s == "Animate") return CutsceneActionType::Animate;
    return CutsceneActionType::Animate;
}

// Smoothstep easing for the camera and MoveTo tweens (ease-in/out; keeps
// scripted moves from starting and stopping at a dead stop).
float EaseInOut(float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

// --- CutsceneManager -------------------------------------------------------

bool CutsceneManager::LoadFile(const char* path,
                               const std::unordered_map<std::string, glm::vec3>& refs) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Logger::Warning("Cutscenes: cannot open %s (no cutscenes loaded)", path);
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
        Logger::Error("Cutscenes: JSON parse failed in %s (%s)", path, err.c_str());
        return false;
    }
    if (root.type != JValue::Type::Object) {
        Logger::Error("Cutscenes: %s root is not an object", path);
        return false;
    }

    m_cutscenes.clear();
    size_t loaded = 0;
    const JValue* arr = root.Find("cutscenes");
    if (!arr || arr->type != JValue::Type::Array) {
        Logger::Error("Cutscenes: %s has no \"cutscenes\" array", path);
        return false;
    }
    for (const JValue& jc : arr->items) {
        Cutscene cs;
        if (const JValue* s = jc.Find("id"); s) cs.id = s->str;
        double d = 0.0;
        GetNumber(jc, "duration", d);
        cs.duration = float(d);
        if (const JValue* s = jc.Find("auto_mission"); s) cs.autoMission = s->str;
        if (const JValue* s = jc.Find("on_end_mission"); s) cs.onEndMission = s->str;

        // Actors.
        if (const JValue* ja = jc.Find("actors"); ja && ja->type == JValue::Type::Array) {
            for (const JValue& a : ja->items) {
                CutsceneActor actor;
                if (const JValue* s = a.Find("name"); s) actor.name = s->str;
                if (const JValue* s = a.Find("kind"); s) actor.kind = s->str;
                // Only NPC actors bind by proximity; "jimmy" binds the player.
                if (actor.kind != "jimmy" &&
                    !ResolvePos(a, "near", refs, actor.npcNear))
                    Logger::Warning("Cutscenes: npc actor '%s' missing a valid 'near' point",
                                    actor.name.c_str());
                double r = 0.0;
                if (GetNumber(a, "radius", r) && r > 0.0) actor.npcRadius = float(r);
                if (!actor.name.empty()) cs.actors.push_back(actor);
            }
        }

        // Camera keyframes.
        const JValue* cam = jc.Find("camera");
        const JValue* keys = cam ? cam->Find("keyframes") : nullptr;
        if (!keys) keys = jc.Find("keyframes"); // tolerate the flat spelling
        if (keys && keys->type == JValue::Type::Array) {
            for (const JValue& k : keys->items) {
                CutsceneCameraKey key;
                double t = 0.0;
                GetNumber(k, "time", t);
                key.time = std::max(float(t), kMinKeyTime);
                if (!ResolvePos(k, "position", refs, key.position)) {
                    Logger::Warning("Cutscenes: keyframe of '%s' missing position, skipped",
                                    cs.id.c_str());
                    continue;
                }
                if (!ResolvePos(k, "look_at", refs, key.lookAt)) {
                    key.lookAt = key.position + glm::vec3(0.0f, 0.0f, 1.0f);
                }
                key.cut = GetBool(k, "cut");
                if (const JValue* s = k.Find("face_actor"); s) key.faceActor = s->str;
                cs.keys.push_back(key);
            }
        }
        std::stable_sort(cs.keys.begin(), cs.keys.end(),
                         [](const CutsceneCameraKey& a, const CutsceneCameraKey& b) {
                             return a.time < b.time;
                         });

        // Character actions.
        if (const JValue* ja = jc.Find("actions"); ja && ja->type == JValue::Type::Array) {
            for (const JValue& a : ja->items) {
                CutsceneAction action;
                if (const JValue* s = a.Find("actor"); s) action.actor = s->str;
                if (const JValue* s = a.Find("type"); s)
                    action.type = ParseActionType(s->str);
                double t = 0.0;
                GetNumber(a, "time", t);
                action.time = float(t);
                double dur = 0.0;
                GetNumber(a, "duration", dur);
                if (dur > 0.0) action.duration = float(dur);
                if (action.type == CutsceneActionType::Face && action.duration <= 0.0f)
                    action.duration = 0.0f; // instant turn (no tween)
                if (const JValue* s = a.Find("anim"); s) action.anim = s->str;
                // Target point: MoveTo/Teleport use "position"/"target"; Face too.
                if (!ResolvePos(a, "position", refs, action.target) &&
                    !ResolvePos(a, "target", refs, action.target) &&
                    !GetVec3(a, "position", action.target) &&
                    !GetVec3(a, "target", action.target)) {
                    if (action.type != CutsceneActionType::Animate) {
                        Logger::Warning("Cutscenes: action of '%s' missing a target point",
                                        cs.id.c_str());
                    }
                }
                if (!action.actor.empty()) cs.actions.push_back(action);
            }
        }
        std::stable_sort(cs.actions.begin(), cs.actions.end(),
                         [](const CutsceneAction& a, const CutsceneAction& b) {
                             return a.time < b.time;
                         });

        // Subtitle lines.
        if (const JValue* jl = jc.Find("lines"); jl && jl->type == JValue::Type::Array) {
            for (const JValue& l : jl->items) {
                CutsceneLine line;
                double t = 0.0, dur = 0.0;
                GetNumber(l, "time", t);
                GetNumber(l, "duration", dur);
                line.time = float(t);
                if (dur > 0.0) line.duration = float(dur);
                if (const JValue* s = l.Find("speaker"); s) line.speaker = s->str;
                if (const JValue* s = l.Find("text"); s) line.text = s->str;
                if (!line.text.empty()) cs.lines.push_back(line);
            }
        }
        std::stable_sort(cs.lines.begin(), cs.lines.end(),
                         [](const CutsceneLine& a, const CutsceneLine& b) {
                             return a.time < b.time;
                         });

        if (cs.id.empty() || cs.keys.empty()) {
            Logger::Warning("Cutscenes: skipping cutscene with no id or no camera keys");
            continue;
        }
        // Derive a duration from the content when none is authored.
        if (cs.duration <= 0.0f) {
            float tMax = 0.0f;
            for (const CutsceneCameraKey& k : cs.keys)
                tMax = std::max(tMax, k.time);
            for (const CutsceneAction& a : cs.actions)
                tMax = std::max(tMax, a.time + a.duration);
            for (const CutsceneLine& l : cs.lines)
                tMax = std::max(tMax, l.time + l.duration);
            cs.duration = tMax + 0.5f;
        }
        Logger::Info("Cutscenes: registered '%s' (%.2fs, %zu key(s), %zu action(s), "
                     "%zu line(s)%s%s)",
                     cs.id.c_str(), cs.duration, cs.keys.size(),
                     cs.actions.size(), cs.lines.size(),
                     cs.autoMission.empty() ? ""
                                            : (std::string(" auto_mission='") +
                                               cs.autoMission + "'").c_str(),
                     cs.onEndMission.empty() ? ""
                                             : (std::string(" on_end_mission='") +
                                                cs.onEndMission + "'").c_str());
        m_cutscenes.push_back(cs);
        ++loaded;
    }
    Logger::Info("Cutscenes: loaded %zu cutscene(s) from %s", loaded, path);
    return loaded > 0;
}

const Cutscene* CutsceneManager::At(size_t index) const {
    return index < m_cutscenes.size() ? &m_cutscenes[index] : nullptr;
}

const Cutscene* CutsceneManager::Find(const std::string& id) const {
    for (const Cutscene& cs : m_cutscenes)
        if (cs.id == id) return &cs;
    return nullptr;
}

void CutsceneManager::Play(const Cutscene* cutscene,
                           const std::unordered_map<std::string, glm::vec3>& actorStart) {
    if (!cutscene) return;
    m_active = cutscene;
    m_time = 0.0f;
    m_actionFired.assign(cutscene->actions.size(), 0);
    m_actorPos = actorStart;   // MoveTo tweens start from these positions
    m_moveFrom.clear();
}

void CutsceneManager::Stop() {
    if (!m_active) return;
    const std::string id = m_active->id;
    m_active = nullptr;
    Logger::Info("Cutscenes: '%s' stopped at t=%.2f (skipped)", id.c_str(), m_time);
    if (m_onEnd) m_onEnd(true);
}

void CutsceneManager::Update(float dt) {
    if (!m_active) return;
    const Cutscene* cs = m_active;
    const float dur = std::max(0.001f, cs->duration);
    m_time += dt;

    // Fire actions whose time has arrived (once each; late frames still fire).
    for (size_t i = 0; i < cs->actions.size(); ++i) {
        if (m_actionFired[i] || m_time < cs->actions[i].time) continue;
        m_actionFired[i] = 1;
        const CutsceneAction& a = cs->actions[i];
        if (a.type == CutsceneActionType::Teleport) {
            m_actorPos[a.actor] = a.target;
        } else if (a.type == CutsceneActionType::MoveTo) {
            m_moveFrom[a.actor] = m_actorPos.count(a.actor)
                ? m_actorPos[a.actor] : a.target;
        }
        if (m_onAction) m_onAction(a);
    }

    // Advance MoveTo tweens (the runtime position is queried per frame).
    for (const CutsceneAction& a : cs->actions) {
        if (a.type != CutsceneActionType::MoveTo) continue;
        const float span = std::max(0.001f, a.duration);
        const float u = (m_time - a.time) / span;
        if (u <= 0.0f) continue;
        const glm::vec3 from = m_moveFrom.count(a.actor) ? m_moveFrom[a.actor] : a.target;
        m_actorPos[a.actor] =
            u >= 1.0f ? a.target
                      : glm::mix(from, a.target, EaseInOut(u));
    }

    if (m_time >= dur) {
        const std::string id = cs->id;
        m_active = nullptr;
        Logger::Info("Cutscenes: '%s' finished at t=%.2f", id.c_str(), m_time);
        if (m_onEnd) m_onEnd(false);
    }
}

CutsceneManager::CameraPose CutsceneManager::EvalCamera() const {
    CameraPose pose;
    if (!m_active) return pose;
    const std::vector<CutsceneCameraKey>& keys = m_active->keys;
    if (keys.empty()) return pose;

    const CutsceneCameraKey& first = keys.front();
    const CutsceneCameraKey& last = keys.back();
    if (m_time <= first.time) {
        pose.position = first.position;
        pose.lookAt = first.lookAt;
        pose.faceActor = first.faceActor;
        return pose;
    }
    if (m_time >= last.time) {
        pose.position = last.position;
        pose.lookAt = last.lookAt;
        pose.faceActor = last.faceActor;
        return pose;
    }

    // Segment (k0, k1) with k0.time <= m_time < k1.time.
    size_t i = 1;
    while (i < keys.size() && keys[i].time <= m_time) ++i;
    const CutsceneCameraKey& k1 = keys[i];
    const CutsceneCameraKey& k0 = keys[i - 1];

    // Cut keys: hold the previous pose until the cut lands, then pop.
    if (k1.cut) {
        pose.position = k0.position;
        pose.lookAt = k0.lookAt;
        pose.faceActor = k0.faceActor;
        return pose;
    }
    const float span = std::max(0.001f, k1.time - k0.time);
    const float u = EaseInOut((m_time - k0.time) / span);
    pose.position = glm::mix(k0.position, k1.position, u);
    pose.lookAt = glm::mix(k0.lookAt, k1.lookAt, u);
    pose.faceActor = k1.faceActor.empty() ? k0.faceActor : k1.faceActor;
    return pose;
}

const CutsceneLine* CutsceneManager::CurrentLine() const {
    if (!m_active) return nullptr;
    for (const CutsceneLine& l : m_active->lines) {
        if (m_time >= l.time && m_time < l.time + l.duration) return &l;
    }
    return nullptr;
}

bool CutsceneManager::ActorPosNow(const std::string& actor, glm::vec3& outPos) const {
    const auto it = m_actorPos.find(actor);
    if (it == m_actorPos.end()) return false;
    outPos = it->second;
    return true;
}

bool CutsceneManager::ActorMoving(const std::string& actor) const {
    if (!m_active) return false;
    for (const CutsceneAction& a : m_active->actions) {
        if (a.type != CutsceneActionType::MoveTo || a.actor != actor) continue;
        const float span = std::max(0.001f, a.duration);
        const float u = (m_time - a.time) / span;
        if (u >= 0.0f && u < 1.0f) return true;
    }
    return false;
}

} // namespace OpenBully
