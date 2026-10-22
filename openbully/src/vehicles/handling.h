#pragma once

// Track 33: per-vehicle-type handling profiles.
//
// Pure data, no logic: a single compile-time table keyed by VehicleType.
// Track 31 (vehicle core) reads the profile to drive its rigid body; Track 33
// (this track) reads it for the bike wheelie balance and crash thresholds.
//
// Units:
//   accel       units/s^2  (engine thrust while W is held)
//   topSpeed    units/s    (forward speed clamp)
//   steerRate   deg/s at any speed (steering is clamped, not speed-scaled)
//   mass        arbitrary units (feeds crash-eject impulse comparison)
//   grip        0..1 (lateral velocity damping; 1 = rails, low = slides)
//   flipFactor  >1 = tippy: scales wheelie torque + crash-eject sensitivity
//   suspension  bounce factor for the kart's springy body pitch/roll feel
//   wobble      0..1 (moped: sinusoidal yaw wobble amplitude near top speed)

#include "vehicle.h" // Track 31: canonical VehicleType (Bike/Car/Moped/Kart)

#include <cstdint>

namespace OpenBully {

// VehicleType is Track 31's canonical enum (vehicle.h): Bike, Car, Moped, Kart.

struct VehicleHandling {
    const char* name;

    float accel;       // units/s^2
    float topSpeed;    // units/s
    float steerRate;   // degrees/s (full-lock)
    float mass;        // arbitrary units

    float grip;        // 0..1 lateral damping
    float flipFactor;  // wheelie torque scale + eject sensitivity
    float suspension;  // bounce factor (kart = bouncy)
    float wobble;      // high-speed yaw wobble amplitude (moped)
};

// The single source of truth. Indexed by int(VehicleType).
// Spec values from Track 33 deliverable 1:
//   BIKE : accel 12, topSpeed 18, steer 35, mass 120, grip high, easy to flip
//   CAR  : accel  8, topSpeed 22, steer 20, mass 800, stable, slow to turn
//   MOPED: accel  6, topSpeed 12, steer 28, mass 150, wobbles at top speed
//   KART : accel  9, topSpeed 10, steer 45, mass 250, bouncy suspension
inline constexpr VehicleHandling kVehicleHandling[int(VehicleType::Count)] = {
    /* Bike  */  { "BIKE",  12.0f, 18.0f, 35.0f, 120.0f, 0.90f, 1.60f, 0.35f, 0.00f },
    /* Car   */  { "CAR",    8.0f, 22.0f, 20.0f, 800.0f, 0.98f, 0.40f, 0.10f, 0.00f },
    /* Moped */  { "MOPED",  6.0f, 12.0f, 28.0f, 150.0f, 0.85f, 0.90f, 0.25f, 0.55f },
    /* Kart  */  { "KART",   9.0f, 10.0f, 45.0f, 250.0f, 0.80f, 0.70f, 0.90f, 0.00f },
};

inline const VehicleHandling& GetHandling(VehicleType t) {
    return kVehicleHandling[int(t)];
}

} // namespace OpenBully
