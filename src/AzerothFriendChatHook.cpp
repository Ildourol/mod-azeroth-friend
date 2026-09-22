#include "AzerothFriendChatHook.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendActionDispatcher.h"
#include "AzerothFriendAiControl.h"
#include "AzerothFriendEnvironment.h"
#include "AzerothFriendBotController.h"
#include "AzerothFriendShared.h"
#include "AzerothFriendLiveState.h"
#include "ObjectAccessor.h"
#include "Chat.h"
#include "Group.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
#include <unordered_map>

#if __has_include("Playerbots.h")
    #include "Playerbots.h"
    #include "PlayerbotAI.h"
    #include "PlayerbotMgr.h"
    #define AF_HAS_PLAYERBOTS 1
#else
    #define AF_HAS_PLAYERBOTS 0
#endif

AzerothFriendPlayerScript::AzerothFriendPlayerScript()
    : PlayerScript("AzerothFriendPlayerScript",
                   {PLAYERHOOK_CAN_PLAYER_USE_CHAT,
                    PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
                    PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
                    PLAYERHOOK_ON_PLAYER_ENTER_COMBAT,
                    PLAYERHOOK_ON_PLAYER_LEAVE_COMBAT,
                    PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
                    PLAYERHOOK_ON_DUEL_REQUEST,
                    PLAYERHOOK_ON_DUEL_START,
                    PLAYERHOOK_ON_DUEL_END,
                    PLAYERHOOK_CAN_INIT_TRADE,
                    PLAYERHOOK_ON_LOGIN,
                    PLAYERHOOK_ON_LOGOUT})
{
}

namespace
{
    // MultiBotBridge and the AI addons exchange machine tokens over public chat
    // (MBOT / CAPS-* / ROSTER / PONG / DIALOG / HELLO-AOK). Those packets must
    // never be read as dialogue, and must never be promoted to owner commands -
    // otherwise the companion "hears" protocol frames as instructions.
    bool IsProtocolTraffic(std::string const& msg)
    {
        std::string lower = msg;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        for (char const* token : { "mbot\t", "mbot", "aio\t", "caps-", "caps_",
                                   "hello-aok", "roster:", "pong ", "dialog " })
        {
            if (lower.find(token) != std::string::npos)
                return true;
        }
        return false;
    }

    // A protocol carrier is a transport character (MBOT/AIO), not a person.
    bool IsProtocolCarrier(std::string const& speaker)
    {
        std::string lower = speaker;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        size_t first = lower.find_first_not_of(" \t");
        if (first != std::string::npos)
            lower = lower.substr(first);
        return (lower.rfind("mbot", 0) == 0 || lower.rfind("aio", 0) == 0);
    }

    bool IsAddonTraffic(std::string const& msg)
    {
        return IsProtocolTraffic(msg);
    }

    // Natural-language cast requests ("cast fireball", "heal me", "throw a
    // frostbolt at the wolf") must reach the bridge as commands, otherwise they
    // degrade into ambient dialogue the planner is told to ignore. Only the
    // spell vocabulary is matched here; the bridge does the real resolution and
    // rejects anything the companion has not learned.
    bool IsCastIntent(std::string const& msgLower)
    {
        for (char const* keyword : { "cast", "spell", "heal", "buff", "shield", "dispel",
                                     "cleanse", "resurrect", "rez", "polymorph", "banish",
                                     "interrupt", "purge", "decurse", "portal", "teleport", "open" })
        {
            if (msgLower.find(keyword) != std::string::npos)
                return true;
        }
        return false;
    }

    bool IsBotCommandAck(std::string const& text)
    {
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        size_t first = lower.find_first_not_of(" \t\r\n");
        if (first != std::string::npos)
            lower = lower.substr(first);

        return (lower.rfind("following", 0) == 0 ||
                lower.rfind("staying", 0) == 0 ||
                lower.rfind("holding", 0) == 0 ||
                lower.rfind("fleeing", 0) == 0 ||
                lower.rfind("attacking", 0) == 0 ||
                lower.rfind("drinking", 0) == 0 ||
                lower.rfind("eating", 0) == 0 ||
                lower.rfind("looting", 0) == 0 ||
                lower.rfind("casting", 0) == 0 ||
                lower.rfind("moving to", 0) == 0 ||
                lower.rfind("resting", 0) == 0 ||
                lower.rfind("ready", 0) == 0 ||
                lower.rfind("waiting", 0) == 0);
    }

    struct ChatDeduplicator
    {
        std::unordered_map<uint32, std::pair<std::string, std::chrono::steady_clock::time_point>> lastHeard;

        bool IsDuplicate(uint32 botGuid, std::string const& msg)
        {
            auto now = std::chrono::steady_clock::now();
            auto it = lastHeard.find(botGuid);
            if (it != lastHeard.end())
            {
                if (it->second.first == msg)
                {
                    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.second).count();
                    if (elapsedMs < 2000)
                        return true;
                }
            }
            lastHeard[botGuid] = {msg, now};
            return false;
        }
    };

    ChatDeduplicator sChatDeduplicator;
}

bool AzerothFriendPlayerScript::OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg, Player* receiver)
{
    if (!sAzerothFriendConfig->enable || !player || !receiver || IsAddonTraffic(msg) ||
        IsProtocolCarrier(player->GetName()))
        return true;

    // A bot's whisper to another player or master is NEVER a player_command
    if (sAzerothFriendConfig->IsBotControlled(player->GetName()))
        return true;

    // Direct whisper to bot
    if (AzerothFriendShared::CanControl(player, receiver))
    {
        uint32 botGuid = receiver->GetGUID().GetCounter();
        if (sChatDeduplicator.IsDuplicate(botGuid, msg))
            return true;

        // When the bot is claimed as a mechanical companion, natural language / whisper cognitive queueing is disabled (0 tokens)
        if (AzerothFriendBotController::IsBotClaimed(receiver))
            return true;

        std::string payload = "{\"text\":\"" + AzerothFriendShared::EscapeJsonString(msg) + "\",\"channel\":\"whisper\"}";

        // Priority 1: High-priority immediate interrupt without event storm
        AzerothFriendAiControl::NotifyMasterMoved(player->GetGUID().GetCounter());
        AzerothFriendShared::InvalidateControl(receiver);
        sAFDispatcher->InterruptBotPlan(botGuid, "Player whisper: " + msg, false);
        AzerothFriendShared::QueueEvent(botGuid, "player_command", 1, player->GetGUID().GetCounter(), player->GetName(), payload);
    }
    return true;
}

bool AzerothFriendPlayerScript::OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg, Group* group)
{
    if (!sAzerothFriendConfig->enable || !player || !group || IsAddonTraffic(msg) ||
        IsProtocolCarrier(player->GetName()))
        return true;

#if AF_HAS_PLAYERBOTS
    bool isBot = sAzerothFriendConfig->IsBotControlled(player->GetName()) || (PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr);
#else
    bool isBot = sAzerothFriendConfig->IsBotControlled(player->GetName());
#endif

    // If sender is a bot, it can NEVER issue a player_command.
    // Suppress mechanical command acknowledgments completely so they don't trigger dialogue/interrupts.
    if (isBot && (!sAzerothFriendConfig->hearOtherBots || IsBotCommandAck(msg)))
        return true;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != player && sAzerothFriendConfig->IsBotControlled(member->GetName()))
        {
            uint32 botGuid = member->GetGUID().GetCounter();
            if (sChatDeduplicator.IsDuplicate(botGuid, msg))
                continue;

            // When the bot is claimed as a mechanical companion, natural language / party chat cognitive queueing is disabled (0 tokens)
            if (AzerothFriendBotController::IsBotClaimed(member))
                continue;

            std::string payload = "{\"text\":\"" + AzerothFriendShared::EscapeJsonString(msg) + "\",\"channel\":\"party\",\"speaker\":\"" + AzerothFriendShared::EscapeJsonString(player->GetName()) + "\"}";

            std::string botNameLower = member->GetName();
            std::transform(botNameLower.begin(), botNameLower.end(), botNameLower.begin(), ::tolower);
            std::string msgLower = msg;
            std::transform(msgLower.begin(), msgLower.end(), msgLower.begin(), ::tolower);

            bool isMaster = (player->GetGUID() == group->GetLeaderGUID());
#if AF_HAS_PLAYERBOTS
            PlayerbotAI* memberAi = PlayerbotsMgr::instance().GetPlayerbotAI(member);
            if (memberAi && memberAi->GetMaster() == player)
                isMaster = true;
#endif

            // Only actual human players/masters can issue commands
            bool isCommand = AzerothFriendShared::CanControl(player, member) && (isMaster ||
                             (msgLower.find(botNameLower) != std::string::npos ||
                              msgLower.find("come") != std::string::npos ||
                              msgLower.find("attack") != std::string::npos ||
                              msgLower.find("stop") != std::string::npos ||
                              msgLower.find("stay") != std::string::npos ||
                              msgLower.find("wait") != std::string::npos ||
                              msgLower.find("hold") != std::string::npos ||
                              msgLower.find("follow") != std::string::npos ||
                              msgLower.find("rest") != std::string::npos ||
                              msgLower.find("drink") != std::string::npos ||
                              msgLower.find("eat") != std::string::npos ||
                              msgLower.find("loot") != std::string::npos ||
                             msgLower.find("mount") != std::string::npos ||
                             msgLower.find("help") != std::string::npos ||
                             msgLower.find("run") != std::string::npos ||
                             IsCastIntent(msgLower)));

            if (isCommand)
            {
                AzerothFriendShared::InvalidateControl(member);
                sAFDispatcher->InterruptBotPlan(botGuid, "Party chat command: " + msg, false);
                AzerothFriendShared::QueueEvent(botGuid, "player_command", 1, player->GetGUID().GetCounter(), player->GetName(), payload);
            }
            else
            {
                AzerothFriendShared::QueueEvent(botGuid, "dialogue_heard", 2, player->GetGUID().GetCounter(), player->GetName(), payload);
            }
        }
    }
    return true;
}

bool AzerothFriendPlayerScript::OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 /*language*/, std::string& msg)
{
    if (!sAzerothFriendConfig->enable || !player || IsAddonTraffic(msg) ||
        IsProtocolCarrier(player->GetName()))
        return true;

#if AF_HAS_PLAYERBOTS
    bool isBot = sAzerothFriendConfig->IsBotControlled(player->GetName()) || (PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr);
#else
    bool isBot = sAzerothFriendConfig->IsBotControlled(player->GetName());
#endif
    if (isBot && (!sAzerothFriendConfig->hearOtherBots || IsBotCommandAck(msg)))
        return true;

    float hearDist = sAzerothFriendConfig->scanRadius;
    for (const auto& botName : sAzerothFriendConfig->controlledBots)
    {
        Player* bot = ObjectAccessor::FindPlayerByName(botName);
        if (bot && bot != player && bot->IsInWorld() && bot->IsWithinDistInMap(player, hearDist))
        {
            uint32 botGuid = bot->GetGUID().GetCounter();
            if (sChatDeduplicator.IsDuplicate(botGuid, msg))
                continue;

            // When the bot is claimed as a mechanical companion, natural language / say cognitive queueing is disabled (0 tokens)
            if (AzerothFriendBotController::IsBotClaimed(bot))
                continue;

            std::string payload = "{\"text\":\"" + AzerothFriendShared::EscapeJsonString(msg) + "\",\"channel\":\"say\",\"speaker\":\"" + AzerothFriendShared::EscapeJsonString(player->GetName()) + "\"}";

            std::string botNameLower = botName;
            std::transform(botNameLower.begin(), botNameLower.end(), botNameLower.begin(), ::tolower);
            std::string msgLower = msg;
            std::transform(msgLower.begin(), msgLower.end(), msgLower.begin(), ::tolower);

            if (AzerothFriendShared::CanControl(player, bot) &&
                (msgLower.find(botNameLower) != std::string::npos || IsCastIntent(msgLower)))
            {
                AzerothFriendShared::InvalidateControl(bot);
                sAFDispatcher->InterruptBotPlan(botGuid, "Say chat mention: " + msg, false);
                AzerothFriendShared::QueueEvent(botGuid, "player_command", 1, player->GetGUID().GetCounter(), player->GetName(), payload);
            }
            else
            {
                AzerothFriendShared::QueueEvent(botGuid, "dialogue_heard", 2, player->GetGUID().GetCounter(), player->GetName(), payload);
            }
        }
    }
    return true;
}

void AzerothFriendPlayerScript::OnPlayerEnterCombat(Player* player, Unit* enemy)
{
    if (!sAzerothFriendConfig->enable || !player)
        return;

    if (sAzerothFriendConfig->IsBotControlled(player->GetName()))
    {
        uint32 botGuid = player->GetGUID().GetCounter();
        std::string enemyName = enemy ? enemy->GetName() : "";
        uint32 enemyGuid = enemy ? enemy->GetGUID().GetCounter() : 0;
        std::string payload = "{\"enemy_guid\":" + std::to_string(enemyGuid) + ",\"enemy_name\":\"" + AzerothFriendShared::EscapeJsonString(enemyName) + "\"}";

        AzerothFriendShared::QueueEvent(botGuid, "combat_enter", 2, enemyGuid, enemyName, payload);
    }
    else
    {
        // Player is a master/leader: notify companion bots that master engaged an enemy!
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (member && member != player && sAzerothFriendConfig->IsBotControlled(member->GetName()))
                {
                    if (AzerothFriendBotController::IsBotClaimed(member))
                        continue;

                    uint32 botGuid = member->GetGUID().GetCounter();
                    std::string enemyName = enemy ? enemy->GetName() : "";
                    uint32 enemyGuid = enemy ? enemy->GetGUID().GetCounter() : 0;
                    std::string payload = "{\"master_name\":\"" + AzerothFriendShared::EscapeJsonString(player->GetName()) +
                                          "\",\"master_guid\":" + std::to_string(player->GetGUID().GetCounter()) +
                                          ",\"enemy_guid\":" + std::to_string(enemyGuid) +
                                          ",\"enemy_name\":\"" + AzerothFriendShared::EscapeJsonString(enemyName) + "\"}";
                    AzerothFriendShared::QueueEvent(botGuid, "master_engaged", 1, enemyGuid, enemyName, payload);
                }
            }
        }
    }
}

void AzerothFriendPlayerScript::OnPlayerLeaveCombat(Player* player)
{
    if (!sAzerothFriendConfig->enable || !player)
        return;

    if (sAzerothFriendConfig->IsBotControlled(player->GetName()))
    {
        uint32 botGuid = player->GetGUID().GetCounter();
        AzerothFriendShared::QueueEvent(botGuid, "combat_leave", 3, 0, "", "{}");
    }
}

void AzerothFriendPlayerScript::OnPlayerKilledByCreature(Creature* killer, Player* player)
{
    if (!sAzerothFriendConfig->enable || !player)
        return;

    if (sAzerothFriendConfig->IsBotControlled(player->GetName()))
    {
        uint32 botGuid = player->GetGUID().GetCounter();
        std::string killerName = killer ? killer->GetName() : "";
        sAFDispatcher->InterruptBotPlan(botGuid, "Died in combat");
        AzerothFriendShared::QueueEvent(botGuid, "plan_interrupted", 1, killer ? killer->GetGUID().GetCounter() : 0, killerName, "{\"reason\":\"died\"}");
    }
}

void AzerothFriendPlayerScript::OnPlayerDuelRequest(Player* target, Player* challenger)
{
    if (!sAzerothFriendConfig->enable || !target || !challenger)
        return;

    if (sAzerothFriendConfig->IsBotControlled(target->GetName()))
    {
        uint32 botGuid = target->GetGUID().GetCounter();
        std::string payload = "{\"challenger_name\":\"" + AzerothFriendShared::EscapeJsonString(challenger->GetName()) +
                              "\",\"challenger_guid\":" + std::to_string(challenger->GetGUID().GetCounter()) +
                              ",\"challenger_level\":" + std::to_string(challenger->GetLevel()) + "}";

        sAFDispatcher->InterruptBotPlan(botGuid, "Duel requested by " + challenger->GetName());
        AzerothFriendShared::QueueEvent(botGuid, "duel_requested", 1, challenger->GetGUID().GetCounter(), challenger->GetName(), payload);
    }
}

void AzerothFriendPlayerScript::OnPlayerDuelStart(Player* player1, Player* player2)
{
    if (!sAzerothFriendConfig->enable || !player1 || !player2)
        return;

    Player* bot = nullptr;
    Player* opponent = nullptr;

    if (sAzerothFriendConfig->IsBotControlled(player1->GetName()))
    {
        bot = player1;
        opponent = player2;
    }
    else if (sAzerothFriendConfig->IsBotControlled(player2->GetName()))
    {
        bot = player2;
        opponent = player1;
    }

    if (bot && opponent)
    {
        uint32 botGuid = bot->GetGUID().GetCounter();
        std::string payload = "{\"opponent_name\":\"" + AzerothFriendShared::EscapeJsonString(opponent->GetName()) +
                              "\",\"opponent_guid\":" + std::to_string(opponent->GetGUID().GetCounter()) + "}";
        AzerothFriendShared::QueueEvent(botGuid, "duel_started", 2, opponent->GetGUID().GetCounter(), opponent->GetName(), payload);
    }
}

void AzerothFriendPlayerScript::OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType /*type*/)
{
    if (!sAzerothFriendConfig->enable || !winner || !loser)
        return;

    Player* bot = nullptr;
    bool botWon = false;
    Player* other = nullptr;

    if (sAzerothFriendConfig->IsBotControlled(winner->GetName()))
    {
        bot = winner;
        botWon = true;
        other = loser;
    }
    else if (sAzerothFriendConfig->IsBotControlled(loser->GetName()))
    {
        bot = loser;
        botWon = false;
        other = winner;
    }

    if (bot && other)
    {
        uint32 botGuid = bot->GetGUID().GetCounter();
        std::string payload = "{\"bot_won\":" + std::string(botWon ? "true" : "false") +
                              ",\"opponent_name\":\"" + AzerothFriendShared::EscapeJsonString(other->GetName()) + "\"}";
        AzerothFriendShared::QueueEvent(botGuid, "duel_ended", 2, other->GetGUID().GetCounter(), other->GetName(), payload);
    }
}

bool AzerothFriendPlayerScript::OnPlayerCanInitTrade(Player* player, Player* target)
{
    if (!sAzerothFriendConfig->enable || !player || !target)
        return true;

    if (sAzerothFriendConfig->IsBotControlled(target->GetName()))
    {
        uint32 botGuid = target->GetGUID().GetCounter();
        std::string payload = "{\"initiator_name\":\"" + AzerothFriendShared::EscapeJsonString(player->GetName()) +
                              "\",\"initiator_guid\":" + std::to_string(player->GetGUID().GetCounter()) + "}";

        AzerothFriendShared::QueueEvent(botGuid, "trade_requested", 1, player->GetGUID().GetCounter(), player->GetName(), payload);
    }
    return true;
}

void AzerothFriendPlayerScript::OnPlayerLogin(Player* player)
{
    if (!sAzerothFriendConfig->enable || !player)
        return;

    std::string primaryBotName = sAzerothFriendConfig->GetPrimaryBotName();
    Player* bot = ObjectAccessor::FindPlayerByName(primaryBotName.c_str());
    if (bot && bot->IsInWorld())
    {
        sAFEnvironment->BroadcastTelemetry(bot, player);
    }
    else
    {
        // Send state packet so companion addon initializes with the correct bot name from server conf
        std::string payload = "Name: " + primaryBotName +
                              "|Race: 0|Class: 0|Level: 0|Health: 0%|Power: 0%|Zone: Offline|Subzone: Offline|Coordinates: 0, 0, 0|Combat: NOT IN COMBAT|Target: None";
        std::string msg = "[FRIEND_STATE] " + payload;
        if (player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage(msg.c_str());
    }
}

void AzerothFriendPlayerScript::OnPlayerLogout(Player* player)
{
    if (!sAzerothFriendConfig->enable || !player)
        return;

    uint32 playerGuid = player->GetGUID().GetCounter();

    // 1. If the logging out character is directly a controlled bot
    if (sAzerothFriendConfig->IsBotControlled(player->GetName()))
    {
        sAFDispatcher->InterruptBotPlan(playerGuid, "Bot logged out", false);
    }

    // 2. If the logging out character is the master of any controlled bot, interrupt their active plans
    for (std::string const& botName : sAzerothFriendConfig->controlledBots)
    {
        Player* bot = ObjectAccessor::FindPlayerByName(botName.c_str());
        if (bot && bot != player && bot->IsInWorld())
        {
            Player* master = nullptr;
#if AF_HAS_PLAYERBOTS
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (ai)
                master = ai->GetMaster();
#endif
            if (!master && bot->GetGroup())
                master = ObjectAccessor::FindConnectedPlayer(bot->GetGroup()->GetLeaderGUID());

            if (master && master->GetGUID() == player->GetGUID())
            {
                uint32 botGuid = bot->GetGUID().GetCounter();
                sAFDispatcher->InterruptBotPlan(botGuid, "Master logged out", false);
            }
        }
    }
}

AzerothFriendWorldScript::AzerothFriendWorldScript() : WorldScript("AzerothFriendWorldScript") {}

void AzerothFriendWorldScript::OnAfterConfigLoad(bool /*reload*/)
{
    sAzerothFriendConfig->Load();

    // Hydrate the durable Claim gate even when the explicit SQL-compatibility
    // rollback mode has the socket transport disabled. Until this async read
    // completes, bridge access deliberately fails closed.
    if (sAzerothFriendConfig->enable)
        sAFLiveState->RefreshBotControlAsync();

    // The RAM-first listener needs the port and shared secret from the config, so
    // it starts here (after the config file is loaded) and stops when disabled.
    if (sAzerothFriendConfig->liveStateEnable)
        sAFLiveState->Start();
    else
        sAFLiveState->Stop();
}

void AzerothFriendWorldScript::OnUpdate(uint32 diff)
{
    sAFDispatcher->Update(diff);
}

void AzerothFriendWorldScript::OnShutdown()
{
    // Flush nothing to the socket: the bridge treats a closed transport as stale
    // and waits for a fresh baseline after the next start.
    sAFLiveState->Stop();

    // Hand every leased bot back to its own AI before the world goes down.
    sAFDispatcher->Shutdown();
}
