// OpenBully: collision detection implementation (Phase 4 Track 13).

#include "collision.h"

#include <algorithm>
#include <cmath>

namespace OpenBully {

// --- Ray ---------------------------------------------------------------------

bool Ray::IntersectsAABB(const AABB& box, float& outT, float outNormal[3]) const {
    float tmin = 0.0f;
    float tmax = std::numeric_limits<float>::max();
    int axis = -1;
    float sign = 0.0f;
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
        float s = -1.0f;             // entered through the min face
        if (t0 > t1) { std::swap(t0, t1); s = 1.0f; } // entered through max face
        if (t0 > tmin) { tmin = t0; axis = i; sign = s; }
        tmax = std::min(tmax, t1);
        if (tmin > tmax) return false;
    }
    outT = tmin;
    if (outNormal) {
        for (int i = 0; i < 3; i++) outNormal[i] = 0.0f;
        if (axis >= 0) outNormal[axis] = sign;
        else outNormal[1] = 1.0f; // ray started inside: pick +Y as a guess
    }
    return true;
}

bool Ray::IntersectsAABB(const AABB& box, float& outT) const {
    return IntersectsAABB(box, outT, nullptr);
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
    float bestN[3] = { 0.0f, 1.0f, 0.0f };
    for (const Object& obj : m_objects) {
        float t = 0.0f;
        float n[3];
        if (!ray.IntersectsAABB(obj.bounds, t, n)) continue;
        if (t < bestT) {
            bestT = t;
            hit.t = t;
            hit.objectId = static_cast<uint32_t>(&obj - m_objects.data());
            std::copy(n, n + 3, bestN);
        }
    }
    if (!hit.hit()) return false;
    for (int i = 0; i < 3; i++)
        hit.point[i] = ray.origin[i] + ray.direction[i] * hit.t;
    std::copy(bestN, bestN + 3, hit.normal);
    return true;
}

// Shared core of the two ResolveSphere overloads: pushes the sphere out of
// every overlapping box and records the largest-correction face normal.
namespace {
bool ResolveSphereCore(const CollisionWorld& world,
                       const float posIn[3], float radius, float posOut[3],
                       float outNormal[3], int maxIterations) {
    Sphere sphere;
    float p[3] = { posIn[0], posIn[1], posIn[2] };
    bool corrected = false;
    float bestLen = -1.0f;
    float bestN[3] = { 0.0f, 1.0f, 0.0f };

    for (int iter = 0; iter < maxIterations; iter++) {
        std::vector<uint32_t> nearIds;
        world.QuerySphere(p, radius, nearIds);
        if (nearIds.empty()) break;

        bool moved = false;
        for (uint32_t id : nearIds) {
            const AABB& box = world.GetObjectBounds(id);
            float overlap[3];
            bool inside = true;
            for (int i = 0; i < 3; i++) {
                const float lo = p[i] - (box.min[i] - radius);
                const float hi = (box.max[i] + radius) - p[i];
                if (lo <= 0.0f || hi <= 0.0f) { inside = false; break; }
                overlap[i] = std::min(lo, hi);
            }
            if (!inside) continue;

            int axis = 0;
            for (int i = 1; i < 3; i++)
                if (overlap[i] < overlap[axis]) axis = i;
            const float dir = (p[axis] * 2.0f < box.min[axis] + box.max[axis]) ? -1.0f : 1.0f;
            p[axis] += overlap[axis] * dir;
            if (overlap[axis] > bestLen) {
                bestLen = overlap[axis];
                for (int i = 0; i < 3; i++) bestN[i] = 0.0f;
                bestN[axis] = dir; // face normal the sphere was pushed through
            }
            moved = true;
            corrected = true;
        }
        if (!moved) break;
    }

    for (int i = 0; i < 3; i++) posOut[i] = p[i];
    if (outNormal) std::copy(bestN, bestN + 3, outNormal);
    return corrected;
}
} // namespace

bool CollisionWorld::ResolveSphere(const float posIn[3], float radius,
                                   float posOut[3], int maxIterations) const {
    return ResolveSphereCore(*this, posIn, radius, posOut, nullptr, maxIterations);
}

bool CollisionWorld::ResolveSphere(const float posIn[3], float radius,
                                   float posOut[3], float outNormal[3],
                                   int maxIterations) const {
    return ResolveSphereCore(*this, posIn, radius, posOut, outNormal, maxIterations);
}

bool CollisionWorld::GroundProbe(const float pos[3], float probeHeight,
                                 float& outGroundY, float outNormal[3]) const {
    // Per-box down-ray instead of the shared Raycast: a hollow room AABB
    // often contains the probe origin, and the generic slab test reports
    // t=0 for it ("ground at chest height"). Boxes containing the origin
    // are skipped; we want the topmost surface strictly below the feet.
    float bestTop = -FLT_MAX;
    bool found = false;
    Ray down;
    down.origin[0] = pos[0];
    down.origin[1] = pos[1] + probeHeight;
    down.origin[2] = pos[2];
    down.direction[0] = 0.0f;
    down.direction[1] = -1.0f;
    down.direction[2] = 0.0f;

    std::vector<uint32_t> ids;
    QuerySphere(down.origin, probeHeight + 0.5f, ids);
    for (uint32_t id : ids) {
        const AABB& box = m_objects[id].bounds;
        // Ray origin inside this box: skip (we are inside a hollow shell).
        if (box.Contains(down.origin[0], down.origin[1], down.origin[2])) continue;
        // Also skip boxes fully below the feet.
        if (box.max[1] < pos[1] - 0.05f) continue;
        float t = 0.0f;
        if (!down.IntersectsAABB(box, t)) continue;
        const float topY = down.origin[1] - t;
        if (topY > bestTop) {
            bestTop = topY;
            found = true;
            if (outNormal) {
                // Down rays enter through a top face; AABBs are axis-aligned
                // so the surface normal is +Y (triangle-mesh normals arrive
                // with the mesh-based raycaster).
                outNormal[0] = 0.0f; outNormal[1] = 1.0f; outNormal[2] = 0.0f;
            }
        }
    }
    if (!found) return false;
    outGroundY = bestTop;
    return true;
}

} // namespace OpenBully
