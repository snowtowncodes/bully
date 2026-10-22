#pragma once

// EntityID handle (Phase 4, Track 14) — own header so lightweight components
// (AIComponent, AIEvent) can carry entity handles without pulling in the
// whole EntityManager (and without a circular include with entity.h).

#include <cstdint>

namespace OpenBully {

struct EntityID {
    uint32_t index      = 0xFFFFFFFF; // slot in the manager
    uint32_t generation = 0;          // bumped on destroy (stale-handle guard)

    bool IsValid() const { return index != 0xFFFFFFFF; }
    bool operator==(const EntityID& o) const {
        return index == o.index && generation == o.generation;
    }
    bool operator!=(const EntityID& o) const { return !(*this == o); }
};

} // namespace OpenBully
