// Track 23: melee combat core — attack tuning tables (see combat.h).

#include "combat.h"

namespace OpenBully {
namespace Melee {

// One row per AttackType. clipIndex = C_Player.agr animation index in
// MAINPED.HXD group order (verification table in combat.h); authoredDur is
// that record's HXD gameplay duration. Effective duration speeds the clip
// up (light punch 1.033 s authored -> 0.60 s gameplay) so punches feel
// snappy while kicks keep their authored pace.
static const AttackData kAttacks[kAttackTypeCount] = {
    /* LightPunch */ { "atk_light_punch", 34, 1.033f, 0.60f,
                       10.0f, 1.6f, 0.30f, 0.55f, 2.5f, 0.8f },
    /* HeavyPunch */ { "atk_heavy_punch", 93, 1.167f, 0.90f,
                       20.0f, 1.8f, 0.38f, 0.62f, 3.5f, 1.2f },
    /* LightKick  */ { "atk_light_kick", 163, 0.667f, 0.667f,
                       15.0f, 1.7f, 0.35f, 0.65f, 3.0f, 1.0f },
    /* HeavyKick  */ { "atk_heavy_kick", 152, 1.100f, 1.100f,
                       25.0f, 1.9f, 0.40f, 0.70f, 4.5f, 1.4f },
};

const AttackData& GetAttackData(AttackType type) {
    return kAttacks[int(type)];
}

AttackType GetAttackType(int index) {
    if (index < 0) index = 0;
    if (index >= kAttackTypeCount) index = kAttackTypeCount - 1;
    return AttackType(index);
}

// Combo bonus: two consecutive LIGHT hits land, then a HEAVY (punch or kick)
// finishes the string for double damage on the final hit. `comboCount` is
// the count INCLUDING the hit being scored.
float ComboMultiplier(int comboCount, AttackType attack) {
    const bool heavy = attack == AttackType::HeavyPunch ||
                       attack == AttackType::HeavyKick;
    if (heavy && comboCount >= 3) return 2.0f;
    return 1.0f;
}

} // namespace Melee
} // namespace OpenBully
