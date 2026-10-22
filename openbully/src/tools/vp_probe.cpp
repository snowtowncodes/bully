// Track 32 headless verification suite (no GPU/SDL): drives VehiclePhysics
// through every track deliverable and prints PASS/FAIL per check.
//
//   1. Acceleration with W (throttle)
//   2. Braking with S (brake), then slow reverse when held at rest
//   3. Steering with A/D (yaw rate at speed, sign flips with steer input)
//   4. Suspension settle (no sinking, no floating, no micro-bounce)
//   5. Wall bounce (restitution ~0.3, no sticking/tunneling)
//   6. Terrain adaptation (pitch/roll from suspension deltas on a ramp)
//   7. Airborne gravity (all wheels off ground -> falls)
//   8. 2-minute stability (mixed inputs, dt spikes: finite + bounded)

#include "physics/collision.h"
#include "vehicles/vehicle_physics.h"

#include <cmath>
#include <cstdio>

using namespace OpenBully;

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failed;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
}

static CollisionWorld FlatWorld() {
    CollisionWorld w;
    AABB ground;
    ground.min[0] = -60; ground.min[1] = -1; ground.min[2] = -60;
    ground.max[0] =  60; ground.max[1] =  0; ground.max[2] =  60;
    w.RegisterObject(ground);
    return w;
}
static CollisionWorld WallWorld() {
    CollisionWorld w = FlatWorld();
    AABB wall;
    wall.min[0] = 8; wall.min[1] = 0; wall.min[2] = -10;
    wall.max[0] = 10; wall.max[1] = 4; wall.max[2] = 10; // +X wall
    w.RegisterObject(wall);
    return w;
}
// Stepped ramp: two terraces (y=0 up to x=0, y=1.5 beyond) -> pitch visible.
static CollisionWorld RampWorld() {
    CollisionWorld w;
    AABB low, high;
    low.min[0]  = -40; low.min[1]  = -1; low.min[2]  = -40;
    low.max[0]  =   0; low.max[1]  =  0; low.max[2]  =  40;
    high.min[0] =   0; high.min[1] = -1; high.min[2] = -40;
    high.max[0] =  40; high.max[1] = 1.5f; high.max[2] = 40;
    w.RegisterObject(low);
    w.RegisterObject(high);
    return w;
}

int main() {
    const float dt = 1.0f / 60.0f;

    // --- 1. Acceleration -------------------------------------------------------
    {
        CollisionWorld world = FlatWorld();
        VehiclePhysics v; v.world = &world;
        v.position = glm::vec3(0, 0.8f, 0);
        for (int i = 0; i < 120; ++i) v.Update(dt, 0, 1, 0); // W 2 s
        Check(v.speedAlongFacing > 4.0f, "W accelerates (speed > 4 u/s after 2 s)");
    }
    // --- 2. Brake + reverse ----------------------------------------------------
    {
        CollisionWorld world = FlatWorld();
        VehiclePhysics v; v.world = &world;
        v.position = glm::vec3(0, 0.8f, 0);
        for (int i = 0; i < 120; ++i) v.Update(dt, 0, 1, 0);
        const float fast = v.speedAlongFacing;
        for (int i = 0; i < 60; ++i) v.Update(dt, 0, 0, 1); // S 1 s
        Check(v.speedAlongFacing < fast * 0.5f, "S brakes (speed halves in 1 s)");
        for (int i = 0; i < 60; ++i) v.Update(dt, 0, 0, 1); // S held at ~rest
        Check(v.speedAlongFacing < -0.3f && v.speedAlongFacing > -4.5f,
              "S at rest -> slow reverse (capped)");
    }
    // --- 3. Steering -----------------------------------------------------------
    {
        CollisionWorld world = FlatWorld();
        VehiclePhysics v; v.world = &world;
        v.position = glm::vec3(0, 0.8f, 0);
        for (int i = 0; i < 90; ++i) v.Update(dt, 0, 1, 0); // speed up
        const float yaw0 = v.yaw;
        for (int i = 0; i < 60; ++i) v.Update(dt, -1, 1, 0); // A 1 s
        const float dLeft = v.yaw - yaw0;
        Check(std::fabs(dLeft) > 0.15f, "A steers (|dyaw| > 0.15 rad in 1 s)");
        const float yaw1 = v.yaw;
        for (int i = 0; i < 60; ++i) v.Update(dt, 1, 1, 0); // D 1 s
        const float dRight = v.yaw - yaw1;
        Check(dRight * dLeft < 0.0f, "D steers opposite to A");
    }
    // --- 4. Suspension settle ---------------------------------------------------
    {
        CollisionWorld world = FlatWorld();
        VehiclePhysics v; v.world = &world;
        v.position = glm::vec3(0, 0.8f, 0);
        float maxY = -1e9f, minY = 1e9f;
        for (int i = 0; i < 300; ++i) { // 5 s settle
            v.Update(dt, 0, 0, 0);
            if (i > 120) { // after the initial bounce
                maxY = std::max(maxY, v.position.y);
                minY = std::min(minY, v.position.y);
            }
        }
        Check(v.wheelContacts > 0, "suspension settles in contact");
        Check(v.position.y > 0.3f && v.position.y < 1.6f,
              "no sinking / no floating (0.3 < rest height < 1.6)");
        Check(maxY - minY < 0.05f, "no micro-bounce (y band < 0.05 over 3 s)");
    }
    // --- 5. Wall bounce ----------------------------------------------------------
    {
        CollisionWorld world = WallWorld();
        VehiclePhysics v; v.world = &world;
        v.position = glm::vec3(0, 0.8f, 0); // faces +Z, turn to face +X
        v.yaw = 1.5707963f;                 // yaw 90 deg -> forward = +X
        for (int i = 0; i < 120 && v.position.x < 6.0f; ++i)
            v.Update(dt, 0, 1, 0);
        Check(v.position.x > 5.0f && v.position.x < 9.6f,
              "vehicle reached the wall (did not tunnel through)");
        const float xAtImpact = v.position.x;
        for (int i = 0; i < 90; ++i) v.Update(dt, 0, 0, 0); // coast after impact
        Check(v.position.x < xAtImpact - 0.2f,
              "bounces off the wall (recoils instead of sticking)");
    }
    // --- 6. Terrain adaptation ----------------------------------------------------
    {
        CollisionWorld world = RampWorld();
        VehiclePhysics v; v.world = &world;
        v.position = glm::vec3(-5, 0.8f, 0);
        v.yaw = 1.5707963f; // face +X (toward the step)
        // Drive onto the step; the chassis plane must tilt nose-up.
        float maxPitch = 0.0f;
        for (int i = 0; i < 240; ++i) {
            v.Update(dt, 0, 0.6f, 0);
            if (v.frontContacts > 0 && v.rearContacts > 0)
                maxPitch = std::max(maxPitch, std::fabs(v.pitch));
        }
        Check(maxPitch > 0.1f, "pitch adapts to a terrain step (|pitch| > 0.1)");
        Check(std::isfinite(v.pitch) && std::isfinite(v.roll),
              "pitch/roll stay finite on steps");
    }
    // --- 7. Airborne gravity --------------------------------------------------------
    {
        CollisionWorld world; // empty: no ground anywhere
        VehiclePhysics v; v.world = &world;
        v.position = glm::vec3(0, 5, 0);
        const float y0 = v.position.y;
        for (int i = 0; i < 30; ++i) v.Update(dt, 0, 1, 0);
        Check(v.position.y < y0 - 1.0f, "airborne: gravity pulls the vehicle down");
    }
    // --- 8. 2-minute stability ---------------------------------------------------------
    {
        CollisionWorld world = WallWorld();
        VehiclePhysics v; v.world = &world;
        v.position = glm::vec3(0, 0.8f, 0);
        bool bad = false;
        for (int i = 0; i < 60 * 120; ++i) {
            const float step = (i % 97 == 0) ? 0.35f : dt; // dt spikes
            const float thr  = (i % 1500) < 1100 ? 1.0f : 0.0f;
            const float brk  = (i % 1500) >= 1100 && (i % 1500) < 1300 ? 1.0f : 0.0f;
            const float str  = ((i / 200) % 3) - 1.0f;      // -1/0/+1 slalom
            v.Update(step, str, thr, brk);
            if (!std::isfinite(v.position.x + v.position.y + v.position.z) ||
                !std::isfinite(v.velocity.x + v.velocity.y + v.velocity.z) ||
                !std::isfinite(v.yaw) || !std::isfinite(v.pitch + v.roll)) {
                bad = true; break;
            }
            if (glm::length(v.velocity) > 30.0f) { bad = true; break; }
        }
        Check(!bad, "2-min stability: finite + bounded under mixed input/dt spikes");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
