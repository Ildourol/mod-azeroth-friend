#ifndef MOD_AZEROTH_FRIEND_STRATEGY_H
#define MOD_AZEROTH_FRIEND_STRATEGY_H

#include "AzerothFriendPlayerbot.h"
#include <cstdint>

#if AF_HAS_PLAYERBOTS
class PlayerbotAI;
#else
class PlayerbotAI {};
enum BotState : uint8
{
    BOT_STATE_COMBAT = 0,
    BOT_STATE_NON_COMBAT = 1,
    BOT_STATE_DEAD = 2,
    BOT_STATE_MAX = 3
};
#endif

inline constexpr char const* AUTOPILOT_STRATEGY_NAME = "af";

namespace AzerothFriendStrategy
{
    bool Register();
    bool IsRegistered();
    void SetNoHandouts(bool on);
    bool NoHandouts();
    void SetModelGroups(bool on);
    bool ModelGroups();
    void ClearDeathCount(PlayerbotAI* ai);
    void SetTravelling(uint64_t botGuid, bool on);
    void SetQuestTarget(PlayerbotAI* ai, uint64_t targetGuid);
    void ClearQuestTarget(PlayerbotAI* ai);
    bool EngageQuestTarget(PlayerbotAI* ai);
    void LootObject(PlayerbotAI* ai, uint64_t objectGuid);
    bool OpenObject(PlayerbotAI* ai, uint64_t objectGuid);
    void AutopilotStrategy_Apply(Player* bot);
    void AutopilotStrategy_Remove(Player* bot);

    bool IsBotEnrolled(uint64_t botGuid);
    void SetBotEnrolled(uint64_t botGuid, bool enrolled);
    void ClearEnrolledBots();

    std::string CaptureStrategies(PlayerbotAI* ai);
    void RestoreEngine(PlayerbotAI* ai, const std::string& baseline, BotState state);
}

#endif // MOD_AZEROTH_FRIEND_STRATEGY_H
