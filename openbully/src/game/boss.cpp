// Track 49: boss definition tables + name lookups.
//
// Per-boss tuning mirrors the Bully roster flavor:
//   Russell  — Jock tank: strong punches, huge charge, grapple grabs.
//   Derby    — Preppy: stink bombs + perfume/sand sprays at range.
//   Johnny   — Greaser: calls Greaser reinforcements + heavy swings.
//   Gary     — Mastermind: sand in the eyes + ground slam, aggressive.
//   Ted      — Jock: charge + ground slam, fastest baseline cadence.

#include "boss.h"

namespace OpenBully {

namespace {

const BossDefinition kBossTable[int(BossType::Count)] = {
    // Gary: sand throw early, ground slam late; fights up close.
    { "GARY", "The Mastermind", 260, 2.0f, 1.2f, 6.0f, 11.0f,
      18.0f, 7.0f,
      { BossSpecial::SandThrow, BossSpecial::GroundSlam, BossSpecial::Flurry },
      2, 3 },
    // Russell: strong punches, charge attack, grapple grabs.
    { "RUSSELL", "The Jock Titan", 320, 1.6f, 1.5f, 8.0f, 13.0f,
      22.0f, 10.0f,
      { BossSpecial::Charge, BossSpecial::Grab, BossSpecial::GroundSlam },
      2, 3 },
    // Johnny: calls reinforcements + heavy flurry up close.
    { "JOHNNY", "Greaser Leader", 280, 1.9f, 1.3f, 5.0f, 12.0f,
      16.0f, 7.5f,
      { BossSpecial::Flurry, BossSpecial::Reinforce, BossSpecial::Charge },
      2, 3 },
    // Derby: ranged stink bombs + sand sprays, keeps his distance.
    { "DERBY", "Prep King", 240, 1.7f, 1.4f, 5.5f, 14.0f,
      12.0f, 6.0f,
      { BossSpecial::StinkBomb, BossSpecial::SandThrow, BossSpecial::GroundSlam },
      2, 3 },
    // Ted: fastest cadence, charge + slam finisher.
    { "TED", "Jock Bruiser", 300, 2.1f, 1.1f, 6.5f, 12.0f,
      20.0f, 9.0f,
      { BossSpecial::Charge, BossSpecial::GroundSlam, BossSpecial::Flurry },
      2, 3 },
};

} // namespace

const BossDefinition& GetBossDefinition(BossType type) {
    const int i = int(type);
    if (i < 0 || i >= int(BossType::Count)) return kBossTable[0];
    return kBossTable[i];
}

const char* BossTypeName(BossType type) {
    return GetBossDefinition(type).name;
}

const char* BossSpecialName(BossSpecial s) {
    switch (s) {
        case BossSpecial::Charge:     return "CHARGE";
        case BossSpecial::StinkBomb:  return "STINK BOMB";
        case BossSpecial::Reinforce:  return "REINFORCEMENTS";
        case BossSpecial::SandThrow:  return "SAND THROW";
        case BossSpecial::GroundSlam: return "GROUND SLAM";
        case BossSpecial::Grab:       return "GRAB";
        case BossSpecial::Flurry:     return "FLURRY";
        default:                      return "";
    }
}

char BossQteKeyChar(int sdlScancode) {
    switch (sdlScancode) {
        case 26: return 'W'; // SDL_SCANCODE_W
        case 22: return 'S'; // SDL_SCANCODE_S
        case 4:  return 'A'; // SDL_SCANCODE_A
        case 7:  return 'D'; // SDL_SCANCODE_D
        case 44: return ' '; // SDL_SCANCODE_SPACE
        default: return '?';
    }
}

} // namespace OpenBully
