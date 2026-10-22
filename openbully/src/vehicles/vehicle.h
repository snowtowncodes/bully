#pragma once

// Track 31: Vehicle Core.
//
// Drivable vehicles loaded from World.img NIFs (bike, car, moped, go-kart).
// Each vehicle is an entity carrying Transform + Mesh(worldMeshIndex on the
// engine side) + a VehicleComponent; it renders as a static model until a
// rider mounts it with E (<2.5u away).
//
// VehicleComponent lives in the entity system as a dense component array
// (same pattern as WeaponComponent / AIComponent). The engine owns the
// model GPU buffers (Engine::m_vehicleModels) and the mount/dismount state.

#include "../entities/entity_id.h"

#include <glm/glm.hpp>

namespace OpenBully {

enum class VehicleType : int {
    Bike = 0,
    Car,
    Moped,
    Kart,
    Count
};

const char* VehicleTypeName(VehicleType t);

// Per-vehicle state on the vehicle entity. Static display until mounted;
// the engine's UpdateVehicles drives physics + camera while mounted.
struct VehicleComponent {
    VehicleType type      = VehicleType::Bike;
    float       speed     = 0.0f;          // units/s along vehicle forward
    float       steerAngle = 0.0f;         // radians, applied to yaw
    float       yaw       = 0.0f;          // current facing (radians, +Z forward)
    glm::vec3   wheelPositions[4] = {glm::vec3(0.0f)}; // local-space wheel anchors
    int         wheelCount = 2;            // 2 for bikes/mopeds, 4 for car/kart
    EntityID    rider{};                   // invalid = unmounted
    bool        mounted = false;
};

} // namespace OpenBully
