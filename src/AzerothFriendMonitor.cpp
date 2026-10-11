#include "AzerothFriendMonitor.h"
#include "AzerothFriendAutopilot.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendEnvironment.h"
#include "AzerothFriendShared.h"
#include "AzerothFriendUtilities.h"

#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#if __has_include("Playerbots.h")
    #include "Playerbots.h"
    #include "PlayerbotAI.h"
    #include "PlayerbotMgr.h"
    #define AF_HAS_PLAYERBOTS 1
#else
    #define AF_HAS_PLAYERBOTS 0
#endif

#include <algorithm>
#include <ctime>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr size_t kPieceBytes = 220;

    struct MonitorConfig
    {
        bool     enable      = true;
        uint32_t minSecurity = SEC_PLAYER;
    };
    MonitorConfig g_mc;

    struct Watch
    {
        uint64_t bot          = 0;
        uint32_t lastTeleport = 0;
    };

    std::mutex                          g_watchMutex;
    std::unordered_map<uint64_t, Watch> g_watches;
    std::vector<uint64_t>               g_pendingRelease;
    uint32_t                            g_watchTimer = 0;

    void Send(Player* to, const std::string& prefix, const std::string& body)
    {
        if (!to || !to->GetSession())
            return;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, to, to, prefix + "\t" + body);
        to->GetSession()->SendPacket(&data);
    }

    std::string Clean(std::string s)
    {
        for (char& c : s)
        {
            if (c == '\t' || c == '\n' || c == '\r')
                c = ' ';
            else if (c == '|')
                c = '/';
        }
        return s;
    }

    bool CanInspect(Player* viewer, Player* bot)
    {
        if (!viewer || !viewer->GetSession() || !bot || !bot->IsInWorld())
            return false;
        // Personal HUD access must not grant the imported GM monitor's powers.
        return viewer->GetSession()->GetSecurity() >= SEC_GAMEMASTER ||
               AzerothFriendShared::CanControl(viewer, bot);
    }

    void SendPage(Player* to, const std::string& prefix, uint64_t guid, const std::string& page, const std::vector<std::string>& lines)
    {
        Send(to, prefix, SafeFormat("B\t{}\t{}", guid, page));
        for (const std::string& raw : lines)
        {
            const std::string line = Clean(raw);
            size_t pos = 0;
            do
            {
                size_t len = std::min(kPieceBytes, line.size() - pos);
                while (len > 0 && pos + len < line.size() &&
                       (static_cast<unsigned char>(line[pos + len]) & 0xC0) == 0x80)
                    --len;
                if (len == 0)
                    len = std::min(kPieceBytes, line.size() - pos);
                const bool last = pos + len >= line.size();
                Send(to, prefix, std::string("P\t") + (last ? "e" : "c") + "\t" + line.substr(pos, len));
                pos += len;
            } while (pos < line.size());
        }
        Send(to, prefix, SafeFormat("Z\t{}\t{}", guid, page));
    }

    void ReleaseView(Player* watcher)
    {
        if (!watcher || !watcher->IsInWorld())
            return;

        watcher->SetViewpoint(watcher, false);
    }

    void StepWatch(uint64_t watcherGuid, Watch& w, uint32_t /*now*/, std::vector<uint64_t>& finished)
    {
        Player* watcher = ObjectAccessor::FindConnectedPlayer(ObjectGuid(watcherGuid));
        if (!watcher || !watcher->IsInWorld())
        {
            finished.push_back(watcherGuid);
            return;
        }

        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(w.bot));
        if (!bot || !bot->IsInWorld())
        {
            ReleaseView(watcher);
            finished.push_back(watcherGuid);
            return;
        }

        if (watcher->GetMapId() != bot->GetMapId())
        {
            ReleaseView(watcher);
            finished.push_back(watcherGuid);
        }
    }

    void HandleRequest(Player* player, const std::string& prefix, const std::string& payload)
    {
        std::vector<std::string> parts = SplitString(payload, '\t');
        if (parts.empty())
            return;

        const std::string& cmd = parts[0];

        if (cmd == "HELLO")
        {
            Send(player, prefix, Autopilot_IsActive() ? "H\t1\tactive" : "H\t1\tinactive");
        }
        else if (cmd == "LIST")
        {
            Send(player, prefix, "LB");
            auto list = Autopilot_MonitorList();
            size_t count = 0;
            for (const auto& row : list)
            {
                Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(row.guid));
                if (!CanInspect(player, bot))
                    continue;
                Send(player, prefix, SafeFormat("L\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                                               row.guid, Clean(row.name), uint32_t(row.level), uint32_t(row.cls),
                                               Utf8Truncate(Clean(row.zone), 48), Clean(row.tier), Clean(row.state),
                                               Utf8Truncate(Clean(row.doing), 80)));
                ++count;
            }
            Send(player, prefix, SafeFormat("LZ\t{}", count));
        }
        else if (cmd == "PAGE" && parts.size() >= 3)
        {
            uint64_t guid = 0;
            try { guid = std::stoull(parts[1]); } catch (...) { return; }
            const std::string& pageName = parts[2];

            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
            if (!bot || !bot->IsInWorld())
            {
                Send(player, prefix, SafeFormat("G\t{}", guid));
                return;
            }
            if (!CanInspect(player, bot))
            {
                Send(player, prefix, "M\tCompanion unavailable or not owned by you.");
                return;
            }

            std::vector<std::string> lines;
            if (Autopilot_MonitorPage(bot, pageName, lines))
                SendPage(player, prefix, guid, pageName, lines);
            if (pageName == "overview")
                AzerothFriendEnvironment::instance()->BroadcastTelemetry(bot, player);
        }
        else if (cmd == "WATCH" && parts.size() >= 2)
        {
            uint64_t guid = 0;
            try { guid = std::stoull(parts[1]); } catch (...) { return; }

            if (player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
            {
                Send(player, prefix, "W\toff\t0\t-\tGM permissions required for camera watch");
                return;
            }

            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
            if (!bot || !bot->IsInWorld())
            {
                Send(player, prefix, "W\toff\t0\t-\tBot not found");
                return;
            }

            if (player->GetMapId() != bot->GetMapId())
            {
                Send(player, prefix, "W\toff\t0\t-\tBot is on another map");
                return;
            }

            player->SetViewpoint(bot, true);
            {
                std::lock_guard<std::mutex> lock(g_watchMutex);
                Watch w;
                w.bot = guid;
                g_watches[player->GetGUID().GetRawValue()] = w;
            }
            Send(player, prefix, SafeFormat("W\ton\t{}\t{}\tWatching", guid, bot->GetName()));
        }
        else if (cmd == "UNWATCH")
        {
            ReleaseView(player);
            {
                std::lock_guard<std::mutex> lock(g_watchMutex);
                g_watches.erase(player->GetGUID().GetRawValue());
            }
            Send(player, prefix, "W\toff\t0\t-\tCamera returned");
        }
        else if (cmd == "GOTO" && parts.size() >= 2)
        {
            uint64_t guid = 0;
            try { guid = std::stoull(parts[1]); } catch (...) { return; }

            if (player->GetSession()->GetSecurity() < SEC_GAMEMASTER)
            {
                Send(player, prefix, "M\tGM permissions required for GOTO");
                return;
            }

            if (Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid)))
            {
                player->TeleportTo(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetOrientation());
                Send(player, prefix, SafeFormat("M\tTeleported to {}", bot->GetName()));
            }
        }
        else if (cmd == "CMD" && parts.size() >= 3)
        {
            const std::string& sub = parts[1];
            const std::string& name = parts[2];
            if (!Autopilot_MonitorCommand(player, sub, name))
                Send(player, prefix, "M\tMonitor command rejected; see companion diagnostics.");
        }
    }
}

void Monitor_LoadConfig()
{
    g_mc.enable      = sConfigMgr->GetOption<bool>("AzerothFriend.Monitor.Enable", true);
    g_mc.minSecurity = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Monitor.MinSecurity", SEC_PLAYER);
}

void Monitor_Update(uint32_t diff)
{
    g_watchTimer += diff;
    if (g_watchTimer < 1000)
        return;
    g_watchTimer = 0;

    const uint32_t now = static_cast<uint32_t>(time(nullptr));
    std::vector<std::pair<uint64_t, Watch>> watches;
    {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        if (g_watches.empty())
            return;
        watches.assign(g_watches.begin(), g_watches.end());
    }

    std::vector<uint64_t> finished;
    for (auto& [watcher, w] : watches)
        StepWatch(watcher, w, now, finished);

    if (!finished.empty())
    {
        std::lock_guard<std::mutex> lock(g_watchMutex);
        for (uint64_t w : finished)
            g_watches.erase(w);
    }
}

AzerothFriendMonitorScript::AzerothFriendMonitorScript()
    : PlayerScript("AzerothFriendMonitorScript",
                   { PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_BEFORE_TELEPORT, PLAYERHOOK_ON_LOGOUT })
{
}

bool AzerothFriendMonitorScript::OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver)
{
    if (!player || !player->GetSession() || language != LANG_ADDON || type != CHAT_MSG_WHISPER || receiver != player)
        return true;

    std::string prefix;
    if (msg.compare(0, 5, "AFUI\t") == 0)
        prefix = "AFUI";
    else if (msg.compare(0, 5, "OAPM\t") == 0)
        prefix = "OAPM";
    else
        return true;

    if (!g_mc.enable || player->GetSession()->GetSecurity() < g_mc.minSecurity)
    {
        Send(player, prefix, "H\t0");
        Send(player, prefix, "M\tMonitor disabled or insufficient privileges.");
        return false;
    }

    HandleRequest(player, prefix, msg.substr(5));
    return false;
}

bool AzerothFriendMonitorScript::OnPlayerBeforeTeleport(Player* player, uint32 /*mapid*/, float /*x*/, float /*y*/,
                                                        float /*z*/, float /*orientation*/, uint32 /*options*/, Unit* /*target*/)
{
    if (!player)
        return true;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_watchMutex);
    if (g_watches.count(guid))
    {
        ReleaseView(player);
        g_watches.erase(guid);
    }
    return true;
}

void AzerothFriendMonitorScript::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_watchMutex);
    g_watches.erase(guid);
}
