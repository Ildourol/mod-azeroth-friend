#include "AzerothFriendActionDispatcher.h"
#include "AzerothFriendActionRegistry.h"
#include "AzerothFriendAiControl.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendEnvironment.h"
#include "AzerothFriendBotController.h"
#include "AzerothFriendPlayerbot.h"
#include "AzerothFriendPlayerbotActions.h"
#include "AzerothFriendShared.h"
#include "AzerothFriendLiveState.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Creature.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "Group.h"
#include "Map.h"
#include "QuestDef.h"
#include "Log.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <regex>
#include <sstream>

AzerothFriendActionDispatcher* AzerothFriendActionDispatcher::instance()
{
    static AzerothFriendActionDispatcher instance;
    return &instance;
}

namespace
{
    std::string TelemetryField(std::string value)
    {
        for (char& c : value)
        {
            if (c == '|' || c == '\r' || c == '\n')
                c = (c == '|') ? '/' : ' ';
        }
        return value;
    }

    std::string ExtractJsonString(std::string const& json, std::string const& key)
    {
        std::regex pattern("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
        std::smatch match;
        if (std::regex_search(json, match, pattern) && match.size() > 1)
            return match[1].str();
        return "";
    }

    bool ExtractJsonBool(std::string const& json, std::string const& key, bool defaultVal = false)
    {
        std::regex pattern("\"" + key + "\"\\s*:\\s*(true|false)");
        std::smatch match;
        if (std::regex_search(json, match, pattern) && match.size() > 1)
            return match[1].str() == "true";
        return defaultVal;
    }

    float ExtractJsonFloat(std::string const& json, std::string const& key, float defaultVal = 0.0f)
    {
        std::regex pattern("\"" + key + "\"\\s*:\\s*([-+]?[0-9]*\\.?[0-9]+)");
        std::smatch match;
        if (std::regex_search(json, match, pattern) && match.size() > 1)
        {
            try
            {
                return std::stof(match[1].str());
            }
            catch (...)
            {
            }
        }
        return defaultVal;
    }

    uint32 ExtractJsonUint(std::string const& json, std::string const& key, uint32 defaultVal = 0)
    {
        std::regex pattern("\"" + key + "\"\\s*:\\s*([0-9]+)");
        std::smatch match;
        if (std::regex_search(json, match, pattern) && match.size() > 1)
        {
            try
            {
                return static_cast<uint32>(std::stoul(match[1].str()));
            }
            catch (...)
            {
            }
        }
        return defaultVal;
    }

    std::vector<std::string> ExtractJsonStringArray(std::string const& json, std::string const& key)
    {
        std::vector<std::string> values;
        std::regex arrayPattern("\"" + key + "\"\\s*:\\s*\\[([^\\]]*)\\]");
        std::smatch arrayMatch;
        if (!std::regex_search(json, arrayMatch, arrayPattern) || arrayMatch.size() < 2)
            return values;

        std::string body = arrayMatch[1].str();
        std::regex itemPattern("\"([^\"]*)\"");
        for (std::sregex_iterator it(body.begin(), body.end(), itemPattern), end; it != end; ++it)
            values.push_back((*it)[1].str());

        return values;
    }

    WorldObject* ResolveWorldObject(Player* bot, uint32 guid)
    {
        if (!bot || !bot->GetMap() || !guid)
            return nullptr;

        if (Creature* creature = AzerothFriendShared::FindCreatureByCounter(bot->GetMap(), guid))
            return creature;

        if (GameObject* go = AzerothFriendShared::FindGameObjectByCounter(bot->GetMap(), guid))
            return go;

        if (Player* player = AzerothFriendShared::FindPlayerByCounter(guid))
            return player;

        return nullptr;
    }

    Player* ResolveTargetOrSpeaker(Player* bot, std::string const& paramsJson)
    {
        if (!bot)
            return nullptr;

        uint32 guid = ExtractJsonUint(paramsJson, "guid");
        Player* target = AzerothFriendShared::FindPlayerByCounter(guid);
        if (!target)
        {
            std::string name = ExtractJsonString(paramsJson, "name");
            if (name.empty())
                name = ExtractJsonString(paramsJson, "target");
            if (name.empty())
                name = ExtractJsonString(paramsJson, "guid");
            if (!name.empty() && name != "master" && name != "self")
                target = ObjectAccessor::FindPlayerByName(name);
        }

        if (!target)
            target = bot->GetSelectedPlayer();

        if (!target)
        {
            Group* group = bot->GetGroup();
            if (group)
                target = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
        }

#if AF_HAS_PLAYERBOTS
        if (!target)
        {
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (ai && ai->GetMaster())
                target = ai->GetMaster();
        }
#endif

        return target;
    }

    Creature* FindNearestQuestGiver(Player* bot, float radius)
    {
        Creature* best = nullptr;
        float bestDist = radius;
        if (!bot || !bot->GetMap())
            return nullptr;

        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
        {
            Creature* creature = pair.second;
            if (!creature || !creature->IsInWorld() || creature->isDead())
                continue;
            if (!creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
                continue;

            float dist = bot->GetDistance(creature);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = creature;
            }
        }
        return best;
    }

    Creature* FindNearestLootableCorpse(Player* bot, float radius)
    {
        Creature* best = nullptr;
        float bestDist = radius;
        if (!bot || !bot->GetMap())
            return nullptr;

        Player* master = nullptr;
#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            master = ai->GetMaster();
#endif

        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
        {
            Creature* creature = pair.second;
            if (!creature || !creature->IsInWorld() || !creature->isDead())
                continue;
            if (!creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
                continue;
            // MAF-054: Do not target master's or other players' exclusive kills for looting
            bool canLoot = (creature->GetLootRecipient() == bot) || creature->isTappedBy(bot);
            if (!canLoot && creature->GetLootRecipientGroup() && bot->GetGroup() == creature->GetLootRecipientGroup())
                canLoot = bot->isAllowedToLoot(creature);
            if (!canLoot)
                continue;
            if (sAFDispatcher && sAFDispatcher->IsCorpseUnreachable(creature->GetGUID().GetCounter()))
                continue;

            // Elastic tether: corpse must not be farther than 25y from master either
            if (master && master->IsInWorld())
            {
                if (master->GetDistance(creature) > 25.0f)
                    continue;
            }

            float dist = bot->GetDistance(creature);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = creature;
            }
        }
        return best;
    }

    GameObject* FindNearestGatheringNode(Player* bot, float radius)
    {
        GameObject* best = nullptr;
        float bestDist = radius;
        if (!bot || !bot->GetMap())
            return nullptr;

        Player* master = nullptr;
#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
            master = ai->GetMaster();
#endif

        for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
        {
            GameObject* go = pair.second;
            if (!go || !go->IsInWorld())
                continue;

            uint32 type = go->GetGoType();
            if (type != GAMEOBJECT_TYPE_CHEST && type != GAMEOBJECT_TYPE_FISHINGHOLE)
                continue;

            // Elastic tether rule: node must not be farther than 25y from master either
            if (master && master->IsInWorld())
            {
                if (master->GetDistance(go) > 25.0f)
                    continue;
            }

            float dist = bot->GetDistance(go);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = go;
            }
        }
        return best;
    }

    Creature* FindNearestVendor(Player* bot, float radius = 20.0f)
    {
        Creature* best = nullptr;
        float bestDist = radius;
        if (!bot || !bot->GetMap())
            return nullptr;

        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
        {
            Creature* creature = pair.second;
            if (!creature || !creature->IsInWorld() || !creature->IsAlive())
                continue;
            if (!creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
                continue;

            float dist = bot->GetDistance(creature);
            if (dist < bestDist)
            {
                bestDist = dist;
                best = creature;
            }
        }
        return best;
    }

    std::string CreatureKind(Creature* creature)
    {
        if (!creature)
            return "none";
        if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR))
            return "vendor";
        if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_REPAIR))
            return "repair";
        if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER))
            return "trainer";
        if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
            return "questgiver";
        if (creature->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_INNKEEPER))
            return "innkeeper";
        return "npc";
    }

    std::string IntToString(uint32 value)
    {
        return std::to_string(value);
    }

    std::string FloatToString(float value)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", value);
        return std::string(buffer);
    }
}

void AzerothFriendActionDispatcher::Initialize()
{
    if (_initialized)
        return;

    if (_schemaCheckInFlight)
        return;
    _schemaCheckInFlight = true;

    // Schema probes are database work too: they run asynchronously and the
    // callback is pumped on the world thread. Nothing here blocks the tick.
    std::string sql =
        "SELECT "
        "(SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
        " AND TABLE_NAME = 'azeroth_friend_bots' AND COLUMN_NAME IN "
        " ('autonomy_enabled','goal_status','goal_progress','goal_result','control_revision','long_term_goal')),"
        "(SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
        " AND TABLE_NAME = 'azeroth_friend_actions' AND COLUMN_NAME IN "
        " ('authority','origin_event_id','origin_source_guid')),"
        "(SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() "
        " AND TABLE_NAME = 'azeroth_friend_action_catalog' AND COLUMN_NAME IN "
        " ('api_version','params_schema_json','result_schema_json','binding_kind','native_binding',"
        "  'authority','preconditions','completion_policy','capability_revision'))";
    sAFLiveState->SubmitAsyncQuery(sql, [this](QueryResult schema)
    {
        _schemaCheckInFlight = false;
        if (!schema || schema->Fetch()[0].Get<uint32>() != 6 || schema->Fetch()[1].Get<uint32>() != 3 ||
            schema->Fetch()[2].Get<uint32>() != 9)
        {
            LOG_ERROR("server.loading", "[AzerothFriend] Missing schema migrations, including 2026_09_21_azeroth_friend_bot_api_v1.sql; module disabled.");
            sAzerothFriendConfig->enable = false;
            return;
        }

        _initialized = true;

        // Runtime loop only enqueues statements; schema creation lives in the
        // idempotent migrations under data/sql/characters.
        for (std::string const& botName : sAzerothFriendConfig->controlledBots)
        {
            if (botName.empty())
                continue;
            std::string escaped = AzerothFriendShared::EscapeSqlString(botName);
            CharacterDatabase.Execute(
                ("INSERT INTO azeroth_friend_bots (bot_guid, bot_name, enabled, mode) "
                 "SELECT guid, name, 1, 'companion' FROM characters WHERE LOWER(name) = LOWER('" + escaped + "') "
                 "ON DUPLICATE KEY UPDATE bot_name = VALUES(bot_name)").c_str());
        }

        // Steps left mid-flight by a crash/restart can never complete on their own.
        CharacterDatabase.Execute(
            "UPDATE azeroth_friend_actions SET status = 'interrupted', "
            "failure_reason = 'module restart while step in progress' "
            "WHERE status = 'in_progress'");
        CharacterDatabase.Execute(
            "UPDATE azeroth_friend_actions SET status = 'interrupted', "
            "failure_reason = 'plan interrupted by restart' "
            "WHERE status = 'pending'");
        CharacterDatabase.Execute(
            "UPDATE azeroth_friend_bots SET control_revision = control_revision + 1");

        LOG_INFO("server.loading", "[AzerothFriend] Action executor initialized ({} curated actions)",
                 AzerothFriendActionRegistry::Curated().size());
    });
}

void AzerothFriendActionDispatcher::Shutdown()
{
    AzerothFriendAiControl::ReleaseAll();
    _activeActions.clear();
    _claimedSteps.clear();
    _claimInFlight.clear();
    _unreachableCorpses.clear();
}

bool AzerothFriendActionDispatcher::IsCorpseUnreachable(uint32 corpseGuid) const
{
    if (!corpseGuid)
        return false;
    time_t now = time(nullptr);
    auto it = _unreachableCorpses.find(corpseGuid);
    if (it != _unreachableCorpses.end())
    {
        if (now < it->second)
            return true;
    }
    return false;
}

void AzerothFriendActionDispatcher::MarkCorpseUnreachable(uint32 corpseGuid, uint32 cooldownSeconds)
{
    if (!corpseGuid)
        return;
    _unreachableCorpses[corpseGuid] = time(nullptr) + cooldownSeconds;
}

void AzerothFriendActionDispatcher::Update(uint32 diff)
{
    if (!sAzerothFriendConfig->enable)
        return;

    // Pump first: the initialization probe and every cache refresh arrive here.
    sAFLiveState->PumpCallbacks();
    Initialize();
    if (!_initialized)
        return;

    _updateTimer += diff;
    if (_updateTimer < sAzerothFriendConfig->tickIntervalMs)
        return;

    _updateTimer = 0;

    RefreshDispatcherCaches();

    AzerothFriendAiControl::ReleaseExpired();

    // Auto-enroll configured controlled bots that are online but not yet registered in azeroth_friend_bots
    for (std::string const& botName : sAzerothFriendConfig->controlledBots)
    {
        Player* bot = ObjectAccessor::FindPlayerByName(botName);
        if (bot && bot->IsInWorld())
        {
            uint32 botGuid = bot->GetGUID().GetCounter();
            CharacterDatabase.Execute(
                ("INSERT IGNORE INTO azeroth_friend_bots (bot_guid, bot_name, mode, enabled) VALUES (" +
                 std::to_string(botGuid) + ", '" + AzerothFriendShared::EscapeSqlString(botName) + "', 'companion', 1)").c_str());
        }
    }

    uint32 activeControlled = 0;
    if (sAFLiveState->IsRunning())
    {
        // RAM-first: the bot table is read asynchronously once per cache window.
        for (AzerothFriend::BotControlRecord const& record : sAFLiveState->GetControlledBots())
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(record.botGuid));
            if (!bot || !bot->IsInWorld() || !sAzerothFriendConfig->IsBotControlled(bot->GetName()))
                continue;
            if (sAzerothFriendConfig->maxControlledBots > 0 && activeControlled >= sAzerothFriendConfig->maxControlledBots)
                break;
            ++activeControlled;

            sAFEnvironment->UpdateBotEnvironment(bot);
            if (!record.bridgeEnabled)
            {
                // Re-establish and continuously renew the manual-control lease.
                // This also restores an indefinite Claim after a server restart.
                AzerothFriendAiControl::Acquire(bot, 300);
                continue;
            }
            CheckEmergencyReflexes(bot);
            ProcessBotActions(bot, record.mode.empty() ? "companion" : record.mode);
            if (record.autonomyEnabled && record.goalStatus == "active")
                EmitAutonomyTick(bot, record.mode);

            if (_catalogExported.insert(record.botGuid).second)
            {
                std::string summary = ExportCatalog(bot);
                LOG_INFO("server.loading", "[AzerothFriend] Action catalogue exported for {} ({})",
                         bot->GetName(), summary);
            }
        }
        // Every bot has had the chance to publish a complete baseline.
        sAFLiveState->ClearFullRefreshSentinel();
        return;
    }

    // Explicit SQL compatibility mode (rollback path): synchronous reads stay
    // available only while the RAM-first service is disabled.
    QueryResult result = CharacterDatabase.Query(
        "SELECT bot_guid, mode, autonomy_enabled, goal_status, bridge_enabled "
        "FROM azeroth_friend_bots WHERE enabled = 1 ORDER BY bot_guid");

    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 botGuid = fields[0].Get<uint32>();
        std::string botMode = fields[1].Get<std::string>();
        if (botMode.empty())
            botMode = "companion";

        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
        if (bot && bot->IsInWorld() && sAzerothFriendConfig->IsBotControlled(bot->GetName()))
        {
            if (sAzerothFriendConfig->maxControlledBots > 0 && activeControlled >= sAzerothFriendConfig->maxControlledBots)
                break;
            activeControlled++;

            sAFEnvironment->UpdateBotEnvironment(bot);
            if (!fields[4].Get<bool>())
            {
                AzerothFriendAiControl::Acquire(bot, 300);
                continue;
            }
            CheckEmergencyReflexes(bot);
            ProcessBotActionsLegacy(bot, botMode);
            if (fields[2].Get<bool>() && fields[3].Get<std::string>() == "active")
                EmitAutonomyTick(bot, botMode);

            if (_catalogExported.insert(botGuid).second)
            {
                std::string summary = ExportCatalog(bot);
                LOG_INFO("server.loading", "[AzerothFriend] Action catalogue exported for {} ({})",
                         bot->GetName(), summary);
            }
        }
    } while (result->NextRow());
}

/**
 * Refresh the RAM caches asynchronously on a slow cadence. Nothing here blocks
 * the world thread: the queries are handed to the database worker pool and the
 * value-only callbacks are pumped by Update().
 */
void AzerothFriendActionDispatcher::RefreshDispatcherCaches(bool force)
{
    if (!sAFLiveState->IsRunning())
        return;

    time_t now = time(nullptr);
    if (!force && now - _lastCacheRefresh < (time_t)sAzerothFriendConfig->cacheBotRefreshSeconds)
        return;

    _lastCacheRefresh = now;
    sAFLiveState->RefreshBotControlAsync();
    sAFLiveState->RefreshTelemetryAsync();
    sAFLiveState->RefreshActivePlansAsync();

    for (auto const& entry : _lastAutonomyTick)
        sAFLiveState->RefreshThoughtAsync(entry.first);
    for (auto const& entry : _activeActions)
        sAFLiveState->RefreshHistoryAsync(entry.first);

    // Pending event counts (value cache) replace a per-tick SQL probe in
    // EmitAutonomyTick.
    sAFLiveState->SubmitAsyncQuery(
        "SELECT bot_guid, COUNT(*) FROM azeroth_friend_events WHERE status IN ('pending','processing') GROUP BY bot_guid",
        [this](QueryResult result)
        {
            std::unordered_map<uint32, uint32> counts;
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();
                    counts[fields[0].Get<uint32>()] = fields[1].Get<uint32>();
                } while (result->NextRow());
            }
            _pendingEventCount = std::move(counts);
        });
}

void AzerothFriendActionDispatcher::CheckEmergencyReflexes(Player* bot)
{
    if (!sAzerothFriendConfig->combatReflexes || !bot || !bot->IsInCombat())
        return;

    if (bot->GetHealthPct() <= sAzerothFriendConfig->emergencyThresholdPct)
    {
        if (bot->getClass() == CLASS_PALADIN)
            AzerothFriendPlayerbotActions::DoAction(bot, "divine shield");
        else if (bot->getClass() == CLASS_MAGE)
            AzerothFriendPlayerbotActions::DoAction(bot, "ice block");

        AzerothFriendPlayerbotActions::DoAction(bot, "healing potion");
        AzerothFriendPlayerbotActions::DoAction(bot, "healthstone");
    }
}

void AzerothFriendActionDispatcher::InterruptBotPlan(uint32 botGuid, std::string const& reason, bool emitEvent)
{
    _activeActions.erase(botGuid);
    _claimedSteps.erase(botGuid);

    CharacterDatabase.Execute(
        ("UPDATE azeroth_friend_actions SET status = 'interrupted', failure_reason = '" +
         AzerothFriendShared::EscapeSqlString(reason) + "' WHERE bot_guid = " + std::to_string(botGuid) +
         " AND status IN ('pending', 'in_progress')").c_str());

    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(botGuid));
    if (bot && bot->IsInWorld())
    {
        AzerothFriendPlayerbotActions::StopMovement(bot);
        AzerothFriendAiControl::Release(bot, "plan interrupted: " + reason);
    }

    if (emitEvent)
    {
        AzerothFriendShared::QueueEvent(botGuid, "plan_interrupted", 4, 0, "",
                                        "{\"reason\":\"" + AzerothFriendShared::EscapeJsonString(reason) + "\"}");
    }
}

void AzerothFriendActionDispatcher::PublishStepTelemetry(Player* bot, BotActiveAction const& action,
                                                         std::string const& status, std::string const& result,
                                                         std::string const& error)
{
    if (!bot)
        return;

    std::ostringstream payload;
    payload << "Active: " << TelemetryField(action.actionType)
            << "|Status: " << status
            << "|Step: " << (uint32)(action.stepIndex + 1) << " of " << (uint32)action.totalSteps
            << "|Params: " << TelemetryField(action.paramsJson);

    if (AzerothFriendActionRegistry::ActionInfo const* info =
            AzerothFriendActionRegistry::Find(action.actionType))
    {
        std::string verified = "pending";
        if (status == "completed")
            verified = (action.verified || action.kind != AFStepKind::Immediate) ? "true" : "false";
        else if (status == "failed")
            verified = "true";

        payload << "|ApiVersion: " << (uint32)info->apiVersion
                << "|Capability: " << info->name
                << "|Authority: " << info->authority
                << "|BindingKind: " << info->bindingKind
                << "|NativeBinding: " << (info->playerbotAction.empty() ? "none" : info->playerbotAction)
                << "|Completion: " << info->completionPolicy
                << "|Verified: " << verified;
    }

    if (!result.empty())
        payload << "|Result: " << TelemetryField(result);

    if (!error.empty())
        payload << "|Error: " << TelemetryField(error);

    AzerothFriendShared::SendTelemetry(bot, "FRIEND_ACTIONS", payload.str());
}

void AzerothFriendActionDispatcher::CompleteStep(Player* bot, BotActiveAction const& action,
                                                 std::string const& result, bool success,
                                                 std::string const& error)
{
    std::string status = success ? "completed" : "failed";
    std::string sql = "UPDATE azeroth_friend_actions SET status = '" + status +
                      "', completed_at = NOW()";

    std::string effects = result;
    if (effects.empty())
        effects = "{}";
    else if (effects.front() != '{')
        effects = "{\"result\":\"" + AzerothFriendShared::EscapeJsonString(effects) + "\"}";

    // A native action returning true only proves that dispatch was accepted.
    // Movement/wait/target-death steps and explicitly marked workflows have a
    // world-state postcondition and may be called verified.
    bool verified = !success || action.verified || action.kind != AFStepKind::Immediate;
    std::string errorJson = "null";
    if (!error.empty())
    {
        size_t codeEnd = error.find_first_of(" ('");
        std::string code = error.substr(0, codeEnd);
        bool retryable = error.find("timeout") != std::string::npos ||
                         error.find("unavailable") != std::string::npos ||
                         error.find("not_found") != std::string::npos ||
                         error.find("too_far") != std::string::npos;
        errorJson = "{\"code\":\"" + AzerothFriendShared::EscapeJsonString(code) +
                    "\",\"message\":\"" + AzerothFriendShared::EscapeJsonString(error) +
                    "\",\"retryable\":" + (retryable ? "true" : "false") + "}";
    }
    std::string jsonPayload = "{\"api_version\":1,\"capability\":\"" +
        AzerothFriendShared::EscapeJsonString(action.actionType) + "\",\"status\":\"" + status +
        "\",\"verified\":" + (verified ? "true" : "false") + ",\"effects\":" + effects +
        ",\"error\":" + errorJson + "}";
    sql += ", result_json = '" + AzerothFriendShared::EscapeSqlString(jsonPayload) + "'";

    if (!error.empty())
        sql += ", failure_reason = '" + AzerothFriendShared::EscapeSqlString(error) + "'";

    sql += " WHERE id = " + std::to_string(action.actionId);

    if (action.actionId)
        CharacterDatabase.Execute(sql.c_str());

    // Publish the verified outcome over the RAM channel: the bridge consumes it
    // once, feeds the zero-token summary engine and never replays history.
    if (bot)
    {
        uint32 botGuid = bot->GetGUID().GetCounter();
        AzerothFriend::BotControlRecord control;
        uint32 revision = sAFLiveState->GetBotControl(botGuid, control) ? control.controlRevision : 0;
        std::string outcome = action.actionType + " " + status;
        if (!error.empty())
            outcome += " (" + error + ")";
        if (verified)
            sAFLiveState->PublishActionOutcome(botGuid, revision, outcome);
        sAFLiveState->RefreshActivePlansAsync();
        sAFLiveState->RefreshHistoryAsync(botGuid);
    }

    PublishStepTelemetry(bot, action, status, result, error);

    if (sAzerothFriendConfig->debug)
    {
        LOG_DEBUG("server.loading", "[AzerothFriend] Step {} '{}' -> {} {}",
                  action.stepIndex + 1, action.actionType, status,
                  success ? result : error);
    }
}

void AzerothFriendActionDispatcher::CancelRemainingSteps(Player* bot, std::string const& planId,
                                                         std::string const& reason)
{
    if (planId.empty())
        return;

    uint32 botGuid = bot ? bot->GetGUID().GetCounter() : 0;
    std::string sql = "UPDATE azeroth_friend_actions SET status = 'interrupted', failure_reason = '" +
                      AzerothFriendShared::EscapeSqlString(reason) + "'";
    if (botGuid)
        sql += " WHERE bot_guid = " + std::to_string(botGuid) + " AND plan_id = '" +
               AzerothFriendShared::EscapeSqlString(planId) + "' AND status = 'pending'";
    else
        sql += " WHERE plan_id = '" + AzerothFriendShared::EscapeSqlString(planId) + "' AND status = 'pending'";

    CharacterDatabase.Execute(sql.c_str());

    if (bot && !reason.empty())
    {
        std::string errPayload = "error:" + reason + "|bot:" + bot->GetName();
        AzerothFriendShared::SendTelemetry(bot, "FRIEND_ERROR", errPayload);
    }
}

void AzerothFriendActionDispatcher::RunStuckRecovery(Player* bot, BotActiveAction const& action)
{
    if (!bot || !sAzerothFriendConfig->stuckDetectionEnable)
        return;

    LOG_INFO("server.loading", "[AzerothFriend] Bot {} stuck while '{}' - attempting recovery",
             bot->GetName(), action.actionType);

    if (sAzerothFriendConfig->stuckStrategy == "repath_only")
    {
        AzerothFriendPlayerbotActions::MoveToCoords(bot, action.targetX, action.targetY, action.targetZ,
                                                   action.targetRange);
        return;
    }

    AzerothFriendPlayerbotActions::MoveToCoords(bot, action.targetX, action.targetY, action.targetZ,
                                               action.targetRange);
}

void AzerothFriendActionDispatcher::AttendActiveStep(Player* bot, BotActiveAction& action)
{
    time_t now = time(nullptr);
    uint32 botGuid = bot->GetGUID().GetCounter();

    auto Finish = [&](std::string const& result, bool success, std::string const& error)
    {
        CompleteStep(bot, action, result, success, error);

        _activeActions.erase(botGuid);

        // Remaining work comes from the claimed plan shape, not from a queue
        // scan, so finishing a step never blocks the world thread on SQL.
        uint32 remaining = (!action.handedOff && action.stepIndex + 1 < action.totalSteps)
            ? (action.totalSteps - action.stepIndex - 1) : 0;
        if (!remaining)
            AzerothFriendAiControl::Release(bot, "plan complete");
    };

    // Combat Supremacy Check: If combat starts during an active step, yield immediately
    if (AzerothFriendAiControl::IsInCombatSupremacy(bot))
    {
        Finish("yielded_to_combat", false, "combat supremacy engaged");
        CancelRemainingSteps(bot, action.planId, "combat supremacy engaged");
        return;
    }

    // Elastic Tether Check: If owner moves >35y away or mounts up during active step, abort & follow
    if (!action.handedOff && !AzerothFriendAiControl::CheckOwnerTether(bot, 35.0f))
    {
        Finish("tether_broken", false, "tether distance exceeded");
        CancelRemainingSteps(bot, action.planId, "tether distance exceeded; sprinting back to formation");
        AzerothFriendPlayerbotActions::DoAction(bot, "follow");
        return;
    }

    if (!action.handedOff)
    {
        std::string leaseReason;
        if (!AzerothFriendAiControl::Acquire(
                bot, sAzerothFriendConfig->leaseRefreshSeconds, &leaseReason))
        {
            std::string error = leaseReason.empty() ? "playerbot_lease_unavailable" : leaseReason;
            CompleteStep(bot, action, "", false, error);
            CancelRemainingSteps(bot, action.planId, "playerbot control unavailable");
            _activeActions.erase(botGuid);
            return;
        }
    }

    if (action.kind == AFStepKind::Wait)
    {
        if (now >= action.deadline)
            Finish("waited", true, "");
        return;
    }

    if (action.kind == AFStepKind::TargetDead)
    {
        WorldObject* object = ResolveWorldObject(bot, action.targetGuid);
        Unit* target = object ? object->ToUnit() : nullptr;
        if (!target && action.targetGuid)
            target = AzerothFriendShared::FindPlayerByCounter(action.targetGuid);

        bool gone = (!target || !target->IsAlive() || !bot->IsValidAttackTarget(target));
        if (gone && action.targetGuid)
        {
            Finish("target_down", true, "");
            return;
        }

        if (now >= action.deadline)
        {
            Finish("timeout", false, "target still alive after " +
                   std::to_string(sAzerothFriendConfig->actionStepTimeoutSeconds) + "s");
        }
        return;
    }

    float targetX = action.targetX;
    float targetY = action.targetY;
    float targetZ = action.targetZ;
    float range = action.targetRange;

    bool entityGone = false;
    if (action.kind == AFStepKind::Approach && action.targetGuid)
    {
        WorldObject* object = ResolveWorldObject(bot, action.targetGuid);
        if (!object)
        {
            entityGone = true;
        }
        else
        {
            targetX = object->GetPositionX();
            targetY = object->GetPositionY();
            targetZ = object->GetPositionZ();
            range = std::max(range, 1.5f);
        }
    }

    float dist = std::sqrt(std::pow(targetX - bot->GetPositionX(), 2) +
                           std::pow(targetY - bot->GetPositionY(), 2) +
                           std::pow(targetZ - bot->GetPositionZ(), 2));

    if (entityGone || dist <= range)
    {
        std::string result = entityGone
            ? "target_gone"
            : ("arrived distance=" + FloatToString(dist));

        bool followUpOk = true;
        if (!action.followUpAction.empty())
        {
            followUpOk = AzerothFriendPlayerbotActions::DoAction(
                bot, action.followUpAction, action.followUpParam);
            result += followUpOk ? " followup=" + action.followUpAction + ":ok"
                                 : " followup=" + action.followUpAction + ":failed";
        }

        Finish(result, followUpOk, followUpOk ? "" : "followup_failed");
        return;
    }

    if (now >= action.deadline)
    {
        uint32 totalSec = static_cast<uint32>(action.deadline > action.startTime ? action.deadline - action.startTime : sAzerothFriendConfig->actionStepTimeoutSeconds);
        if (action.actionType == "loot" || action.actionType == "loot_nearest" || action.followUpAction == "loot")
        {
            if (action.targetGuid)
                MarkCorpseUnreachable(action.targetGuid, 60);
            Finish("unreachable_corpse", false, "timed out reaching corpse after " + std::to_string(totalSec) + "s");
            AzerothFriendPlayerbotActions::DoAction(bot, "follow");
            return;
        }
        Finish("timeout", false, "did not reach target within " + std::to_string(totalSec) + "s");
        return;
    }

    if (sAzerothFriendConfig->stuckDetectionEnable &&
        now - action.lastProgressCheck >= static_cast<time_t>(sAzerothFriendConfig->stuckTimeoutSeconds))
    {
        float moved = std::sqrt(std::pow(bot->GetPositionX() - action.lastCheckX, 2) +
                                std::pow(bot->GetPositionY() - action.lastCheckY, 2));
        if (moved < 1.0f)
        {
            bool isLoot = (action.actionType == "loot" || action.actionType == "loot_nearest" || action.followUpAction == "loot");
            uint8 maxRetries = isLoot ? std::min<uint8>(sAzerothFriendConfig->actionStepRetries, 2) : sAzerothFriendConfig->actionStepRetries;
            if (action.attempts < maxRetries)
            {
                action.attempts++;
                RunStuckRecovery(bot, action);
            }
            else
            {
                if (isLoot)
                {
                    if (action.targetGuid)
                        MarkCorpseUnreachable(action.targetGuid, 60);
                    Finish("unreachable_corpse", false, "stuck and unable to reach corpse after recovery attempts");
                    AzerothFriendPlayerbotActions::DoAction(bot, "follow");
                    return;
                }
                Finish("unreachable", false, "stuck and no progress after recovery attempts");
                return;
            }
        }
        action.lastProgressCheck = now;
        action.lastCheckX = bot->GetPositionX();
        action.lastCheckY = bot->GetPositionY();
    }
}

/**
 * RAM-first step dispatch.
 *
 * The world thread never selects from the action queue itself. Instead it asks
 * the database worker for the next pending row; the value-only callback runs on
 * the world thread, validates the control revision against the RAM control
 * state, and hands the step to StartClaimedStep(). A missed outcome notification
 * therefore reconciles through the exact pending/in-progress lookup rather than a
 * history scan.
 */
void AzerothFriendActionDispatcher::ProcessBotActions(Player* bot, std::string const& botMode)
{
    uint32 botGuid = bot->GetGUID().GetCounter();

    auto it = _activeActions.find(botGuid);
    if (it != _activeActions.end())
    {
        AttendActiveStep(bot, it->second);
        return;
    }

    auto claimed = _claimedSteps.find(botGuid);
    if (claimed != _claimedSteps.end())
    {
        BotClaimedStep step = claimed->second;
        _claimedSteps.erase(claimed);
        StartClaimedStep(bot, step);
        return;
    }

    if (_claimInFlight.count(botGuid))
        return;

    RequestNextStep(botGuid);

    // Native autonomy ticks still observe the bot even while a step is in flight.
    (void)botMode;
}

void AzerothFriendActionDispatcher::RequestNextStep(uint32 botGuid)
{
    if (!botGuid)
        return;

    _claimInFlight.insert(botGuid);
    std::string sql = "SELECT id, plan_id, step_index, total_steps, action_type, params_json, "
        "COALESCE(thought, ''), COALESCE(authority, 'autonomous'), COALESCE(origin_event_id, 0), "
        "COALESCE(origin_source_guid, 0) FROM azeroth_friend_actions "
                      "WHERE bot_guid = " + std::to_string(botGuid) + " AND status = 'pending' "
                      "ORDER BY step_index ASC, id ASC LIMIT 1";

    sAFLiveState->SubmitAsyncQuery(sql, [this, botGuid](QueryResult result)
    {
        _claimInFlight.erase(botGuid);
        if (!result)
            return;

        Field* fields = result->Fetch();
        BotClaimedStep step;
        step.actionId = fields[0].Get<uint64>();
        step.botGuid = botGuid;
        step.planId = fields[1].Get<std::string>();
        step.stepIndex = fields[2].Get<uint8>();
        step.totalSteps = fields[3].Get<uint8>();
        step.actionType = fields[4].Get<std::string>();
        step.paramsJson = fields[5].Get<std::string>();
        step.thought = fields[6].Get<std::string>();
        step.authority = fields[7].Get<std::string>();
        step.originEventId = fields[8].Get<uint64>();
        step.originSourceGuid = fields[9].Get<uint32>();

        // Control-revision gate: an owner command issued while the plan was in
        // flight bumps the revision, and the stale step is discarded here.
        AzerothFriend::BotControlRecord control;
        if (!sAFLiveState->GetBotControl(botGuid, control))
        {
            // Control state not cached yet (startup): keep the step pending and
            // wait for the asynchronous refresh instead of guessing.
            sAFLiveState->RefreshBotControlAsync();
            return;
        }

        Player* bot = AzerothFriendShared::FindPlayerByCounter(botGuid);

        if (!control.bridgeEnabled)
        {
            CancelRemainingSteps(bot, step.planId, "AzerothFriend bridge disconnected by Claim");
            return;
        }

        uint32 stepRev = ExtractJsonUint(step.paramsJson, "_control_revision");
        if (step.paramsJson.find("_control_revision") == std::string::npos || stepRev < control.controlRevision)
        {
            CancelRemainingSteps(bot, step.planId, "stale control revision");
            return;
        }

        if (stepRev > control.controlRevision)
        {
            // Python planned against a newer database revision that the async cache hasn't loaded yet.
            sAFLiveState->RefreshBotControlAsync();
            return;
        }

        step.controlRevision = stepRev;
        _claimedSteps[botGuid] = step;
    });
}

void AzerothFriendActionDispatcher::StartClaimedStep(Player* bot, BotClaimedStep const& claimed)
{
    if (!bot || !bot->IsInWorld())
        return;

    uint32 botGuid = bot->GetGUID().GetCounter();
    if (!sAFLiveState->IsBotBridgeEnabled(botGuid))
    {
        CancelRemainingSteps(bot, claimed.planId, "AzerothFriend bridge disconnected by Claim");
        return;
    }
    time_t now = time(nullptr);

    // Claim the row durably. Execute() enqueues without waiting for a result.
    CharacterDatabase.Execute(
        ("UPDATE azeroth_friend_actions SET status = 'in_progress', started_at = NOW() WHERE id = " +
         std::to_string(claimed.actionId) + " AND status = 'pending'").c_str());

    if (!claimed.thought.empty())
    {
        std::string thoughtPayload = "Bot: " + bot->GetName() + "|PlanID: " + claimed.planId;
        if (claimed.thought.find("Thought:") == 0)
            thoughtPayload += "|" + claimed.thought;
        else
            thoughtPayload += "|Thought: " + claimed.thought;
        AzerothFriendShared::SendTelemetry(bot, "FRIEND_THOUGHT", thoughtPayload);
    }

    BotActiveAction active;
    active.actionId = claimed.actionId;
    active.planId = claimed.planId;
    active.stepIndex = claimed.stepIndex;
    active.totalSteps = claimed.totalSteps;
    active.actionType = claimed.actionType;
    active.paramsJson = claimed.paramsJson;
    active.startTime = now;
    active.lastProgressCheck = now;
    active.lastCheckX = bot->GetPositionX();
    active.lastCheckY = bot->GetPositionY();

    if (!AzerothFriendActionRegistry::IsCurated(claimed.actionType))
    {
        CompleteStep(bot, active, "", false, "unknown_action '" + claimed.actionType + "'");
        return;
    }

    AzerothFriend::BotControlRecord control;
    if (!sAFLiveState->GetBotControl(botGuid, control))
    {
        CompleteStep(bot, active, "", false, "control_state_unavailable");
        return;
    }
    if (!sAzerothFriendConfig->botApiEnable)
    {
        CompleteStep(bot, active, "", false, "bot_api_disabled");
        CancelRemainingSteps(bot, claimed.planId, "Bot API disabled");
        return;
    }
    else
    {
        bool ownerTier = AzerothFriendActionRegistry::RequiredAuthority(claimed.actionType) ==
            AzerothFriendActionRegistry::ActionAuthority::OwnerCommand;
        if ((ownerTier && !sAzerothFriendConfig->botApiOwnerTierEnable) ||
            !AzerothFriendActionRegistry::IsAuthorized(claimed.actionType, claimed.authority,
                                                       claimed.originSourceGuid, control.masterGuid))
        {
            CompleteStep(bot, active, "", false, "authority_denied");
            CancelRemainingSteps(bot, claimed.planId, "authority denied for '" + claimed.actionType + "'");
            return;
        }
    }

    // Combat Supremacy Gate: If bot or master is in combat, yield 100% to mod-playerbots BOT_STATE_COMBAT
    if (AzerothFriendAiControl::IsInCombatSupremacy(bot))
    {
        CompleteStep(bot, active, "", false, "yielded_to_combat_supremacy");
        CancelRemainingSteps(bot, claimed.planId, "yielded to native playerbot combat supremacy");
        return;
    }

    // Elastic Tether Gate: Autonomous actions require bot to be within tether distance (default 35y)
    if (claimed.authority == "autonomous" && !AzerothFriendAiControl::CheckOwnerTether(bot, 35.0f))
    {
        CompleteStep(bot, active, "", false, "tether_broken_distance_exceeded");
        CancelRemainingSteps(bot, claimed.planId, "owner tether broken; sprinting back to formation");
        AzerothFriendPlayerbotActions::DoAction(bot, "follow");
        return;
    }

    std::string leaseReason;
    if (!AzerothFriendAiControl::Acquire(bot, sAzerothFriendConfig->leaseRefreshSeconds, &leaseReason))
    {
        CompleteStep(bot, active, "", false,
                     leaseReason.empty() ? "playerbot_lease_unavailable" : leaseReason);
        CancelRemainingSteps(bot, claimed.planId, "playerbot control unavailable");
        return;
    }

    StepPlan plan;
    std::string error;
    if (!TryBuildStep(bot, claimed.actionType, claimed.paramsJson, plan, error))
    {
        CompleteStep(bot, active, plan.result, false, error);

        // Remaining work is derived from the claimed plan shape (step index and
        // total steps), so no "how many rows are pending" scan is needed.
        uint32 remaining = (claimed.stepIndex + 1 < claimed.totalSteps) ? (claimed.totalSteps - claimed.stepIndex - 1) : 0;
        if (!remaining)
            AzerothFriendAiControl::Release(bot, "plan finished with failure");
        return;
    }

    PublishStepTelemetry(bot, active, "in_progress", plan.result, "");

    bool const handoff = (plan.followUpAction == "__handoff");
    active.verified = plan.verified;

    if (plan.kind == AFStepKind::Immediate)
    {
        CompleteStep(bot, active, plan.result, true, "");

        uint32 remaining = (claimed.stepIndex + 1 < claimed.totalSteps) ? (claimed.totalSteps - claimed.stepIndex - 1) : 0;

        if (handoff)
        {
            // A long-lived behaviour change (follow/grind/travel) supersedes the
            // rest of the plan: playerbots takes over from here by design.
            CancelRemainingSteps(bot, claimed.planId,
                                 "superseded by long-lived behaviour '" + claimed.actionType + "'");
            remaining = 0;
        }

        if (!remaining)
            AzerothFriendAiControl::Release(bot, handoff ? "mode action handed off" : "plan complete", handoff);
        return;
    }

    active.kind = plan.kind;
    active.followUpAction = plan.followUpAction;
    active.followUpParam = plan.followUpParam;
    active.targetGuid = plan.targetGuid;
    active.targetName = plan.targetName;
    active.targetX = plan.targetX;
    active.targetY = plan.targetY;
    active.targetZ = plan.targetZ;
    active.targetRange = plan.targetRange;
    uint32 stepTimeout = plan.waitSeconds;
    if (!stepTimeout)
    {
        stepTimeout = sAzerothFriendConfig->actionStepTimeoutSeconds;
        if (plan.kind == AFStepKind::Approach && (plan.targetX != 0.0f || plan.targetY != 0.0f))
        {
            float dist = std::sqrt(std::pow(bot->GetPositionX() - plan.targetX, 2) + std::pow(bot->GetPositionY() - plan.targetY, 2));
            stepTimeout = std::max(stepTimeout, static_cast<uint32>(dist / 4.0f) + 8);
        }
    }
    if (active.actionType == "loot" || active.actionType == "loot_nearest" || plan.followUpAction == "loot")
    {
        stepTimeout = std::min(stepTimeout, 12u);
    }
    active.deadline = now + stepTimeout;
    active.handedOff = handoff;

    if (active.kind == AFStepKind::Approach)
    {
        if (!AzerothFriendPlayerbotActions::MoveToCoords(
                bot, active.targetX, active.targetY, active.targetZ, active.targetRange))
        {
            CompleteStep(bot, active, plan.result, false, "move_rejected");
            uint32 remaining = (claimed.stepIndex + 1 < claimed.totalSteps)
                ? (claimed.totalSteps - claimed.stepIndex - 1) : 0;
            if (!remaining)
                AzerothFriendAiControl::Release(bot, "plan movement rejected");
            return;
        }
    }

    _activeActions[botGuid] = active;
    PublishStepTelemetry(bot, active, "in_progress", plan.result, "");
}

void AzerothFriendActionDispatcher::ProcessBotActionsLegacy(Player* bot, std::string const& /*botMode*/)
{
    uint32 botGuid = bot->GetGUID().GetCounter();
    time_t now = time(nullptr);

    auto it = _activeActions.find(botGuid);
    if (it != _activeActions.end())
    {
        AttendActiveStep(bot, it->second);
        return;
    }

    std::string sql = "SELECT id, plan_id, step_index, total_steps, action_type, params_json, "
                      "COALESCE(thought, ''), COALESCE(authority, 'autonomous'), COALESCE(origin_event_id, 0), "
                      "COALESCE(origin_source_guid, 0) FROM azeroth_friend_actions "
                      "WHERE bot_guid = " + std::to_string(botGuid) + " AND status = 'pending' "
                      "ORDER BY step_index ASC, id ASC LIMIT 1";

    QueryResult result = CharacterDatabase.Query(sql.c_str());
    if (!result)
        return;

    Field* fields = result->Fetch();
    uint64 actionId = fields[0].Get<uint64>();
    std::string planId = fields[1].Get<std::string>();
    uint8 stepIndex = fields[2].Get<uint8>();
    uint8 totalSteps = fields[3].Get<uint8>();
    std::string actionType = fields[4].Get<std::string>();
    std::string paramsJson = fields[5].Get<std::string>();
    std::string thought = fields[6].Get<std::string>();
    std::string authority = fields[7].Get<std::string>();
    uint32 originSourceGuid = fields[9].Get<uint32>();

    QueryResult revision = CharacterDatabase.Query(("SELECT control_revision, master_guid, bridge_enabled "
        "FROM azeroth_friend_bots WHERE bot_guid=" +
        std::to_string(botGuid)).c_str());
    if (!revision)
    {
        CancelRemainingSteps(bot, planId, "control state unavailable");
        return;
    }

    Field* controlFields = revision->Fetch();
    uint32 liveRevision = controlFields[0].Get<uint32>();
    uint32 masterGuid = controlFields[1].Get<uint32>();
    bool bridgeEnabled = controlFields[2].Get<uint8>() != 0;
    uint32 stepRevision = ExtractJsonUint(paramsJson, "_control_revision");
    if (!bridgeEnabled)
    {
        CancelRemainingSteps(bot, planId, "AzerothFriend bridge disconnected by Claim");
        return;
    }
    if (paramsJson.find("_control_revision") == std::string::npos || stepRevision < liveRevision)
    {
        CancelRemainingSteps(bot, planId, "stale control revision");
        return;
    }
    if (stepRevision > liveRevision)
    {
        // SQL compatibility mode has no newer RAM snapshot to wait for, so a
        // future revision is malformed and must fail closed.
        CancelRemainingSteps(bot, planId, "future control revision");
        return;
    }

    CharacterDatabase.Execute(
        ("UPDATE azeroth_friend_actions SET status = 'in_progress', started_at = NOW() WHERE id = " +
         std::to_string(actionId)).c_str());

    if (!thought.empty())
    {
        std::string thoughtPayload = "Bot: " + bot->GetName() + "|PlanID: " + planId;
        if (thought.find("Thought:") == 0)
            thoughtPayload += "|" + thought;
        else
            thoughtPayload += "|Thought: " + thought;
        AzerothFriendShared::SendTelemetry(bot, "FRIEND_THOUGHT", thoughtPayload);
    }

    BotActiveAction active;
    active.actionId = actionId;
    active.planId = planId;
    active.stepIndex = stepIndex;
    active.totalSteps = totalSteps;
    active.actionType = actionType;
    active.paramsJson = paramsJson;
    active.startTime = now;
    active.lastProgressCheck = now;
    active.lastCheckX = bot->GetPositionX();
    active.lastCheckY = bot->GetPositionY();

    if (!AzerothFriendActionRegistry::IsCurated(actionType))
    {
        CompleteStep(bot, active, "", false, "unknown_action '" + actionType + "'");
        return;
    }

    if (!sAzerothFriendConfig->botApiEnable)
    {
        CompleteStep(bot, active, "", false, "bot_api_disabled");
        CancelRemainingSteps(bot, planId, "Bot API disabled");
        return;
    }
    else
    {
        bool ownerTier = AzerothFriendActionRegistry::RequiredAuthority(actionType) ==
            AzerothFriendActionRegistry::ActionAuthority::OwnerCommand;
        if ((ownerTier && !sAzerothFriendConfig->botApiOwnerTierEnable) ||
            !AzerothFriendActionRegistry::IsAuthorized(actionType, authority, originSourceGuid, masterGuid))
        {
            CompleteStep(bot, active, "", false, "authority_denied");
            CancelRemainingSteps(bot, planId, "authority denied for '" + actionType + "'");
            return;
        }
    }

    std::string leaseReason;
    if (!AzerothFriendAiControl::Acquire(bot, sAzerothFriendConfig->leaseRefreshSeconds, &leaseReason))
    {
        CompleteStep(bot, active, "", false,
                     leaseReason.empty() ? "playerbot_lease_unavailable" : leaseReason);
        CancelRemainingSteps(bot, planId, "playerbot control unavailable");
        return;
    }

    StepPlan plan;
    std::string error;
    if (!TryBuildStep(bot, actionType, paramsJson, plan, error))
    {
        CompleteStep(bot, active, plan.result, false, error);

        QueryResult pending = CharacterDatabase.Query(
            ("SELECT COUNT(*) FROM azeroth_friend_actions WHERE bot_guid = " + std::to_string(botGuid) +
             " AND plan_id = '" + AzerothFriendShared::EscapeSqlString(planId) +
             "' AND status IN ('pending','in_progress')").c_str());
        uint32 remaining = pending ? pending->Fetch()[0].Get<uint32>() : 0;
        if (!remaining)
            AzerothFriendAiControl::Release(bot, "plan finished with failure");
        return;
    }

    PublishStepTelemetry(bot, active, "in_progress", plan.result, "");

    bool const handoff = (plan.followUpAction == "__handoff");
    active.verified = plan.verified;

    if (plan.kind == AFStepKind::Immediate)
    {
        CompleteStep(bot, active, plan.result, true, "");

        QueryResult pending = CharacterDatabase.Query(
            ("SELECT COUNT(*) FROM azeroth_friend_actions WHERE bot_guid = " + std::to_string(botGuid) +
             " AND plan_id = '" + AzerothFriendShared::EscapeSqlString(planId) +
             "' AND status IN ('pending','in_progress')").c_str());
        uint32 remaining = pending ? pending->Fetch()[0].Get<uint32>() : 0;

        if (handoff)
        {
            // A long-lived behaviour change (follow/grind/travel) supersedes the
            // rest of the plan: playerbots takes over from here by design.
            CancelRemainingSteps(bot, planId,
                                 "superseded by long-lived behaviour '" + actionType + "'");
            if (remaining)
                remaining = 0;
        }

        if (!remaining)
            AzerothFriendAiControl::Release(bot, handoff ? "mode action handed off" : "plan complete", handoff);
        return;
    }

    active.kind = plan.kind;
    active.followUpAction = plan.followUpAction;
    active.followUpParam = plan.followUpParam;
    active.targetGuid = plan.targetGuid;
    active.targetName = plan.targetName;
    active.targetX = plan.targetX;
    active.targetY = plan.targetY;
    active.targetZ = plan.targetZ;
    active.targetRange = plan.targetRange;
    uint32 stepTimeout = plan.waitSeconds;
    if (!stepTimeout)
    {
        stepTimeout = sAzerothFriendConfig->actionStepTimeoutSeconds;
        if (plan.kind == AFStepKind::Approach && (plan.targetX != 0.0f || plan.targetY != 0.0f))
        {
            float dist = std::sqrt(std::pow(bot->GetPositionX() - plan.targetX, 2) + std::pow(bot->GetPositionY() - plan.targetY, 2));
            stepTimeout = std::max(stepTimeout, static_cast<uint32>(dist / 4.0f) + 8);
        }
    }
    if (active.actionType == "loot" || active.actionType == "loot_nearest" || plan.followUpAction == "loot")
    {
        stepTimeout = std::min(stepTimeout, 12u);
    }
    active.deadline = now + stepTimeout;
    active.handedOff = handoff;

    if (active.kind == AFStepKind::Approach)
    {
        if (!AzerothFriendPlayerbotActions::MoveToCoords(
                bot, active.targetX, active.targetY, active.targetZ, active.targetRange))
        {
            CompleteStep(bot, active, plan.result, false, "move_rejected");
            uint32 remaining = (stepIndex + 1 < totalSteps) ? (totalSteps - stepIndex - 1) : 0;
            if (!remaining)
                AzerothFriendAiControl::Release(bot, "plan movement rejected");
            return;
        }
    }

    _activeActions[botGuid] = active;
}

bool AzerothFriendActionDispatcher::TryBuildStep(Player* bot, std::string const& actionType,
                                                 std::string const& paramsJson, StepPlan& plan,
                                                 std::string& error)
{
    auto Fail = [&](std::string const& message)
    {
        error = message;
        return false;
    };

    auto Payload = [](std::string const& body) { return "{" + body + "}"; };

    // ------------------------------------------------------------------ movement
    if (actionType == "move_to")
    {
        float x = ExtractJsonFloat(paramsJson, "x");
        float y = ExtractJsonFloat(paramsJson, "y");
        float z = ExtractJsonFloat(paramsJson, "z");
        float range = ExtractJsonFloat(paramsJson, "range", 2.0f);

        if (x == 0.0f && y == 0.0f && z == 0.0f)
            return Fail("move_to requires x/y/z");

        if (!AzerothFriendPlayerbotActions::MoveToCoords(bot, x, y, z, range))
            return Fail("move_rejected");

        plan.kind = AFStepKind::Move;
        plan.targetX = x;
        plan.targetY = y;
        plan.targetZ = z;
        plan.targetRange = range;
        plan.result = Payload("\"destination\":[" + FloatToString(x) + "," + FloatToString(y) + "," +
                              FloatToString(z) + "],\"range\":" + FloatToString(range));
        return true;
    }

    if (actionType == "go_to" || actionType == "interact" || actionType == "use_object" ||
        actionType == "talk_to" || actionType == "attack")
    {
        uint32 guid = ExtractJsonUint(paramsJson, "guid");
        std::string name = ExtractJsonString(paramsJson, "name");

        WorldObject* object = nullptr;
        if (guid)
            object = ResolveWorldObject(bot, guid);

        if (!object && !name.empty() && bot->GetMap())
        {
            std::string nameLower = name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
            {
                Creature* creature = pair.second;
                if (creature && creature->IsInWorld())
                {
                    std::string cName = creature->GetName();
                    std::transform(cName.begin(), cName.end(), cName.begin(), ::tolower);
                    if (cName.find(nameLower) != std::string::npos)
                    {
                        object = creature;
                        break;
                    }
                }
            }
        }

        if (!object)
        {
            Player* master = nullptr;
#if AF_HAS_PLAYERBOTS
            PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (ai)
                master = ai->GetMaster();
#endif
            if (!master && bot->GetGroup())
                master = ObjectAccessor::FindConnectedPlayer(bot->GetGroup()->GetLeaderGUID());

            if (master && master->GetSelectedUnit())
                object = master->GetSelectedUnit();
        }

        if (!object && actionType != "attack")
            return Fail(actionType + " target not found");

        if (actionType == "attack")
        {
            Unit* target = object ? object->ToUnit() : nullptr;
            if (!target)
            {
                Group* group = bot->GetGroup();
                Player* master = group ? ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID()) : nullptr;
                target = master ? master->GetSelectedUnit() : nullptr;
                if (!target)
                    target = bot->GetSelectedUnit();
            }

            if (!target || !target->IsAlive() || !bot->IsValidAttackTarget(target) || bot->IsFriendlyTo(target))
                return Fail("invalid_attack_target");

            bot->SetSelection(target->GetGUID());
            bot->SetFacingToObject(target);
            if (!AzerothFriendBotController::Attack(bot, target->GetGUID().GetCounter()))
                return Fail("attack_failed");

            plan.kind = AFStepKind::TargetDead;
            plan.targetGuid = target->GetGUID().GetCounter();
            plan.targetName = target->GetName();
            plan.result = Payload("\"target\":\"" + AzerothFriendShared::EscapeJsonString(target->GetName()) + "\"");
            return true;
        }

        float range = ExtractJsonFloat(paramsJson, "range", actionType == "go_to" ? 3.0f : 4.5f);
        plan.kind = AFStepKind::Approach;
        plan.targetGuid = object->GetGUID().GetCounter();
        plan.targetName = object->GetName();
        plan.targetX = object->GetPositionX();
        plan.targetY = object->GetPositionY();
        plan.targetZ = object->GetPositionZ();
        plan.targetRange = range;

        if (actionType == "talk_to" || actionType == "interact")
            plan.followUpAction = object->ToGameObject() ? "use" : "talk";
        else if (actionType == "use_object")
            plan.followUpAction = "use";

        plan.result = Payload("\"target\":\"" + AzerothFriendShared::EscapeJsonString(object->GetName()) +
                              "\",\"kind\":\"" + CreatureKind(object->ToCreature()) + "\"");
        return true;
    }

    if (actionType == "follow")
    {
        if (!AzerothFriendPlayerbotActions::FollowMaster(bot))
            return Fail("follow_failed");

        if (!AzerothFriendAiControl::ApplyMode(bot, "follow"))
            return Fail("follow_mode_failed");
        plan.kind = AFStepKind::Immediate;
        plan.followUpAction = "__handoff";
        plan.result = Payload("\"mode\":\"follow\"");
        return true;
    }

    if (actionType == "grind")
    {
        if (!AzerothFriendAiControl::ApplyMode(bot, "grind") ||
            !AzerothFriendPlayerbotActions::DoAction(bot, "grind"))
            return Fail("grind_failed");
        plan.kind = AFStepKind::Immediate;
        plan.followUpAction = "__handoff";
        plan.result = Payload("\"mode\":\"grind\"");
        return true;
    }

    if (actionType == "wander")
    {
        if (!AzerothFriendAiControl::ApplyMode(bot, "default") ||
            !AzerothFriendPlayerbotActions::DoAction(bot, "move random"))
            return Fail("wander_failed");
        plan.kind = AFStepKind::Immediate;
        plan.followUpAction = "__handoff";
        plan.result = Payload("\"mode\":\"wander\"");
        return true;
    }

    if (actionType == "travel_to")
    {
        std::string destination = ExtractJsonString(paramsJson, "destination");
        if (destination.empty())
            destination = ExtractJsonString(paramsJson, "param");
        if (destination.empty())
            return Fail("travel_to requires a destination");

        if (!AzerothFriendAiControl::ApplyMode(bot, "travel") ||
            !AzerothFriendPlayerbotActions::DoAction(bot, "go", "travel " + destination))
            return Fail("travel_to_failed");
        plan.kind = AFStepKind::Immediate;
        plan.followUpAction = "__handoff";
        plan.result = Payload("\"mode\":\"travel\",\"destination\":\"" +
                              AzerothFriendShared::EscapeJsonString(destination) + "\"");
        return true;
    }

    if (actionType == "taxi")
    {
        std::string destination = ExtractJsonString(paramsJson, "destination");
        if (destination.empty())
            return Fail("taxi requires destination (flight option number)");
        if (!AzerothFriendPlayerbotActions::DoCommand(bot, "taxi " + destination))
            return Fail("taxi_failed");

        plan.kind = AFStepKind::Immediate;
        plan.followUpAction = "__handoff";
        plan.result = Payload("\"taxi\":\"" + AzerothFriendShared::EscapeJsonString(destination) + "\"");
        return true;
    }

    if (actionType == "stay" || actionType == "stop")
    {
        if (!AzerothFriendPlayerbotActions::StopMovement(bot) ||
            !AzerothFriendAiControl::ApplyMode(bot, "stay"))
            return Fail("stay_failed");
        plan.kind = AFStepKind::Immediate;
        plan.followUpAction = "__handoff";
        plan.result = Payload("\"mode\":\"hold\"");
        return true;
    }

    if (actionType == "mount")
    {
        if (!AzerothFriendPlayerbotActions::DoAction(bot, "mount"))
            return Fail("mount_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"mounted\":true");
        return true;
    }

    if (actionType == "dismount")
    {
        bot->Dismount();
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"mounted\":false");
        return true;
    }

    if (actionType == "flee")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "flee");
        if (!ok)
            return Fail("flee_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"flee\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "runaway")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "runaway");
        if (!ok)
            ok = AzerothFriendPlayerbotActions::DoAction(bot, "flee");
        if (!ok)
            return Fail("runaway_failed");
        plan.kind = AFStepKind::Immediate;
        plan.followUpAction = "__handoff";
        plan.result = Payload(std::string("\"runaway\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "summon")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "summon");
        if (!ok)
            return Fail("summon_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"summon\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "disperse")
    {
        uint32 dist = ExtractJsonUint(paramsJson, "distance", 10);
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "disperse set " + std::to_string(dist));
        if (!ok)
            return Fail("disperse_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"disperse\":" + std::to_string(dist) + ",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "disperse_disable")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "disperse disable");
        if (!ok)
            return Fail("disperse_disable_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"disperse_disable\":") + (ok ? "true" : "false"));
        return true;
    }

    // -------------------------------------------------------------------- combat
    if (actionType == "attack_my_target")
    {
#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        {
            ai->ChangeStrategy("-wait for attack", BOT_STATE_COMBAT);
            if (ai->GetAiObjectContext())
            {
                if (auto* waitVal = ai->GetAiObjectContext()->GetValue<uint8>("wait for attack time"))
                    waitVal->Set(0);
            }
        }
#endif
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "do attack my target");
        if (!ok)
            return Fail("attack_my_target_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"attack_my_target\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "assist")
    {
#if AF_HAS_PLAYERBOTS
        if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
        {
            ai->ChangeStrategy("-wait for attack", BOT_STATE_COMBAT);
            if (ai->GetAiObjectContext())
            {
                if (auto* waitVal = ai->GetAiObjectContext()->GetValue<uint8>("wait for attack time"))
                    waitVal->Set(0);
            }
        }
#endif
        std::string role = ExtractJsonString(paramsJson, "role");
        std::string action = "dps assist";
        if (role == "tank")
            action = "tank assist";
        else if (role == "aoe")
            action = "dps aoe";

        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, action);
        if (!ok)
            return Fail("assist_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"assist\":\"" + action + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "aoe")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "dps aoe");
        if (!ok)
            return Fail("aoe_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"aoe\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "pull")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "pull my target");
        if (!ok)
            return Fail("pull_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"pull\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "pull_back")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "pull back");
        if (!ok)
            return Fail("pull_back_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"pull_back\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "mark_rti")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "mark rti");
        if (!ok)
            return Fail("mark_rti_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"mark_rti\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "behind")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "co +behind");
        if (!ok)
            return Fail("behind_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"behind\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "tank_face")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "co +tank face");
        if (!ok)
            return Fail("tank_face_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"tank_face\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "focus")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "co +focus");
        if (!ok)
            return Fail("focus_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"focus\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "threat")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "co +threat");
        if (!ok)
            return Fail("threat_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"threat\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "boost")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "co +boost");
        if (!ok)
            return Fail("boost_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"boost\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "cc")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "co +cc");
        if (!ok)
            return Fail("cc_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"cc\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "cast" || actionType == "cast_on")
    {
        uint32 spellId = ExtractJsonUint(paramsJson, "spellid");
        std::string spellName = ExtractJsonString(paramsJson, "spell");
        uint32 targetGuid = ExtractJsonUint(paramsJson, "guid", 0);
        std::string targetName = ExtractJsonString(paramsJson, "target");
        Unit* resolvedTarget = nullptr;
        if (!targetName.empty())
        {
            Player* master = nullptr;
#if AF_HAS_PLAYERBOTS
            if (PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(bot))
                master = ai->GetMaster();
#endif
            Unit* target = nullptr;
            if (targetName == "self")
                target = bot;
            else if (targetName == "me" || targetName == "master" || targetName == "owner")
                target = master;
            else if (targetName == "target" || targetName == "master_target")
                target = master ? ObjectAccessor::GetUnit(*master, master->GetTarget()) : nullptr;
            else
            {
                target = ObjectAccessor::FindPlayerByName(targetName.c_str());
                if (!target)
                {
                    for (auto const& entry : bot->GetMap()->GetCreatureBySpawnIdStore())
                    {
                        Creature* creature = entry.second;
                        if (creature && bot->IsWithinDistInMap(creature, sAzerothFriendConfig->scanRadius) &&
                            AzerothFriendShared::EqualCaseInsensitive(creature->GetName(), targetName))
                        {
                            if (target)
                                return Fail("ambiguous_spell_target");
                            target = creature;
                        }
                    }
                }
            }
            if (!target || target->GetMap() != bot->GetMap())
                return Fail("invalid_spell_target");
            targetGuid = target->GetGUID().GetCounter();
            resolvedTarget = target;
        }

        if (!spellId && spellName.empty())
            return Fail("cast requires spellid or spell name");

        if (!spellId && !spellName.empty())
            spellId = AzerothFriendBotController::ResolveSpellId(bot, spellName);

        bool ok = false;
        if (spellId)
            ok = AzerothFriendBotController::CastSpell(bot, spellId, targetGuid, resolvedTarget);
        
        if (!ok)
            return Fail("cast_failed: check learned rank, target, power, cooldown, range and line of sight");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"cast\":" + IntToString(spellId) + ",\"spell\":\"" + AzerothFriendShared::EscapeJsonString(spellName) + "\"");
        return true;
    }

    if (actionType == "spell_exclude")
    {
        std::string action = ExtractJsonString(paramsJson, "action");
        if (action.empty())
            action = "add";
        uint32 spellId = ExtractJsonUint(paramsJson, "spellid");
        std::string spellName = ExtractJsonString(paramsJson, "spell");

        if (!spellId && !spellName.empty())
            spellId = AzerothFriendBotController::ResolveSpellId(bot, spellName);

        if (!spellId && action != "reset")
            return Fail("spell_exclude requires a valid spell id or resolvable spell name");

        std::string cmd;
        if (action == "reset")
            cmd = "ss reset";
        else if (action == "remove")
            cmd = "ss -" + std::to_string(spellId);
        else
            cmd = "ss +" + std::to_string(spellId);

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, cmd);
        if (!ok)
            return Fail("spell_exclude_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"spell_exclude\":\"" + cmd + "\",\"spellid\":" + IntToString(spellId) + ",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "pet_attack")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "pet attack");
        if (!ok)
            return Fail("pet_attack_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"pet_attack\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "pet_summon")
    {
        std::string pet = ExtractJsonString(paramsJson, "pet");
        if (pet.empty())
            return Fail("pet_summon requires pet name");

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "co +" + pet);
        if (!ok)
            return Fail("pet_summon_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"pet\":\"" + AzerothFriendShared::EscapeJsonString(pet) + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "soulstone")
    {
        std::string target = ExtractJsonString(paramsJson, "target");
        if (target.empty())
            target = "master";

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "ss " + target);
        if (!ok)
            return Fail("soulstone_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"soulstone\":\"" + AzerothFriendShared::EscapeJsonString(target) + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "use_trinket")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "use trinket");
        if (!ok)
            return Fail("use_trinket_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"trinket\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "racial")
    {
        std::string racialAction;
        switch (bot->getRace())
        {
            case RACE_ORC:            racialAction = "blood fury"; break;
            case RACE_TROLL:          racialAction = "berserking"; break;
            case RACE_UNDEAD_PLAYER:  racialAction = "will of the forsaken"; break;
            case RACE_DWARF:          racialAction = "stoneform"; break;
            case RACE_GNOME:          racialAction = "escape artist"; break;
            case RACE_TAUREN:         racialAction = "war stomp"; break;
            case RACE_HUMAN:          racialAction = "every man for himself"; break;
            default: break;
        }

        if (racialAction.empty() || !AzerothFriendPlayerbotActions::IsSupportedAction(bot, racialAction))
            return Fail("no_supported_racial");

        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, racialAction);
        if (!ok)
            return Fail("racial_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"racial\":\"" + racialAction + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    // --------------------------------------------------------------- quests/NPC
    if (actionType == "accept_quest")
    {
        uint32 questId = ExtractJsonUint(paramsJson, "id");
        if (!questId)
            return Fail("accept_quest requires id");

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return Fail("unknown_quest " + IntToString(questId));

        if (bot->GetQuestStatus(questId) != QUEST_STATUS_NONE)
        {
            plan.kind = AFStepKind::Immediate;
            plan.result = Payload("\"quest\":" + IntToString(questId) + ",\"status\":\"already_known\"");
            return true;
        }

        bool accepted = AzerothFriendPlayerbotActions::DoAction(bot, "accept quest", IntToString(questId));
        accepted = accepted && bot->GetQuestStatus(questId) != QUEST_STATUS_NONE;

        if (!accepted)
            return Fail("quest_not_acceptable " + IntToString(questId));

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"quest\":" + IntToString(questId) + ",\"accepted\":true");
        plan.verified = true;
        return true;
    }

    if (actionType == "accept_all_quests")
    {
        bool accepted = AzerothFriendPlayerbotActions::DoAction(bot, "accept quest", "*");
        if (!accepted)
            return Fail("no_acceptable_quests_in_range");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"accepted_available_quests\":true");
        return true;
    }

    if (actionType == "turn_in_quest")
    {
        uint32 questId = ExtractJsonUint(paramsJson, "id");
        if (!questId)
            return Fail("turn_in_quest requires id");

        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return Fail("unknown_quest " + IntToString(questId));

        if (bot->GetQuestRewardStatus(questId))
        {
            plan.kind = AFStepKind::Immediate;
            plan.result = Payload("\"quest\":" + IntToString(questId) + ",\"status\":\"already_rewarded\"");
            return true;
        }

        bool rewarded = AzerothFriendPlayerbotActions::DoAction(bot, "talk to quest giver", IntToString(questId));
        rewarded = rewarded && bot->GetQuestRewardStatus(questId);

        if (!rewarded)
            return Fail("quest_not_rewardable " + IntToString(questId));

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"quest\":" + IntToString(questId) + ",\"rewarded\":true");
        plan.verified = true;
        return true;
    }

    if (actionType == "share_quest")
    {
        uint32 questId = ExtractJsonUint(paramsJson, "id");
        if (!questId)
            return Fail("share_quest requires id");

        if (!AzerothFriendPlayerbotActions::DoAction(bot, "share", IntToString(questId)))
            return Fail("share_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"quest\":" + IntToString(questId) + ",\"shared\":true");
        return true;
    }

    if (actionType == "drop_quest")
    {
        uint32 questId = ExtractJsonUint(paramsJson, "id");
        if (!questId)
            return Fail("drop_quest requires id");

        if (!AzerothFriendPlayerbotActions::DoAction(bot, "drop", IntToString(questId)))
            return Fail("drop_quest_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"quest\":" + IntToString(questId) + ",\"dropped\":true");
        return true;
    }

    if (actionType == "trainer")
    {
        if (!AzerothFriendPlayerbotActions::DoAction(bot, "trainer"))
            return Fail("trainer_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"trained\":true");
        return true;
    }

    if (actionType == "talents")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "auto talents");
        if (!ok)
            return Fail("talents_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"talents\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "choose_reward")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        if (item.empty())
            return Fail("choose_reward requires item");

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "r " + item);
        if (!ok)
            return Fail("choose_reward_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"choose_reward\":\"" + AzerothFriendShared::EscapeJsonString(item) + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "quest_summary")
    {
        bool all = ExtractJsonBool(paramsJson, "all", false);
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, all ? "quests all" : "quests");
        if (!ok)
            return Fail("quest_summary_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"quests\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "trainer_learn")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "trainer learn");
        if (!ok)
            return Fail("trainer_learn_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"trainer_learn\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "rpg_do_quest")
    {
        uint32 questId = ExtractJsonUint(paramsJson, "id");
        if (!questId)
            return Fail("rpg_do_quest requires quest id");

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "rpg do quest " + std::to_string(questId));
        if (!ok)
            return Fail("rpg_do_quest_failed");
        if (!AzerothFriendAiControl::ApplyMode(bot, "questing"))
            return Fail("rpg_do_quest_mode_failed");
        plan.kind = AFStepKind::Immediate;
        plan.followUpAction = "__handoff";
        plan.result = Payload("\"rpg_do_quest\":" + std::to_string(questId) + ",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "rpg_status")
    {
        std::string state = ExtractJsonString(paramsJson, "state");
        if (state.empty())
            state = "idle";
        uint32 questId = ExtractJsonUint(paramsJson, "id");

        std::string cmd = "rpg status " + state;
        if (questId && state == "do quest")
            cmd += " " + std::to_string(questId);

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, cmd);
        if (!ok)
            return Fail("rpg_status_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"rpg_status\":\"" + cmd + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    // -------------------------------------------------------------------- loot
    if (actionType == "loot" || actionType == "loot_nearest")
    {
        float radius = ExtractJsonFloat(paramsJson, "radius", 20.0f);
        Creature* corpse = FindNearestLootableCorpse(bot, radius);

        if (corpse && bot->GetDistance(corpse) > 4.0f)
        {
            plan.kind = AFStepKind::Approach;
            plan.targetGuid = corpse->GetGUID().GetCounter();
            plan.targetName = corpse->GetName();
            plan.targetX = corpse->GetPositionX();
            plan.targetY = corpse->GetPositionY();
            plan.targetZ = corpse->GetPositionZ();
            plan.targetRange = 3.0f;
            plan.followUpAction = "loot";
            plan.result = Payload("\"corpse\":\"" + AzerothFriendShared::EscapeJsonString(corpse->GetName()) + "\"");
            return true;
        }

        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "loot");
        if (!ok)
            return Fail("loot_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"loot\":") + ((corpse || ok) ? "true" : "false") +
                              ",\"corpse\":" + (corpse ? "true" : "false"));
        return true;
    }

    if (actionType == "loot_all")
    {
        if (!AzerothFriendPlayerbotActions::DoAction(bot, "add all loot"))
            return Fail("loot_all_strategy_failed");
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "loot");
        if (!ok)
            return Fail("loot_all_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"loot_all\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "gather")
    {
        GameObject* node = FindNearestGatheringNode(bot, 30.0f);
        if (node && bot->GetDistance(node) > 4.0f)
        {
            plan.kind = AFStepKind::Approach;
            plan.targetGuid = node->GetGUID().GetCounter();
            plan.targetName = node->GetName();
            plan.targetX = node->GetPositionX();
            plan.targetY = node->GetPositionY();
            plan.targetZ = node->GetPositionZ();
            plan.targetRange = 3.0f;
            plan.followUpAction = "use";
            plan.result = Payload("\"node\":\"" + AzerothFriendShared::EscapeJsonString(node->GetName()) + "\"");
            return true;
        }

        if (!AzerothFriendPlayerbotActions::DoAction(bot, "add gathering loot"))
            return Fail("gather_strategy_failed");
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "loot");
        if (!ok)
            return Fail("gather_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"gather\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "open_loot")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "open loot");
        if (!ok)
            return Fail("open_loot_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"open_loot\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "loot_filter")
    {
        std::string filter = ExtractJsonString(paramsJson, "filter");
        std::string item = ExtractJsonString(paramsJson, "item");
        std::string cmd = "ll";
        if (!filter.empty())
            cmd += " " + filter;
        if (!item.empty())
            cmd += " " + item;

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, cmd);
        if (!ok)
            return Fail("loot_filter_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"loot_filter\":\"" + AzerothFriendShared::EscapeJsonString(cmd) + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    // ----------------------------------------------------------------- economy
    if (actionType == "sell")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        bool allJunk = ExtractJsonBool(paramsJson, "all_junk", item.empty());

        uint32 sold = 0;
        if (allJunk)
            sold = AzerothFriendBotController::AutoSellJunk(bot);
        else if (AzerothFriendPlayerbotActions::DoAction(bot, "sell", item))
            sold = 1;

        if (!sold)
            return Fail("sell_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"sold\":") + IntToString(sold));
        return true;
    }

    if (actionType == "buy")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        uint32 count = ExtractJsonUint(paramsJson, "count", 1);
        if (item.empty())
            return Fail("buy requires item");

        Creature* vendor = FindNearestVendor(bot);
        uint32 resolvedItemId = 0;
        std::string resolvedItemName = item;

        // Try numeric ID first
        if (!item.empty() && std::all_of(item.begin(), item.end(), ::isdigit))
        {
            resolvedItemId = static_cast<uint32>(std::stoul(item));
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(resolvedItemId))
                resolvedItemName = proto->Name1;
        }

        // If not numeric or if vendor is present, scan vendor's items for exact or colloquial match
        if (vendor)
        {
            VendorItemData const* vendorItems = vendor->GetVendorItems();
            if (vendorItems)
            {
                std::string itemLower = item;
                std::transform(itemLower.begin(), itemLower.end(), itemLower.begin(), ::tolower);

                // 1. Exact match pass
                for (auto const& vItem : vendorItems->m_items)
                {
                    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vItem->item))
                    {
                        std::string protoNameLower = proto->Name1;
                        std::transform(protoNameLower.begin(), protoNameLower.end(), protoNameLower.begin(), ::tolower);
                        if (protoNameLower == itemLower || vItem->item == resolvedItemId)
                        {
                            resolvedItemId = vItem->item;
                            resolvedItemName = proto->Name1;
                            break;
                        }
                    }
                }

                // 2. Substring / colloquial match pass (e.g. "Apple" matches "Shiny Red Apple")
                if (resolvedItemId == 0)
                {
                    for (auto const& vItem : vendorItems->m_items)
                    {
                        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(vItem->item))
                        {
                            std::string protoNameLower = proto->Name1;
                            std::transform(protoNameLower.begin(), protoNameLower.end(), protoNameLower.begin(), ::tolower);
                            if (protoNameLower.find(itemLower) != std::string::npos)
                            {
                                resolvedItemId = vItem->item;
                                resolvedItemName = proto->Name1;
                                break;
                            }
                        }
                    }
                }
            }
        }

        // If still unresolved, try global ObjectMgr lookup by name
        if (resolvedItemId == 0)
        {
            std::string itemLower = item;
            std::transform(itemLower.begin(), itemLower.end(), itemLower.begin(), ::tolower);
            for (uint32 id = 1; id < 60000; ++id)
            {
                if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(id))
                {
                    std::string protoNameLower = proto->Name1;
                    std::transform(protoNameLower.begin(), protoNameLower.end(), protoNameLower.begin(), ::tolower);
                    if (protoNameLower == itemLower || protoNameLower.find(itemLower) != std::string::npos)
                    {
                        resolvedItemId = id;
                        resolvedItemName = proto->Name1;
                        break;
                    }
                }
            }
        }

        if (resolvedItemId == 0)
            return Fail("unknown_item_to_buy: " + item);

        // Format proper WoW item link containing Hitem:id: for mod-playerbots parseItems
        std::string itemLink = "|Hitem:" + std::to_string(resolvedItemId) + ":|h[" + resolvedItemName + "]|h";

        bool anyBought = false;
        for (uint32 c = 0; c < count; ++c)
        {
            if (AzerothFriendPlayerbotActions::DoAction(bot, "buy", itemLink))
                anyBought = true;
            else
                break;
        }

        if (!anyBought)
            return Fail("buy_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"item\":\"" + AzerothFriendShared::EscapeJsonString(resolvedItemName) +
                              "\",\"item_id\":" + std::to_string(resolvedItemId) +
                              ",\"count\":" + IntToString(count));
        return true;
    }

    if (actionType == "repair")
    {
        if (!AzerothFriendPlayerbotActions::DoAction(bot, "repair"))
            return Fail("repair_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"repaired\":true");
        return true;
    }

    if (actionType == "bank")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "bank");
        if (!ok)
            return Fail("bank_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"bank\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "mail")
    {
        std::string operation = ExtractJsonString(paramsJson, "operation");
        std::string filter = ExtractJsonString(paramsJson, "filter");
        std::string param = operation;
        if (!filter.empty())
            param += (param.empty() ? "" : " ") + filter;
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "mail", param);
        if (!ok)
            return Fail("mail_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"mail\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "send_mail")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        std::string money = ExtractJsonString(paramsJson, "money");
        std::string param = !item.empty() ? item : money;
        if (param.empty())
            return Fail("send_mail requires item or money");
        if (!AzerothFriendPlayerbotActions::DoAction(bot, "sendmail", param))
            return Fail("send_mail_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"send_mail\":\"" + AzerothFriendShared::EscapeJsonString(param) + "\"");
        return true;
    }

    if (actionType == "craft")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        if (item.empty())
            return Fail("craft requires item id or link");
        if (!AzerothFriendPlayerbotActions::DoAction(bot, "craft", item))
            return Fail("craft_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"craft\":\"" + AzerothFriendShared::EscapeJsonString(item) + "\"");
        return true;
    }

    if (actionType == "equip")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        if (item.empty())
            return Fail("equip requires item");

        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "equip", item);
        if (!ok)
            return Fail("equip_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"equipped\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "equip_upgrades")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "equip upgrade");
        if (!ok)
            return Fail("equip_upgrades_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"equip_upgrades\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "open_items")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "open items");
        if (!ok)
            return Fail("open_items_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"open_items\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "use_item")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        if (item.empty())
            return Fail("use_item requires item");

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "u " + item);
        if (!ok)
            return Fail("use_item_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"use_item\":\"" + AzerothFriendShared::EscapeJsonString(item) + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "use_item_on")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        std::string target = ExtractJsonString(paramsJson, "target");
        if (item.empty() || target.empty())
            return Fail("use_item_on requires both item and target");

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "u " + item + " " + target);
        if (!ok)
            return Fail("use_item_on_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"use_item_on\":\"" + AzerothFriendShared::EscapeJsonString(item) + " -> " + AzerothFriendShared::EscapeJsonString(target) + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "unequip")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        if (item.empty())
            return Fail("unequip requires item");

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "ue " + item);
        if (!ok)
            return Fail("unequip_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"unequip\":\"" + AzerothFriendShared::EscapeJsonString(item) + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "destroy_item")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        if (item.empty())
            return Fail("destroy_item requires item");

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "destroy " + item);
        if (!ok)
            return Fail("destroy_item_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"destroy_item\":\"" + AzerothFriendShared::EscapeJsonString(item) + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "give_gold")
    {
        uint32 gold = ExtractJsonUint(paramsJson, "gold", 0);
        uint32 silver = ExtractJsonUint(paramsJson, "silver", 0);
        uint32 copper = ExtractJsonUint(paramsJson, "copper", 0);
        if (gold == 0 && silver == 0 && copper == 0)
            gold = 1;

        std::string cmd = (gold ? std::to_string(gold) + "g " : "") +
                          (silver ? std::to_string(silver) + "s " : "") +
                          (copper ? std::to_string(copper) + "c" : "");
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, cmd);
        if (!ok)
            return Fail("give_gold_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"give_gold\":\"" + cmd + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "guild_bank")
    {
        std::string action = ExtractJsonString(paramsJson, "action");
        std::string item = ExtractJsonString(paramsJson, "item");
        if (item.empty())
            return Fail("guild_bank requires item");

        std::string cmd = (action == "withdraw") ? "gb -" + item : "gb " + item;
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, cmd);
        if (!ok)
            return Fail("guild_bank_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"guild_bank\":\"" + cmd + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "outfit")
    {
        std::string name = ExtractJsonString(paramsJson, "name");
        std::string action = ExtractJsonString(paramsJson, "action");
        std::string item = ExtractJsonString(paramsJson, "item");

        std::string cmd = "outfit";
        if (action == "list" || action == "help" || action == "?")
            cmd += " ?";
        else if (!name.empty())
        {
            cmd += " " + name;
            if (action == "add" && !item.empty())
                cmd += " +" + item;
            else if (action == "remove" && !item.empty())
                cmd += " -" + item;
            else if (!action.empty())
                cmd += " " + action;
        }

        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, cmd);
        if (!ok)
            return Fail("outfit_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"outfit\":\"" + cmd + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "maintenance")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "maintenance");
        if (!ok)
            return Fail("maintenance_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"maintenance\":") + (ok ? "true" : "false"));
        return true;
    }

    // ---------------------------------------------------------------- survival
    if (actionType == "food" || actionType == "drink" || actionType == "eat_drink")
    {
        bool ok = false;
        if (actionType != "drink")
            ok = AzerothFriendPlayerbotActions::DoAction(bot, "food") || ok;
        if (actionType != "food")
            ok = AzerothFriendPlayerbotActions::DoAction(bot, "drink") || ok;

        if (!ok)
            return Fail("rest_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"rested\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "tavern_rest")
    {
        // Living downtime routine: sit down in an inn or city, have a drink, or relax
        AzerothFriendBotController::Emote(bot, "drink");
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "food") ||
                  AzerothFriendPlayerbotActions::DoAction(bot, "drink") ||
                  AzerothFriendPlayerbotActions::DoCommand(bot, "sit");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"tavern_rest\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "campfire_cook")
    {
        // Living downtime routine: pitch a basic campfire or sit by the fire in the wilderness
        uint32 fireSpellId = 818; // Basic Campfire
        bool ok = false;
        if (bot->HasSpell(fireSpellId))
            ok = AzerothFriendBotController::CastSpell(bot, fireSpellId, 0, nullptr);
        if (!ok)
            ok = AzerothFriendPlayerbotActions::DoCommand(bot, "sit");

        AzerothFriendBotController::Emote(bot, "warm");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"campfire_cook\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "healing_potion" || actionType == "mana_potion" || actionType == "healthstone")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, actionType);
        if (!ok)
            return Fail(actionType + "_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"" + actionType + "\":" + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "hearthstone")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "hearthstone");
        if (!ok)
            return Fail("hearthstone_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"hearthstone\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "revive")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "revive from corpse");
        if (!ok)
            ok = AzerothFriendPlayerbotActions::DoAction(bot, "self resurrect");
        if (!ok)
            return Fail("revive_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"revive\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "release")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "release");
        if (!ok)
            ok = AzerothFriendPlayerbotActions::DoAction(bot, "auto release");
        if (!ok)
            return Fail("release_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"released\":") + (ok ? "true" : "false"));
        return true;
    }

    // ------------------------------------------------------------------ social
    if (actionType == "emote")
    {
        std::string emote = ExtractJsonString(paramsJson, "emote");
        if (emote.empty())
            emote = "nod";

        if (!AzerothFriendBotController::Emote(bot, emote))
            return Fail("emote_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"emote\":\"" + AzerothFriendShared::EscapeJsonString(emote) + "\"");
        return true;
    }

    if (actionType == "say")
    {
        std::string text = ExtractJsonString(paramsJson, "text");
        std::string channel = ExtractJsonString(paramsJson, "channel");
        if (channel.empty())
            channel = sAzerothFriendConfig->defaultChannel;

        if (text.empty())
            return Fail("say requires text");

        bool ok = AzerothFriendBotController::Say(bot, text, channel);
        if (!ok)
            return Fail("say_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"spoken\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "greet")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "greet");
        if (!ok)
            return Fail("greet_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"greet\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "invite")
    {
        std::string name = ExtractJsonString(paramsJson, "name");
        bool ok = name.empty() ? AzerothFriendPlayerbotActions::DoAction(bot, "invite nearby")
                               : AzerothFriendPlayerbotActions::DoAction(bot, "invite", name);
        if (!ok)
            return Fail("invite_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"invite\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "leave_group")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "leave");
        if (!ok)
            return Fail("leave_group_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"left\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "ready")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "ready");
        if (!ok)
            return Fail("ready_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"ready\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "duel_start" || actionType == "duel")
    {
        Player* target = ResolveTargetOrSpeaker(bot, paramsJson);
        if (!target || !bot->IsWithinDistInMap(target, 30.0f))
            return Fail("duel_target_not_found");

        bot->SetSelection(target->GetGUID());
        if (!AzerothFriendPlayerbotActions::DoAction(
                bot, "cast custom spell", target->GetName() + " 7266"))
            return Fail("duel_start_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"duel\":\"" + AzerothFriendShared::EscapeJsonString(target->GetName()) + "\"");
        return true;
    }

    if (actionType == "duel_accept")
    {
        if (!AzerothFriendBotController::AcceptDuel(bot))
            return Fail("no_pending_duel");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"duel\":\"accepted\"");
        return true;
    }

    if (actionType == "duel_decline")
    {
        if (!AzerothFriendBotController::DeclineDuel(bot))
            return Fail("no_pending_duel");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"duel\":\"declined\"");
        return true;
    }

    if (actionType == "attack_duel_opponent")
    {
        bool ok = AzerothFriendPlayerbotActions::DoAction(bot, "attack duel opponent");
        if (!ok)
            return Fail("attack_duel_opponent_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"duel_attack\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "trade_accept")
    {
        if (!AzerothFriendBotController::AcceptTrade(bot))
            return Fail("no_pending_trade");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"trade\":\"accepted\"");
        return true;
    }

    if (actionType == "trade_cancel")
    {
        if (!AzerothFriendBotController::CancelTrade(bot))
            return Fail("trade_cancel_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"trade\":\"cancelled\"");
        return true;
    }

    if (actionType == "trade" || actionType == "trade_start")
    {
        Player* target = ResolveTargetOrSpeaker(bot, paramsJson);
        if (!target)
            return Fail("trade_target_not_found");

        if (!bot->IsWithinDistInMap(target, 11.11f))
            return Fail("trade_target_too_far");

        bot->SetSelection(target->GetGUID());
        bool ok = AzerothFriendBotController::InitiateTrade(bot, target);
        if (!ok)
        {
            // If it returns an error, the next logical step is to target the speaker and retry the command
            bot->SetSelection(target->GetGUID());
            ok = AzerothFriendBotController::InitiateTrade(bot, target);
            if (!ok)
                return Fail("trade_initiate_failed");
        }

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"trade\":\"initiated\",\"target\":\"" + AzerothFriendShared::EscapeJsonString(target->GetName()) + "\"");
        return true;
    }

    if (actionType == "trade_set_item" || actionType == "trade_item" || actionType == "trade_put_item")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        if (item.empty())
            item = ExtractJsonString(paramsJson, "name");
        if (item.empty())
            return Fail("trade_set_item requires item parameter");

        uint32 count = ExtractJsonUint(paramsJson, "count", 1);
        uint32 rawSlot = ExtractJsonUint(paramsJson, "slot", 255);
        int8 slot = (rawSlot != 255) ? static_cast<int8>(rawSlot) : static_cast<int8>(-1);
        std::string itemLink;

        if (!AzerothFriendBotController::TradeSetItem(bot, item, count, slot, &itemLink))
            return Fail("trade_set_item_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"trade_item\":\"" + AzerothFriendShared::EscapeJsonString(item) + "\",\"link\":\"" +
                              AzerothFriendShared::EscapeJsonString(itemLink) + "\"");
        return true;
    }

    if (actionType == "trade_clear_item" || actionType == "trade_remove_item")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        uint32 rawSlot = ExtractJsonUint(paramsJson, "slot", 255);
        int8 slot = (rawSlot != 255) ? static_cast<int8>(rawSlot) : static_cast<int8>(-1);

        if (!AzerothFriendBotController::TradeClearItem(bot, item, slot))
            return Fail("trade_clear_item_failed");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"trade_clear\":\"success\"");
        return true;
    }

    if (actionType == "trade_set_gold" || actionType == "trade_gold" || actionType == "trade_set_money")
    {
        uint32 copper = ExtractJsonUint(paramsJson, "copper", 0);
        if (!copper)
        {
            std::string moneyStr = ExtractJsonString(paramsJson, "gold");
            if (moneyStr.empty())
                moneyStr = ExtractJsonString(paramsJson, "money");

            if (!moneyStr.empty())
            {
                std::string num;
                for (char ch : moneyStr)
                {
                    if (std::isdigit(static_cast<unsigned char>(ch)))
                        num += ch;
                    else if (ch == 'g' || ch == 'G')
                    {
                        if (!num.empty()) { copper += static_cast<uint32>(std::stoul(num) * 10000); num.clear(); }
                    }
                    else if (ch == 's' || ch == 'S')
                    {
                        if (!num.empty()) { copper += static_cast<uint32>(std::stoul(num) * 100); num.clear(); }
                    }
                    else if (ch == 'c' || ch == 'C')
                    {
                        if (!num.empty()) { copper += static_cast<uint32>(std::stoul(num)); num.clear(); }
                    }
                }
                if (!num.empty() && copper == 0)
                {
                    copper = static_cast<uint32>(std::stoul(num) * 10000);
                }
            }
        }

        uint32 actualCopper = 0;
        if (!AzerothFriendBotController::TradeSetGold(bot, copper, &actualCopper))
            return Fail("trade_set_gold_failed");

        uint32 g = actualCopper / 10000;
        uint32 s = (actualCopper % 10000) / 100;
        uint32 c = actualCopper % 100;
        std::string formattedMoney = std::to_string(g) + "g " + std::to_string(s) + "s " + std::to_string(c) + "c";

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"copper\":" + std::to_string(actualCopper) + ",\"formatted\":\"" + formattedMoney + "\"");
        return true;
    }

    if (actionType == "trade_link_items" || actionType == "link_items")
    {
        std::string category = ExtractJsonString(paramsJson, "category");
        if (category.empty())
            category = "all";
        Player* receiver = ResolveTargetOrSpeaker(bot, paramsJson);
        if (!receiver && bot->GetTrader())
            receiver = bot->GetTrader();
        if (!receiver)
        {
            Group* group = bot->GetGroup();
            if (group)
                receiver = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
        }

        if (!receiver)
            return Fail("no_receiver_for_link_items");

        std::string links = AzerothFriendBotController::LinkItemsInChat(bot, receiver, category, 10);
        if (links.empty())
            return Fail("no_matching_trade_items");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"linked\":\"" + AzerothFriendShared::EscapeJsonString(links) + "\"");
        return true;
    }

    if (actionType == "inspect")
    {
        Player* target = ResolveTargetOrSpeaker(bot, paramsJson);
        if (!target)
            return Fail("inspect_target_not_found");

        bot->SetSelection(target->GetGUID());
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"inspect\":\"" + AzerothFriendShared::EscapeJsonString(target->GetName()) + "\"");
        return true;
    }

    if (actionType == "target")
    {
        Player* target = ResolveTargetOrSpeaker(bot, paramsJson);
        if (!target)
            return Fail("target_not_found");

        bot->SetSelection(target->GetGUID());
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"target\":\"" + AzerothFriendShared::EscapeJsonString(target->GetName()) + "\"");
        return true;
    }

    if (actionType == "pvp")
    {
        if (!AzerothFriendPlayerbotActions::DoCommand(bot, "flag pvp toggle"))
            return Fail("pvp_toggle_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"pvp_toggle\":\"dispatched\"");
        return true;
    }

    if (actionType == "roll")
    {
        std::string item = ExtractJsonString(paramsJson, "item");
        bool ok = item.empty() ? AzerothFriendPlayerbotActions::DoAction(bot, "roll")
                               : AzerothFriendPlayerbotActions::DoCommand(bot, "roll " + item);
        if (!ok)
            return Fail("roll_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"roll\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "give_leader")
    {
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, "give leader");
        if (!ok)
            return Fail("give_leader_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload(std::string("\"give_leader\":") + (ok ? "true" : "false"));
        return true;
    }

    if (actionType == "lfg")
    {
        uint32 size = ExtractJsonUint(paramsJson, "size", 0);
        std::string cmd = (size > 0) ? "lfg " + std::to_string(size) : "lfg";
        bool ok = AzerothFriendPlayerbotActions::DoCommand(bot, cmd);
        if (!ok)
            return Fail("lfg_failed");
        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"lfg\":\"" + cmd + "\",\"ok\":" + (ok ? "true" : "false"));
        return true;
    }

    // -------------------------------------------------------------------- meta
    if (actionType == "playerbot_action")
    {
        std::string action = ExtractJsonString(paramsJson, "action");
        if (action.empty())
            action = ExtractJsonString(paramsJson, "name");
        std::string param = ExtractJsonString(paramsJson, "param");

        if (action.empty())
            return Fail("playerbot_action requires action");

        if (!sAzerothFriendConfig->rawPassthroughEnable)
            return Fail("raw_passthrough_disabled");

        if (!AzerothFriendPlayerbotActions::IsSupportedAction(bot, action))
            return Fail("unsupported_playerbot_action '" + action + "'");

        if (!AzerothFriendPlayerbotActions::DoAction(bot, action, param))
            return Fail("playerbot_action_failed '" + action + "'");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"playerbot_action\":\"" + AzerothFriendShared::EscapeJsonString(action) + "\"");
        return true;
    }

    if (actionType == "playerbot_command")
    {
        std::string command = ExtractJsonString(paramsJson, "command");
        if (command.empty())
            return Fail("playerbot_command requires command");

        if (!sAzerothFriendConfig->rawPassthroughEnable)
            return Fail("raw_passthrough_disabled");

        if (!AzerothFriendPlayerbotActions::DoCommand(bot, command))
        {
            // If it returns an error, the next logical step is to target the speaker and retry the command
            Player* speaker = ResolveTargetOrSpeaker(bot, paramsJson);
            if (speaker)
            {
                bot->SetSelection(speaker->GetGUID());
                if (!AzerothFriendPlayerbotActions::DoCommand(bot, command))
                    return Fail("playerbot_command_failed '" + command + "'");
            }
            else
            {
                return Fail("playerbot_command_failed '" + command + "'");
            }
        }

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"command\":\"" + AzerothFriendShared::EscapeJsonString(command) + "\"");
        return true;
    }

    if (actionType == "strategy")
    {
        std::string spec = ExtractJsonString(paramsJson, "spec");
        bool combat = ExtractJsonString(paramsJson, "state") == "combat";

        for (std::string raw : ExtractJsonStringArray(paramsJson, "add"))
        {
            if (!raw.empty() && raw[0] != '+')
                raw = "+" + raw;
            spec += (spec.empty() ? "" : ",") + raw;
        }
        for (std::string raw : ExtractJsonStringArray(paramsJson, "remove"))
        {
            if (!raw.empty() && raw[0] != '-')
                raw = "-" + raw;
            spec += (spec.empty() ? "" : ",") + raw;
        }

        if (spec.empty())
            return Fail("strategy requires spec, add or remove");

        if (!AzerothFriendPlayerbotActions::ChangeStrategies(bot, spec, combat))
            return Fail("strategy_failed '" + spec + "'");

        plan.kind = AFStepKind::Immediate;
        plan.result = Payload("\"strategy\":\"" + AzerothFriendShared::EscapeJsonString(spec) + "\"");
        return true;
    }

    if (actionType == "set_action_mode")
    {
        std::string mode = ExtractJsonString(paramsJson, "mode");
        if (mode != "combat" && mode != "travel" && mode != "idle" && mode != "social")
            return Fail("invalid_action_mode");
        if (!AzerothFriendAiControl::ApplyMode(bot, mode))
            return Fail("action_mode_failed");

        CharacterDatabase.Execute(("UPDATE azeroth_friend_bots SET action_mode='" + mode +
            "' WHERE bot_guid=" + std::to_string(bot->GetGUID().GetCounter())).c_str());
        plan.kind = AFStepKind::Immediate;
        plan.followUpAction = "__handoff";
        plan.result = Payload("\"action_mode\":\"" + mode + "\"");
        return true;
    }

    if (actionType == "wait")
    {
        uint32 seconds = ExtractJsonUint(paramsJson, "seconds", 2);
        if (seconds == 0)
            seconds = 2;
        if (seconds > 60)
            seconds = 60;

        plan.kind = AFStepKind::Wait;
        plan.waitSeconds = seconds;
        plan.result = Payload("\"wait\":" + IntToString(seconds));
        return true;
    }

    return Fail("unhandled_action '" + actionType + "'");
}

void AzerothFriendActionDispatcher::EmitAutonomyTick(Player* bot, std::string const& botMode)
{
    if (!bot || !sAzerothFriendConfig->autonomyEnable)
        return;

    if (!sAzerothFriendConfig->IsAutonomyMode(botMode))
        return;

    uint32 botGuid = bot->GetGUID().GetCounter();
    if (_activeActions.count(botGuid))
        return;

    if (sAFLiveState->IsRunning())
    {
        // RAM-first: pending events and active plans come from value caches that
        // are refreshed asynchronously, so no query runs inside the world tick.
        auto queued = _pendingEventCount.find(botGuid);
        if (queued != _pendingEventCount.end() && queued->second > 0)
            return;
        if (sAFLiveState->HasActivePlan(botGuid))
            return;
    }
    else
    {
        // Explicit SQL compatibility mode (rollback path).
        QueryResult queued = CharacterDatabase.Query(("SELECT id FROM azeroth_friend_events WHERE bot_guid = " +
            std::to_string(botGuid) + " AND status IN ('pending','processing') LIMIT 1").c_str());
        if (queued)
            return;

        QueryResult pending = CharacterDatabase.Query(
            ("SELECT COUNT(*) FROM azeroth_friend_actions WHERE bot_guid = " + std::to_string(botGuid) +
             " AND status IN ('pending','in_progress')").c_str());
        if (pending && pending->Fetch()[0].Get<uint32>() > 0)
            return;
    }

    time_t now = time(nullptr);

    uint32 healthPct = static_cast<uint32>(bot->GetHealthPct());
    if (healthPct <= sAzerothFriendConfig->autonomyHealthPct)
    {
        time_t last = _lastHealthAlert.count(botGuid) ? _lastHealthAlert[botGuid] : 0;
        if (now - last >= 30)
        {
            _lastHealthAlert[botGuid] = now;
            std::string payload = "{\"health_pct\":" + IntToString(healthPct) +
                                  ",\"in_combat\":" + (bot->IsInCombat() ? "true" : "false") + "}";
            AzerothFriendShared::QueueEvent(botGuid, "health_critical", 2, 0, "", payload);
            return;
        }
    }

    time_t lastTick = _lastAutonomyTick.count(botGuid) ? _lastAutonomyTick[botGuid] : 0;
    if (now - lastTick < static_cast<time_t>(sAzerothFriendConfig->GetThinkingCadenceSeconds()))
        return;

    _lastAutonomyTick[botGuid] = now;

    if (bot->IsInCombat() && !sAzerothFriendConfig->autonomyCombatTicks)
        return;

    Creature* corpse = FindNearestLootableCorpse(bot, static_cast<float>(sAzerothFriendConfig->autonomyLootRadius));

    std::ostringstream payload;
    payload << "{\"health_pct\":" << healthPct
            << ",\"power_pct\":" << (uint32)bot->GetPowerPct(bot->getPowerType())
            << ",\"in_combat\":" << (bot->IsInCombat() ? "true" : "false")
            << ",\"lootable_nearby\":" << (corpse ? "true" : "false")
            << ",\"mode\":\"" << AzerothFriendShared::EscapeJsonString(botMode) << "\"";

    if (corpse)
    {
        payload << ",\"loot_guid\":" << corpse->GetGUID().GetCounter()
                << ",\"loot_name\":\"" << AzerothFriendShared::EscapeJsonString(corpse->GetName()) << "\""
                << ",\"loot_dist\":" << (uint32)bot->GetDistance(corpse);
    }

    Group* group = bot->GetGroup();
    Player* master = group ? ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID()) : nullptr;
    if (master)
    {
        payload << ",\"master_dist\":" << (uint32)bot->GetDistance(master)
                << ",\"master_in_combat\":" << (master->IsInCombat() ? "true" : "false");
    }

    uint8 downtimeRoutine = AzerothFriendAiControl::GetDowntimeRoutine(bot);
    if (downtimeRoutine > 0)
        payload << ",\"downtime_routine\":" << (downtimeRoutine == 1 ? "\"tavern_rest\"" : "\"campfire_cook\"");

    payload << "}";

    std::string eventType = (downtimeRoutine > 0) ? "owner_idle_downtime" : (corpse ? "loot_available" : "idle_tick");
    uint8 priority = (downtimeRoutine > 0) ? 4 : (corpse ? 3 : 5);
    AzerothFriendShared::QueueEvent(botGuid, eventType, priority, 0, "", payload.str());
}

bool AzerothFriendActionDispatcher::ExecuteManualAction(Player* bot, std::string const& actionType,
                                                        std::string const& paramsJson, std::string& outMessage)
{
    if (!bot || !bot->IsInWorld())
    {
        outMessage = "Bot not in world.";
        return false;
    }

    if (!AzerothFriendActionRegistry::IsCurated(actionType))
    {
        outMessage = "Unknown action '" + actionType + "'. Use .af catalog for the full list.";
        return false;
    }

    std::string leaseReason;
    if (!AzerothFriendAiControl::Acquire(bot, sAzerothFriendConfig->leaseRefreshSeconds, &leaseReason))
    {
        outMessage = "Action '" + actionType + "' failed: " +
            (leaseReason.empty() ? "playerbot control unavailable" : leaseReason);
        return false;
    }

    StepPlan plan;
    std::string error;
    if (!TryBuildStep(bot, actionType, paramsJson, plan, error))
    {
        AzerothFriendAiControl::Release(bot, "manual validation failed");
        outMessage = "Action '" + actionType + "' failed: " + error;
        return false;
    }

    BotActiveAction active;
    active.actionType = actionType;
    active.paramsJson = paramsJson;
    active.stepIndex = 0;
    active.totalSteps = 1;
    active.kind = plan.kind;
    active.followUpAction = plan.followUpAction;
    active.followUpParam = plan.followUpParam;
    active.targetGuid = plan.targetGuid;
    active.targetName = plan.targetName;
    active.targetX = plan.targetX;
    active.targetY = plan.targetY;
    active.targetZ = plan.targetZ;
    active.targetRange = plan.targetRange;
    active.startTime = time(nullptr);
    active.lastProgressCheck = active.startTime;
    active.lastCheckX = bot->GetPositionX();
    active.lastCheckY = bot->GetPositionY();
    active.deadline = active.startTime +
        (plan.waitSeconds ? plan.waitSeconds : sAzerothFriendConfig->actionStepTimeoutSeconds);
    active.handedOff = (plan.followUpAction == "__handoff");
    active.verified = plan.verified;

    if (active.kind == AFStepKind::Approach &&
        !AzerothFriendPlayerbotActions::MoveToCoords(
            bot, active.targetX, active.targetY, active.targetZ, active.targetRange))
    {
        AzerothFriendAiControl::Release(bot, "manual movement rejected");
        outMessage = "Action '" + actionType + "' failed: move_rejected";
        return false;
    }

    PublishStepTelemetry(bot, active,
        plan.kind == AFStepKind::Immediate ? "completed" : "in_progress", plan.result, "");

    if (plan.kind != AFStepKind::Immediate)
        _activeActions[bot->GetGUID().GetCounter()] = active;
    else if (!active.handedOff)
        AzerothFriendAiControl::Release(bot, "manual action");

    outMessage = actionType + " dispatched " + plan.result;
    return true;
}

std::string AzerothFriendActionDispatcher::ExportCatalog(Player* bot)
{
    uint32 botGuid = bot ? bot->GetGUID().GetCounter() : 0;

    // Remove stale runtime discoveries before republishing. Execute() is queued
    // in order and never blocks the world thread.
    CharacterDatabase.Execute(("DELETE FROM azeroth_friend_action_catalog WHERE bot_guid=" +
                               std::to_string(botGuid)).c_str());

    std::ostringstream summary;
    uint32 curatedCount = 0;
    for (AzerothFriendActionRegistry::ActionInfo const& info : AzerothFriendActionRegistry::Curated())
    {
        std::string sql =
            "INSERT INTO azeroth_friend_action_catalog "
            "(bot_guid, action_name, category, source, params_schema, description, safe, api_version, "
            "params_schema_json, result_schema_json, binding_kind, native_binding, authority, preconditions, "
            "completion_policy, capability_revision) VALUES (" +
            std::to_string(botGuid) + ", '" + AzerothFriendShared::EscapeSqlString(info.name) + "', '" +
            AzerothFriendShared::EscapeSqlString(info.category) + "', 'curated', '" +
            AzerothFriendShared::EscapeSqlString(info.params) + "', '" +
            AzerothFriendShared::EscapeSqlString(info.description) + "', 1, " +
            std::to_string(info.apiVersion) + ", '" +
            AzerothFriendShared::EscapeSqlString(info.paramsJsonSchema) + "', '" +
            AzerothFriendShared::EscapeSqlString(info.resultJsonSchema) + "', '" +
            AzerothFriendShared::EscapeSqlString(info.bindingKind) + "', '" +
            AzerothFriendShared::EscapeSqlString(info.playerbotAction) + "', '" +
            AzerothFriendShared::EscapeSqlString(info.authority) + "', '" +
            AzerothFriendShared::EscapeSqlString(info.preconditions) + "', '" +
            AzerothFriendShared::EscapeSqlString(info.completionPolicy) + "', 1) "
            "ON DUPLICATE KEY UPDATE category = VALUES(category), params_schema = VALUES(params_schema), "
            "description = VALUES(description), safe = VALUES(safe), api_version = VALUES(api_version), "
            "params_schema_json = VALUES(params_schema_json), result_schema_json = VALUES(result_schema_json), "
            "binding_kind = VALUES(binding_kind), native_binding = VALUES(native_binding), "
            "authority = VALUES(authority), preconditions = VALUES(preconditions), "
            "completion_policy = VALUES(completion_policy), capability_revision = VALUES(capability_revision)";
        CharacterDatabase.Execute(sql.c_str());
        curatedCount++;
    }

    uint32 playerbotCount = 0;
    uint32 strategyCount = 0;
    if (bot && sAzerothFriendConfig->botApiNativeDiscoveryEnable)
    {
        for (std::string const& name : AzerothFriendPlayerbotActions::SupportedActions(bot))
        {
            bool denied = AzerothFriendPlayerbotActions::IsDeniedAction(name);
            std::string sql =
                "INSERT INTO azeroth_friend_action_catalog "
                "(bot_guid, action_name, category, source, params_schema, description, safe, api_version, "
                "params_schema_json, result_schema_json, binding_kind, native_binding, authority, preconditions, "
                "completion_policy, capability_revision) VALUES (" +
                std::to_string(botGuid) + ", '" + AzerothFriendShared::EscapeSqlString(name) +
                "', 'playerbots', 'playerbots', 'param', 'native mod-playerbots action', " +
                (denied ? "0" : "1") + ", 1, "
                "'{\"type\":\"object\",\"properties\":{\"param\":{\"type\":\"string\"}},\"additionalProperties\":false}', "
                "NULL, 'native_passthrough', '" + AzerothFriendShared::EscapeSqlString(name) +
                "', 'owner_command', 'bot_in_world,verified_owner_command', 'dispatch_acknowledged', 1) "
                "ON DUPLICATE KEY UPDATE safe = VALUES(safe)";
            CharacterDatabase.Execute(sql.c_str());
            playerbotCount++;
        }

        for (std::string const& name : AzerothFriendPlayerbotActions::SupportedStrategies(bot))
        {
            std::string sql =
                "INSERT INTO azeroth_friend_action_catalog "
                "(bot_guid, action_name, category, source, params_schema, description, safe, api_version, "
                "params_schema_json, result_schema_json, binding_kind, native_binding, authority, preconditions, "
                "completion_policy, capability_revision) VALUES (" +
                std::to_string(botGuid) + ", '" + AzerothFriendShared::EscapeSqlString(name) +
                "', 'strategy', 'strategy', '+name / -name', 'native mod-playerbots strategy', 1, 1, "
                "'{\"type\":\"object\",\"properties\":{\"enabled\":{\"type\":\"boolean\"}},\"additionalProperties\":false}', "
                "NULL, 'native_strategy', '" + AzerothFriendShared::EscapeSqlString(name) +
                "', 'owner_command', 'bot_in_world,verified_owner_command', 'dispatch_acknowledged', 1) "
                "ON DUPLICATE KEY UPDATE safe = VALUES(safe)";
            CharacterDatabase.Execute(sql.c_str());
            strategyCount++;
        }
    }

    summary << "curated=" << curatedCount << " playerbots_actions=" << playerbotCount
            << " strategies=" << strategyCount;
    return summary.str();
}
