#pragma once

// Weapon system (Phase 6, Track 26; Track 38 melee weapons).
//
// Data-driven weapon definitions (type, damage, durability, range, attack
// speed) plus the per-slot inventory entry. Pure data: no engine, glm or SDL
// dependencies, so entity/player/UI layers can all include it.
//
// Model names refer to NIFs extracted from World.img (data/extracted/world):
//   bat.nif, garbagelid.nif, yardstick.nif, slingshot.nif
// (matches Bully's real wprops: bat=300, slingshot=303, garbage lid, etc.)
//
// Track 38: knockback added per weapon; the Plank joins the melee set.
// AtcPlank.nif exists in the extracted set (world_nif_inventory.txt), so it
// doubles as the plank's model. Durability values follow the Track 38 spec
// (~50 hits) where the model can take it; 0 = never breaks.

#include <cstdint>

namespace OpenBully {

enum class WeaponType : uint8_t {
    Unarmed = 0,
    MeleeBlunt,        // bat, garbage lid, plank
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
    float        knockback;      // Track 38: impulse (u/s) on a melee hit
};

// Static weapon table (script-driven in real Bully; table-driven here).
inline const WeaponData& GetWeaponData(WeaponType type) {
    static const WeaponData kTable[] = {
        { "Fists",      WeaponType::Unarmed,         10,  0,  0, 1.5f, 0.50f, 1.0f, nullptr,          2.0f },
        { "Bat",        WeaponType::MeleeBlunt,      25, 50,  0, 2.5f, 0.70f, 1.0f, "bat.nif",        5.0f },
        { "GarbageLid", WeaponType::MeleeBlunt,      12, 50,  0, 1.8f, 0.80f, 1.0f, "garbagelid.nif", 4.0f },
        { "Yardstick",  WeaponType::MeleeSharp,      10, 50,  0, 1.7f, 0.45f, 1.0f, "yardstick.nif",  3.0f },
        { "Slingshot",  WeaponType::RangedProjectile,15, 50, 20, 30.0f, 0.90f, 1.0f, "slingshot.nif",  3.0f },
        { "Plank",      WeaponType::MeleeBlunt,      15, 50,  0, 2.0f, 0.75f, 1.0f, "AtcPlank.nif",   3.0f },
        // Track 41: Chemistry class reward. No modelNif: never spawns as a
        // pickup, and LoadWeaponModels' 1..5 loop skips the row, so it only
        // enters the inventory through the class-reward grant. Throws like a
        // ranged projectile (bigger pop, shorter range than the slingshot).
        { "Firecracker",WeaponType::RangedProjectile,20,  0,  5, 14.0f, 1.10f, 0.75f, nullptr,          6.0f },
    };
    const int n = int(sizeof(kTable) / sizeof(kTable[0]));
    return kTable[uint8_t(type) < n ? uint8_t(type)
                                    : uint8_t(WeaponType::Unarmed)];
}

// Resolves a table row by weaponId (bounds-checked; Unarmed on junk).
inline const WeaponData& GetWeaponDataById(int weaponId) {
    return GetWeaponData(WeaponType(uint8_t(weaponId >= 0 ? weaponId : 0)));
}

// Track 38: the garbage lid blocks incoming projectiles while equipped.
inline bool WeaponBlocksProjectiles(int weaponId) {
    return weaponId == 2; // GarbageLid row
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

} // namespace OpenBully
