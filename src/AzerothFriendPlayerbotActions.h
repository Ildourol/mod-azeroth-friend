#ifndef MOD_AZEROTH_FRIEND_PLAYERBOT_ACTIONS_H
#define MOD_AZEROTH_FRIEND_PLAYERBOT_ACTIONS_H

#include "Common.h"
#include "Player.h"
#include <string>
#include <vector>

// Thin, validated bridge onto mod-playerbots' own action engine.
//
// Everything the companion executes physically goes through here so that a single
// place owns: action-name validation against the bot's own AiObjectContext, the
// raw "any playerbot action/command" passthrough, its denylist, and coordinate
// movement that uses playerbots pathfinding instead of bare MotionMaster calls.
namespace AzerothFriendPlayerbotActions
{
    bool HasPlayerbotAi(Player* bot);
    inline bool IsPlayerbot(Player* bot) { return HasPlayerbotAi(bot); }

    // Executes any action registered in the bot's AiObjectContext by internal name
    // (e.g. "talk to quest giver", "add all loot", "dps assist"). Event param is
    // optional and action specific.
    bool DoAction(Player* bot, std::string const& actionName, std::string const& param = "", bool silent = true);

    // Raw playerbot chat command passthrough (separator aware, denylist filtered).
    bool DoCommand(Player* bot, std::string const& command);

    bool IsSupportedAction(Player* bot, std::string const& actionName);
    bool IsDeniedAction(std::string const& actionName);
    bool IsDeniedCommand(std::string const& command);

    std::vector<std::string> SupportedActions(Player* bot);
    std::vector<std::string> SupportedStrategies(Player* bot);

    // Moves the bot to coordinates using playerbots pathfinding. There is no raw
    // MotionMaster fallback: without a native playerbot body the call fails closed.
    bool MoveToCoords(Player* bot, float x, float y, float z, float range);

    bool FollowMaster(Player* bot);
    bool StopMovement(Player* bot);

    // Applies one or more strategy changes, e.g. ChangeStrategies(bot, "+grind,-stay", false).
    bool ChangeStrategies(Player* bot, std::string const& spec, bool combat);

    std::string Describe(Player* bot);
}

#endif // MOD_AZEROTH_FRIEND_PLAYERBOT_ACTIONS_H
