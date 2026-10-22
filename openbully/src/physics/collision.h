#pragma once

// OpenBully: basic collision detection (Phase 4 Track 13).
//
// Left-handed, Y-up world space to match Gamebryo geometry. Everything here
// is CPU-side and renderer-agnostic; the Engine wires CollisionWorld into
// the camera and the debug wireframe renderer.
//
// Components:
//   AABB            - axis-aligned bounding box (min/max)
//   Sphere          - center + radius
//   Ray             - origin + normalized direction, slab-method intersection
//   CollisionWorld  - uniform spatial-hash grid over registered AABBs,
//                     radius/sphere queries, raycasts and slide resolution

#include <cstdint>
#include <vector>

namespace OpenBully {

// --- Primitives --------------------------------------------------------------

struct AABB {
    float min[3] = { 0.0f, 0.0f, 0.0f };
    float max[3] = { 0.0f, 0.0f, 0.0f };

    bool Intersects(const AABB& o) const {
        return min[0] <= o.max[0] && max[0] >= o.min[0] &&
               min[1] <= o.max[1] && max[1] >= o.min[1] &&
               min[2] <= o.max[2] && max[2] >= o.min[2];
    }
    bool Contains(float x, float y, float z) const {
        return x >= min[0] && x <= max[0] &&
               y >= min[1] && y <= max[1] &&
               z >= min[2] && z <= max[2];
    }
    // Closest point on the box to (x,y,z), written to out[3].
    void ClosestPoint(float x, float y, float z, float out[3]) const {
        out[0] = x < min[0] ? min[0] : (x > max[0] ? max[0] : x);
        out[1] = y < min[1] ? min[1] : (y > max[1] ? max[1] : y);
        out[2] = z < min[2] ? min[2] : (z > max[2] ? max[2] : z);
    }
};

struct Sphere {
    float center[3] = { 0.0f, 0.0f, 0.0f };
    float radius    = 0.0f;

    bool Intersects(const AABB& box) const {
        float cp[3];
        box.ClosestPoint(center[0], center[1], center[2], cp);
        const float dx = center[0] - cp[0];
        const float dy = center[1] - cp[1];
        const float dz = center[2] - cp[2];
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }
    bool Intersects(const Sphere& o) const {
        const float dx = center[0] - o.center[0];
        const float dy = center[1] - o.center[1];
        const float dz = center[2] - o.center[2];
        const float r  = radius + o.radius;
        return dx * dx + dy * dy + dz * dz <= r * r;
    }
};

struct Ray {
    float origin[3]    = { 0.0f, 0.0f, 0.0f };
    float direction[3] = { 0.0f, 0.0f, 1.0f }; // expected normalized

    bool IntersectsAABB(const AABB& box) const { float t; return IntersectsAABB(box, t); }
    // Slab method. `outT` receives the entry distance (>= 0).
    bool IntersectsAABB(const AABB& box, float& outT) const;
    // Track 18: same, but also writes the face normal the ray entered through
    // (unit axis vector, may be null when not needed).
    bool IntersectsAABB(const AABB& box, float& outT, float outNormal[3]) const;
    bool IntersectsSphere(const Sphere& s) const { float t; return IntersectsSphere(s, t); }
    bool IntersectsSphere(const Sphere& s, float& outT) const;
};

// Raycast hit result (CollisionWorld::Raycast).
struct RayHit {
    uint32_t objectId = 0;
    float    t        = -1.0f;    // distance along the ray; negative = miss
    float    point[3] = { 0.0f, 0.0f, 0.0f };
    float    normal[3] = { 0.0f, 1.0f, 0.0f }; // surface normal at the hit (Track 18)

    bool hit() const { return t >= 0.0f; }
};

// --- CollisionWorld ----------------------------------------------------------

class CollisionWorld {
public:
    CollisionWorld() = default;

    // Registers an AABB and returns its object id (index into the table).
    // Spatial cells are populated lazily per axis-aligned cell the box spans.
    uint32_t RegisterObject(const AABB& bounds);

    // Collects ids of registered objects whose grid cells overlap the sphere
    // (pos, radius) and whose AABB intersects it. Returns the hit count.
    size_t QuerySphere(const float pos[3], float radius,
                       std::vector<uint32_t>& outIds) const;

    // Convenience: broad+exact test around a position (QuerySphere with a
    // point-radius circle).
    size_t QueryRadius(const float pos[3], float radius,
                       std::vector<uint32_t>& outIds) const {
        return QuerySphere(pos, radius, outIds);
    }

    // Nearest registered AABB raycast. Returns true and fills `hit` on hit.
    bool Raycast(const Ray& ray, RayHit& hit) const;

    // Pushes `pos` (treated as a sphere of `radius`) out of every registered
    // box it overlaps, using minimum-penetration-axis resolution. Iterates a
    // few times so corner cases converge; sliding against walls emerges from
    // the per-axis push. Returns true when a correction was applied.
    bool ResolveSphere(const float posIn[3], float radius,
                       float posOut[3], int maxIterations = 3) const;

    // Track 18: same resolution, also returning the surface normal of the
    // largest push-out (the face the sphere was ejected through) so callers
    // can slide movement along walls. outNormal may be null.
    bool ResolveSphere(const float posIn[3], float radius,
                       float posOut[3], float outNormal[3],
                       int maxIterations = 3) const;

    // Track 18: ground probe. Casts a ray straight down from slightly above
    // `pos` and returns the surface height + face normal. `probeHeight` is
    // how far above the feet the ray starts (meshes can be hollow AABBs, so
    // the ray must start outside any box the feet may touch).
    bool GroundProbe(const float pos[3], float probeHeight,
                     float& outGroundY, float outNormal[3]) const;

    // Debug/inspection.
    size_t ObjectCount() const { return m_objects.size(); }
    const AABB& GetObjectBounds(uint32_t id) const { return m_objects[id].bounds; }

    static constexpr float kCellSize = 8.0f; // world units per grid cell

private:
    struct Object {
        AABB bounds;
    };

    // Cell key -> object id list. Grids use a linear-probe map; a flat
    // std::vector of pairs keeps the build dependency-light.
    struct Cell {
        int64_t key;
        std::vector<uint32_t> ids;
    };
    Cell* FindCell(int64_t key);
    const Cell* FindCell(int64_t key) const;
    static int64_t CellKey(int cx, int cy, int cz) {
        // Pack three 21-bit signed coords into 63 bits.
        return (int64_t(uint64_t(uint32_t(cx) & 0x1FFFFF)) << 42) |
               (int64_t(uint64_t(uint32_t(cy) & 0x1FFFFF)) << 21) |
                int64_t(uint64_t(uint32_t(cz) & 0x1FFFFF));
    }

    std::vector<Object> m_objects;
    std::vector<Cell>   m_cells;
};

} // namespace OpenBully
