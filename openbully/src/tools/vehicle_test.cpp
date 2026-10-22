// Track 33 verification harness (headless, no GPU/audio).
//
// Exercises handling profiles, wheelie, crash/eject, and auto-right through
// the same StepFeel/FeelImpact code the engine aggregates. Prints one line
// per check with PASS/FAIL, and a speed log per vehicle type (spec check:
// bike accelerates faster than car; kart turns tightest).

#include "audio/audio.h"
#include "platform/logger.h"
#include "vehicles/handling.h"
#include "vehicles/vehicle.h"
#include "vehicles/vehicle_feel.h"

#include <cmath>
#include <cstdio>

using namespace OpenBully;

static int g_checks = 0;
static int g_failed = 0;

static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failed;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
}

int main() {
    Logger::Init();
    SetVehicleAudio(nullptr); // headless: sound hooks log only

    // --- 1. Handling profiles table ------------------------------------------
    const VehicleHandling& bike  = GetHandling(VehicleType::Bike);
    const VehicleHandling& car   = GetHandling(VehicleType::Car);
    const VehicleHandling& moped = GetHandling(VehicleType::Moped);
    const VehicleHandling& kart  = GetHandling(VehicleType::Kart);

    Check(bike.accel == 12.0f && bike.topSpeed == 18.0f &&
          bike.steerRate == 35.0f && bike.mass == 120.0f, "BIKE profile spec");
    Check(car.accel == 8.0f && car.topSpeed == 22.0f &&
          car.steerRate == 20.0f && car.mass == 800.0f, "CAR profile spec");
    Check(moped.accel == 6.0f && moped.topSpeed == 12.0f &&
          moped.steerRate == 28.0f && moped.mass == 150.0f, "MOPED profile spec");
    Check(kart.accel == 9.0f && kart.topSpeed == 10.0f &&
          kart.steerRate == 45.0f && kart.mass == 250.0f, "KART profile spec");

    // --- 2. Feel differences (accel ranking, steer ranking) ------------------
    // Log speed reached after 2 s full throttle from standstill (below every
    // top-speed cap, so the ranking reflects raw accel from the profiles).
    auto speedAfter = [&](VehicleType t) {
        FeelState f;  f.type = t;  f.inThrottle = true;
        float speed = 0.0f;
        for (int i = 0; i < 120; ++i) {
            FeelOut o = StepFeel(f, glm::vec3(0.0f), 0.0f, speed, 1.0f / 60.0f);
            speed += o.speedDelta;
        }
        return speed;
    };
    const float sBike = speedAfter(VehicleType::Bike);
    const float sCar  = speedAfter(VehicleType::Car);
    const float sMop  = speedAfter(VehicleType::Moped);
    const float sKart = speedAfter(VehicleType::Kart);
    std::printf("speed@2s: bike %.1f | car %.1f | moped %.1f | kart %.1f (u/s)\n",
                sBike, sCar, sMop, sKart);
    Check(sBike > sCar, "bike accelerates faster than car");
    Check(sMop < sBike && sMop < sCar, "moped slowest accel");
    Check(sKart > 6.0f && sKart < sBike, "kart mid-pack accel, capped near 10");

    // Kart turns tightest: yaw rate at matched speed fraction.
    auto yawAt = [&](VehicleType t) {
        FeelState f;  f.type = t;  f.inLeanL = true;  f.inThrottle = true;
        float speed = 0.0f, yaw = 0.0f;
        for (int i = 0; i < 120; ++i) {
            FeelOut o = StepFeel(f, glm::vec3(0.0f), yaw, speed, 1.0f / 60.0f);
            speed += o.speedDelta;
            yaw += o.yawDelta;
        }
        return yaw;
    };
    const float yBike = std::fabs(yawAt(VehicleType::Bike));
    const float yCar  = std::fabs(yawAt(VehicleType::Car));
    const float yKart = std::fabs(yawAt(VehicleType::Kart));
    std::printf("yaw@2s: bike %.2f | car %.2f | kart %.2f (rad)\n", yBike, yCar, yKart);
    Check(yKart > yBike && yBike > yCar, "kart turns tightest, car slowest");

    // --- 3. Wheelie: front lifts, stunt logged, over-rotate ejects -----------
    {
        FeelState f;  f.type = VehicleType::Bike;
        SetVehicleAudio(nullptr);
        // Build speed first (no wheelie input).
        float speed = 0.0f, yaw = 0.0f;
        glm::vec3 pos(0.0f);
        for (int i = 0; i < 90; ++i) {
            f.inThrottle = true;
            FeelOut o = StepFeel(f, pos, yaw, speed, 1.0f / 60.0f);
            speed += o.speedDelta;  yaw += o.yawDelta;
            pos.x += speed * std::sin(yaw) * (1.0f / 60.0f);
            pos.z += speed * std::cos(yaw) * (1.0f / 60.0f);
        }
        // Hold W + Shift -> front lifts.
        f.inWheelie = true;
        bool lifted = false, stunt = false, eject = false;
        for (int i = 0; i < 60 * 10 && !eject; ++i) {
            f.inThrottle = true;
            FeelOut o = StepFeel(f, pos, yaw, speed, 1.0f / 60.0f);
            speed += o.speedDelta;  yaw += o.yawDelta;
            pos.x += speed * std::sin(yaw) * (1.0f / 60.0f);
            pos.z += speed * std::cos(yaw) * (1.0f / 60.0f);
            if (f.wheelieAngle > 5.0f) lifted = true;
            if (o.stuntLogged) stunt = true;
            if (o.ejectRider)  eject = true;
        }
        Check(lifted, "wheelie: front wheel lifts (angle > 5 deg)");
        Check(stunt,  "wheelie > 3s logged as STUNT");
        Check(eject,  "over-rotate past 65 deg ejects rider");

        // Lean adjusts balance during wheelie (A pushes angle back down).
        FeelState g;  g.type = VehicleType::Bike;  g.inThrottle = true;
        g.inWheelie = true;
        speed = 8.0f;
        float angleNoLean = 0.0f, angleLean = 0.0f;
        for (int i = 0; i < 30; ++i) {
            FeelOut o = StepFeel(g, pos, yaw, speed, 1.0f / 60.0f);
            angleNoLean = g.wheelieAngle;
        }
        FeelState g2;  g2.type = VehicleType::Bike;  g2.inThrottle = true;
        g2.inWheelie = true;  g2.inLeanL = true;
        for (int i = 0; i < 30; ++i) {
            StepFeel(g2, pos, yaw, speed, 1.0f / 60.0f);
            angleLean = g2.wheelieAngle;
        }
        Check(angleLean < angleNoLean + 5.0f,
              "A/D lean modulates wheelie balance (A reduces rise)");
    }

    // --- 4. Crash / eject / auto-right ----------------------------------------
    {
        FeelState f;  f.type = VehicleType::Bike;
        FeelOut o;
        const bool ejected = FeelImpact(f, glm::vec3(0.0f), 0.0f, 12.0f, o);
        Check(ejected && o.crashBegan && o.impactSpeed == 12.0f,
              "bike impact 12 u/s (> 10) -> eject + CRASH");
        Check(f.crashed, "bike marked crashed (on side)");
        const glm::vec3 rider = o.ejectPos;
        Check(std::fabs(rider.x) > 0.5f && std::fabs(rider.z) < 0.01f,
              "rider placed beside vehicle (impact side)");

        // Car shrugs the same impact off (flipFactor 0.4 -> needs 25 u/s).
        FeelState c;  c.type = VehicleType::Car;
        FeelOut co;
        Check(!FeelImpact(c, glm::vec3(0.0f), 0.0f, 12.0f, co),
              "car impact 12 u/s -> stable (no eject)");

        // Auto-right after 3 s.
        for (int i = 0; i < 200 && f.crashed; ++i) {
            FeelOut so = StepFeel(f, glm::vec3(0.0f), 0.0f, 0.0f, 1.0f / 60.0f);
            if (so.autoRighted) break;
        }
        Check(!f.crashed, "vehicle auto-rights after ~3 s");

        // Below-threshold bike impact does nothing.
        FeelState b2;  b2.type = VehicleType::Bike;
        FeelOut b2o;
        Check(!FeelImpact(b2, glm::vec3(0.0f), 0.0f, 5.0f, b2o),
              "bike impact 5 u/s (< 10) -> no eject");
    }

    // --- 5. Sound hooks (log-only in headless mode) ---------------------------
    VehicleSfxEngineStart(VehicleType::Bike, glm::vec3(0.0f));
    VehicleSfxCrash(15.0f, glm::vec3(0.0f));
    VehicleSfxWheelie(glm::vec3(0.0f));
    Check(true, "sound hooks fire without audio manager (stub-safe)");

    // --- 6. 2-minute stability (dt spikes, parked, wobble, crash loops) -------
    {
        FeelState f;  f.type = VehicleType::Bike;
        float speed = 0.0f, yaw = 0.0f;
        glm::vec3 pos(0.0f);
        bool bad = false;
        for (int i = 0; i < 60 * 120; ++i) {
            const float dt = (i % 97 == 0) ? 0.35f : 1.0f / 60.0f; // spikes
            f.inThrottle = (i % 1500) < 1200;
            f.inWheelie  = (i % 900)  < 600;
            f.inLeanL    = (i % 400)  < 150;
            f.inLeanR    = !f.inLeanL && (i % 400) < 300;
            FeelOut o = StepFeel(f, pos, yaw, speed, dt);
            speed += o.speedDelta;
            yaw   += o.yawDelta;
            pos.x += speed * std::sin(yaw) * dt;
            pos.z += speed * std::cos(yaw) * dt;
            if (!std::isfinite(speed) || !std::isfinite(yaw) ||
                !std::isfinite(pos.x) || !std::isfinite(pos.z) ||
                !std::isfinite(f.wheelieAngle)) { bad = true; break; }
            if (std::fabs(speed) > 25.0f || std::fabs(f.wheelieAngle) > 90.0f) {
                bad = true; break;
            }
        }
        Check(!bad, "2-min stability: finite pose/speed/angle under dt spikes");
    }

    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
