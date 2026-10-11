#include "AzerothFriendAutopilot.h"
#include "AzerothFriendAutopilotSchema.h"
#include "AzerothFriendActionDispatcher.h"
#include "AzerothFriendAiControl.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendErrands.h"
#include "AzerothFriendEnvironment.h"
#include "AzerothFriendLiveState.h"
#include "AzerothFriendMovement.h"
#include "AzerothFriendProgress.h"
#include "AzerothFriendRoute.h"
#include "AzerothFriendStrategy.h"
#include "AzerothFriendShared.h"
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
#include "World.h"

#if __has_include("Playerbots.h")
    #include "Playerbots.h"
    #include "PlayerbotAI.h"
    #include "PlayerbotAIConfig.h"
    #include "PlayerbotMgr.h"
    #include "RandomPlayerbotMgr.h"
#if __has_include("PlayerbotRepository.h")
    #include "PlayerbotRepository.h"
    #define AF_HAS_PLAYERBOT_REPOSITORY 1
#else
    #define AF_HAS_PLAYERBOT_REPOSITORY 0
#endif
    #define AF_HAS_PLAYERBOTS 1
#else
    #define AF_HAS_PLAYERBOTS 0
    #define AF_HAS_PLAYERBOT_REPOSITORY 0
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

    inline std::string Lower(std::string const& s)
    {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
        return out;
    }

    inline uint64_t StableHash(uint64_t x)
    {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    inline std::unordered_set<uint32_t> ParseIds(const std::string& text)
    {
        std::unordered_set<uint32_t> out;
        std::stringstream ss(text);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch) { return !std::isspace(ch); }));
            token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), token.end());
            if (!token.empty())
            {
                try { out.insert(static_cast<uint32_t>(std::stoul(token))); }
                catch (...) { LOG_ERROR("module.azerothfriend", "Autopilot: '{}' is not a valid numeric ID.", token); }
            }
        }
        return out;
    }

    inline std::unordered_set<std::string> ParseNames(const std::string& text)
    {
        std::unordered_set<std::string> out;
        std::stringstream ss(text);
        std::string token;
        while (std::getline(ss, token, ','))
        {
            token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch) { return !std::isspace(ch); }));
            token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), token.end());
            if (!token.empty())
                out.insert(Lower(token));
        }
        return out;
    }

    inline Scope ParseScope(const std::string& str, Scope fallback)
    {
        const std::string s = Lower(str);
        if (s == "range")  return Scope::Range;
        if (s == "zone")   return Scope::Zone;
        if (s == "map")    return Scope::Map;
        if (s == "world")  return Scope::World;
        if (s == "always") return Scope::Always;
        return fallback;
    }

    inline Tier ParseTier(const std::string& str, Tier fallback)
    {
        const std::string s = Lower(str);
        if (s == "dormant")    return Tier::Dormant;
        if (s == "background") return Tier::Background;
        if (s == "foreground") return Tier::Foreground;
        return fallback;
    }

    struct AutopilotConfig
    {
        bool     enable                   = false; // Default safe opt-in
        bool     debug                    = false;
        bool     allowMasterEnroll        = true;
        bool     altBots                  = false;
        uint32_t randomBotPercent         = 0;     // Default safe opt-in (0%)
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
        bool                  evaluated       = false;
        bool                  markerOurs      = false;
        bool                  reviveHeld      = false;
        uint32_t              teleportDeferredAt = 0;
        AutopilotErrand       currentErrand;
        std::vector<AutopilotErrand> errandQueue;
    };

    std::mutex                                g_mutex;
    std::unordered_map<uint64_t, Row>         g_rows;
    std::unordered_map<uint64_t, OnlineState> g_online;
    std::unordered_set<uint64_t>              g_realOnline;
    std::vector<uint64_t>                     g_sweepRoster;
    size_t                                    g_sweepCursor   = 0;
    uint32_t                                  g_sweepTimer    = 0;
    uint32_t                                  g_flushTimer    = 0;
    uint32_t                                  g_enrolledCount = 0;
    uint32_t                                  g_cappedCount   = 0;

    uint32_t Now() { return static_cast<uint32_t>(time(nullptr)); }

    std::string Escape(std::string s)
    {
        CharacterDatabase.EscapeString(s);
        return s;
    }

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
        return player != nullptr && !IsBotPlayer(player);
    }
#endif

    bool InScope(Scope scope, Player* bot, Player* p)
    {
        switch (scope)
        {
            case Scope::Always:
            case Scope::World:  return true;
            case Scope::Map:    return p->GetMapId() == bot->GetMapId();
            case Scope::Zone:   return p->GetMapId() == bot->GetMapId() && p->GetZoneId() == bot->GetZoneId();
            case Scope::Range:  return p->GetMapId() == bot->GetMapId() && bot->GetDistance(p) <= g_cfg.foregroundRange;
        }
        return false;
    }

    bool IsCappedSource(const std::string& source)
    {
        return source == "random" || source == "guild" || source == "account" || source == "alt";
    }

    void CountEnrolled(Row const& row, int delta)
    {
        if (delta > 0)
        {
            ++g_enrolledCount;
            if (IsCappedSource(row.source))
                ++g_cappedCount;
        }
        else
        {
            if (g_enrolledCount > 0)
                --g_enrolledCount;
            if (IsCappedSource(row.source) && g_cappedCount > 0)
                --g_cappedCount;
        }
    }

    Tier CalculateTier(Player* bot)
    {
        if (!bot || !bot->IsInWorld())
            return Tier::Dormant;

        if (!g_cfg.requireRealPlayer)
            return Tier::Foreground;

        Tier tier = Tier::Dormant;
        auto raise = [&tier](Tier t) { if (static_cast<uint8_t>(t) > static_cast<uint8_t>(tier)) tier = t; };

        if (g_cfg.backgroundScope == Scope::Always)
            raise(Tier::Background);
        if (g_cfg.foregroundScope == Scope::Always)
            raise(Tier::Foreground);

        for (uint64_t guid : g_realOnline)
        {
            if (tier == Tier::Foreground)
                break;

            Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
            if (!p || !p->IsInWorld())
                continue;

            if (bot->GetGuildId() && p->GetGuildId() == bot->GetGuildId())
                raise(Tier::Foreground);
            else if (InScope(g_cfg.foregroundScope, bot, p))
                raise(Tier::Foreground);
            else if (InScope(g_cfg.backgroundScope, bot, p))
                raise(Tier::Background);
        }

        raise(g_cfg.minimumTier);
        return tier;
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

    bool WithRealPlayer(Player* bot)
    {
        if (!bot)
            return false;
        if (Group* grp = bot->GetGroup())
        {
            for (auto const& slot : grp->GetMemberSlots())
            {
                if (Player* member = ObjectAccessor::FindConnectedPlayer(slot.guid))
                {
                    if (!IsBotPlayer(member))
                        return true;
                }
            }
        }
        return false;
    }

    bool InInstance(Player* bot)
    {
        if (!bot)
            return false;
        return bot->GetMap() && (bot->GetMap()->Instanceable() || bot->InBattleground());
    }

    void HandBack(PlayerbotAI* ai, Row* row, OnlineState* ob)
    {
#if AF_HAS_PLAYERBOTS
        if (!ai)
            return;

        ai->ResetStrategies();
        Player* bot = ai->GetBot();
#if AF_HAS_PLAYERBOT_REPOSITORY
        if (bot && !sRandomPlayerbotMgr.IsRandomBot(bot))
            PlayerbotRepository::instance().Load(ai);
#endif
        if (row && !row->baseline.empty())
        {
            AzerothFriendStrategy::RestoreEngine(ai, row->baseline, BOT_STATE_NON_COMBAT);
            AzerothFriendStrategy::RestoreEngine(ai, row->baseline, BOT_STATE_COMBAT);
#if AF_HAS_PLAYERBOT_REPOSITORY
            if (bot && !sRandomPlayerbotMgr.IsRandomBot(bot))
                PlayerbotRepository::instance().Save(ai);
#endif
        }

        if (ob)
        {
            if (ob->currentErrand.active)
                AzerothFriendErrands::StopErrand(ai, ob->currentErrand);
            ob->currentErrand = AutopilotErrand();
            ob->errandQueue.clear();
            if (bot && sRandomPlayerbotMgr.IsRandomBot(bot))
                sRandomPlayerbotMgr.SetValue(bot->GetGUID().GetCounter(), "revive", 0);
            ob->reviveHeld = false;
        }
#else
        (void)ai; (void)row; (void)ob;
#endif
    }

    std::string EvaluateRules(Player* bot, Row const* row)
    {
        if (!g_cfg.enable || !bot)
            return "";

        // Claimed or leased bots are managed by owner / companion system
        AzerothFriend::BotControlRecord control;
        if (sAFLiveState->GetBotControl(bot->GetGUID().GetCounter(), control) &&
            (!control.bridgeEnabled || control.mode == "claimed"))
            return "";
        if (AzerothFriendAiControl::IsLeased(bot))
            return "";

        if (row && row->mode == MODE_ON)
            return "command";
        if (row && row->mode == MODE_OFF)
            return "";

        const std::string name = Lower(bot->GetName());
        if (g_cfg.excludeNames.count(name))
            return "";
        if (g_cfg.includeNames.count(name))
            return "name";

#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        {
            if (Player* master = ai->GetMaster())
            {
                if (!g_cfg.allowMasterEnroll)
                    return "";
                if (row && row->enrolled && row->source == "master")
                    return "master";
                return "";
            }
        }
#endif

        const uint32_t level = bot->GetLevel();
        if (level < g_cfg.minLevel || level > g_cfg.maxLevel)
            return "";

        if (bot->GetGuildId() && g_cfg.guilds.count(bot->GetGuildId()))
            return "guild";

        if (bot->GetSession() && g_cfg.accounts.count(bot->GetSession()->GetAccountId()))
            return "account";

#if AF_HAS_PLAYERBOTS
        if (sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            if (g_cfg.randomBotPercent > 0 &&
                (StableHash(bot->GetGUID().GetRawValue()) % 100 < g_cfg.randomBotPercent))
                return "random";
        }
        else if (g_cfg.altBots)
        {
            return "alt";
        }
#else
        if (g_cfg.randomBotPercent > 0 &&
            (StableHash(bot->GetGUID().GetRawValue()) % 100 < g_cfg.randomBotPercent))
            return "random";
#endif

        return "";
    }

    void ApplyEnrollment(Player* bot, Row& row, OnlineState& state, const std::string& source, uint32_t now)
    {
        const bool wasEnrolled = (row.enrolled != 0);
        bool want = !source.empty();

        if (want && !wasEnrolled && IsCappedSource(source) &&
            g_cfg.maxEnrolled > 0 && g_cappedCount >= g_cfg.maxEnrolled)
        {
            want = false;
            if (g_cfg.debug)
                LOG_INFO("module.azerothfriend", "Autopilot: '{}' matched '{}' but MaxEnrolled ({}) is reached.",
                         bot->GetName(), source, g_cfg.maxEnrolled);
        }

        if (want && !wasEnrolled)
        {
            row.enrolled   = 1;
            row.enrolledAt = now;
            row.source     = source;
            row.dirty      = true;
            CountEnrolled(row, 1);

#if AF_HAS_PLAYERBOTS
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            {
                // Capture baseline strategies before autopilot changes anything
                if (row.baseline.empty())
                {
                    row.baseline = AzerothFriendStrategy::CaptureStrategies(ai);
                }
            }
#endif
            AzerothFriendStrategy::AutopilotStrategy_Apply(bot);
            state.markerOurs = true;
            AzerothFriendStrategy::SetBotEnrolled(bot->GetGUID().GetRawValue(), true);
            AzerothFriendProgress::QueueEvent(bot->GetGUID().GetRawValue(), "enroll", "Enrolled into Autopilot (" + source + ")");
            if (g_cfg.debug)
                LOG_INFO("module.azerothfriend", "Autopilot: Enrolled '{}' via source '{}'.", bot->GetName(), source);
        }
        else if (!want && wasEnrolled)
        {
            if (row.mode == MODE_ON && g_cfg.enable)
                return; // preserve forced command enrollment while globally enabled

            CountEnrolled(row, -1);
            row.enrolled = 0;
            row.dirty    = true;
#if AF_HAS_PLAYERBOTS
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
            {
                HandBack(ai, &row, &state);
                row.baseline.clear();
            }
#endif
            if (state.markerOurs)
            {
                AzerothFriendStrategy::AutopilotStrategy_Remove(bot);
                state.markerOurs = false;
            }
            AzerothFriendStrategy::SetBotEnrolled(bot->GetGUID().GetRawValue(), false);
            AzerothFriendProgress::QueueEvent(bot->GetGUID().GetRawValue(), "unenroll", "Unenrolled from Autopilot");
            if (g_cfg.debug)
                LOG_INFO("module.azerothfriend", "Autopilot: Unenrolled '{}'.", bot->GetName());
        }
        state.evaluated = true;
    }

    void SweepBot(Player* bot, uint32_t now)
    {
        if (!bot || !bot->IsInWorld())
            return;

        const uint64_t guid = bot->GetGUID().GetRawValue();
        const uint32_t lowGuid = bot->GetGUID().GetCounter();

        // 1. Ownership arbitration: manual owner claim, companion lease, or disabled bridge
        AzerothFriend::BotControlRecord control;
        const bool hasControl = sAFLiveState->GetBotControl(lowGuid, control);
        const bool isClaimed = (hasControl && (!control.bridgeEnabled || control.mode == "claimed"));
        const bool isLeased = AzerothFriendAiControl::IsLeased(bot);

        std::lock_guard<std::mutex> lock(g_mutex);

        Row& row = g_rows[guid];
        row.botGuid = guid;
        OnlineState& state = g_online[guid];

        if (isClaimed || isLeased)
        {
            if (row.enrolled)
            {
                CountEnrolled(row, -1);
                row.enrolled = 0;
                row.dirty = true;
#if AF_HAS_PLAYERBOTS
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                {
                    HandBack(ai, &row, &state);
                    row.baseline.clear();
                }
#endif
                if (state.markerOurs)
                {
                    AzerothFriendStrategy::AutopilotStrategy_Remove(bot);
                    state.markerOurs = false;
                }
                AzerothFriendStrategy::SetBotEnrolled(guid, false);
                AzerothFriendProgress::QueueEvent(guid, "unenroll", "Handed back to owner / companion lease");
            }
            state.evaluated = true;
            return;
        }

        if (row.mode == MODE_OFF)
        {
            if (row.enrolled)
            {
                CountEnrolled(row, -1);
                row.enrolled = 0;
                row.dirty = true;
#if AF_HAS_PLAYERBOTS
                if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
                {
                    HandBack(ai, &row, &state);
                    row.baseline.clear();
                }
#endif
                if (state.markerOurs)
                {
                    AzerothFriendStrategy::AutopilotStrategy_Remove(bot);
                    state.markerOurs = false;
                }
                AzerothFriendStrategy::SetBotEnrolled(guid, false);
                AzerothFriendProgress::QueueEvent(guid, "unenroll", "Unenrolled from Autopilot (mode OFF)");
            }
            state.evaluated = true;
            return;
        }

        if (!state.evaluated)
        {
            ApplyEnrollment(bot, row, state, EvaluateRules(bot, &row), now);
        }

        if (!row.enrolled)
            return;

        state.tier = CalculateTier(bot);

        // 2. Reassert marker strategy if playerbots reset engine strategies
#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
        {
            const bool ncMissing = !ai->HasStrategy(AUTOPILOT_STRATEGY_NAME, BOT_STATE_NON_COMBAT);
            const bool coMissing = !ai->HasStrategy(AUTOPILOT_STRATEGY_NAME, BOT_STATE_COMBAT);
            const bool deadMissing = !ai->HasStrategy(AUTOPILOT_STRATEGY_NAME, BOT_STATE_DEAD);

            if (ncMissing || coMissing || deadMissing)
            {
                AzerothFriendStrategy::AutopilotStrategy_Apply(bot);
                state.markerOurs = true;
            }

            // Diary only (Autopilot.Control = 0): restore any model modifications
            if (!g_cfg.control)
            {
                if (!row.baseline.empty())
                {
                    HandBack(ai, &row, &state);
                    AzerothFriendStrategy::AutopilotStrategy_Apply(bot);
                    row.baseline.clear();
                    row.dirty = true;
                }
                return;
            }

            // Taxi cheat removal for random bots under anti-handouts
            if (g_cfg.noHandouts && bot->isTaxiCheater())
                bot->SetTaxiCheater(false);

            // Group / instance arbitration: in a real player's group or dungeon,
            // movement and errands yield to the party
            const bool withRealPlayer = WithRealPlayer(bot);
            const bool inInstance = InInstance(bot);
            if (withRealPlayer || inInstance)
            {
                if (state.currentErrand.active)
                {
                    AzerothFriendErrands::StopErrand(ai, state.currentErrand);
                    state.errandQueue.clear();
                }
            }
            else
            {
                // Solo in open world: suppress competing autonomous controllers
                for (const char* key : { "new rpg", "rpg", "travel" })
                {
                    if (ai->HasStrategy(key, BOT_STATE_NON_COMBAT))
                        ai->ChangeStrategy(std::string("-") + key, BOT_STATE_NON_COMBAT);
                }
            }

            // Periodic teleports and randomize holds
            if (sRandomPlayerbotMgr.IsRandomBot(bot))
            {
                if (!state.teleportDeferredAt || now - state.teleportDeferredAt >= 3600)
                {
                    if (g_cfg.noTeleport)
                        sRandomPlayerbotMgr.ScheduleTeleport(lowGuid, 7200);
                    if (g_cfg.noRandomize)
                        sRandomPlayerbotMgr.SetValue(lowGuid, "randomize", 1);
                    if (g_cfg.keepOnline)
                        sRandomPlayerbotMgr.SetValue(lowGuid, "add", 1);
                    state.teleportDeferredAt = now;
                }

                if (g_cfg.noTeleport && !bot->IsAlive() && !bot->InBattleground())
                {
                    if (!state.reviveHeld)
                    {
                        sRandomPlayerbotMgr.SetValue(lowGuid, "revive", 1);
                        state.reviveHeld = true;
                    }
                }
                else if (state.reviveHeld && bot->IsAlive())
                {
                    sRandomPlayerbotMgr.SetValue(lowGuid, "revive", 0);
                    state.reviveHeld = false;
                }
            }
        }
#endif

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

void Autopilot_Off()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [guid, ob] : g_online)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid(guid));
        if (!bot || !bot->IsInWorld())
            continue;

#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(bot))
        {
            auto it = g_rows.find(guid);
            if (it != g_rows.end() && (!it->second.baseline.empty() || ob.currentErrand.active || ob.reviveHeld || !bot->IsAlive()))
            {
                HandBack(ai, &it->second, &ob);
                it->second.baseline.clear();
                it->second.dirty = true;
            }
            if (ob.markerOurs)
            {
                AzerothFriendStrategy::AutopilotStrategy_Remove(bot);
                ob.markerOurs = false;
            }
        }
#endif
        AzerothFriendStrategy::SetBotEnrolled(guid, false);
        ob.evaluated = false;
    }
    AzerothFriendStrategy::ClearEnrolledBots();
    LOG_INFO("module.azerothfriend", "Autopilot is off; bots handed back to native playerbots.");
}

void Autopilot_LoadConfig()
{
    AutopilotConfig c;
    c.enable                   = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.Enable", false);
    c.debug                    = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.Debug", false);
    c.allowMasterEnroll        = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.AllowMasterEnroll", true);
    c.altBots                  = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.Select.AltBots", false);
    c.randomBotPercent         = std::min<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.RandomBotPercent", 0));
    c.minLevel                 = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.MinLevel", 1);
    c.maxLevel                 = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.MaxLevel", 80);
    c.maxEnrolled              = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.MaxEnrolled", 0);

    c.guilds                   = ParseIds(sConfigMgr->GetOption<std::string>("AzerothFriend.Autopilot.Select.Guilds", ""));
    c.accounts                 = ParseIds(sConfigMgr->GetOption<std::string>("AzerothFriend.Autopilot.Select.Accounts", ""));
    c.includeNames             = ParseNames(sConfigMgr->GetOption<std::string>("AzerothFriend.Autopilot.Select.Include", ""));
    c.excludeNames             = ParseNames(sConfigMgr->GetOption<std::string>("AzerothFriend.Autopilot.Select.Exclude", ""));

    c.sweepIntervalMs          = std::max<uint32_t>(100, sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.SweepIntervalMs", 1000));
    c.botsPerSweep             = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.BotsPerSweep", 25));
    c.snapshotIntervalMinutes  = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.SnapshotIntervalMinutes", 10));
    c.flushIntervalSeconds     = std::max<uint32_t>(5, sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.FlushIntervalSeconds", 60));
    c.snapshotRetention        = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.SnapshotRetention", 500);
    c.eventRetention           = sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.EventRetention", 300);

    c.control                  = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.Control", true);
    c.noTeleport               = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.NoTeleport", true);
    c.noRandomize              = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.NoRandomize", true);
    c.noHandouts               = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.NoHandouts", true);
    c.keepOnline               = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.KeepOnline", true);
    c.corpseRunMinutes         = std::max<uint32_t>(1, sConfigMgr->GetOption<uint32_t>("AzerothFriend.Autopilot.CorpseRunMinutes", 10));

    c.foregroundRange          = sConfigMgr->GetOption<float>("AzerothFriend.Autopilot.ForegroundRange", 100.0f);
    c.requireRealPlayer        = sConfigMgr->GetOption<bool>("AzerothFriend.Autopilot.RequireRealPlayer", false);
    c.foregroundScope          = ParseScope(sConfigMgr->GetOption<std::string>("AzerothFriend.Autopilot.ForegroundScope", "range"), Scope::Range);
    c.backgroundScope          = ParseScope(sConfigMgr->GetOption<std::string>("AzerothFriend.Autopilot.BackgroundScope", "zone"), Scope::Zone);
    c.minimumTier              = ParseTier(sConfigMgr->GetOption<std::string>("AzerothFriend.Autopilot.MinimumTier", "dormant"), Tier::Dormant);

    const bool wasEnabled = g_cfg.enable;
    g_cfg = c;

    AzerothFriendStrategy::SetNoHandouts(g_cfg.noHandouts);
    AzerothFriendStrategy::SetModelGroups(g_cfg.groups);

    if (wasEnabled && !g_cfg.enable)
    {
        Autopilot_Off();
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [guid, state] : g_online)
        state.evaluated = false;
}

void Autopilot_Load()
{
    AzerothFriendAutopilotSchema::EnsureTables();
    AzerothFriendStrategy::Register();
    Autopilot_LoadConfig();

    std::lock_guard<std::mutex> lock(g_mutex);
    g_rows.clear();
    g_enrolledCount = 0;
    g_cappedCount   = 0;

    if (QueryResult result = CharacterDatabase.Query(
            "SELECT bot_guid, mode, enrolled, source, style, outlook, profile, "
            "kills_total, deaths_total, quests_total, dungeons_total, "
            "doing, doing_since, decided_by, strategies, last_results, "
            "goal_kind, goal_target, goal_target_id, goal_value, goal_baseline, "
            "goal_set_at, goal_text, last_reason, baseline, "
            "plan_until, last_plan_at, enrolled_at, updated_at "
            "FROM azeroth_friend_autopilot"))
    {
        do
        {
            Field* f = result->Fetch();
            Row r;
            r.botGuid       = f[0].Get<uint64>();
            r.mode          = f[1].Get<uint8>();
            r.enrolled      = f[2].Get<uint8>();
            r.source        = f[3].Get<std::string>();
            r.style         = f[4].Get<std::string>();
            r.outlook       = f[5].Get<std::string>();
            r.profile       = f[6].Get<std::string>();
            r.killsTotal    = f[7].Get<uint32>();
            r.deathsTotal   = f[8].Get<uint32>();
            r.questsTotal   = f[9].Get<uint32>();
            r.dungeonsTotal = f[10].Get<uint32>();
            r.doing         = f[11].Get<std::string>();
            r.doingSince    = f[12].Get<uint32>();
            r.decidedBy     = f[13].Get<std::string>();
            r.strategies    = f[14].Get<std::string>();
            r.lastResults   = f[15].Get<std::string>();
            r.goalKind      = f[16].Get<std::string>();
            r.goalTarget    = f[17].Get<std::string>();
            r.goalTargetId  = f[18].Get<uint32>();
            r.goalValue     = f[19].Get<uint32>();
            r.goalBaseline  = f[20].Get<uint32>();
            r.goalSetAt     = f[21].Get<uint32>();
            r.goalText      = f[22].Get<std::string>();
            r.lastReason    = f[23].Get<std::string>();
            r.baseline      = f[24].Get<std::string>();
            r.planUntil     = f[25].Get<uint32>();
            r.lastPlanAt    = f[26].Get<uint32>();
            r.enrolledAt    = f[27].Get<uint32>();
            r.updatedAt     = f[28].Get<uint32>();
            r.dirty         = false;

            if (r.enrolled)
            {
                ++g_enrolledCount;
                if (IsCappedSource(r.source))
                    ++g_cappedCount;
            }
            g_rows[r.botGuid] = r;
        } while (result->NextRow());
    }

    g_realOnline.clear();
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (player && !IsBotPlayer(player) && player->IsInWorld())
            g_realOnline.insert(guid.GetRawValue());
    }

    LOG_INFO("module.azerothfriend", "Autopilot loaded {} rows ({} enrolled, {} capped).",
             g_rows.size(), g_enrolledCount, g_cappedCount);
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
    std::vector<uint64_t> dirtyGuids;
    dirtyGuids.reserve(g_rows.size());

    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    bool hasUpdates = false;

    for (auto& [guid, r] : g_rows)
    {
        if (!r.dirty)
            continue;

        std::string sql = SafeFormat(
            "INSERT INTO azeroth_friend_autopilot ("
            "bot_guid, mode, enrolled, source, style, outlook, profile, "
            "kills_total, deaths_total, quests_total, dungeons_total, "
            "doing, doing_since, decided_by, strategies, last_results, "
            "goal_kind, goal_target, goal_target_id, goal_value, goal_baseline, "
            "goal_set_at, goal_text, last_reason, baseline, "
            "plan_until, last_plan_at, enrolled_at, updated_at"
            ") VALUES ("
            "{}, {}, {}, '{}', '{}', '{}', '{}', "
            "{}, {}, {}, {}, "
            "'{}', {}, '{}', '{}', '{}', "
            "'{}', '{}', {}, {}, {}, "
            "{}, '{}', '{}', '{}', "
            "{}, {}, {}, {}"
            ") ON DUPLICATE KEY UPDATE "
            "mode = VALUES(mode), "
            "enrolled = VALUES(enrolled), "
            "source = VALUES(source), "
            "style = VALUES(style), "
            "outlook = VALUES(outlook), "
            "profile = VALUES(profile), "
            "kills_total = VALUES(kills_total), "
            "deaths_total = VALUES(deaths_total), "
            "quests_total = VALUES(quests_total), "
            "dungeons_total = VALUES(dungeons_total), "
            "doing = VALUES(doing), "
            "doing_since = VALUES(doing_since), "
            "decided_by = VALUES(decided_by), "
            "strategies = VALUES(strategies), "
            "last_results = VALUES(last_results), "
            "goal_kind = VALUES(goal_kind), "
            "goal_target = VALUES(goal_target), "
            "goal_target_id = VALUES(goal_target_id), "
            "goal_value = VALUES(goal_value), "
            "goal_baseline = VALUES(goal_baseline), "
            "goal_set_at = VALUES(goal_set_at), "
            "goal_text = VALUES(goal_text), "
            "last_reason = VALUES(last_reason), "
            "baseline = VALUES(baseline), "
            "plan_until = VALUES(plan_until), "
            "last_plan_at = VALUES(last_plan_at), "
            "enrolled_at = VALUES(enrolled_at), "
            "updated_at = VALUES(updated_at)",
            r.botGuid, uint32_t(r.mode), uint32_t(r.enrolled),
            Escape(Utf8Truncate(r.source, 32)),
            Escape(Utf8Truncate(r.style, 64)),
            Escape(Utf8Truncate(r.outlook, 32)),
            Escape(SanitizeUTF8(r.profile)),
            r.killsTotal, r.deathsTotal, r.questsTotal, r.dungeonsTotal,
            Escape(Utf8Truncate(r.doing, 64)), r.doingSince,
            Escape(Utf8Truncate(r.decidedBy, 16)),
            Escape(Utf8Truncate(r.strategies, 2000)),
            Escape(Utf8Truncate(r.lastResults, 2000)),
            Escape(Utf8Truncate(r.goalKind, 24)),
            Escape(Utf8Truncate(r.goalTarget, 64)),
            r.goalTargetId, r.goalValue, r.goalBaseline,
            r.goalSetAt,
            Escape(Utf8Truncate(r.goalText, 255)),
            Escape(Utf8Truncate(r.lastReason, 255)),
            Escape(Utf8Truncate(r.baseline, 4000)),
            r.planUntil, r.lastPlanAt, r.enrolledAt, Now());

        trans->Append(sql.c_str());
        dirtyGuids.push_back(guid);
        hasUpdates = true;
    }

    if (hasUpdates)
    {
        CharacterDatabase.CommitTransaction(trans);
        for (uint64_t guid : dirtyGuids)
        {
            auto it = g_rows.find(guid);
            if (it != g_rows.end())
                it->second.dirty = false;
        }
    }
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
    if (g_cfg.randomBotPercent == 0 && g_cfg.includeNames.empty() && g_cfg.guilds.empty() && g_cfg.accounts.empty())
        reasons.push_back("No bots eligible by rules (RandomBotPercent=0 and no include filters)");
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
            if (!bot || !bot->IsInWorld())
                continue;

            seen.insert(guid);
            AutopilotMonitorRow row;
            row.guid  = guid;
            row.name  = bot->GetName();
            row.level = bot->GetLevel();
            row.cls   = bot->getClass();
            AreaTableEntry const* zoneEntry = sAreaTableStore.LookupEntry(bot->GetZoneId());
            row.zone  = zoneEntry ? zoneEntry->area_name[sWorld->GetDefaultDbcLocale()] : "World";
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
        if (!bot || !bot->IsInWorld())
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
        AreaTableEntry const* zoneEntry = sAreaTableStore.LookupEntry(bot->GetZoneId());
        row.zone  = zoneEntry ? zoneEntry->area_name[sWorld->GetDefaultDbcLocale()] : "World";
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
    if (!bot || !bot->IsInWorld())
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
            for (auto it = events.rbegin(); it != events.rend(); ++it)
            {
                const auto& e = *it;
                lines.push_back("  [" + e.type + "] " + e.detail);
            }
        }
        return true;
    }

    lines.push_back("unknown page: " + page);
    return true;
}

bool Autopilot_MonitorCommand(Player* gm, const std::string& sub, const std::string& name)
{
    if (!gm || !gm->IsInWorld() || !gm->GetSession())
        return false;

    Player* bot = ObjectAccessor::FindPlayerByName(name, false);
    if (!bot || !bot->IsInWorld() || !IsBotPlayer(bot))
    {
        AzerothFriendShared::SendAddonError(gm, "Bot not found or not a valid playerbot.", name);
        return false;
    }

    bool const isGm = gm->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    if (!isGm && !AzerothFriendShared::CanControl(gm, bot))
    {
        AzerothFriendShared::SendAddonError(gm, "Companion unavailable or not owned by you.", name);
        return false;
    }
    if (sub == "status")
    {
        AzerothFriendEnvironment::instance()->BroadcastTelemetry(bot, gm);
        return true;
    }
    if (sub == "replan" || sub == "plan")
    {
        uint32 const botGuid = bot->GetGUID().GetCounter();
        if (!sAFLiveState->IsBotBridgeEnabled(botGuid))
        {
            AzerothFriendShared::SendAddonError(gm, "Release the companion claim before requesting a replan.", name);
            return false;
        }
        // A diary entry is not a planner input. Preserve owner provenance and
        // invalidate old steps before sending the request to the Python mind.
        AzerothFriendShared::InvalidateControl(bot);
        sAFDispatcher->InterruptBotPlan(botGuid, "Owner requested immediate replan", false);
        AzerothFriendShared::QueueEvent(botGuid, "player_command", 1,
            gm->GetGUID().GetCounter(), gm->GetName(),
            "{\"text\":\"Replan and carry out my current objective using native playerbot actions.\",\"channel\":\"command\"}");
        AzerothFriendProgress::QueueEvent(bot->GetGUID().GetRawValue(), "replan", "Owner requested immediate replan");
        return true;
    }
    if (!isGm)
    {
        AzerothFriendShared::SendAddonError(gm, "GM permissions required to change autopilot enrollment; use companion Auto for personal autonomy.", name);
        return false;
    }
    if (sub != "on" && sub != "off")
    {
        AzerothFriendShared::SendAddonError(gm, "Unknown monitor command.", name);
        return false;
    }

    AzerothFriendShared::InvalidateControl(bot);
    sAFDispatcher->InterruptBotPlan(bot->GetGUID().GetCounter(), "Operator changed autopilot enrollment", false);

    const uint64_t guid = bot->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);
    Row& r = g_rows[guid];
    r.botGuid = guid;

    if (sub == "on")
    {
        r.mode = MODE_ON;
        OnlineState& state = g_online[guid];
        ApplyEnrollment(bot, r, state, "command", Now());
        ChatHandler(gm->GetSession()).PSendSysMessage("Autopilot forced ON for %s.", bot->GetName().c_str());
        return true;
    }
    else if (sub == "off")
    {
        r.mode = MODE_OFF;
        OnlineState& state = g_online[guid];
        ApplyEnrollment(bot, r, state, "", Now());
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
            std::string reason = "NOT enrolled in Autopilot";
            if (!g_cfg.enable)
                reason += " (Autopilot is globally disabled)";
            else if (it != g_rows.end() && it->second.mode == MODE_OFF)
                reason += " (mode forced OFF by operator)";
            else
            {
                AzerothFriend::BotControlRecord control;
                if (sAFLiveState->GetBotControl(bot->GetGUID().GetCounter(), control) &&
                    (!control.bridgeEnabled || control.mode == "claimed"))
                    reason += " (claimed by owner / manual control)";
                else
                {
                    std::string eligibleSource = EvaluateRules(bot, it != g_rows.end() ? &it->second : nullptr);
                    if (eligibleSource.empty())
                        reason += " (not eligible under current level/percentage/filters)";
                    else
                        reason += " (eligible via '" + eligibleSource + "', awaiting evaluation or capped)";
                }
            }
            handler->PSendSysMessage("%s is %s.", bot->GetName().c_str(), reason.c_str());
            return true;
        }

        const Row& r = it->second;
        auto onIt = g_online.find(guid);
        std::string tierStr = "dormant";
        if (onIt != g_online.end())
        {
            switch (onIt->second.tier)
            {
                case Tier::Foreground: tierStr = "foreground"; break;
                case Tier::Background: tierStr = "background"; break;
                default:               tierStr = "dormant";    break;
            }
        }
        handler->PSendSysMessage("Autopilot [%s]: Enrolled (source: '%s', mode: %u, tier: %s), Doing '%s', Goal '%s'",
                                 bot->GetName().c_str(), r.source.c_str(), r.mode, tierStr.c_str(),
                                 r.doing.c_str(), r.goalText.c_str());
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
        OnlineState& state = g_online[guid];
        ApplyEnrollment(bot, r, state, "command", Now());
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
        OnlineState& state = g_online[guid];
        ApplyEnrollment(bot, r, state, "", Now());
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
            handler->PSendSysMessage("- %s (L%u, source: %s): %s",
                                     bot ? bot->GetName().c_str() : "Offline",
                                     bot ? bot->GetLevel() : 0,
                                     r.source.c_str(),
                                     r.doing.c_str());
            ++count;
        }
        handler->PSendSysMessage("Total enrolled bots: %u (Capped quota: %u / %u)",
                                 count, g_cappedCount, g_cfg.maxEnrolled);
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
                   { PLAYERHOOK_ON_LOGIN,
                     PLAYERHOOK_ON_LOGOUT,
                     PLAYERHOOK_ON_CREATURE_KILL,
                     PLAYERHOOK_ON_PVP_KILL,
                     PLAYERHOOK_ON_PLAYER_JUST_DIED,
                     PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
                     PLAYERHOOK_ON_LEVEL_CHANGED,
                     PLAYERHOOK_ON_STORE_NEW_ITEM,
                     PLAYERHOOK_ON_ACHI_COMPLETE,
                     PLAYERHOOK_ON_DELETE_FROM_DB })
{
}

void AutopilotPlayerScript::OnPlayerLogin(Player* player)
{
    if (!player)
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!IsBotPlayer(player))
    {
        g_realOnline.insert(guid);
        return;
    }

    OnlineState& state = g_online[guid];
    state = OnlineState();
    state.evaluated = false;

    auto it = g_rows.find(guid);
    if (it != g_rows.end() && it->second.enrolled)
    {
        AzerothFriendStrategy::AutopilotStrategy_Apply(player);
        state.markerOurs = true;
        AzerothFriendStrategy::SetBotEnrolled(guid, true);
        state.evaluated = true;
    }
}

void AutopilotPlayerScript::OnPlayerLogout(Player* player)
{
    if (!player)
        return;

    const uint64_t guid = player->GetGUID().GetRawValue();
    std::lock_guard<std::mutex> lock(g_mutex);

    if (IsBotPlayer(player))
    {
#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(player))
        {
            auto it = g_rows.find(guid);
            auto onIt = g_online.find(guid);
            if (it != g_rows.end() && onIt != g_online.end() && it->second.enrolled)
            {
                HandBack(ai, &it->second, &onIt->second);
            }
        }
#endif
        AzerothFriendStrategy::SetBotEnrolled(guid, false);
    }

    g_realOnline.erase(guid);
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

    auto onIt = g_online.find(guid);
    if (onIt != g_online.end())
        onIt->second.evaluated = false; // Trigger re-evaluation against minLevel / maxLevel on next sweep
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
    ObjectGuid rawGuid = ObjectGuid::Create<HighGuid::Player>(lowGuid);
    uint64 raw = rawGuid.GetRawValue();

    trans->Append(SafeFormat("DELETE FROM azeroth_friend_autopilot WHERE bot_guid = {} OR bot_guid = {}", raw, lowGuid).c_str());
    trans->Append(SafeFormat("DELETE FROM azeroth_friend_autopilot_snapshots WHERE bot_guid = {} OR bot_guid = {}", raw, lowGuid).c_str());
    trans->Append(SafeFormat("DELETE FROM azeroth_friend_autopilot_events WHERE bot_guid = {} OR bot_guid = {}", raw, lowGuid).c_str());

    std::lock_guard<std::mutex> lock(g_mutex);
    g_rows.erase(raw);
    g_rows.erase(lowGuid);
    g_online.erase(raw);
    g_online.erase(lowGuid);
    g_realOnline.erase(raw);
    g_realOnline.erase(lowGuid);

    AzerothFriendStrategy::SetBotEnrolled(raw, false);
    AzerothFriendStrategy::SetBotEnrolled(lowGuid, false);
}

AutopilotGuildScript::AutopilotGuildScript() : GuildScript("AutopilotGuildScript") { }
void AutopilotGuildScript::OnAddMember(Guild* /*guild*/, Player* /*player*/, uint8& /*plRank*/) { }

AutopilotServerScript::AutopilotServerScript() : ServerScript("AutopilotServerScript") { }
void AutopilotServerScript::OnPacketSent(WorldSession* /*session*/, WorldPacket const& /*packet*/) { }
