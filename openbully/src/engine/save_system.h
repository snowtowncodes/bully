#pragma once

// Track 36: Save/Load system.
//
// Captures the runtime game state (player position/vitals/inventory, mission
// progress, game time, weather placeholder) into a SaveData struct and
// serializes it to human-readable JSON. The Engine drives save/load via F5/F9
// keys; NPCs respawn from schedules on load (not persisted individually).

#include <cstdint>
#include <string>
#include <vector>

namespace OpenBully {

// One inventory slot in the save file (mirrors InventorySlot from weapon.h).
struct SlotSave {
    int type       = 0;   // WeaponType enum value
    int weaponId   = 0;   // kTable index (authoritative)
    int durability = 0;
    int ammo       = 0;
};

// Mission state snapshot for persistence.
struct MissionSave {
    std::string          id;
    int                  state          = 0;  // MissionState enum value
    int                  currentObjective = 0;
    std::vector<bool>    objectives;          // per-objective complete flag
};

// Trigger zone state snapshot.
struct TriggerSave {
    std::string id;
    bool        fired = false;
};

// Class progression snapshot (Track 41): per-class completion counters.
struct ClassSave {
    std::string id;
    int completions = 0;
    int fails       = 0;
};

// Faction reputation snapshot (Track 42): per-faction respect (-100..100) +
// the completed-mission tally that feeds the overall-respect aggregate.
// Index order matches the Faction enum (faction.h).
struct ReputationSave {
    static constexpr int kCount = 7;   // Faction::Count (keep in sync)
    float respect[kCount] = {};
    int   completedMissions = 0;
};

// Full game state for one save slot.
struct SaveData {
    int         version   = 1;
    int         slot      = 0;
    std::string savedAt;   // ISO-8601 timestamp (UTC)

    // --- Player ---------------------------------------------------------------
    float  playerPos[3]     = {0.0f, 0.0f, 0.0f};
    float  playerRot[3]     = {0.0f, 0.0f, 0.0f};
    int    healthCurrent    = 100;
    int    healthMax        = 100;
    float  staminaCurrent   = 100.0f;
    float  staminaMax       = 100.0f;
    int    money            = 0;
    int    wanted           = 0;
    float  facingYaw        = 0.0f;
    bool   facingValid      = false;
    int    equippedSlot     = -1;
    SlotSave slots[5];

    // --- Mission --------------------------------------------------------------
    std::vector<MissionSave> missions;
    std::vector<TriggerSave> triggers;

    // --- Class (Track 41) -----------------------------------------------------
    std::vector<ClassSave> classes;
    int classRespect       = 0;
    int slingshotCapBonus  = 0;   // SHOP class reward (Track 41)

    // --- Reputation (Track 42) -------------------------------------------------
    ReputationSave reputation;

    // --- World ----------------------------------------------------------------
    double gameTimeMinutes = 8.0 * 60.0;   // fractional minutes since midnight
    int    gameDay         = 0;            // Track 41: day counter (0 = Monday)
    int    weather          = 0;            // placeholder (Track 34 not yet done)

    // --- Track 45: arcade progression ----------------------------------------
    // Per-game high scores in ArcadeGameId order (Nut Shots, Monkey Fling);
    // wins = rounds that set a new high score (drives the $5 prize and the
    // 10-win "Arcade Winner" achievement).
    int    arcadeHigh[2]      = {0, 0};
    int    arcadePlays        = 0;   // coins inserted
    int    arcadeWins         = 0;
    int    arcadeAchievement  = 0;   // 1 = unlocked
};

// Stateless save/load helper. All methods are static; the Engine computes the
// save directory path (via SDL_GetBasePath) and passes it in.
class SaveSystem {
public:
    // Serialize SaveData to a pretty-printed JSON string.
    static std::string Serialize(const SaveData& data);

    // Deserialize a JSON string into SaveData. Returns false on parse error.
    static bool Deserialize(const std::string& json, SaveData& out);

    // Write SaveData to a file path (creates parent dirs as needed).
    static bool WriteToFile(const std::string& path, const SaveData& data);

    // Read SaveData from a file path. Returns false if the file doesn't exist
    // or fails to parse.
    static bool ReadFromFile(const std::string& path, SaveData& out);

    // Build the canonical save file path for a slot relative to a base dir.
    // Returns "<base>/saves/slot_<n>.json".
    static std::string SlotPath(const std::string& baseDir, int slot);

    // Check whether a save file exists for the given slot.
    static bool Exists(const std::string& baseDir, int slot);
};

} // namespace OpenBully
