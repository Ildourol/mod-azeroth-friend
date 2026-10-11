#ifndef MOD_AZEROTH_FRIEND_MOVEMENT_H
#define MOD_AZEROTH_FRIEND_MOVEMENT_H

#include "AzerothFriendPlayerbot.h"
#include <cstdint>

class Creature;

#if AF_HAS_PLAYERBOTS
class PlayerbotAI;
#else
class PlayerbotAI {};
#endif

namespace AzerothFriendMovement
{
    bool MoveTo(PlayerbotAI* ai, float x, float y, float z, bool generatePath);
    void Hold(PlayerbotAI* ai, uint32_t ms, bool overCombat);
    bool IsMoving(PlayerbotAI* ai);
    void Stop(PlayerbotAI* ai);
    bool Yield(PlayerbotAI* ai);
    bool TalkTo(PlayerbotAI* ai, Creature* npc);
}

#endif // MOD_AZEROTH_FRIEND_MOVEMENT_H
