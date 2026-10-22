#include "entity.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace OpenBully {

glm::mat4 Transform::Matrix() const {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::rotate(m, rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::rotate(m, rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::scale(m, scale);
}

EntityManager::EntityManager() {
    // Slot 0 reserved: default-constructed (invalid) EntityID{0,0} must never
    // alias a real entity (records start generation at 1, alive=false).
    m_entities.emplace_back();
}

EntityID EntityManager::Create() {
    // Reuse a dead slot when one exists (FIFO scan; entity counts are small).
    for (size_t i = 0; i < m_entities.size(); ++i) {
        EntityRecord& rec = m_entities[i];
        if (!rec.alive) {
            rec.alive = true;
            rec.transform = uint32_t(m_transforms.size());
            rec.mesh      = 0xFFFFFFFF;
            rec.collision = 0xFFFFFFFF;
            rec.physics   = 0xFFFFFFFF;
            rec.player    = 0xFFFFFFFF;
            m_transforms.emplace_back();
            ++m_aliveCount;
            return EntityID{uint32_t(i), rec.generation};
        }
    }
    EntityRecord rec;
    rec.alive = true;
    rec.transform = uint32_t(m_transforms.size());
    rec.mesh      = 0xFFFFFFFF;
    rec.collision = 0xFFFFFFFF;
    rec.physics   = 0xFFFFFFFF;
    rec.player    = 0xFFFFFFFF;
    m_transforms.emplace_back();
    m_entities.push_back(rec);
    ++m_aliveCount;
    return EntityID{uint32_t(m_entities.size() - 1), rec.generation};
}

void EntityManager::Destroy(EntityID id) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec)) return; // stale or dead handle: no-op
    rec->alive = false;
    rec->generation++;                 // invalidates every outstanding handle
    --m_aliveCount;
    // Dead slots keep their transform index; Create() overwrites the fields.
}

bool EntityManager::IsAlive(EntityID id) const {
    const EntityRecord* rec = nullptr;
    return FindRecord(id, &rec);
}

bool EntityManager::FindRecord(EntityID id, const EntityRecord** out) const {
    if (!id.IsValid() || id.index >= m_entities.size()) return false;
    const EntityRecord& rec = m_entities[id.index];
    if (!rec.alive || rec.generation != id.generation) return false;
    *out = &rec;
    return true;
}

bool EntityManager::FindRecord(EntityID id, EntityRecord** out) {
    const EntityRecord* crc = nullptr;
    if (!static_cast<const EntityManager*>(this)->FindRecord(id, &crc)) return false;
    *out = const_cast<EntityRecord*>(crc);
    return true;
}

// -- Component add/get (per-type boilerplate around the shared pattern) ------

Transform* EntityManager::AddTransform(EntityID id, const Transform& t) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec)) return nullptr;
    m_transforms[rec->transform] = t;
    return &m_transforms[rec->transform];
}
Transform* EntityManager::GetTransform(EntityID id) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec)) return nullptr;
    return &m_transforms[rec->transform];
}
const Transform* EntityManager::GetTransform(EntityID id) const {
    return const_cast<EntityManager*>(this)->GetTransform(id);
}

MeshComponent* EntityManager::AddMesh(EntityID id, const MeshComponent& m) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec)) return nullptr;
    if (rec->mesh == 0xFFFFFFFF) { rec->mesh = uint32_t(m_meshes.size()); m_meshes.emplace_back(); }
    m_meshes[rec->mesh] = m;
    return &m_meshes[rec->mesh];
}
MeshComponent* EntityManager::GetMesh(EntityID id) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec) || rec->mesh == 0xFFFFFFFF) return nullptr;
    return &m_meshes[rec->mesh];
}
const MeshComponent* EntityManager::GetMesh(EntityID id) const {
    return const_cast<EntityManager*>(this)->GetMesh(id);
}

void EntityManager::RemoveMesh(EntityID id) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec)) return;
    rec->mesh = 0xFFFFFFFF; // dense-array leak is acceptable (tiny counts)
}

CollisionComponent* EntityManager::AddCollision(EntityID id, const CollisionComponent& c) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec)) return nullptr;
    if (rec->collision == 0xFFFFFFFF) {
        rec->collision = uint32_t(m_collisions.size());
        m_collisions.emplace_back();
    }
    m_collisions[rec->collision] = c;
    return &m_collisions[rec->collision];
}
CollisionComponent* EntityManager::GetCollision(EntityID id) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec) || rec->collision == 0xFFFFFFFF) return nullptr;
    return &m_collisions[rec->collision];
}
const CollisionComponent* EntityManager::GetCollision(EntityID id) const {
    return const_cast<EntityManager*>(this)->GetCollision(id);
}

void EntityManager::RemoveCollision(EntityID id) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec)) return;
    rec->collision = 0xFFFFFFFF;
}

PhysicsComponent* EntityManager::AddPhysics(EntityID id, const PhysicsComponent& p) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec)) return nullptr;
    if (rec->physics == 0xFFFFFFFF) {
        rec->physics = uint32_t(m_physics.size());
        m_physics.emplace_back();
    }
    m_physics[rec->physics] = p;
    return &m_physics[rec->physics];
}
PhysicsComponent* EntityManager::GetPhysics(EntityID id) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec) || rec->physics == 0xFFFFFFFF) return nullptr;
    return &m_physics[rec->physics];
}

// -- PlayerController (Track 15) ----------------------------------------------

PlayerController* EntityManager::AddPlayer(EntityID id, const PlayerController& p) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec)) return nullptr;
    if (rec->player == 0xFFFFFFFF) {
        rec->player = uint32_t(m_players.size());
        m_players.emplace_back();
    }
    m_players[rec->player] = p;
    return &m_players[rec->player];
}

PlayerController* EntityManager::GetPlayer(EntityID id) {
    EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec) || rec->player == 0xFFFFFFFF) return nullptr;
    return &m_players[rec->player];
}

// -- Spatial queries ----------------------------------------------------------

bool EntityManager::GetWorldAABB(EntityID id, glm::vec3& outMin, glm::vec3& outMax) const {
    const EntityRecord* rec = nullptr;
    if (!FindRecord(id, &rec) || rec->collision == 0xFFFFFFFF) return false;
    const Transform& t = m_transforms[rec->transform];
    const CollisionComponent& c = m_collisions[rec->collision];
    // AABB of the rotated-scaled box: use |R*S| per axis (exact for
    // axis-aligned rotations; conservative and cheap for arbitrary yaw).
    const glm::vec3 ex = glm::abs(t.scale) *
        glm::vec3(std::abs(glm::cos(t.rotation.y)) * (c.boundsMax.x - c.boundsMin.x) * 0.5f +
                  std::abs(glm::sin(t.rotation.y)) * (c.boundsMax.z - c.boundsMin.z) * 0.5f,
                  t.scale.y * (c.boundsMax.y - c.boundsMin.y) * 0.5f,
                  std::abs(glm::sin(t.rotation.y)) * (c.boundsMax.x - c.boundsMin.x) * 0.5f +
                  std::abs(glm::cos(t.rotation.y)) * (c.boundsMax.z - c.boundsMin.z) * 0.5f);
    outMin = t.position - ex;
    outMax = t.position + ex;
    return true;
}

bool EntityManager::QueryPoint(const glm::vec3& point, uint32_t layerMask,
                               EntityID* outEntity) const {
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const EntityRecord& rec = m_entities[i];
        if (!rec.alive || rec.collision == 0xFFFFFFFF) continue;
        if ((m_collisions[rec.collision].layerMask & layerMask) == 0) continue;
        const Transform& t = m_transforms[rec.transform];
        glm::vec3 mn, mx;
        // Inline world AABB (same math as GetWorldAABB, without the lookup).
        const CollisionComponent& c = m_collisions[rec.collision];
        const glm::vec3 ex = glm::abs(t.scale) *
            glm::vec3(std::abs(glm::cos(t.rotation.y)) * (c.boundsMax.x - c.boundsMin.x) * 0.5f +
                      std::abs(glm::sin(t.rotation.y)) * (c.boundsMax.z - c.boundsMin.z) * 0.5f,
                      t.scale.y * (c.boundsMax.y - c.boundsMin.y) * 0.5f,
                      std::abs(glm::sin(t.rotation.y)) * (c.boundsMax.x - c.boundsMin.x) * 0.5f +
                      std::abs(glm::cos(t.rotation.y)) * (c.boundsMax.z - c.boundsMin.z) * 0.5f);
        mn = t.position - ex;
        mx = t.position + ex;
        if (point.x >= mn.x && point.x <= mx.x &&
            point.y >= mn.y && point.y <= mx.y &&
            point.z >= mn.z && point.z <= mx.z) {
            if (outEntity) *outEntity = EntityID{uint32_t(i), rec.generation};
            return true;
        }
    }
    return false;
}

bool EntityManager::QuerySphere(const glm::vec3& point, float radius, uint32_t layerMask,
                                EntityID* outEntity) const {
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const EntityRecord& rec = m_entities[i];
        if (!rec.alive || rec.collision == 0xFFFFFFFF) continue;
        if ((m_collisions[rec.collision].layerMask & layerMask) == 0) continue;
        const Transform& t = m_transforms[rec.transform];
        // Cheap sphere-vs-sphere pre-filter on the AABB bounding circle.
        const float boundR = glm::length((m_collisions[rec.collision].boundsMax -
                                          m_collisions[rec.collision].boundsMin) *
                                         glm::abs(t.scale) * 0.5f);
        if (glm::length(point - t.position) > boundR + radius) continue;
        // Exact: expand the world AABB by the radius and test the point.
        glm::vec3 mn, mx;
        const_cast<EntityManager*>(this)->GetWorldAABB(
            EntityID{uint32_t(i), rec.generation}, mn, mx);
        mn -= glm::vec3(radius);
        mx += glm::vec3(radius);
        if (point.x >= mn.x && point.x <= mx.x &&
            point.y >= mn.y && point.y <= mx.y &&
            point.z >= mn.z && point.z <= mx.z) {
            if (outEntity) *outEntity = EntityID{uint32_t(i), rec.generation};
            return true;
        }
    }
    return false;
}

size_t EntityManager::CountNearby(const glm::vec3& point, float radius,
                                  uint32_t layerMask) const {
    size_t n = 0;
    EntityID hit;
    // QuerySphere returns the first hit; walk hits by masking them out via a
    // simple full pass (counts are tiny; clarity over cleverness).
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const EntityRecord& rec = m_entities[i];
        if (!rec.alive || rec.collision == 0xFFFFFFFF) continue;
        if ((m_collisions[rec.collision].layerMask & layerMask) == 0) continue;
        const Transform& t = m_transforms[rec.transform];
        const float boundR = glm::length((m_collisions[rec.collision].boundsMax -
                                          m_collisions[rec.collision].boundsMin) *
                                         glm::abs(t.scale) * 0.5f);
        if (glm::length(point - t.position) > boundR + radius) continue;
        glm::vec3 mn, mx;
        const_cast<EntityManager*>(this)->GetWorldAABB(
            EntityID{uint32_t(i), rec.generation}, mn, mx);
        mn -= glm::vec3(radius);
        mx += glm::vec3(radius);
        if (point.x >= mn.x && point.x <= mx.x &&
            point.y >= mn.y && point.y <= mx.y &&
            point.z >= mn.z && point.z <= mx.z) {
            ++n;
            hit = EntityID{uint32_t(i), rec.generation};
        }
    }
    (void)hit;
    return n;
}

} // namespace OpenBully
