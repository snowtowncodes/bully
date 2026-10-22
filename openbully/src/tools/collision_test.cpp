// OpenBully: collision self-test (Track 13).
//
//   collision_test
//
// Exercises the Track-13 collision primitives without a GPU: AABB overlap/
// containment, sphere vs box/sphere, ray vs box/sphere (including edge
// cases), spatial-grid queries, and camera-style sphere push-out (slide).
// Exit code 0 = all checks passed; 1 = at least one failure.

#include "physics/collision.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace OpenBully;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

int main() {
    // --- AABB ------------------------------------------------------------
    {
        AABB a; a.min[0]=-1; a.min[1]=-1; a.min[2]=-1; a.max[0]=1; a.max[1]=1; a.max[2]=1;
        AABB b; b.min[0]= 0; b.min[1]= 0; b.min[2]= 0; b.max[0]=2; b.max[1]=2; b.max[2]=2;
        AABB c; c.min[0]= 5; c.min[1]= 5; c.min[2]= 5; c.max[0]=6; c.max[1]=6; c.max[2]=6;
        CHECK(a.Intersects(b));
        CHECK(!a.Intersects(c));
        CHECK(a.Contains(0.0f, 0.0f, 0.0f));
        CHECK(!a.Contains(1.5f, 0.0f, 0.0f));
    }

    // --- Sphere vs AABB / Sphere ------------------------------------------
    {
        AABB box; box.min[0]=-1; box.min[1]=-1; box.min[2]=-1; box.max[0]=1; box.max[1]=1; box.max[2]=1;
        Sphere inside;  inside.center[0]=0;  inside.center[1]=0; inside.center[2]=0;  inside.radius = 0.5f;
        Sphere surface; surface.center[0]=1; surface.center[1]=0; surface.center[2]=0; surface.radius = 0.5f; // touches face
        Sphere miss;    miss.center[0]=3;    miss.center[1]=0;    miss.center[2]=0;    miss.radius   = 0.5f;
        CHECK(inside.Intersects(box));
        CHECK(surface.Intersects(box));
        CHECK(!miss.Intersects(box));

        Sphere other; other.center[0]=2; other.center[1]=0; other.center[2]=0; other.radius = 1.0f;
        CHECK(surface.Intersects(other));   // dist 1 <= r+r = 1.5
        CHECK(miss.Intersects(other));      // dist 1 <= 1.5 (grazes)
        Sphere faraway; faraway.center[0]=10; faraway.center[1]=0; faraway.center[2]=0; faraway.radius = 0.5f;
        CHECK(!faraway.Intersects(other));  // dist 8 > 1.5
    }

    // --- Ray vs AABB -------------------------------------------------------
    {
        AABB box; box.min[0]=-1; box.min[1]=-1; box.min[2]=-1; box.max[0]=1; box.max[1]=1; box.max[2]=1;

        Ray hit;    hit.origin[0]=0; hit.origin[1]=0; hit.origin[2]=-5; hit.direction[0]=0; hit.direction[1]=0; hit.direction[2]=1;
        float t = -1.0f;
        CHECK(hit.IntersectsAABB(box, t));
        CHECK(Near(t, 4.0f));               // enters at z=-1

        Ray miss;   miss.origin[0]=0; miss.origin[1]=5; miss.origin[2]=-5; miss.direction[0]=0; miss.direction[1]=0; miss.direction[2]=1;
        float tm = -1.0f;
        CHECK(!miss.IntersectsAABB(box, tm));

        Ray inside; inside.origin[0]=0; inside.origin[1]=0; inside.origin[2]=0;  inside.direction[0]=1; inside.direction[1]=0; inside.direction[2]=0;
        float ti = -1.0f;
        CHECK(inside.IntersectsAABB(box, ti));
        CHECK(Near(ti, 0.0f));              // starts inside

        Ray behind; behind.origin[0]=0; behind.origin[1]=0; behind.origin[2]=-5; behind.direction[0]=0; behind.direction[1]=0; behind.direction[2]=-1;
        CHECK(!behind.IntersectsAABB(box)); // box behind the ray
    }

    // --- Ray vs Sphere -----------------------------------------------------
    {
        Sphere s; s.center[0]=0; s.center[1]=0; s.center[2]=5; s.radius = 1.0f;
        Ray r;    r.origin[0]=0; r.origin[1]=0; r.origin[2]=0; r.direction[0]=0; r.direction[1]=0; r.direction[2]=1;
        float t = -1.0f;
        CHECK(r.IntersectsSphere(s, t));
        CHECK(Near(t, 4.0f));

        Ray off;  off.origin[0]=0; off.origin[1]=5; off.origin[2]=0; off.direction[0]=0; off.direction[1]=0; off.direction[2]=1;
        CHECK(!off.IntersectsSphere(s));
    }

    // --- CollisionWorld: register + query + raycast -------------------------
    {
        CollisionWorld world;
        AABB floor; floor.min[0]=-50; floor.min[1]=-1; floor.min[2]=-50; floor.max[0]=50; floor.max[1]=0; floor.max[2]=50;
        AABB wall;  wall.min[0]= 10; wall.min[1]= 0; wall.min[2]=-50; wall.max[0]=12; wall.max[1]=10; wall.max[2]=50;
        const uint32_t idFloor = world.RegisterObject(floor);
        const uint32_t idWall  = world.RegisterObject(wall);
        CHECK(world.ObjectCount() == 2);

        // Radius query near the wall.
        std::vector<uint32_t> ids;
        const float p[3] = { 9.5f, 1.0f, 0.0f };
        CHECK(world.QueryRadius(p, 3.0f, ids) >= 1);
        bool sawWall = false;
        for (uint32_t id : ids) if (id == idWall) sawWall = true;
        CHECK(sawWall);

        // Far query: nothing (raise Y clear of the floor slab).
        const float far_p[3] = { -40.0f, 5.0f, -40.0f };
        CHECK(world.QueryRadius(far_p, 1.0f, ids) == 0);

        // Ray straight down hits the floor at y=0.
        Ray down; down.origin[0]=0; down.origin[1]=10; down.origin[2]=0; down.direction[0]=0; down.direction[1]=-1; down.direction[2]=0;
        RayHit hit;
        CHECK(world.Raycast(down, hit));
        CHECK(hit.objectId == idFloor);
        CHECK(Near(hit.t, 10.0f));
        CHECK(Near(hit.point[1], 0.0f));

        // Ray straight up from below: miss.
        Ray up; up.origin[0]=0; up.origin[1]=-5; up.origin[2]=0; up.direction[0]=0; up.direction[1]=1; up.direction[2]=0;
        CHECK(world.Raycast(up, hit)); // floor bottom at y=-1 -> hit at t=4
        CHECK(Near(hit.t, 4.0f));
    }

    // --- ResolveSphere: push out + slide ------------------------------------
    {
        CollisionWorld world;
        AABB wall; wall.min[0]=5; wall.min[1]=-10; wall.min[2]=-10; wall.max[0]=6; wall.max[1]=10; wall.max[2]=10;
        world.RegisterObject(wall);

        // Sphere fully inside the wall, pushed out on X (thinnest axis).
        float out[3];
        const float p1[3] = { 5.5f, 0.0f, 0.0f };
        CHECK(world.ResolveSphere(p1, 0.5f, out));
        CHECK(Near(out[0], 4.5f) || Near(out[0], 6.5f)); // either face
        CHECK(Near(out[1], 0.0f));
        CHECK(Near(out[2], 0.0f));

        // Sphere just outside: untouched.
        const float p2[3] = { 4.2f, 0.0f, 0.0f };
        CHECK(!world.ResolveSphere(p2, 0.5f, out));
        CHECK(Near(out[0], 4.2f));

        // Slide: sphere overlapping the wall's -X half is pushed out through
        // the nearest face and keeps its tangential (z) motion.
        const float p3[3] = { 5.9f, 0.0f, 3.0f };
        CHECK(world.ResolveSphere(p3, 0.5f, out));
        CHECK(Near(out[0], 4.5f) || Near(out[0], 6.5f)); // nearest face wins
        CHECK(Near(out[2], 3.0f, 1e-3f));                // slide preserved
    }

    if (g_failures == 0) {
        std::printf("collision_test: all checks passed\n");
        return 0;
    }
    std::printf("collision_test: %d failure(s)\n", g_failures);
    return 1;
}
