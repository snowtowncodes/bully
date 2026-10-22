// Track 31: Vehicle Core — minimal shared definitions.
// (Track 33 provides this stub so engine.cpp links; extend with mounting etc.)

#include "vehicles/vehicle.h"

namespace OpenBully {

const char* VehicleTypeName(VehicleType t) {
    switch (t) {
        case VehicleType::Bike:  return "Bike";
        case VehicleType::Car:   return "Car";
        case VehicleType::Moped: return "Moped";
        case VehicleType::Kart:  return "Kart";
        default:                 return "?";
    }
}

} // namespace OpenBully
