#include "AzerothFriendAutopilot.h"
#include "AzerothFriendAutopilotSchema.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendErrands.h"
#include "AzerothFriendLiveState.h"
#include "AzerothFriendMovement.h"
#include "AzerothFriendProgress.h"
#include "AzerothFriendRoute.h"
#include "AzerothFriendStrategy.h"
#include "AzerothFriendTravel.h"
#include "AzerothFriendUtilities.h"
#include "AzerothFriendWorldIndex.h"

#include "CellImpl.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#if __has_include("Playerbots.h")
    #include "Playerbots.h"
    #include "PlayerbotAI.h"
    #include "PlayerbotAIConfig.h"
    #include "PlayerbotMgr.h"
    #include "RandomPlayerbotMgr.h"
    #define AF_HAS_PLAYERBOTS 1
#else
    #define AF_HAS_PLAYERBOTS 0
#endif

#include <algorithm>
#include <ctime>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    enum class Tier : uint8_t { Dormant, Background, Foreground };
    enum class Scope : uint8_t { Range, Zone, Map, World, Always };

    struct AutopilotConfig
    {
        bool     enable                   = true;
        bool     debug                    = false;
        bool     allowMasterEnroll        = true;
        uint32_t randomBotPercent         = 100;
        uint32_t minLevel                 = 1;
        uint32_t maxLevel                 = 80;
        uint32_t maxEnrolled              = 0;

        std::unordered_set<uint32_t>    guilds;
        std::unordered_set<uint32_t>    accounts;
        std::unordered_set<std::string> includeNames;
        std::unordered_set<std::string> excludeNames;

        uint32_t sweepIntervalMs         = 1000;
        uint32_t botsPerSweep            = 25;
        uint32_t snapshotIntervalMinutes = 10;
        uint32_t flushIntervalSeconds    = 60;
        uint32_t snapshotRetention       = 500;
        uint32_t eventRetention          = 300;

        bool     control                 = true;
        uint32_t withRealPlayer          = 1;
        bool     noTeleport              = true;
        bool     noRandomize             = true;
        bool     noHandouts              = true;
        bool     keepOnline              = true;
        uint32_t corpseRunMinutes        = 10;

        uint32_t decisionIntervalMinutes = 15;
        uint32_t backgroundMinutes       = 30;
        uint32_t defaultPlanMinutes      = 30;
        uint32_t quickReplanSeconds      = 60;
        float    foregroundRange         = 100.0f;

        Scope    foregroundScope         = Scope::Zone;
        Scope    backgroundScope         = Scope::Map;
        Tier     minimumTier             = Tier::Dormant;
        bool     requireRealPlayer       = false;

        bool     selectRealPlayerGuilds  = false;
        Tier     realGuildTier           = Tier::Background;
        bool     groups                  = true;
        uint32_t playerRequests          = 1;
        uint32_t obeySecurity            = 0;

        uint32_t alertDeaths             = 3;
        uint32_t alertDeathWindowMinutes = 15;
        uint32_t alertDurabilityPct      = 20;
        uint32_t alertFreeBagSlots       = 2;
        uint32_t alertCooldownMinutes    = 30;
        uint32_t alertIdleMinutes        = 5;
        uint32_t goalStaleMinutes        = 120;
    };
    AutopilotConfig g_cfg;

    enum : uint8_t
    {
        MODE_RULES = 0,
        MODE_ON    = 1,
        MODE_OFF   = 2
    };

    struct Row
    {
        uint64_t    botGuid       = 0;
        uint8_t     mode          = MODE_RULES;
        uint8_t     enrolled      = 0;
        std::string source;
        std::string style;
        std::string outlook;
        std::string profile;
        uint32_t    killsTotal    = 0;
        uint32_t    deathsTotal   = 0;
        uint32_t    questsTotal   = 0;
        uint32_t    dungeonsTotal = 0;
        std::string doing;
        uint32_t    doingSince    = 0;
        std::string decidedBy;
        std::string strategies;
        std::string lastResults;
        std::string goalKind;
        std::string goalTarget;
        uint32_t    goalTargetId  = 0;
        uint32_t    goalValue     = 0;
        uint32_t    goalBaseline  = 0;
        uint32_t    goalSetAt     = 0;
        std::string goalText;
        std::string lastReason;
        std::string baseline;
        uint32_t    planUntil     = 0;
        uint32_t    lastPlanAt    = 0;
        uint32_t    enrolledAt    = 0;
        uint32_t    updatedAt     = 0;

        bool        dirty         = false;
    };

    struct OnlineState
    {
        Tier                  tier            = Tier::Dormant;
        uint32_t              tierUpdatedAt   = 0;
        uint32_t              nextSnapshotAt  = 0;
        uint32_t              deathCount      = 0;
        uint32_t              lastDeathTime   = 0;
        bool                  corpseRunActive = false;
        uint32_t              corpseRunStart  = 0;
        AutopilotErrand       currentErrand;
        std::vector<AutopilotErrand> errandQueue;
    };

    std::mutex                                g_mutex;
    std::unordered_map<uint64_t, Row>         g_rows;
    std::unordered_map<uint64_t, OnlineState> g_online;
    std::vector<uint64_t>                     g_sweepRoster;
    size_t                                    g_sweepCursor   = 0;
    uint32_t                                  g_sweepTimer    = 0;
    uint32_t                                  g_flushTimer    = 0;

    uint32_t Now() { return static_cast<uint32_t>(time(nullptr)); }

    bool IsBotPlayer(Player* player)
    {
        if (!player)
            return false;
        if (player->GetSession() && player->GetSession()->IsHeadless())
            return true;
#if AF_HAS_PLAYERBOTS
        return PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr;
#else
        return false;
#endif
    }

#if !AF_HAS_PLAYERBOTS
    bool IsRealPlayer(Player* player)
    {
        return player != nullptr;
    }
#endif

    Tier CalculateTier(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return Tier::Dormant;

        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !::IsRealPlayer(player) || !player->IsInWorld())
                continue;

            if (player->GetMapId() == bot->GetMapId())
            {
                if (player->GetZoneId() == bot->GetZoneId())
                {
                    if (bot->GetDistance(player) <= g_cfg.foregroundRange)
                        return Tier::Foreground;
                    return Tier::Foreground;
                }
                return Tier::Background;
            }
        }

        return g_cfg.requireRealPlayer ? Tier::Dormant : Tier::Background;
    }

    void HandleCorpseRun(Player* bot, OnlineState& state, uint32_t now)
    {
        if (bot->IsAlive())
        {
            state.corpseRunActive = false;
            return;
        }

        if (!bot->HasCorpse())
        {
            if (!state.corpseRunActive)
            {
                bot->RepopAtGraveyard();
                state.corpseRunActive = true;
                state.corpseRunStart  = now;
            }
            return;
        }

        Corpse* corpse = bot->GetCorpse();
        if (!corpse)
            return;

        const float dist = bot->GetDistance(corpse);
        if (dist <= 35.0f)
        {
            bot->ResurrectPlayer(0.5f, false);
            bot->SpawnCorpseBones();
            state.corpseRunActive = false;
            return;
        }

        if (now - state.corpseRunStart > g_cfg.corpseRunMinutes * 60)
        {
            bot->ResurrectPlayer(0.5f, true);
            state.corpseRunActive = false;
            return;
        }

#if AF_HAS_PLAYERBOTS
        AzerothFriendMovement::MoveTo(GET_PLAYERBOT_AI(bot), corpse->GetPositionX(), corpse->GetPositionY(), corpse->GetPositionZ(), true);
#else
        (void)corpse;
#endif
    }

    void SweepBot(Player* bot, uint32_t now)
    {
        const uint64_t guid = bot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_mutex);

        Row& row = g_rows[guid];
        row.botGuid = guid;
        OnlineState& state = g_online[guid];

        if (row.mode == MODE_OFF)
        {
            if (row.enrolled)
            {
                row.enrolled = 0;
                row.dirty = true;
                AzerothFriendStrategy::AutopilotStrategy_Remove(bot);
            }
            return;
        }

        if (row.mode == MODE_ON || (!row.enrolled && g_cfg.enable))
        {
            if (!row.enrolled)
            {
                row.enrolled   = 1;
                row.enrolledAt = now;
                row.source     = (row.mode == MODE_ON) ? "command" : "rule";
                row.dirty      = true;
                AzerothFriendStrategy::AutopilotStrategy_Apply(bot);
                AzerothFriendProgress::QueueEvent(guid, "enroll", "Enrolled into Autopilot");
            }
        }

        if (!row.enrolled)
            return;

        state.tier = CalculateTier(bot);

        if (!bot->IsAlive())
        {
            HandleCorpseRun(bot, state, now);
            return;
        }

        if (now >= state.nextSnapshotAt)
        {
            AzerothFriendProgress::QueueSnapshot(AzerothFriendProgress::Capture(bot));
            state.nextSnapshotAt = now + (g_cfg.snapshotIntervalMinutes * 60);
        }

        if (g_cfg.control)
        {
#if AF_HAS_PLAYERBOTS
            AzerothFriendErrands::UpdateErrand(bot, GET_PLAYERBOT_AI(bot), state.currentErrand, now);
#endif
        }
    }
}

void Autopilot_LoadConfig()
{
    AutopilotConfig c;
    c.enable                   = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.Enable", true);
    c.debug                    = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.Debug", false);
    c.allowMasterEnroll        = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.AllowMasterEnroll", true);
    c.randomBotPercent         = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.RandomBotPercent", 100);
    c.minLevel                 = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.MinLevel", 1);
    c.maxLevel                 = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.MaxLevel", 80);
    c.maxEnrolled              = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.MaxEnrolled", 0);
    c.sweepIntervalMs          = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.SweepIntervalMs", 1000);
    c.botsPerSweep             = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.BotsPerSweep", 25);
    c.snapshotIntervalMinutes  = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.SnapshotIntervalMinutes", 10);
    c.flushIntervalSeconds     = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.FlushIntervalSeconds", 60);
    c.control                  = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.Control", true);
    c.noTeleport               = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.NoTeleport", true);
    c.noRandomize              = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.NoRandomize", true);
    c.noHandouts               = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.NoHandouts", true);
    c.keepOnline               = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.KeepOnline", true);
    c.corpseRunMinutes         = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.CorpseRunMinutes", 10);
    g_cfg = c;
}

void Autopilot_Load()
{
    AzerothFriendAutopilotSchema::EnsureTables();
    AzerothFriendStrategy::Register();
    Autopilot_LoadConfig();

    std::lock_guard<std::mutex> lock(g_mutex);
    g_rows.clear();

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, mode, enrolled, source, style, outlook, doing, doing_since, goal_kind, goal_target, goal_value "
            "FROM azeroth_friend_autopilot"))
    {
        do
        {
            Field* f = result->Fetch();
            Row r;
            r.botGuid      = f[0].Get<uint64>();
            r.mode         = f[1].Get<uint8>();
            r.enrolled     = f[2].Get<uint8>();
            r.source       = f[3].Get<std::string>();
            r.style        = f[4].Get<std::string>();
            r.outlook      = f[5].Get<std::string>();
            r.doing        = f[6].Get<std::string>();
            r.doingSince   = f[7].Get<uint32>();
            r.goalKind     = f[8].Get<std::string>();
            r.goalTarget   = f[9].Get<std::string>();
            r.goalValue    = f[10].Get<uint32>();
            g_rows[r.botGuid] = r;
        } while (result->NextRow());
    }

    LOG_INFO("module.azerothfriend", "Autopilot loaded {} rows.", g_rows.size());
}

void Autopilot_Update(uint32_t diff)
{
    if (!g_cfg.enable)
        return;

    const uint32_t now = Now();
    g_sweepTimer += diff;
    if (g_sweepTimer >= g_cfg.sweepIntervalMs)
    {
        g_sweepTimer = 0;
        if (g_sweepRoster.empty() || g_sweepCursor >= g_sweepRoster.size())
        {
            g_sweepRoster.clear();
            for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                if (player && IsBotPlayer(player))
                    g_sweepRoster.push_back(player->GetGUID().GetRawValue());
            g_sweepCursor = 0;
        }

        const size_t limit = std::min<size_t>(g_cfg.botsPerSweep, g_sweepRoster.size());
        for (size_t i = 0; i < limit && !g_sweepRoster.empty(); ++i)
        {
            if (g_sweepCursor >= g_sweepRoster.size())
                g_sweepCursor = 0;
            const uint64_t guid = g_sweepRoster[g_sweepCursor++];
            if (Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid)))
                SweepBot(bot, now);
        }
    }

    g_flushTimer += diff;
    if (g_flushTimer >= g_cfg.flushIntervalSeconds * 1000)
    {
        g_flushTimer = 0;
        AzerothFriendProgress::Flush(g_cfg.snapshotRetention, g_cfg.eventRetention);
        Autopilot_SaveAll();
    }
}

void Autopilot_SaveAll()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

    for (auto& [guid, r] : g_rows)
    {
        if (!r.dirty)
            continue;

        std::string sql = SafeFormat(
            "REPLACE INTO azeroth_friend_autopilot (bot_guid, mode, enrolled, source, style, outlook, "
            "doing, doing_since, goal_kind, goal_target, goal_value, enrolled_at, updated_at) "
            "VALUES ({}, {}, {}, '{}', '{}', '{}', '{}', {}, '{}', '{}', {}, {}, {})",
            r.botGuid, uint32_t(r.mode), uint32_t(r.enrolled), r.source, r.style, r.outlook,
            r.doing, r.doingSince, r.goalKind, r.goalTarget, r.goalValue, r.enrolledAt, Now());

        trans->Append(sql.c_str());
        r.dirty = false;
    }

    CharacterDatabase.CommitTransaction(trans);
}

bool Autopilot_IsActive()
{
    return g_cfg.enable;
}

std::vector<std::string> Autopilot_InactiveReasons()
{
    std::vector<std::string> reasons;
    if (!g_cfg.enable)
        reasons.push_back("AzerothFriend.Autopilot.Enable is false");
    return reasons;
}

std::string Autopilot_ChatContext(Player* bot)
{
    if (!bot)
        return "";

    const uint64_t guid = bot->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_rows.find(guid);
    if (it == g_rows.end() || !it->second.enrolled)
        return "";

    const Row& r = it->second;
    std::string out = "Currently ";
    out += r.doing.empty() ? "wandering Azeroth" : r.doing;
    if (!r.goalText.empty())
        out += " aiming to " + r.goalText;

    return out;
}

int Autopilot_RequestMode(Player* bot, Player* player)
{
    if (!g_cfg.enable || !bot || !player || !IsBotPlayer(bot) || IsBotPlayer(player))
        return 0;

    return g_cfg.playerRequests;
}

void Autopilot_NotePlayerRequest(uint64_t botGuid, uint64_t /*playerGuid*/, const std::string& what, const std::string& words)
{
    AzerothFriendProgress::QueueEvent(botGuid, "request", what + ": " + words);
}

void Autopilot_NoteWhisper(Player* from, Player* to, const std::string& text)
{
    if (!from || !to)
        return;

    if (IsBotPlayer(to))
        AzerothFriendProgress::QueueEvent(to->GetGUID().GetRawValue(), "whisper", from->GetName() + ": " + text);
}

std::vector<AutopilotMonitorRow> Autopilot_MonitorList()
{
    std::vector<AutopilotMonitorRow> list;
    std::unordered_set<uint64_t> seen;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        // 1. All enrolled autopilot bots
        for (const auto& [guid, r] : g_rows)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
            if (!bot)
                continue;

            seen.insert(guid);
            AutopilotMonitorRow row;
            row.guid  = guid;
            row.name  = bot->GetName();
            row.level = bot->GetLevel();
            row.cls   = bot->getClass();
            row.zone  = bot->GetZoneId();
            row.doing = r.doing.empty() ? "Adventuring" : r.doing;

            auto onIt = g_online.find(guid);
            if (onIt != g_online.end())
            {
                switch (onIt->second.tier)
                {
                    case Tier::Foreground: row.tier = "foreground"; break;
                    case Tier::Background: row.tier = "background"; break;
                    default:               row.tier = "dormant";    break;
                }
            }
            else
                row.tier = "dormant";

            if (bot->IsInCombat() || !bot->getAttackers().empty())
                row.state = "fighting";
            else if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
                row.state = "corpse run";
            else if (!bot->IsAlive())
                row.state = "dead";
            else if (bot->IsInFlight())
                row.state = "flying";
            else if (bot->GetTransport())
                row.state = "aboard";
            else if (bot->isMoving())
                row.state = "travelling";
            else
                row.state = "idle";

            list.push_back(row);
        }
    }

    // 2. All registered companions known to RAM live state
    auto controlled = sAFLiveState->GetControlledBots();
    for (const auto& ctrl : controlled)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ctrl.botGuid));
        if (!bot)
            continue;

        uint64_t guid = bot->GetGUID().GetRawValue();
        if (seen.count(guid))
            continue;
        seen.insert(guid);

        AutopilotMonitorRow row;
        row.guid  = guid;
        row.name  = bot->GetName();
        row.level = bot->GetLevel();
        row.cls   = bot->getClass();
        row.zone  = bot->GetZoneId();
        row.doing = ctrl.currentGoal.empty() ? "Idle" : ctrl.currentGoal;
        row.tier  = ctrl.autonomyEnabled ? "background" : "dormant";

        if (bot->IsInCombat() || !bot->getAttackers().empty())
            row.state = "fighting";
        else if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            row.state = "corpse run";
        else if (!bot->IsAlive())
            row.state = "dead";
        else if (bot->IsInFlight())
            row.state = "flying";
        else if (bot->GetTransport())
            row.state = "aboard";
        else if (bot->isMoving())
            row.state = "travelling";
        else
            row.state = "idle";

        list.push_back(row);
    }

    return list;
}

bool Autopilot_MonitorPage(Player* bot, const std::string& page, std::vector<std::string>& lines)
{
    if (!bot)
        return false;

    const uint64_t guid = bot->GetGUID().GetRawValue();
    const uint32_t now  = Now();
    Row r;
    OnlineState ob;
    bool hasRow = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_rows.find(guid);
        if (it != g_rows.end())
        {
            r = it->second;
            hasRow = true;
        }
        auto onIt = g_online.find(guid);
        if (onIt != g_online.end())
            ob = onIt->second;
    }

    AzerothFriend::BotControlRecord ctrl;
    bool hasCtrl = sAFLiveState->GetBotControl(bot->GetGUID().GetCounter(), ctrl);

    if (page == "overview")
    {
        lines.push_back("# Bot Profile: " + bot->GetName());
        lines.push_back("character: level " + std::to_string(bot->GetLevel()) + " " +
                        (bot->getGender() == GENDER_FEMALE ? "female" : "male") + " " +
                        std::to_string(bot->getRace()) + " " + std::to_string(bot->getClass()));
        lines.push_back("where: " + std::to_string(bot->GetZoneId()) + " - map " + std::to_string(bot->GetMapId()) +
                        " (" + std::to_string(bot->GetPositionX()) + ", " + std::to_string(bot->GetPositionY()) + ", " +
                        std::to_string(bot->GetPositionZ()) + ")" + (bot->GetTransport() ? " - on transport" : ""));
        lines.push_back("health: " + std::to_string(uint32_t(bot->GetHealthPct())) + "% | power " +
                        std::to_string(bot->GetPower(bot->getPowerType())) + "/" +
                        std::to_string(bot->GetMaxPower(bot->getPowerType())) + " | gold " +
                        std::to_string(bot->GetMoney() / 10000));

        std::string attackers;
        for (Unit* u : bot->getAttackers())
            if (u)
                attackers += (attackers.empty() ? "" : ", ") + u->GetName();
        lines.push_back("combat: " + std::string(bot->IsInCombat() ? "In Combat" : "Out of Combat") +
                        " | attackers: " + (attackers.empty() ? "none" : attackers) +
                        " | target: " + (bot->GetVictim() ? bot->GetVictim()->GetName() : "none"));
        lines.push_back("movement: moving " + std::string(bot->isMoving() ? "yes" : "no"));

        lines.push_back("# Autopilot & Agency");
        lines.push_back("enrolled: " + std::string(hasRow && r.enrolled ? "yes" : "no") +
                        " | mode " + std::to_string(r.mode) +
                        " | autonomy: " + (hasCtrl && ctrl.autonomyEnabled ? "ON" : "OFF") +
                        " | claim: " + (hasCtrl && ctrl.mode == "claimed" ? "CLAIMED" : "RELEASED"));
        lines.push_back("doing: " + (r.doing.empty() ? (hasCtrl && !ctrl.currentGoal.empty() ? ctrl.currentGoal : "Idle") : r.doing));
        lines.push_back("goal: " + (r.goalText.empty() ? (hasCtrl && !ctrl.longTermGoal.empty() ? ctrl.longTermGoal : "None") : r.goalText));

        lines.push_back("# Watchdogs");
        lines.push_back("since enrollment: " + std::to_string(r.killsTotal) + " kills, " +
                        std::to_string(r.deathsTotal) + " deaths, " + std::to_string(r.questsTotal) + " quests");
        if (bot->HasPlayerFlag(PLAYER_FLAGS_GHOST))
        {
            if (Corpse* corpse = bot->GetCorpse())
                lines.push_back("corpse: " + (corpse->GetMapId() != bot->GetMapId() ?
                                "body on another map" :
                                "body " + std::to_string(uint32_t(bot->GetExactDist2d(corpse))) + " yd away"));
        }
        return true;
    }
    else if (page == "travel")
    {
        lines.push_back("# Active Errand");
        lines.push_back("active: " + std::string(ob.currentErrand.active ? "yes" : "no"));
        lines.push_back("kind: " + std::to_string(static_cast<uint32_t>(ob.currentErrand.kind)));
        lines.push_back("bound for: " + (ob.currentErrand.label.empty() ? "None" : ob.currentErrand.label));
        lines.push_back("# Trip Legs");
        lines.push_back("status: " + std::string(bot->isMoving() ? "In Transit" : "Arrived / Idle"));
        lines.push_back("movement: moving " + std::string(bot->isMoving() ? "yes" : "no"));
        lines.push_back("# Corpse Run");
        lines.push_back("corpse run: " + std::string(ob.corpseRunActive ? "ACTIVE" : "none"));
        return true;
    }
    else if (page == "planner")
    {
        lines.push_back("# Last LLM Exchange");
        lines.push_back("status: Synchronized via RAM live-state bridge");
        lines.push_back("last plan: " + std::to_string(r.lastPlanAt ? (now - r.lastPlanAt) : 0) + "s ago");
        lines.push_back("# Cognitive Thought");
        std::string thought = sAFLiveState->GetThought(bot->GetGUID().GetCounter());
        lines.push_back(thought.empty() ? "(none recorded)" : thought);
        lines.push_back("# Action Pipeline History");
        std::string history = sAFLiveState->GetHistory(bot->GetGUID().GetCounter());
        lines.push_back(history.empty() ? "(none recorded)" : history);
        lines.push_back("# Planner Results");
        lines.push_back(r.lastResults.empty() ? "(none)" : r.lastResults);
        return true;
    }
    else if (page == "events")
    {
        lines.push_back("# Orders and What They Did");
        lines.push_back(r.lastResults.empty() ? "  (no recent order results)" : "  " + r.lastResults);
        lines.push_back("# Progress Diary (newest last)");
        auto events = AzerothFriendProgress::LoadEvents(guid, 15);
        if (events.empty())
            lines.push_back("  (no progress diary events yet)");
        else
        {
            for (const auto& e : events)
                lines.push_back("  [" + e.type + "] " + e.detail);
        }
        return true;
    }

    lines.push_back("unknown page: " + page);
    return true;
}

bool Autopilot_MonitorCommand(Player* gm, const std::string& sub, const std::string& name)
{
    if (!gm)
        return false;

    Player* bot = ObjectAccessor::FindPlayerByName(name, false);
    if (!bot || !IsBotPlayer(bot))
    {
        ChatHandler(gm->GetSession()).SendSysMessage("Bot not found or not a valid playerbot.");
        return false;
    }

    const uint64_t guid = bot->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    Row& r = g_rows[guid];
    r.botGuid = guid;

    if (sub == "on")
    {
        r.mode = MODE_ON;
        r.enrolled = 1;
        r.dirty = true;
        AzerothFriendStrategy::AutopilotStrategy_Apply(bot);
        ChatHandler(gm->GetSession()).PSendSysMessage("Autopilot forced ON for %s.", bot->GetName().c_str());
        return true;
    }
    else if (sub == "off")
    {
        r.mode = MODE_OFF;
        r.enrolled = 0;
        r.dirty = true;
        AzerothFriendStrategy::AutopilotStrategy_Remove(bot);
        ChatHandler(gm->GetSession()).PSendSysMessage("Autopilot forced OFF for %s.", bot->GetName().c_str());
        return true;
    }
    return false;
}

// --------------------------------------------------------------------------
// Chat Commands
// --------------------------------------------------------------------------

class AutopilotChatCommands
{
public:
    static bool HandleAutopilotStatusCommand(ChatHandler* handler, Optional<std::string> name)
    {
        Player* bot = nullptr;
        if (name)
            bot = ObjectAccessor::FindPlayerByName(*name, false);
        else
            bot = handler->GetPlayer();

        if (!bot || !IsBotPlayer(bot))
        {
            handler->SendSysMessage("Select or specify a valid playerbot.");
            return true;
        }

        const uint64_t guid = bot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_rows.find(guid);
        if (it == g_rows.end() || !it->second.enrolled)
        {
            handler->PSendSysMessage("%s is NOT enrolled in Autopilot.", bot->GetName().c_str());
            return true;
        }

        const Row& r = it->second;
        handler->PSendSysMessage("Autopilot [%s]: Doing '%s', Goal '%s', Mode %u",
                                 bot->GetName().c_str(), r.doing.c_str(), r.goalText.c_str(), r.mode);
        return true;
    }

    static bool HandleAutopilotOnCommand(ChatHandler* handler, Optional<std::string> name)
    {
        Player* bot = name ? ObjectAccessor::FindPlayerByName(*name, false) : handler->GetPlayer();
        if (!bot || !IsBotPlayer(bot))
        {
            handler->SendSysMessage("Specify a valid playerbot.");
            return true;
        }

        const uint64_t guid = bot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_mutex);
        Row& r = g_rows[guid];
        r.botGuid = guid;
        r.mode = MODE_ON;
        r.enrolled = 1;
        r.dirty = true;
        AzerothFriendStrategy::AutopilotStrategy_Apply(bot);
        handler->PSendSysMessage("Autopilot forced ON for %s.", bot->GetName().c_str());
        return true;
    }

    static bool HandleAutopilotOffCommand(ChatHandler* handler, Optional<std::string> name)
    {
        Player* bot = name ? ObjectAccessor::FindPlayerByName(*name, false) : handler->GetPlayer();
        if (!bot || !IsBotPlayer(bot))
        {
            handler->SendSysMessage("Specify a valid playerbot.");
            return true;
        }

        const uint64_t guid = bot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_mutex);
        Row& r = g_rows[guid];
        r.botGuid = guid;
        r.mode = MODE_OFF;
        r.enrolled = 0;
        r.dirty = true;
        AzerothFriendStrategy::AutopilotStrategy_Remove(bot);
        handler->PSendSysMessage("Autopilot forced OFF for %s.", bot->GetName().c_str());
        return true;
    }

    static bool HandleAutopilotListCommand(ChatHandler* handler)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        uint32_t count = 0;
        for (const auto& [guid, r] : g_rows)
        {
            if (!r.enrolled)
                continue;
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
            handler->PSendSysMessage("- %s (L%u): %s",
                                     bot ? bot->GetName().c_str() : "Offline",
                                     bot ? bot->GetLevel() : 0,
                                     r.doing.c_str());
            ++count;
        }
        handler->PSendSysMessage("Total enrolled bots: %u", count);
        return true;
    }
};

Acore::ChatCommands::ChatCommandTable const& Autopilot_CommandTable()
{
    static Acore::ChatCommands::ChatCommandTable const autopilotCommandTable =
    {
        { "status", AutopilotChatCommands::HandleAutopilotStatusCommand, SEC_PLAYER,     Console::No },
        { "on",     AutopilotChatCommands::HandleAutopilotOnCommand,     SEC_GAMEMASTER, Console::No },
        { "off",    AutopilotChatCommands::HandleAutopilotOffCommand,    SEC_GAMEMASTER, Console::No },
        { "list",   AutopilotChatCommands::HandleAutopilotListCommand,   SEC_PLAYER,     Console::No }
    };
    return autopilotCommandTable;
}

// --------------------------------------------------------------------------
// Script Hooks
// --------------------------------------------------------------------------

AutopilotPlayerScript::AutopilotPlayerScript()
    : PlayerScript("AutopilotPlayerScript",
                   { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_CREATURE_KILL,
                     PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_ON_LEVEL_CHANGED })
{
}

void AutopilotPlayerScript::OnPlayerLogin(Player* player)
{
    if (!player || !IsBotPlayer(player))
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_rows.find(guid);
    if (it != g_rows.end() && it->second.enrolled)
    {
        AzerothFriendStrategy::AutopilotStrategy_Apply(player);
    }
}

void AutopilotPlayerScript::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_online.erase(guid);
}

void AutopilotPlayerScript::OnPlayerCreatureKill(Player* killer, Creature* victim)
{
    if (!killer || !IsBotPlayer(killer) || !victim)
        return;

    const uint64_t guid = killer->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_rows.find(guid);
    if (it != g_rows.end() && it->second.enrolled)
    {
        ++it->second.killsTotal;
        it->second.dirty = true;
    }
}

void AutopilotPlayerScript::OnPlayerPVPKill(Player* killer, Player* /*killed*/)
{
    if (!killer || !IsBotPlayer(killer))
        return;

    const uint64_t guid = killer->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_rows.find(guid);
    if (it != g_rows.end() && it->second.enrolled)
    {
        ++it->second.killsTotal;
        it->second.dirty = true;
    }
}

void AutopilotPlayerScript::OnPlayerJustDied(Player* player)
{
    if (!player || !IsBotPlayer(player))
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_rows.find(guid);
    if (it != g_rows.end() && it->second.enrolled)
    {
        ++it->second.deathsTotal;
        it->second.dirty = true;
        AzerothFriendProgress::QueueEvent(guid, "death", "Died at " + player->GetName());
    }
}

void AutopilotPlayerScript::OnPlayerCompleteQuest(Player* player, Quest const* quest)
{
    if (!player || !IsBotPlayer(player) || !quest)
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_rows.find(guid);
    if (it != g_rows.end() && it->second.enrolled)
    {
        ++it->second.questsTotal;
        it->second.dirty = true;
        AzerothFriendProgress::QueueEvent(guid, "quest_complete", quest->GetTitle());
    }
}

void AutopilotPlayerScript::OnPlayerLevelChanged(Player* player, uint8 oldLevel)
{
    if (!player || !IsBotPlayer(player))
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_rows.find(guid);
    if (it != g_rows.end() && it->second.enrolled)
    {
        it->second.dirty = true;
        AzerothFriendProgress::QueueEvent(guid, "levelup", SafeFormat("Level {} -> {}", oldLevel, player->GetLevel()));
    }
}

void AutopilotPlayerScript::OnPlayerStoreNewItem(Player* player, Item* item, uint32 count)
{
    if (!player || !IsBotPlayer(player) || !item)
        return;
}

void AutopilotPlayerScript::OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement)
{
    if (!player || !IsBotPlayer(player) || !achievement)
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    AzerothFriendProgress::QueueEvent(guid, "achievement", achievement->name[LOCALE_enUS]);
}

void AutopilotPlayerScript::OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 lowGuid)
{
    trans->Append(SafeFormat("DELETE FROM azeroth_friend_autopilot WHERE bot_guid = {}", lowGuid).c_str());
    trans->Append(SafeFormat("DELETE FROM azeroth_friend_autopilot_snapshots WHERE bot_guid = {}", lowGuid).c_str());
    trans->Append(SafeFormat("DELETE FROM azeroth_friend_autopilot_events WHERE bot_guid = {}", lowGuid).c_str());
}

AutopilotGuildScript::AutopilotGuildScript() : GuildScript("AutopilotGuildScript") { }
void AutopilotGuildScript::OnAddMember(Guild* /*guild*/, Player* /*player*/, uint8& /*plRank*/) { }

AutopilotServerScript::AutopilotServerScript() : ServerScript("AutopilotServerScript") { }
void AutopilotServerScript::OnPacketSent(WorldSession* /*session*/, WorldPacket const& /*packet*/) { }
