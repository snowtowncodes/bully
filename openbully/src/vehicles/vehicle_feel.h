#pragma once

// Track 33: vehicle handling & feel — per-type drive character, wheelie,
// crash/eject, sound hooks. Built ON TOP of Track 31/32:
//   - handling.h: per-type profile table (pure data; spec values)
//   - vehicle_feel.h: runtime "feel" layer a vehicle entity aggregates.
// Track 31's VehicleComponent stays authoritative for pose/speed; the feel
// layer reads/writes through ApplyFeel() given the component + physics body.
//
// Alternatively (headless / test path): FeelState + StepFeel() is fully
// standalone so tools/vehicle_test.cpp verifies behavior without entities.

#include "vehicles/handling.h"

#include <glm/glm.hpp>

namespace OpenBully {

// --- Tunables ----------------------------------------------------------------
inline constexpr float kEjectImpactSpeed   = 10.0f;  // u/s: crash-eject threshold
inline constexpr float kWheelieTorque      = 140.0f; // deg/s^2 pitch accel
inline constexpr float kWheelieSpring      = 6.4f;   // 1/s^2 gravity restoring (per deg)
inline constexpr float kWheelieDamping     = 5.5f;   // 1/s angular damping
inline constexpr float kWheelieInstability = 38.0f;  // deg/s^2 per s held (grows)
inline constexpr float kWheelieBalanceMax  = 65.0f;  // deg: over-rotate -> fall
inline constexpr float kWheelieStuntTime   = 3.0f;   // s: logged as "stunt"
inline constexpr float kAutoRightDelay     = 3.0f;   // s: crashed -> auto-right
inline constexpr float kFeelGravity        = 9.8f;   // u/s^2 downward
inline constexpr float kFeelRollingDrag    = 1.2f;   // 1/s coasting damping
// Track 46: stoppie tuning. The stoppie mirrors the wheelie spring model:
// brake torque pitches the nose DOWN (rear rises), the restoring spring is
// weight on the front wheel, and the balance decays as speed bleeds off so
// the endo settles back down instead of hanging forever.
inline constexpr float kStoppieTorque      = 120.0f; // deg/s^2 pitch accel
inline constexpr float kStoppieSpring      = 7.0f;   // 1/s^2 restoring (per deg)
inline constexpr float kStoppieDamping     = 5.0f;   // 1/s angular damping
inline constexpr float kStoppieBalanceMax  = 50.0f;  // deg: over-rotate -> fail
inline constexpr float kStoppieMinSpeed    = 1.5f;   // u/s: too slow to endo

// Runtime feel state (per mounted vehicle; standalone path).
struct FeelState {
    VehicleType type = VehicleType::Bike;

    // Input snapshot, set by owner each frame before StepFeel.
    bool inThrottle = false;   // W
    bool inBrake    = false;   // S
    bool inLeanL    = false;   // A
    bool inLeanR    = false;   // D
    bool inWheelie  = false;   // Shift held, OR W+S (Track 46 trick trigger)
    // Track 46: true when the bike is grounded (>= 1 wheel contact). Tricks
    // cannot start mid-air; the wheelie torque gate below also uses it so a
    // Shift/W+S hold doesn't lift the front while jumping.
    bool inGrounded = true;

    // Wheelie state (bike only).
    bool  wheelieActive  = false;
    float wheelieAngle   = 0.0f;  // deg, 0 = level, + = front lifted
    float wheelieBalance = 0.0f;  // deg/s pitch angular velocity
    float wheelieTime    = 0.0f;  // s continuous
    bool  wheelieStuntLogged = false;

    // Track 46: stoppie state (bike only). Mirror of the wheelie channel with
    // the opposite pitch sense: + stoppieAngle means the REAR is lifted (the
    // front dives under braking). Kept separate from wheelieAngle so the two
    // poses can never blend — they start from mutually exclusive inputs.
    bool  stoppieActive  = false;
    float stoppieAngle   = 0.0f;  // deg, 0 = level, + = rear lifted
    float stoppieBalance = 0.0f;  // deg/s pitch angular velocity
    float stoppieTime    = 0.0f;  // s continuous

    // Crash state.
    bool  crashed    = false;
    float crashTimer = 0.0f;      // s since crash
    float tiltRoll   = 0.0f;      // deg cosmetic on-side tilt
};

struct FeelOut {
    // Pose deltas the owner applies to the vehicle (standalone path).
    float yawDelta    = 0.0f;   // rad
    float speedDelta  = 0.0f;   // u/s
    float pitch       = 0.0f;   // rad (wheelie angle)
    float roll        = 0.0f;   // rad (lean/tilt)
    // Events.
    bool  ejectRider  = false;
    glm::vec3 ejectPos{0.0f};   // player placement (impact side of vehicle)
    bool  crashBegan  = false;  // first frame of crash -> log CRASH + sfx
    bool  stuntLogged = false;  // wheelie > kWheelieStuntTime
    bool  autoRighted = false;
    float impactSpeed = 0.0f;
    // Track 46: stoppie over-rotation (rear too high) -> loop out backwards.
    bool  stoppieFail = false;
};

// Per-frame feel update (standalone drive model). pos is read for wobble phase
// and eject placement; dt is clamped internally (<= 0.1 s).
FeelOut StepFeel(FeelState& feel, const glm::vec3& pos, float yaw, float speed, float dt);

// Wall/impact response: call with the pre-collision forward speed when the
// vehicle hits a wall this frame. Returns true when the rider is ejected
// (fills out.ejectPos, out.crashBegan, out.impactSpeed).
bool FeelImpact(FeelState& feel, const glm::vec3& pos, float yaw, float impactSpeed,
                FeelOut& out);

// Sound hooks (module-level; stub-safe no-ops until SetVehicleAudio is called).
// Track 31/Engine calls SetVehicleAudio once after AudioManager::Init().
void SetVehicleAudio(class AudioManager* audio);
void VehicleSfxEngineStart(VehicleType type, const glm::vec3& pos);
void VehicleSfxCrash(float impactSpeed, const glm::vec3& pos);
void VehicleSfxWheelie(const glm::vec3& pos);

} // namespace OpenBully
