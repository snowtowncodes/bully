#pragma once

// Track 28: AI reaction event system.
//
// NPCs, combat code and the player emit abstract events into a queue that
// Engine::UpdateNpcs drains once per frame. This decouples "something
// happened" (a punch landed, an NPC went down) from "how the crowd reacts"
// (fight back, flee, help, gather, bust) and keeps the reaction logic in one
// place instead of sprinkled through the combat paths.

#include <cstdint>

#include <glm/glm.hpp>

#include "entity.h"

namespace OpenBully {

enum class AIEventType : uint8_t {
    Hit            = 0, // sourceEntity damaged targetEntity at position
    WitnessedFight = 1, // sourceEntity = aggressor; position = fight location
    HelpCall       = 2, // targetEntity attacked sourceEntity; position = caller
    NpcDefeated    = 3, // targetEntity reached 0 health (KO)
};

struct AIEvent {
    AIEventType type = AIEventType::Hit;
    EntityID    sourceEntity{};   // attacker / aggressor / caller
    EntityID    targetEntity{};   // victim / defeated NPC
    glm::vec3   position{0.0f};   // world-space location of the event
    double      time = 0.0;       // engine time when queued
};

} // namespace OpenBully
