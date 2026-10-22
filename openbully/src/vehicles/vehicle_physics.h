#pragma once

// OpenBully: arcade vehicle physics (Phase 8, Track 32).
//
// Left-handed, Y-up world space to match CollisionWorld. Standalone rigid
// body approximation — no entity-system dependency, so Track 31 (vehicle
// core) can bind entities/mounting on top without touching this file.
//
// Model:
//   - 4 wheels at fixed local offsets (FL/FR/RL/RR); each raycasts straight
//     down (CollisionWorld::Raycast) for ground contact.
//   - Per-wheel spring-damper suspension along the ground normal:
//       force = springK * (restLength - length) - damperK * relVel
//   - Drive force at the rear wheels (W), brake / slow reverse (S),
//     front-wheel steering with yaw rate ~ speed * steer angle (A/D).
//   - Gravity when airborne, slow drag (momentum), sphere-vs-level collision
//     with a 0.3 restitution bounce.
//   - Body pitch/roll smoothed from wheel suspension-length deltas (terrain
//     adaptation) + cosmetic nose dip/lift from accel/brake (weight transfer).

#include "../physics/collision.h"

#include <glm/glm.hpp>

namespace OpenBully {

enum WheelSlot { WheelFL = 0, WheelFR = 1, WheelRL = 2, WheelRR = 3 };

struct WheelState {
    glm::vec3 localOffset{0.0f};      // (x, 0, z) from body center
    bool      contact          = false;
    float     suspensionLength = 0.0f; // current spring length
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
    float     contactY         = 0.0f; // ground height at the hit
};

class VehiclePhysics {
public:
    // Tuning (spec: springK ~50, damperK ~5, restLength ~0.5).
    float springK       = 50.0f;
    float damperK       = 5.0f;
    float restLength    = 0.5f;
    float wheelRadius   = 0.35f;
    float mass          = 2.0f;      // light rigid body (normalized units)
    float driveAccel    = 9.0f;      // W: force = accel * mass per spec
    float brakeAccel    = 22.0f;     // S: stronger decel
    float reverseAccel  = 3.0f;      // S while stopped: slow reverse
    float maxReverse    = 4.0f;      // reverse speed cap (u/s)
    float maxSteerAngle = 0.52359877f; // 30 degrees
    float steerSpeed    = 3.0f;      // yaw-rate approach rate (1/s)
    float wheelBase     = 2.6f;      // front<->rear axle distance
    float trackWidth    = 1.7f;      // left<->right wheel distance
    float dragCoeff     = 0.35f;     // velocity decays slowly (momentum)
    float rollingResist = 1.2f;      // constant u/s^2 opposing motion
    float restitution   = 0.3f;      // wall bounce
    float gravity       = -9.8f;
    float bodyRadius    = 0.9f;      // body collision sphere (raised above road)
    float maxYawRate    = 2.2f;      // stability clamp (runaway guard)
    float maxSpeed      = 22.0f;     // stability clamp (u/s)

    // Body state. Position = chassis center; sits ~wheelRadius+restLength
    // above the ground when settled.
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float     yaw        = 0.0f;
    float     yawRate    = 0.0f;
    float     pitch      = 0.0f;      // smoothed terrain pitch (nose up +)
    float     roll       = 0.0f;      // smoothed terrain roll (right side up +)
    float     cosmeticPitch = 0.0f;   // weight-transfer dip/lift (brake/accel)
    float     steerAngle = 0.0f;      // current front-wheel angle (rad)

    WheelState wheels[4];             // FL, FR, RL, RR

    int   wheelContacts    = 0;       // contacts last Update
    int   rearContacts     = 0;
    int   frontContacts    = 0;
    float speedAlongFacing = 0.0f;    // signed forward speed

    const CollisionWorld* world = nullptr; // null disables ground raycasts

    // Debug: when > 0, print a per-stage velocity trace to stdout and
    // decrement. Engine-side verification hook (drive-gate debugging).
    int traceFrames = 0;

    void Update(float dt, float steerInput, float throttle, float brake);

    // Sub-steps of Update (exposed for testing; Update calls them in order).
    void UpdateWheels();
    void ApplySuspension(float dt);
    void ApplyDrive(float dt, float steerInput, float throttle, float brake);
    void Integrate(float dt);
    void UpdateOrientation(float dt, float throttle, float brake);

    glm::vec3 Forward() const; // yaw-only (pitch/roll are visual)
    glm::vec3 Right()   const;

    bool Airborne() const { return wheelContacts == 0; }

    // Vehicle body dimensions for the engine's visual box.
    glm::vec3 BodyHalfExtents() const {
        return { trackWidth * 0.5f + 0.35f, 0.55f, wheelBase * 0.5f + 0.7f };
    }
};

} // namespace OpenBully
