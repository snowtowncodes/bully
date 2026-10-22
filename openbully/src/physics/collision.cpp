// OpenBully: collision detection implementation (Phase 4 Track 13).

#include "collision.h"

#include <algorithm>
#include <cmath>

namespace OpenBully {

// --- Ray ---------------------------------------------------------------------

bool Ray::IntersectsAABB(const AABB& box, float& outT) const {
    float tmin = 0.0f;
    float tmax = std::numeric_limits<float>::max();
    for (int i = 0; i < 3; i++) {
        const float d = direction[i];
        if (std::fabs(d) < 1e-8f) {
            // Parallel to the slab pair: origin must be inside the slab.
            if (origin[i] < box.min[i] || origin[i] > box.max[i]) return false;
            continue;
        }
        const float inv = 1.0f / d;
        float t0 = (box.min[i] - origin[i]) * inv;
        float t1 = (box.max[i] - origin[i]) * inv;
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmin > tmax) return false;
    }
    outT = tmin;
    return true;
}

bool Ray::IntersectsSphere(const Sphere& s, float& outT) const {
    const float ox = origin[0] - s.center[0];
    const float oy = origin[1] - s.center[1];
    const float oz = origin[2] - s.center[2];
    const float b = ox * direction[0] + oy * direction[1] + oz * direction[2];
    const float c = ox * ox + oy * oy + oz * oz - s.radius * s.radius;
    const float disc = b * b - c;
    if (disc < 0.0f) return false;
    const float t = -b - std::sqrt(disc);
    if (t < 0.0f) return false; // behind origin or origin inside; treat as miss
    outT = t;
    return true;
}

// --- CollisionWorld ----------------------------------------------------------

uint32_t CollisionWorld::RegisterObject(const AABB& bounds) {
    const uint32_t id = static_cast<uint32_t>(m_objects.size());
    m_objects.push_back({ bounds });

    // Insert into every cell the box spans (2D x/z coverage matters most for
    // level geometry, but the Y axis is included so stacked floors partition).
    const int cx0 = int(std::floor(bounds.min[0] / kCellSize));
    const int cx1 = int(std::floor(bounds.max[0] / kCellSize));
    const int cy0 = int(std::floor(bounds.min[1] / kCellSize));
    const int cy1 = int(std::floor(bounds.max[1] / kCellSize));
    const int cz0 = int(std::floor(bounds.min[2] / kCellSize));
    const int cz1 = int(std::floor(bounds.max[2] / kCellSize));
    for (int cz = cz0; cz <= cz1; cz++)
        for (int cy = cy0; cy <= cy1; cy++)
            for (int cx = cx0; cx <= cx1; cx++) {
                Cell& cell = *FindCell(CellKey(cx, cy, cz));
                cell.ids.push_back(id);
            }
    return id;
}

CollisionWorld::Cell* CollisionWorld::FindCell(int64_t key) {
    for (Cell& c : m_cells)
        if (c.key == key) return &c;
    m_cells.push_back({ key, {} });
    return &m_cells.back();
}

const CollisionWorld::Cell* CollisionWorld::FindCell(int64_t key) const {
    for (const Cell& c : m_cells)
        if (c.key == key) return &c;
    return nullptr;
}

size_t CollisionWorld::QuerySphere(const float pos[3], float radius,
                                   std::vector<uint32_t>& outIds) const {
    outIds.clear();
    AABB probe;
    probe.min[0] = pos[0] - radius; probe.max[0] = pos[0] + radius;
    probe.min[1] = pos[1] - radius; probe.max[1] = pos[1] + radius;
    probe.min[2] = pos[2] - radius; probe.max[2] = pos[2] + radius;
    Sphere sphere;
    for (int i = 0; i < 3; i++) sphere.center[i] = pos[i];
    sphere.radius = radius;

    const int cx0 = int(std::floor(probe.min[0] / kCellSize));
    const int cx1 = int(std::floor(probe.max[0] / kCellSize));
    const int cy0 = int(std::floor(probe.min[1] / kCellSize));
    const int cy1 = int(std::floor(probe.max[1] / kCellSize));
    const int cz0 = int(std::floor(probe.min[2] / kCellSize));
    const int cz1 = int(std::floor(probe.max[2] / kCellSize));

    for (int cz = cz0; cz <= cz1; cz++)
        for (int cy = cy0; cy <= cy1; cy++)
            for (int cx = cx0; cx <= cx1; cx++) {
                const Cell* cell = FindCell(CellKey(cx, cy, cz));
                if (!cell) continue;
                for (uint32_t id : cell->ids) {
                    if (std::find(outIds.begin(), outIds.end(), id) != outIds.end())
                        continue; // box spans multiple probed cells
                    if (m_objects[id].bounds.Intersects(probe) &&
                        sphere.Intersects(m_objects[id].bounds)) {
                        outIds.push_back(id);
                    }
                }
            }
    return outIds.size();
}

bool CollisionWorld::Raycast(const Ray& ray, RayHit& hit) const {
    hit.objectId = 0;
    hit.t = -1.0f;
    float bestT = std::numeric_limits<float>::max();
    for (const Object& obj : m_objects) {
        float t = 0.0f;
        if (!ray.IntersectsAABB(obj.bounds, t)) continue;
        if (t < bestT) {
            bestT = t;
            hit.t = t;
            hit.objectId = static_cast<uint32_t>(&obj - m_objects.data());
        }
    }
    if (!hit.hit()) return false;
    for (int i = 0; i < 3; i++)
        hit.point[i] = ray.origin[i] + ray.direction[i] * hit.t;
    return true;
}

bool CollisionWorld::ResolveSphere(const float posIn[3], float radius,
                                   float posOut[3], int maxIterations) const {
    Sphere sphere;
    float p[3] = { posIn[0], posIn[1], posIn[2] };
    bool corrected = false;

    for (int iter = 0; iter < maxIterations; iter++) {
        std::vector<uint32_t> nearIds;
        QuerySphere(p, radius, nearIds);
        if (nearIds.empty()) break;

        bool moved = false;
        for (uint32_t id : nearIds) {
            const AABB& box = m_objects[id].bounds;
            // Overlap along each axis (sphere center vs box expanded by r).
            float overlap[3];
            bool inside = true;
            for (int i = 0; i < 3; i++) {
                const float lo = p[i] - (box.min[i] - radius);
                const float hi = (box.max[i] + radius) - p[i];
                if (lo <= 0.0f || hi <= 0.0f) { inside = false; break; }
                // Push out through whichever face is closer.
                overlap[i] = std::min(lo, hi);
            }
            if (!inside) continue;

            // Minimum-penetration axis push-out.
            int axis = 0;
            for (int i = 1; i < 3; i++)
                if (overlap[i] < overlap[axis]) axis = i;
            const float dir = (p[axis] * 2.0f < box.min[axis] + box.max[axis]) ? -1.0f : 1.0f;
            p[axis] += overlap[axis] * dir;
            moved = true;
            corrected = true;
        }
        if (!moved) break;
    }

    for (int i = 0; i < 3; i++) posOut[i] = p[i];
    return corrected;
}

} // namespace OpenBully
