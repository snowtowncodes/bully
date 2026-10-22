#pragma once

// Engine: frame update + rendering orchestration (bgfx-backed, Phase 2).

#include "rendering/renderer.h"
#include "rendering/daynight.h"  // Track 35: day/night cycle
#include "physics/collision.h"
#include "rendering/mesh.h"
#include "entities/entity.h"
#include "entities/ai_events.h" // Track 28: NPC reaction events
#include "game/schedule.h" // Track 29: NPC schedules
#include "animation/animation_controller.h"
#include "game/camera.h"
#include "game/input.h"
#include "game/player.h"
#include "game/weapon.h" // Track 26: WeaponType + WeaponData
#include "../combat/combat.h" // Track 23: melee combat core
#include "../mission/mission.h"
#include "../dialogue/dialogue.h" // Track 30: DialogueManager
#include "../vehicles/vehicle.h" // Track 31: VehicleType + VehicleComponent
#include "../ui/ui.h" // Track 20 (UI): HUDState + UIRenderer
#include "../vehicles/vehicle_physics.h" // Track 32: arcade vehicle physics
#include "../vehicles/vehicle_feel.h"    // Track 33: handling feel (FeelState)
#include "../weather/weather.h"            // Track 34: weather system
#include "save_system.h" // Track 36: Save/Load system
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenBully {

class Window;
class AudioManager; // Track 21: audio device + procedural SFX + mixer

// Track 29: global time-of-day (24h clock, hour 0-23 / minute 0-59).
struct GameTime { int hour = 0; int minute = 0; };

// One World.img NIF mesh loaded, uploaded to the GPU and tracked for the
// debug camera fit. Kept alive for the engine's lifetime.
struct WorldMesh {
    std::string name;      // NIF file name (e.g. "in_longdraw06.nif")
    GpuMesh gpu;           // GPU buffers
    // Sub-mesh list snapshot: index ranges + resolved texture handles.
    struct SubRange {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        GpuTexture texture;   // invalid = renderer placeholder
        // Track 17: skinned-shape palette index (-1 = not skinned).
        int shapeIndex = -1;
    };
    std::vector<SubRange> subs;
    float boundsMin[3] = {0, 0, 0};
    float boundsMax[3] = {0, 0, 0};
    uint32_t collisionId = 0; // CollisionWorld object id (Track 13)
};

class Engine {
public:
    Engine();                 // out-of-line: unique_ptr to fwd-declared AudioManager
    ~Engine(); // out-of-line: unique_ptr to forward-declared Renderer

    bool Init(const Window& window);
    void Update();
    void Render();
    void Shutdown();

    // True once Input reports Escape (or the window-close event fired).
    // main.cpp polls this after Update().
    bool QuitRequested() const { return m_quitRequested; }

    // Track 20 (UI): HUD snapshot, refreshed each Update() and consumed by
    // UIRenderer::DrawHUD (health / wanted / clock / money / minimap dots).
    const HUDState& GetHUDState() const { return m_hudState; }

    // Track 36: Save/Load system.
    // F5 -> SaveGame(0): snapshots player/motion/time to saves/slot_0.json.
    // F9 -> LoadGame(0): restores all saved state; NPCs respawn from schedules.
    // Returns true on success, false on failure.
    bool SaveGame(int slot = 0);
    bool LoadGame(int slot = 0);

    // Track 29: global game time, advancing at 60x real time (1 real second
    // = 1 game minute). The HUD clock reads the same accumulator.
    GameTime GetGameTime() const {
        return { (m_clockMinutes / 60) % 24, m_clockMinutes % 60 };
    }

private:
    void LogFrameTiming();
    void RegisterLevelEntities();
    void SpawnTestEntities(const glm::vec3& center);
    void UpdateEntities(float dt);
    void BuildEntityRenderList();
    bool SpawnJimmy(const glm::vec3& position);
    void UpdateJimmy(float dt);
    void UpdateJimmyCamera(float dt);

    // Track 25: dodge / block / stamina state machine. Runs before
    // UpdateJimmy so the movement code can honor the combat state
    // (dodge overrides movement, block roots the character).
    void UpdateJimmyCombat(float dt);
    // Track 25: central incoming-damage gate for the player. Honors dodge
    // i-frames (0 damage) and block (50% reduction + counter-window check);
    // all player damage sources must route through this.
    void DamagePlayer(int amount, const char* source);

    // Track 26: weapon system. Weapon models load from the extracted World.img
    // set (bat/garbagelid/yardstick/slingshot); pickups spawn near Jimmy and
    // are collected with E; 1-5 selects inventory slots; the equipped weapon
    // replaces the unarmed punch (LMB) — melee extends the hit range and uses
    // weapon damage, the slingshot spawns a projectile entity.
    struct WeaponModel {
        std::string name;                            // NIF file name
        WeaponType  type   = WeaponType::Unarmed;
        int         id     = 0;                      // kTable index (authoritative)
        GpuMesh     gpu;
        std::vector<WorldMesh::SubRange> subs;
        float       drawScale = 1.0f;                // normalizes to ~0.9u
    };
    struct Projectile {
        EntityID  id;                                // Transform + Mesh(isBox)
        glm::vec3 velocity{0.0f};
        float     life    = 3.0f;                    // seconds until despawn
        int       damage  = 0;                       // 0 = inert debris
    };
    bool LoadWeaponModels();
    void SpawnWeaponPickups(const glm::vec3& center);
    void UpdateWeapons(float dt);                    // pickups (E) + slot keys
    void WeaponTryAttack(PlayerController* pc);      // LMB with weapon equipped
    void SpawnProjectile(const glm::vec3& pos, const glm::vec3& vel, int dmg);
    void SpawnDebris(const glm::vec3& pos);          // break effect
    void UpdateProjectiles(float dt);
    void RenderWeaponInHand(const glm::mat4& view, const glm::mat4& proj);
    const WeaponModel* EquippedWeaponModel(const PlayerController* pc, int* slotOut = nullptr) const;

    // Track 17: skeletal animation. Loads Anim/C_Player.agr once into a
    // shared rig (skeleton + clips); attaches AnimationComponents to Jimmy
    // and NPCs; per-frame playback update; skinned draws in Render.
    bool   LoadAnimationRig();                       // .agr + rig setup
    uint32_t CreateRig(const struct NifLoadResult& nif);
    // Track 17: clone an existing rig's controller (same skeleton + clips +
    // GPU buffers) so several entities can animate independently.
    uint32_t CloneRig(uint32_t sourceRigIndex);
    void   UpdateAnimations(float dt);               // drive all controllers
    void   UpdateEntityAnimation(EntityID id, bool moving, float dt);

    // Track 23: melee combat. Loads Anim/C_Player.agr once (World.img player
    // group; attack clips picked through the MAINPED.HXD name dictionary) and
    // registers the punch/kick clips on every rig. UpdateMeleeCombat drives
    // attack input, hit-frame sphere checks, damage/knockback/stun and the
    // combo counter; damage popups are drawn as HUD text in Render.
    bool   LoadCombatAnimations();
    void   UpdateMeleeCombat(float dt);
    void   StartMeleeAttack(Melee::AttackType type);
    void   ResolveMeleeHit(Melee::CombatComponent* mc, const Melee::AttackData& atk);
    void   DriveMeleeDemo(float dt);  // OPENBULLY_MELEE_DEMO scripted attacks

    // Track 16: NPC models + spawn + AI update.
    struct NpcModel {
        std::string name;
        GpuMesh gpu;
        std::vector<WorldMesh::SubRange> subs;
        float bodyScale = 1.0f;      // normalize to ~1.8u humanoid height
        // Track 17: skinned rig for this model (0xFFFFFFFF = static mesh).
        // Built lazily on first spawn from `sourcePath` when the .agr clips
        // are loaded; all instances of the model share it.
        std::string sourcePath;
        uint32_t rigIndex = 0xFFFFFFFF;
    };
    size_t LoadNpcModels(const char* dir);
    void   SpawnNpcs(const glm::vec3& center);
    void   UpdateNpcs(float dt);

    // Track 32: arcade vehicle physics. VehiclePhysics owns the wheel
    // raycasts, spring-damper suspension, drive/brake/steer forces, drag and
    // wall bounce; Engine drives it for the mounted vehicle and syncs the
    // result back into VehicleComponent + Transform (replacing the flat
    // speed-integration the mount update started with).
    void     UpdateMountedVehiclePhysics(float dt, VehicleComponent* vc,
                                         Transform* vt);
    void     UpdateVehicleFeel(float dt, VehicleComponent* vc, Transform* vt,
                               float throttle, float brake, float steer); // Track 33
    VehiclePhysics m_vehicle;                    // physics body (mounted vehicle)
    bool     m_vehiclePhysSeeded = false;        // body synced from the entity
    float    m_vehicleAccum = 0.0f;              // fixed-step accumulator (60 Hz)
    glm::vec3 m_vehicleDemoCenter{0.0f};         // DEMO=2 orbit center (track 32)
    FeelState m_vehicleFeel;                     // Track 33: handling/wheelie/crash state

    // Track 28: NPC reaction system. Combat and projectile damage funnel
    // through DamageNpc, which queues AIEvents; UpdateNpcs drains the queue
    // and runs the reaction state machine (fight back / flee / help call /
    // crowd watch / prefect bust). Fights are registered hot-spots that
    // witnesses react to while active.
    struct FightInfo {
        glm::vec3 pos{0.0f};            // last hit location
        EntityID  aggressor{};          // who dealt the last hit here
        double    lastActivity = 0.0;   // engine time of the last hit
    };
    void     QueueAIEvent(const AIEvent& e);
    void     ProcessAIEvents();            // drain m_aiEvents -> state changes
    void     ScanFightWitnesses();         // LOS + range -> WITNESSED_FIGHT
    void     RegisterFight(const glm::vec3& pos, EntityID aggressor);
    void     DamageNpc(EntityID npc, EntityID attacker, int dmg,
                       const glm::vec3& hitPos, float stunTime);
    void     KnockOutNpc(EntityID npc, EntityID attacker);
    void     NpcAttackHit(EntityID attacker, EntityID target);
    bool     HasLineOfSight(const glm::vec3& from, const glm::vec3& to) const;
    void     Busted();                     // stub: prefect defeated the player
    void     RespawnNpc(EntityID id, AIComponent& ai, Transform& t); // KO timer expiry

    // Track 27: NPC combat helpers. StartNpcAttack is the NPC-side twin of
    // StartMeleeAttack (locks the melee state, plays the clip, faces the
    // target); ProgressNpcAttack advances the NPC's own attack (hit window ->
    // ResolveNpcMeleeHit, which routes player damage through DamagePlayer so
    // dodge i-frames and blocking apply).
    void StartNpcAttack(EntityID npc, Melee::AttackType type, float targetYaw);
    void ProgressNpcAttack(EntityID npc, Melee::CombatComponent* mc, float dt);
    void ResolveNpcMeleeHit(Melee::CombatComponent* mc,
                            const Melee::AttackData& atk, EntityID attacker);
    void DriveNpcDemo(float dt);  // OPENBULLY_NPC_DEMO scripted chase/combat

    // Track 20 (UI): debug keys (H damage / J,K wanted) + HUDState refresh.
    void HandleHudDebugKeys();
    void UpdateHUD(float dt);

    // Track 24: grappling system. One active grapple at a time: Jimmy is the
    // attacker, m_grappleTarget the held NPC. TryStartGrapple pairs the two
    // closest characters within range; GrappleAttack applies grapple moves;
    // the target breaks free by mashing (human input edges or simulated NPC
    // presses); the attacker releases with G.
    EntityID FindNearestNpc(const glm::vec3& pos, float maxDist);
    bool     TryStartGrapple(float maxDist = 1.5f);
    void     EndGrapple(bool targetBrokeFree);
    void     GrappleAttack(GrappleMove move);
    void     HandleCombatInput();
    void     UpdateGrapple(float dt);
    void     DriveGrappleDemo(); // OPENBULLY_GRAPPLE_DEMO scripted sequence

    // Track 36: Save/Load helpers.
    // Captures the current runtime state into a SaveData struct.
    SaveData CaptureSaveState();
    // Applies a SaveData to the engine: restores player position/vitals,
    // game time, mission state, and respawns NPCs/weapons/vehicles.
    void ApplySaveState(const SaveData& data);
    // Respawn all dynamic entities (NPCs, weapons, vehicles) after a load.
    void RespawnDynamicEntities();
    // Destroy all dynamic entities (NPCs, projectiles, pickups, vehicles).
    void DestroyDynamicEntities();

    // Track 20: mission system. Loads data/missions/demo_missions.json,
    // registers trigger zones around the demo level, and drives the manager
    // with Jimmy's position every frame. RenderObjectiveMarker draws the
    // active objective's beacon column.
    bool InitMissions();
    void RenderObjectiveMarker();
    void DriveMissionDemo(); // OPENBULLY_MISSION_DEMO scripted objective walk

    // Track 30: dialogue system. Loads data/dialogue/demo_dialogue.json
    // (hand-authored Bully-flavored trees; .lur/XACT sources are deferred),
    // starts conversations with E near an NPC (2.5u, not in combat), drives
    // the typewriter + input (E/Space advance, 1-4 / arrows+Enter choices),
    // freezes the player and the dialogue NPC, and completes TALK_TO_NPC
    // objectives on conversation end.
    bool InitDialogue();
    void UpdateDialogue(float dt);
    void TryStartDialogue();            // E-press path (also the demo hook)
    void OnDialogueEnded();             // fires via DialogueManager::SetOnEnd
    DialogueManager m_dialogue;
    EntityID        m_dialogueNpc{};    // NPC currently in conversation
    int             m_dialogueChoiceSel = 0; // arrow-key selection highlight

    // Track 31: vehicles. Loads 4 drivable models from the extracted World.img
    // set (bike / car / moped / go-kart), spawns one of each around Jimmy,
    // and drives mount/dismount (E near a vehicle, guarded against combat /
    // grapple / dialogue) + the mounted driving update (throttle/steer, world
    // collision, camera follow with wider FOV).
    struct VehicleModel {
        std::string name;                            // NIF file name
        VehicleType type = VehicleType::Bike;
        GpuMesh     gpu;
        std::vector<WorldMesh::SubRange> subs;
        float       drawScale = 1.0f;                // normalizes to ~2.2u length
        float       length    = 2.2f;                // fitted model length (u)
        float       height    = 1.0f;                // fitted model height (u)
        float       cameraDist = 6.0f;                // mounted follow distance
        float       fovDeg    = 68.0f;                // mounted FOV (wider than 60)
    };
    bool LoadVehicleModels();
    void SpawnVehicles(const glm::vec3& center);
    void UpdateVehicles(float dt);                   // mount/dismount + drive
    void TryToggleVehicleMount();                    // E-press path
    void UpdateVehicleCamera(float dt);              // mounted follow camera
    void MountVehicle(EntityID vehicle);
    void DismountVehicle();
    EntityID FindNearestVehicle(const glm::vec3& pos, float maxDist) const;
    std::vector<VehicleModel> m_vehicleModels;
    std::vector<EntityID>     m_vehicleIds;
    EntityID  m_mountedVehicle{};                    // invalid = on foot
    glm::vec3 m_preMountCamPos{0.0f};                // camera restore point
    float     m_preMountPitch = 0.0f;
    float     m_lastFovDeg = 60.0f;                    // projection rebuild guard
    glm::vec3 m_vehicleMountSeedPos{0.0f};             // Track 32: void-guard reset point
    float     m_vehicleVoidFloorY = -1e9f;             // falling below = off-geometry

    // Track 34: weather system. WeatherManager singleton drives state,
    // particles, fog, and audio. m_prevWeather tracks transitions for
    // rain-loop start/stop.
    WeatherType m_prevWeather = WeatherType::CLEAR;

    // Loads up to maxMeshes NIFs from `dir` into GPU meshes. Returns the
    // number loaded. Skips files that fail to parse, logs a summary.
    // `texDir` is the extracted .nft directory ("" = textures disabled).
    size_t LoadWorldGeometry(const char* dir, size_t maxMeshes,
                             const char* texDir);

    // Resolves a texture name ("wall01") to a GPU texture. Looks the name up
    // in the texture-name index (see BuildTextureIndex), loads the owning
    // .nft, and caches the upload. Missing names return an invalid handle
    // (placeholder is drawn instead) and are negative-cached.
    GpuTexture AcquireTexture(const char* texDir, const std::string& name);

    // One-time scan of every .nft in `texDir`: maps each contained texture's
    // source name ("wall01") to the .nft file that holds its pixels. This is
    // the Bully texture-dictionary model: shapes name a texture, and any NFT
    // may hold it (only ~2,450 of 5,720 models have a same-named .nft).
    void BuildTextureIndex(const char* texDir);

    bool     m_initialized = false;
    double   m_time = 0.0;
    uint64_t m_lastPerf = 0;

    // Frame timing stats (rolling, logged once per second)
    uint32_t m_framesAccum = 0;
    double   m_frameMsAccum = 0.0;
    double   m_lastTimingLog = 0.0;

    std::vector<WorldMesh> m_worldMeshes;

    // Track 13: collision world over registered level-geometry AABBs.
    std::unique_ptr<CollisionWorld> m_collisionWorld;
    bool m_debugCollision = false; // OPENBULLY_DEBUG_COLLISION=1
    bool m_debugPhysics   = false; // OPENBULLY_DEBUG_PHYSICS=1 (Jimmy sphere)
    std::string m_groundRaycastInfo; // latest down-raycast result (logged with frame timing)
    GpuMesh m_triangle;     // single buffer pair, drawn via the 2D path
    bool m_triangleValid = false;
    // Track 14: index into m_worldMeshes per entity mesh component.
    std::vector<uint32_t> m_entityToWorldMesh;
    void* m_nativeWindowHandle = nullptr;
    uint64_t m_frameCount = 0;

    // Track 11 texture cache: texture name -> GPU texture (invalid = known
    // missing, placeholder drawn). Kept alive until Shutdown.
    std::unordered_map<std::string, GpuTexture> m_textureCache;
    // texture-dictionary index: source name -> owning .nft file base name.
    std::unordered_map<std::string, std::string> m_textureIndex;
    std::string m_texDir;               // active textures dir ("" = disabled)
    size_t m_textureLoadFails = 0;
    size_t m_texturedSubMeshes = 0;
    size_t m_placeholderSubMeshes = 0;

    // FPS free camera (Track 10). Spawn outside the geometry bounds,
    // looking at the scene center; Update integrates input, Render builds
    // matrices. Track 14: camera collides with entity world AABBs.
    Camera m_camera;
    Input  m_input;
    bool   m_quitRequested = false;

    // Track 14: entity system. Level geometry is registered as static
    // entities (Transform + Mesh + Collision, no physics); test boxes carry
    // the same triple and a physics component. Render iterates entities.
    EntityManager m_entities;
    GpuMesh       m_testBox;            // shared unit-cube GPU geometry
    bool          m_testBoxValid = false;

    // Track 15: player character.
    EntityID      m_jimmyId;            // invalid when OPENBULLY_NO_JIMMY set
    glm::vec3     m_jimmySpawn{0.0f, 0.0f, 0.0f};
    glm::vec3     m_camDesired{0.0f};   // camera follow target (lerped toward)
    bool          m_jimmyActive = false; // 3rd-person follow enabled
    GpuMesh       m_jimmyMesh;          // dedicated player-mesh GPU buffers
    bool          m_jimmyMeshValid = false;
    std::vector<WorldMesh::SubRange> m_jimmySubs;

    // Track 17: skinned character rendering.
    struct AnimRig {
        AnimationController controller;      // skeleton + skin matrices
        GpuMesh             gpu;             // skinned vertex/index buffers
        std::vector<WorldMesh::SubRange> subs;
        float bodyScale = 1.0f;
        int   walkClip = -1;
        int   idleClip = -1;
        bool  grapplePlaying = false;   // Track 24: grapple clip active
        // Track 23: attack clips (registered from C_Player.agr when loaded).
        int   attackClips[Melee::kAttackTypeCount] = {-1, -1, -1, -1};
    };
    std::vector<AnimRig> m_animRigs;
    std::vector<AgrAnim> m_sharedClips;      // decoded .agr clips (shared)
    int           m_walkClipIndex = -1;      // index into m_sharedClips
    int           m_idleClipIndex = -1;
    float         m_walkScore = 1e9f;        // |travel - 1.4m| clip picker
    float         m_walkTravelM = 0.0f;      // walk clip ground travel (m/cycle)
    GpuMesh       m_jimmySkinnedMesh;        // Jimmy's skinned GPU buffers
    bool          m_jimmySkinnedValid = false;
    std::vector<WorldMesh::SubRange> m_jimmySkinnedSubs;
    uint32_t      m_jimmyRigIndex = 0xFFFFFFFF;
    float         m_walkClipSpeed = 1.0f;  // clips/s tuning for walk playback

    // Track 16: NPC model collection (character NIFs from World.img).
    std::vector<NpcModel> m_npcModels;
    uint32_t              m_npcCount = 0;

    // Track 28: reaction state. m_aiEvents is drained once per frame by
    // UpdateNpcs; m_fights tracks recent hit locations for witness scans.
    std::vector<AIEvent>   m_aiEvents;
    std::vector<FightInfo> m_fights;
    EntityID               m_prefectIds[2] = {{}, {}}; // authority NPCs

    // Track 29: NPC schedules. Each scheduled NPC slot pairs one NPC model
    // with one named schedule; UpdateSchedules spawns/despawns its entity as
    // the game clock crosses window boundaries (data/schedules/
    // npc_schedules.json). Spawned slots re-use their cached rig clone so
    // repeated day cycles do not accumulate rigs.
    struct ScheduledNpc {
        int       scheduleIndex = -1;
        uint32_t  modelIndex    = 0;
        uint32_t  rigIndex      = 0xFFFFFFFF; // cached clone (reused on respawn)
        uint32_t  seed          = 0;          // deterministic per-slot variation
        glm::vec3 spawnPoint{0.0f};             // home base (JSON spawn_point)
        EntityID  entity{};                     // invalid while despawned
        int       lastWindowIndex   = -1;       // behavior applied per window
        bool      walkToSpawnLogged = false;    // 10-min transition log flag
    };
    void     UpdateGameTime(float dt);   // 60x advance + HUD clock sync
    void     SyncGameClock();            // m_gameTimeMinutes -> m_clockMinutes
    void     HandleScheduleKeys();       // T: fast-forward 1 hour
    bool     LoadSchedules();            // JSON load + reference points
    void     UpdateSchedules();          // spawn/despawn/behavior per window
    EntityID SpawnScheduledNpc(ScheduledNpc& sn, const ScheduleWindow& w);
    void     DespawnScheduledNpc(ScheduledNpc& sn, const char* reason);
    std::vector<ScheduledNpc> m_scheduledNpcs;
    ScheduleManager           m_schedules;
    double m_gameTimeMinutes = 8.0 * 60.0; // fractional game minutes since midnight

    // Track 24: grapple state. m_grappleTarget is invalid when no grapple is
    // active. The grapple idle clip comes from a "grap*" .agr group in the
    // extracted animation directory (World.img peds) when present; without
    // it both characters hold their current pose (fallback freeze).
    EntityID m_grappleTarget;          // default-constructed = invalid
    int      m_grappleClipIndex = -1;  // index into m_sharedClips (-1 = none)
    bool     m_grappleAnimLoaded = false; // grapple .agr found on disk

    // Track 26: weapon models + live projectiles.
    std::vector<WeaponModel> m_weaponModels;
    std::vector<Projectile>  m_projectiles;

    // Track 23: melee combat state.
    std::vector<AgrAnim> m_combatClips;  // decoded C_Player.agr (all 439 anims)
    bool   m_combatClipsLoaded = false;
    // Damage popup (top-center HUD text; set on a landed melee hit).
    char     m_meleePopup[96] = {};
    float    m_meleePopupTimer = 0.0f;
    uint32_t m_screenWidth  = 0;  // for popup centering (set in Init)
    uint32_t m_screenHeight = 0;
    double   m_meleeDemoLastAtk = -10.0; // DriveMeleeDemo cadence
    int      m_meleeDemoStep    = 0;     // light/light/heavy chain position

    // Track 20: mission system state.
    MissionManager m_missions;         // missions + trigger zones
    GpuMesh        m_beaconMesh;       // unit cube for the objective marker
    bool           m_beaconValid = false;

    // Track 21: audio. SDL2 device + procedural SFX + software mixer;
    // unique_ptr to keep AudioManager out of this header.
    std::unique_ptr<AudioManager> m_audio;
    float m_footstepTimer = 0.0f;             // accumulates while Jimmy walks
    static constexpr float kFootstepInterval = 0.35f; // walk cadence (s)

    // Track 20 (UI): screen-space HUD renderer (bgfx view 10) + state.
    UIRenderer m_ui;             // owns the view-10 program + textures
    HUDState   m_hudState;       // plain-data snapshot for DrawHUD
    int        m_clockMinutes = 8 * 60; // game clock, minutes since midnight

    // Per-entity GPU render record: which GpuMesh + sub-ranges to draw.
    // The WorldMesh list stays the storage for level geometry; entity
    // MeshComponent.subIndex points into it via m_worldMeshes.
    struct EntityRender {
        EntityID id;
        const GpuMesh* gpu;
        std::vector<WorldMesh::SubRange> subs;
        glm::mat4 model;
        uint32_t rigIndex = 0xFFFFFFFF; // Track 17: skinned draw when set
    };
    std::vector<EntityRender> m_entityRender;

    // Track 12: one-shot verification capture (OPENBULLY_SHOT env var),
    // requested after ~1s of frames so the first draws are on screen.
    std::string m_screenshotName;
    bool        m_screenshotPending = false;

    std::unique_ptr<Renderer> m_renderer;
};

} // namespace OpenBully
