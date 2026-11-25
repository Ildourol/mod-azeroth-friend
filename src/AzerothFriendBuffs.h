#ifndef MOD_AZEROTH_FRIEND_BUFFS_H
#define MOD_AZEROTH_FRIEND_BUFFS_H

#include <cstdint>

// --------------------------------------------------------------------------
// Passer-by buffs for mod-azeroth-friend:
// Bots out of combat buff friendly players (and optionally other bots)
// passing nearby with their known class buffs (Fortitude, MotW, Intellect,
// Blessings, Thorns). Instant, long single-target ally auras with 0 tokens.
// --------------------------------------------------------------------------

void Buffs_LoadConfig();
void Buffs_Update(uint32_t diff);

#endif // MOD_AZEROTH_FRIEND_BUFFS_H
