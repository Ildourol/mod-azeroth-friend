#include "AzerothFriendShared.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendPlayerbot.h"
#include "AzerothFriendLiveState.h"
#include "WorldSession.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "Map.h"
#include "Group.h"
#include "Chat.h"
#include "Log.h"
#include <sstream>
#include <set>

namespace AzerothFriendShared
{
    bool CanControl(Player* issuer, Player* bot)
    {
        if (!bot || !sAzerothFriendConfig->enable)
            return false;
        if (issuer && issuer == bot)
            return true;
        if (!sAzerothFriendConfig->IsBotControlled(bot->GetName()))
            return false;
#if AF_HAS_PLAYERBOTS
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
        if (!ai)
            return false;
        if (!issuer || (issuer->GetSession() && issuer->GetSession()->GetSecurity() >= SEC_GAMEMASTER))
            return true;
        // Only human players may provide authority; ambient bot dialogue is data.
        if (PlayerbotsMgr::instance().GetPlayerbotAI(issuer))
            return false;

        if (ai->GetMaster() == issuer)
            return true;

        Group* group = bot->GetGroup();
        if (group && (group->IsLeader(issuer->GetGUID()) || group->IsMember(issuer->GetGUID())))
            return true;

        if (issuer->GetSelectedPlayer() == bot)
            return true;

        AzerothFriend::BotControlRecord control;
        if (sAFLiveState->GetBotControl(bot->GetGUID().GetCounter(), control) && control.masterGuid == issuer->GetGUID().GetCounter())
            return true;

        return false;
#else
        return false;
#endif
    }

    void InvalidateControl(Player* bot)
    {
        if (bot)
        {
            // Enqueue-only: the world thread must never wait for the database.
            // Statements are processed in submission order, so owner commands
            // still persist in the order they were issued.
            CharacterDatabase.Execute(("UPDATE azeroth_friend_bots SET control_revision = control_revision + 1 "
                "WHERE bot_guid = " + std::to_string(bot->GetGUID().GetCounter())).c_str());
            // The local control generation is invalidated immediately, so
            // in-flight plans cannot execute against the previous revision.
            sAFLiveState->RequestFullRefresh(bot->GetGUID().GetCounter());
            sAFLiveState->RefreshBotControlAsync();
        }
    }

    bool EqualCaseInsensitive(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle)
    {
        if (needle.empty())
            return true;
        if (haystack.size() < needle.size())
            return false;
        auto it = std::search(
            haystack.begin(), haystack.end(),
            needle.begin(), needle.end(),
            [](char ch1, char ch2) {
                return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2));
            }
        );
        return it != haystack.end();
    }

    std::string EscapeJsonString(std::string const& input)
    {
        std::ostringstream ss;
        for (char c : input)
        {
            switch (c)
            {
                case '"': ss << "\\\""; break;
                case '\\': ss << "\\\\"; break;
                case '\b': ss << "\\b"; break;
                case '\f': ss << "\\f"; break;
                case '\n': ss << "\\n"; break;
                case '\r': ss << "\\r"; break;
                case '\t': ss << "\\t"; break;
                default:
                    if ('\x00' <= c && c <= '\x1f')
                    {
                        // Control characters
                        ss << "\\u" << std::hex << (int)c;
                    }
                    else
                    {
                        ss << c;
                    }
                    break;
            }
        }
        return ss.str();
    }

    std::string EscapeSqlString(std::string const& input)
    {
        std::ostringstream ss;
        for (char c : input)
        {
            if (c == '\'')
                ss << "\\'";
            else if (c == '\\')
                ss << "\\\\";
            else
                ss << c;
        }
        return ss.str();
    }

    Creature* FindCreatureByCounter(Map* map, uint32 lowGuid)
    {
        if (!map) return nullptr;
        for (auto const& pair : map->GetCreatureBySpawnIdStore())
        {
            Creature* c = pair.second;
            if (c && c->GetGUID().GetCounter() == lowGuid)
                return c;
        }
        return nullptr;
    }

    GameObject* FindGameObjectByCounter(Map* map, uint32 lowGuid)
    {
        if (!map) return nullptr;
        for (auto const& pair : map->GetGameObjectBySpawnIdStore())
        {
            GameObject* go = pair.second;
            if (go && go->GetGUID().GetCounter() == lowGuid)
                return go;
        }
        return nullptr;
    }

    Player* FindPlayerByCounter(uint32 lowGuid)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
        return ObjectAccessor::FindConnectedPlayer(guid);
    }

    void QueueEvent(uint32 botGuid, std::string const& eventType, uint8 priority,
                    uint32 sourceGuid, std::string const& sourceName, std::string const& payloadJson)
    {
        // Claim is a hard per-bot bridge disconnect. This central gate covers
        // every producer (chat, combat, idle, duel, trade and diagnostics) and
        // uses only the RAM control cache on the world thread.
        if (!botGuid || !sAFLiveState->IsBotBridgeEnabled(botGuid))
        {
            if (botGuid && sAzerothFriendConfig->debug)
                LOG_DEBUG("server.loading", "[AzerothFriend] Dropped event '{}' for bridge-disconnected bot {}",
                          eventType, botGuid);
            return;
        }

        std::string query = "INSERT INTO azeroth_friend_events "
                            "(bot_guid, event_type, priority, source_guid, source_name, payload_json, status) "
                            "VALUES (" +
                            std::to_string(botGuid) + ", '" +
                            eventType + "', " +
                            std::to_string(priority) + ", " +
                            (sourceGuid ? std::to_string(sourceGuid) : "NULL") + ", " +
                            (sourceName.empty() ? "NULL" : "'" + EscapeSqlString(sourceName) + "'") + ", " +
                            (payloadJson.empty() ? "'{}'" : "'" + EscapeSqlString(payloadJson) + "'") + ", 'pending')";

        CharacterDatabase.Execute(query.c_str());

        if (sAzerothFriendConfig->debug)
        {
            LOG_DEBUG("server.loading", "[AzerothFriend] Queued event '{}' for bot {} (priority {})",
                      eventType, botGuid, priority);
        }
    }

    void SendTelemetry(Player* bot, std::string const& tag, std::string const& payload, Player* targetPlayer)
    {
        if (!bot || !bot->IsInWorld() || !sAzerothFriendConfig->enable)
            return;

        std::string msg = "[" + tag + "] " + payload;
        static uint64 sequence = 0;
        std::string serial = std::to_string(++sequence);
        std::string encoded;
        static char const hex[] = "0123456789abcdef";
        for (unsigned char c : msg)
        {
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
        std::set<uint32> delivered;
        auto deliver = [&](Player* recipient)
        {
            if (!recipient || !recipient->GetSession() || !CanControl(recipient, bot))
                return;
            if (!delivered.insert(recipient->GetGUID().GetCounter()).second)
                return;
            size_t total = (encoded.size() + 179) / 180;
            if (total > 512)
                return;
            for (size_t part = 0; part < total; ++part)
            {
                std::string packet = "[AF1] " + serial + ":" + std::to_string(part + 1) + ":" +
                    std::to_string(total) + ":" + encoded.substr(part * 180, 180);
                ChatHandler(recipient->GetSession()).SendSysMessage(packet.c_str());
            }
        };

        // If a specific target player was requested, deliver directly to them
        if (targetPlayer && targetPlayer->GetSession())
        {
            deliver(targetPlayer);
            return;
        }

#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            deliver(ai->GetMaster());
#endif

        // 1. Group members
        if (Group* grp = bot->GetGroup())
        {
            for (GroupReference* itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (member && member->GetSession() && member != bot)
                {
                    deliver(member);
                }
            }
        }

        // 2. Bot master if connected and not in group
        // Stored owner (master) guid: RAM cache first; SQL only in rollback mode.
        uint32 masterGuid = 0;
        if (sAFLiveState->IsRunning())
        {
            AzerothFriend::BotControlRecord control;
            if (sAFLiveState->GetBotControl(bot->GetGUID().GetCounter(), control))
                masterGuid = control.masterGuid;
        }
        else
        {
            QueryResult res = CharacterDatabase.Query(
                ("SELECT master_guid FROM azeroth_friend_bots WHERE bot_guid = " +
                 std::to_string(bot->GetGUID().GetCounter())).c_str());
            if (res)
                masterGuid = res->Fetch()[0].Get<uint32>();
        }

        if (masterGuid)
        {
            Player* master = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(masterGuid));
            if (master && master->GetSession() && master != bot)
            {
                if (!bot->GetGroup() || !bot->GetGroup()->IsMember(master->GetGUID()))
                {
                    deliver(master);
                }
            }
        }

        // 3. Nearby connected players targeting the bot
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!p || !p->GetSession() || p == bot || p->GetMap() != bot->GetMap())
                continue;

            if (p->GetTarget() == bot->GetGUID() && bot->IsWithinDistInMap(p, 60.0f))
            {
                if (bot->GetGroup() && bot->GetGroup()->IsMember(p->GetGUID()))
                    continue;

                deliver(p);
            }
        }
    }

    void SendAddonError(Player* recipient, std::string const& errorMsg, std::string const& botName)
    {
        if (!recipient || !recipient->GetSession() || !sAzerothFriendConfig->enable)
            return;

        std::string msg = "[FRIEND_ERROR] error:" + errorMsg;
        if (!botName.empty())
            msg += "|bot:" + botName;

        static uint64 errSequence = 0;
        std::string serial = std::to_string(++errSequence);
        std::string encoded;
        static char const hex[] = "0123456789abcdef";
        for (unsigned char c : msg)
        {
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }

        size_t total = (encoded.size() + 179) / 180;
        if (total > 512)
            return;

        for (size_t part = 0; part < total; ++part)
        {
            std::string packet = "[AF1] " + serial + ":" + std::to_string(part + 1) + ":" +
                std::to_string(total) + ":" + encoded.substr(part * 180, 180);
            ChatHandler(recipient->GetSession()).SendSysMessage(packet.c_str());
        }
    }
}
