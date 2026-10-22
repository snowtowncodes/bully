#pragma once

// Entity-Component System (Phase 4, Track 14).
//
// Entity handles are (index, generation) pairs: the generation increments on
// every destroy so stale handles (created before a destroy+recycle of the
// same slot) fail IsValid() instead of silently touching a new entity.
//
// Storage is a sparse-set: one dense record array with a per-slot generation
// and alive flag, plus dense per-component arrays (Transform / Mesh /
// Collision / Physics) indexed through the record. Component lookup is a
// single array index; iteration over a component is cache-friendly.

#include "../rendering/mesh.h"
#include "../rendering/renderer.h"
#include "../game/player.h"
#include "../animation/animation_controller.h"
#include "npc.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace OpenBully {

// --- Entity handle ----------------------------------------------------------

struct EntityID {
    uint32_t index      = 0xFFFFFFFF; // slot in the manager
    uint32_t generation = 0;          // bumped on destroy (stale-handle guard)

    bool IsValid() const { return index != 0xFFFFFFFF; }
    bool operator==(const EntityID& o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const EntityID& o) const { return !(*this == o); }
};

// --- Components ---------------------------------------------------------------

struct Transform {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};      // Euler XYZ radians (yaw around Y primary)
    glm::vec3 scale{1.0f};

    glm::mat4 Matrix() const;
};

// GPU geometry owned by the engine (level meshes) or the entity itself
// (procedural test boxes). Exactly one of the two is used.
struct MeshComponent {
    bool           isBox     = false;    // true = draw the shared test box
    bool           isJimmy   = false;    // true = draw the dedicated player mesh (Track 15)
    bool           isNpc     = false;    // true = draw an NPC model (Track 16)
    uint32_t       npcModelIndex = 0xFFFFFFFF; // index into Engine::m_npcModels
    uint32_t       worldMeshIndex = 0xFFFFFFFF; // index into Engine::m_worldMeshes
    GpuTexture     texture;              // invalid = renderer placeholder
};

struct CollisionComponent {
    glm::vec3 boundsMin{0.0f};   // AABB in entity local space (centered)
    glm::vec3 boundsMax{0.0f};
    uint32_t  layerMask = 0xFFFFFFFF; // bitwise layer filter for queries
};

struct PhysicsComponent {
    glm::vec3 velocity{0.0f};
    float     gravity  = -9.8f; // units/s^2 (0 disables)
    bool      onGround = false;

    // --- Track 18: character movement tuning (used by Engine::UpdateJimmy) ---
    float     slopeSlideCos = 0.70f; // cos(min slide angle): ground normals
                                     // with n.y < cos(45 deg) start sliding
    float     slopeSlideAccel = 12.0f; // downhill acceleration (u/s^2) on slides
    float     stepHeight = 0.35f;   // max ledge the step-up logic climbs
    float     maxFallSpeed = 30.0f; // terminal velocity clamp (u/s)
    float     fallDamageThreshold = 10.0f; // landing speed that "hurts" (log)
    float     groundNormalY = 1.0f; // surface normal Y of the ground we stand on
    bool      sliding = false;      // on a too-steep slope (gravity slide active)

    // Horizontal movement is stored here by the controller; the vertical
    // component stays in `velocity.y` so gravity/landing stay independent.
    glm::vec3 HorizontalVelocity() const { return { velocity.x, 0.0f, velocity.z }; }
};

// Track 17: skeletal animation state for one skinned entity. The controller
// (skeleton + clips + pose) lives in the engine's shared rig pool; this
// component references a rig and drives playback (walk when moving, idle
// when stopped).
struct AnimationComponent {
    uint32_t rigIndex = 0xFFFFFFFF; // engine rig pool index
    bool     playing = false;
    bool     wasMoving = false;    // last-frame movement flag (state switching)
};

// --- Entity manager -----------------------------------------------------------

class EntityManager {
public:
    EntityManager();

    EntityID Create();
    // Destroys the entity. A stale handle (generation mismatch or already
    // dead) is a no-op. Subsequent queries never see the entity.
    void Destroy(EntityID id);

    bool IsAlive(EntityID id) const;
    size_t EntityCount() const { return m_aliveCount; }

    // -- Components (nullptr / invalid EntityID when absent or handle stale) --
    Transform*          AddTransform(EntityID id, const Transform& t = {});
    Transform*          GetTransform(EntityID id);
    const Transform*    GetTransform(EntityID id) const;

    MeshComponent*      AddMesh(EntityID id, const MeshComponent& m);
    MeshComponent*      GetMesh(EntityID id);
    const MeshComponent* GetMesh(EntityID id) const;

    CollisionComponent* AddCollision(EntityID id, const CollisionComponent& c);
    CollisionComponent* GetCollision(EntityID id);
    const CollisionComponent* GetCollision(EntityID id) const;

    PhysicsComponent*   AddPhysics(EntityID id, const PhysicsComponent& p = {});
    PhysicsComponent*   GetPhysics(EntityID id);

    // Track 17: skeletal animation component (one per skinned entity).
    AnimationComponent* AddAnimation(EntityID id, const AnimationComponent& a = {});
    AnimationComponent* GetAnimation(EntityID id);

    // Track 15: player-controller component (one per player entity).
    PlayerController*   AddPlayer(EntityID id, const PlayerController& p = {});
    PlayerController*   GetPlayer(EntityID id);

    // Track 16: NPC AI component.
    AIComponent*        AddAI(EntityID id, const AIComponent& a = {});
    AIComponent*        GetAI(EntityID id);

    void RemoveMesh(EntityID id);
    void RemoveCollision(EntityID id);

    // Visits every alive entity carrying a Transform. The callback receives
    // (id, transform, record); the record exposes the component index slots
    // (0xFFFFFFFF = absent) and whether each component exists.
    struct EntityRecord {
        uint32_t generation  = 1;      // starts at 1 so a default EntityID never matches
        bool     alive       = false;
        uint32_t transform   = 0;      // index into the dense component arrays
        uint32_t mesh        = 0xFFFFFFFF;
        uint32_t collision   = 0xFFFFFFFF;
        uint32_t physics     = 0xFFFFFFFF;
        uint32_t player      = 0xFFFFFFFF;
        uint32_t ai          = 0xFFFFFFFF;
        uint32_t animation   = 0xFFFFFFFF;

        bool HasMesh() const      { return mesh      != 0xFFFFFFFF; }
        bool HasCollision() const { return collision != 0xFFFFFFFF; }
        bool HasPhysics() const   { return physics   != 0xFFFFFFFF; }
        bool HasAI() const        { return ai        != 0xFFFFFFFF; }
    };

    template <typename Fn>
    void ForEach(Fn&& fn) {
        for (EntityRecord& rec : m_entities) {
            if (!rec.alive) continue;
            fn(EntityID{uint32_t(&rec - m_entities.data()), rec.generation},
               m_transforms[rec.transform], rec);
        }
    }

    // -- Queries -----------------------------------------------------------

    // True when the point lies inside any alive entity's world AABB on a
    // matching layer; fills `outEntity` (may be null).
    bool QueryPoint(const glm::vec3& point, uint32_t layerMask,
                    EntityID* outEntity = nullptr) const;

    // True when the sphere (point, radius) overlaps any entity's world AABB.
    bool QuerySphere(const glm::vec3& point, float radius, uint32_t layerMask,
                     EntityID* outEntity = nullptr) const;

    // Number of entities whose world AABB overlaps the sphere (debug/verify).
    size_t CountNearby(const glm::vec3& point, float radius,
                       uint32_t layerMask) const;

    // World-space AABB of an entity's collision bounds; false when the entity
    // has no collision component.
    bool GetWorldAABB(EntityID id, glm::vec3& outMin, glm::vec3& outMax) const;

    // Visits every alive entity carrying BOTH Transform and Physics.
    // The callback may mutate the transform (integration) and the physics
    // state (velocity, onGround).
    template <typename Fn>
    void ForEachPhysics(Fn&& fn) {
        for (size_t i = 0; i < m_entities.size(); ++i) {
            EntityRecord& rec = m_entities[i];
            if (!rec.alive || rec.physics == 0xFFFFFFFF) continue;
            fn(EntityID{uint32_t(i), rec.generation},
               m_transforms[rec.transform], m_physics[rec.physics]);
        }
    }

    // Track 16: visits every alive entity carrying BOTH Transform and AI.
    // The callback may mutate the transform (movement) and the AI state.
    template <typename Fn>
    void ForEachAI(Fn&& fn) {
        for (size_t i = 0; i < m_entities.size(); ++i) {
            EntityRecord& rec = m_entities[i];
            if (!rec.alive || rec.ai == 0xFFFFFFFF) continue;
            fn(EntityID{uint32_t(i), rec.generation},
               m_transforms[rec.transform], m_ai[rec.ai]);
        }
    }

private:
    // Dense index of a component for a live handle; aborts on stale handle
    // (callers must IsAlive-check first — internal use only).
    bool FindRecord(EntityID id, const EntityRecord** out) const;
    bool FindRecord(EntityID id, EntityRecord** out);

    std::vector<EntityRecord> m_entities;
    size_t m_aliveCount = 0;

    std::vector<Transform>          m_transforms;
    std::vector<MeshComponent>      m_meshes;
    std::vector<CollisionComponent> m_collisions;
    std::vector<PhysicsComponent>   m_physics;
    std::vector<AnimationComponent> m_animations;
    std::vector<PlayerController>   m_players;
    std::vector<AIComponent>        m_ai;
};

} // namespace OpenBully
