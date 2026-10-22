#pragma once

// Weapon system (Phase 6, Track 26).
//
// Data-driven weapon definitions (type, damage, durability, range, attack
// speed) plus the per-slot inventory entry. Pure data: no engine, glm or SDL
// dependencies, so entity/player/UI layers can all include it.
//
// Model names refer to NIFs extracted from World.img (data/extracted/world):
//   bat.nif, garbagelid.nif, yardstick.nif, slingshot.nif
// (matches Bully's real wprops: bat=300, slingshot=303, garbage lid, etc.)

#include <cstdint>

namespace OpenBully {

enum class WeaponType : uint8_t {
    Unarmed = 0,
    MeleeBlunt,        // bat, garbage lid
    MeleeSharp,        // yardstick
    RangedProjectile,  // slingshot
};

struct WeaponData {
    const char*  name;           // HUD/inventory label
    WeaponType   type;
    int          damage;         // per hit
    int          durability;     // hits before it breaks (0 = never breaks)
    int          ammo;           // starting/stack ammo for ranged (0 = n/a)
    float        range;          // hit-detection / firing reach (units)
    float        attackCooldown; // seconds between attacks
    float        handScale;      // scale applied when drawn in hand / pickup
    const char*  modelNif;       // nullptr = no model (unarmed)
};

// Static weapon table (script-driven in real Bully; table-driven here).
inline const WeaponData& GetWeaponData(WeaponType type) {
    static const WeaponData kTable[] = {
        { "Fists",     WeaponType::Unarmed,         5,  0,  0, 1.1f, 0.50f, 1.0f, nullptr },
        { "Bat",       WeaponType::MeleeBlunt,     15, 20,  0, 1.9f, 0.70f, 1.0f, "bat.nif" },
        { "GarbageLid",WeaponType::MeleeBlunt,     10, 40,  0, 1.6f, 0.80f, 1.0f, "garbagelid.nif" },
        { "Yardstick", WeaponType::MeleeSharp,      8, 30,  0, 1.7f, 0.45f, 1.0f, "yardstick.nif" },
        { "Slingshot", WeaponType::RangedProjectile,12, 50, 10, 30.0f, 0.90f, 1.0f, "slingshot.nif" },
    };
    return kTable[int(type)];
}

// One inventory slot: the owned weapon plus its runtime state.
// `weaponId` is the kTable index (0 = Unarmed) and is the authoritative
// identifier: several weapons share a WeaponType (bat + garbage lid are
// both MELEE_BLUNT), so type alone cannot pick the right WeaponData row.
struct InventorySlot {
    WeaponType type = WeaponType::Unarmed;
    int weaponId   = 0;   // kTable index (authoritative)
    int durability = 0;   // hits remaining (mirrors WeaponData at pickup)
    int ammo       = 0;   // ranged ammo remaining
};

// Resolves a table row by weaponId (bounds-checked; Unarmed on junk).
inline const WeaponData& GetWeaponDataById(int weaponId) {
    return GetWeaponData(WeaponType(uint8_t(weaponId >= 0 && weaponId <= 4
                                                   ? weaponId : 0)));
}

} // namespace OpenBully
