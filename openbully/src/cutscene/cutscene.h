#pragma once

// Track 48: cutscene playback system.
//
// A cutscene is a short scripted camera sequence with character actions and
// subtitle dialogue, hand-authored as JSON (data/cutscenes/*.json — the same
// plain-struct + private JSON parser approach as the Track 20 mission system;
// the retail format, .lur bytecode + XACT banks, stays out of scope).
//
// The manager owns the DATA and the TIMELINE MATH only: no SDL, no entities
// (same layering as DialogueManager). The Engine drives playback:
//   - Play() + per-frame Update(dt) advance the clock,
//   - camera keyframes evaluate into a position + look-at each frame (the
//     engine applies them through Camera::SetPosition / SetLookAt, so the
//     gameplay camera simply overrides),
//   - actions fire through SetOnAction once their time arrives (the engine
//     resolves actor names to entities and executes: MoveTo tweens are
//     queried every frame via ActorPosNow / ActorMoving, Teleport/Face/
//     Animate are event-driven),
//   - subtitle lines are read via CurrentLine for the UI renderer,
//   - the timeline end or Engine-side skip fires SetOnEnd exactly once.
//
// JSON shape (positions are absolute arrays or {"ref","offset"} pairs
// resolved against the reference-point map at load time, mirroring
// demo_missions.json):
//   { "cutscenes": [ {
//       "id": "cs_intro", "duration": 13.0,
//       "auto_mission": "ch1_welcome",       // auto-play when mission activates
//       "on_end_mission": "ch1_next",        // activate this mission on end
//       "actors": [ { "name": "jimmy", "kind": "jimmy" },
//                   { "name": "gary", "kind": "npc",
//                     "near": [x,y,z] | {"ref","offset"}, "radius": 45 } ],
//       "camera": { "keyframes": [
//           { "time": 0.0, "position": [..]|{"ref","offset"},
//             "look_at": [..]|{"ref","offset"}, "face_actor": "gary",
//             "cut": false } ] },
//       "actions": [
//           { "time": 1.0, "actor": "jimmy", "type": "MOVE_TO",
//             "position": [..]|{"ref","offset"}, "duration": 3.0 },
//           { "time": 2.0, "actor": "gary", "type": "TELEPORT", "position": .. },
//           { "time": 3.0, "actor": "gary", "type": "FACE", "position": .. },
//           { "time": 4.0, "actor": "gary", "type": "ANIMATE",
//             "anim": "idle", "duration": 2.0 } ],
//       "lines": [ { "time": 1.5, "duration": 3.0,
//                    "speaker": "Gary", "text": "New kid!" } ] } ] }

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenBully {

// One camera keyframe. Between two adjacent non-cut keys the camera eases
// (smoothstep) position and look-at; a key flagged `cut` is reached
// instantly — the previous pose HOLDS until the cut time, then the view
// pops to this key.
struct CutsceneCameraKey {
    float       time = 0.0f;
    glm::vec3   position{0.0f};
    glm::vec3   lookAt{0.0f};
    bool        cut = false;
    std::string faceActor;             // live look-at tracking (resolved by the Engine); "" = use lookAt
};

// Scripted character action. MoveTo is a per-frame queried tween; the rest
// are one-shot events fired through SetOnAction.
enum class CutsceneActionType : uint8_t {
    MoveTo,     // tween the actor toward `target` over `duration` seconds
    Teleport,   // instant position set
    Face,       // rotate the actor's yaw toward `target`
    Animate     // play animation clip `anim` (`duration` = one-shot window)
};

struct CutsceneAction {
    float              time = 0.0f;
    std::string        actor;               // actor name from the actors list
    CutsceneActionType type = CutsceneActionType::Animate;
    glm::vec3          target{0.0f};        // MoveTo/Teleport/Face target point
    float              duration = 1.0f;     // MoveTo tween length / Animate window
    std::string        anim;                // Animate: clip name ("walk"/"idle"/free-form)
};

// One subtitle line (displayed from `time` for `duration` seconds).
struct CutsceneLine {
    float       time = 0.0f;
    float       duration = 3.0f;
    std::string speaker;
    std::string text;
};

// Actor declared by a cutscene. "jimmy" binds the player entity; "npc"
// binds the nearest AI entity to `npcNear` (resolved when playback starts).
struct CutsceneActor {
    std::string name;
    std::string kind = "npc";              // "jimmy" | "npc"
    glm::vec3   npcNear{0.0f};
    float       npcRadius = 45.0f;
};

// A cutscene: camera keyframes + actor actions + subtitle lines.
struct Cutscene {
    std::string id;
    float       duration = 0.0f;
    std::string autoMission;               // auto-play when this mission id activates
    std::string onEndMission;              // activate this mission id on end
    std::vector<CutsceneActor>       actors;
    std::vector<CutsceneCameraKey>   keys;   // sorted by time
    std::vector<CutsceneAction>      actions;
    std::vector<CutsceneLine>        lines;
};

class CutsceneManager {
public:
    // --- loading -------------------------------------------------------------

    // Parses { "cutscenes": [...] } from `path`, resolving {"ref","offset"}
    // positions against `refs` (mission-loader convention). Logs every
    // registered cutscene. Returns false (with an error log) when the file
    // is missing/malformed.
    bool LoadFile(const char* path,
                  const std::unordered_map<std::string, glm::vec3>& refs);

    size_t          Count() const { return m_cutscenes.size(); }
    const Cutscene* At(size_t index) const;
    const Cutscene* Find(const std::string& id) const;

    // --- playback ------------------------------------------------------------

    // Starts `cutscene` at t=0. `actorStart` seeds the per-actor position
    // runtime for MoveTo tweens (only resolved actors need entries).
    void Play(const Cutscene* cutscene,
              const std::unordered_map<std::string, glm::vec3>& actorStart);

    // Stops immediately and fires OnEnd(skipped=true). No-op when inactive.
    void Stop();

    // Advances the timeline: fires due actions, advances MoveTo tweens, and
    // fires OnEnd(false) exactly once when `duration` is reached.
    void Update(float dt);

    bool            IsActive() const { return m_active != nullptr; }
    const Cutscene* Current() const { return m_active; }
    float           Time() const { return m_time; }

    // --- per-frame derived views ---------------------------------------------

    // Camera pose at the current timeline position (keyframe interpolation).
    struct CameraPose {
        glm::vec3 position{0.0f};
        glm::vec3 lookAt{0.0f};
        std::string faceActor;             // live look-at override ("" = none)
    };
    CameraPose EvalCamera() const;

    // The subtitle line active at the current time, or nullptr.
    const CutsceneLine* CurrentLine() const;

    // Current tweened position of `actor` (Play-time start, then tweens /
    // teleports applied). False when the actor is unknown to the runtime.
    bool ActorPosNow(const std::string& actor, glm::vec3& outPos) const;

    // True while a MoveTo action for `actor` is in flight (0 <= u < 1).
    bool ActorMoving(const std::string& actor) const;

    // --- hooks -----------------------------------------------------------------

    // Fired once per action when its time arrives (all action types; the
    // Engine uses it for TELEPORT/FACE/ANIMATE execution and MoveTo housekeeping).
    void SetOnAction(std::function<void(const CutsceneAction&)> cb) {
        m_onAction = std::move(cb);
    }
    // Fired exactly once when playback ends: `skipped` = user skip (Stop),
    // false = reached `duration`.
    void SetOnEnd(std::function<void(bool skipped)> cb) { m_onEnd = std::move(cb); }

    static constexpr float kMinKeyTime = 0.0f; // keyframe/action time floor

private:
    std::vector<Cutscene>    m_cutscenes;
    const Cutscene*          m_active = nullptr;
    float                    m_time = 0.0f;
    std::vector<uint8_t>     m_actionFired;          // one per action of m_active
    std::unordered_map<std::string, glm::vec3> m_actorPos;
    std::unordered_map<std::string, glm::vec3> m_moveFrom;
    std::function<void(const CutsceneAction&)> m_onAction;
    std::function<void(bool skipped)>          m_onEnd;
};

} // namespace OpenBully
