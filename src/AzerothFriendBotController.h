#ifndef MOD_AZEROTH_FRIEND_BOT_CONTROLLER_H
#define MOD_AZEROTH_FRIEND_BOT_CONTROLLER_H

#include "Common.h"
#include "Player.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

namespace AzerothFriendBotController
{
    // Physical actions
    bool MoveTo(Player* bot, float x, float y, float z, float range = 1.0f);
    bool Attack(Player* bot, uint32 targetGuid);
    bool CastSpell(Player* bot, uint32 spellId, uint32 targetGuid = 0, Unit* resolvedTarget = nullptr);
    uint32 ResolveSpellId(Player* bot, std::string const& spellName, uint8 rank = 0);
    bool Interact(Player* bot, uint32 targetGuid);
    bool Loot(Player* bot);
    bool AcceptQuest(Player* bot, uint32 questId);
    bool TurnInQuest(Player* bot, uint32 questId);
    bool Say(Player* bot, std::string const& message, std::string const& channel = "party");
    bool Emote(Player* bot, std::string const& emoteName);
    bool Follow(Player* bot, Player* master = nullptr);
    bool Stop(Player* bot);
    bool AcceptDuel(Player* bot);
    bool DeclineDuel(Player* bot);
    bool InitiateDuel(Player* bot, uint32 targetGuid);
    bool AcceptTrade(Player* bot);
    bool CancelTrade(Player* bot);
    bool InitiateTrade(Player* bot, Player* target);
    bool TradeSetItem(Player* bot, std::string const& itemQuery, uint32 count = 1,
                      int8 specificSlot = -1, std::string* outItemLink = nullptr);
    bool TradeClearItem(Player* bot, std::string const& itemQuery, int8 slot = -1);
    bool TradeSetGold(Player* bot, uint32 copper, uint32* outActualCopper = nullptr);
    std::string LinkItemsInChat(Player* bot, Player* receiver, std::string const& category = "all", uint32 maxItems = 10);
    std::string FormatItemHyperlink(ItemTemplate const* proto, uint32 count = 1);
    Player* ResolveTargetOrSpeaker(Player* bot, std::string const& paramsJson);
    bool Mount(Player* bot);
    bool Dismount(Player* bot);
    bool EatDrink(Player* bot);

    // Bot Claim API (suspends autonomous roaming during companion plan)
    bool ClaimBot(Player* bot);
    void ReleaseBot(Player* bot);
    bool IsBotClaimed(Player* bot);

    // Self-Context Awareness (gear, spells, bags, quests)
    std::string CollectSelfContext(Player* bot);

    // Playerbot integration
    bool ExecutePlayerbotCommand(Player* bot, std::string const& command);

    // Stuck Recovery
    void TriggerStuckRecovery(Player* bot, float targetX, float targetY, float targetZ);

    // Autonomous Downtime utilities
    void AutoRepairAll(Player* bot);
    uint32 AutoSellJunk(Player* bot);
    void AutoEquipUpgrades(Player* bot);
}

#endif // MOD_AZEROTH_FRIEND_BOT_CONTROLLER_H
