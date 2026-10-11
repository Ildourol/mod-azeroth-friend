#ifndef MOD_AZEROTH_FRIEND_STRATEGY_H
#define MOD_AZEROTH_FRIEND_STRATEGY_H

#include "AzerothFriendPlayerbot.h"
#include <cstdint>

#if AF_HAS_PLAYERBOTS
class PlayerbotAI;
#else
class PlayerbotAI {};
#endif

inline constexpr char const* AUTOPILOT_STRATEGY_NAME = "af";

namespace AzerothFriendStrategy
{
    bool Register();
    bool IsRegistered();
    void SetNoHandouts(bool on);
    bool NoHandouts();
    void SetModelGroups(bool on);
    void ClearDeathCount(PlayerbotAI* ai);
    void SetTravelling(uint64_t botGuid, bool on);
    void SetQuestTarget(PlayerbotAI* ai, uint64_t targetGuid);
    void ClearQuestTarget(PlayerbotAI* ai);
    bool EngageQuestTarget(PlayerbotAI* ai);
    void LootObject(PlayerbotAI* ai, uint64_t objectGuid);
    bool OpenObject(PlayerbotAI* ai, uint64_t objectGuid);
}

#endif // MOD_AZEROTH_FRIEND_STRATEGY_H
