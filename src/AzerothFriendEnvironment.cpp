#include "AzerothFriendEnvironment.h"
#include "AzerothFriendActionRegistry.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendShared.h"
#include "AzerothFriendBotController.h"
#include "AzerothFriendSnapshotMemory.h"
#include "AzerothFriendLiveState.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "Creature.h"
#include "GameObject.h"
#include "Spell.h"
#include "QuestDef.h"
#include "Group.h"
#include "Log.h"
#include "DBCStores.h"
#include "World.h"
#include "ObjectAccessor.h"
#include "TradeData.h"
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>
#include <algorithm>

AzerothFriendEnvironment* AzerothFriendEnvironment::instance()
{
    static AzerothFriendEnvironment instance;
    return &instance;
}

namespace
{
    /**
     * A value-only candidate row. The scan never keeps a Unit or GameObject
     * pointer beyond the tick: only the numeric guid and the values needed to
     * render a prompt or a HUD line survive.
     */
    struct EntityCandidate
    {
        uint32 guid = 0;
        std::string name;
        std::string type;
        float distance = 0.0f;
        float distToMaster = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        int32 levelDiff = 0;
        bool elite = false;
        bool focus = false;
    };

    /// The companion's owner: first non-controlled group member, else group leader.
    Player* FindMaster(Player* bot)
    {
        if (!bot)
            return nullptr;

        Group* group = bot->GetGroup();
        if (!group)
            return nullptr;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (member && member != bot && !sAzerothFriendConfig->IsBotControlled(member->GetName()))
                return member;
        }

        Player* leader = ObjectAccessor::FindPlayer(group->GetLeaderGUID());
        if (leader && leader != bot)
            return leader;
        return nullptr;
    }

    std::string RenderCandidate(EntityCandidate const& candidate)
    {
        std::ostringstream ss;
        ss << "{"
           << "\"guid\":" << candidate.guid << ","
           << "\"name\":\"" << AzerothFriendShared::EscapeJsonString(candidate.name) << "\","
           << "\"type\":\"" << candidate.type << "\","
           << "\"dist\":" << std::round(candidate.distance * 10.0f) / 10.0f << ","
           << "\"focus\":" << (candidate.focus ? "true" : "false") << ","
           << "\"level_diff\":" << candidate.levelDiff << ","
           << "\"elite\":" << (candidate.elite ? "true" : "false") << ","
           << "\"x\":" << std::round(candidate.x * 10.0f) / 10.0f << ","
           << "\"y\":" << std::round(candidate.y * 10.0f) / 10.0f << ","
           << "\"z\":" << std::round(candidate.z * 10.0f) / 10.0f
           << "}";
        return ss.str();
    }

    /**
     * One value scan of everything relevant in line of sight. Sorting keeps the
     * owner's target first, then threats, party, quest givers, loot and hostiles
     * so the capped prompt list never hides the mob that is attacking the owner.
     */
    std::vector<EntityCandidate> CollectNearbyCandidates(Player* bot, Player* master)
    {
        std::vector<EntityCandidate> candidates;

        if (!bot || !bot->GetMap())
            return candidates;

        float scanDist = sAzerothFriendConfig->scanRadius;
        Unit* masterSelected = master ? master->GetSelectedUnit() : nullptr;

        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
        {
            Creature* c = pair.second;
            if (!c || !c->IsInWorld() || !bot->IsWithinDistInMap(c, scanDist) || !bot->IsWithinLOSInMap(c))
                continue;

            float dist = bot->GetDistance(c);
            float dMaster = master ? master->GetDistance(c) : dist;
            std::string type = "neutral";
            bool isFocus = (masterSelected && masterSelected == c);

            if (isFocus)
            {
                type = "focus_target";
            }
            else if (c->isDead())
            {
                if (!c->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
                    continue; // skip non-lootable dead mobs

                // MAF-054: Only flag corpses as dead_lootable if the bot has legal tap or loot rights.
                // Prevents the companion from coveting master's solo kills or generating unauthorized loot plans.
                bool canLoot = (c->GetLootRecipient() == bot) || c->isTappedBy(bot);
                if (!canLoot && c->GetLootRecipientGroup() && bot->GetGroup() == c->GetLootRecipientGroup())
                    canLoot = bot->isAllowedToLoot(c);
                if (!canLoot)
                    continue;

                type = "dead_lootable";
            }
            else if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
            {
                type = "questgiver";
            }
            else if (c->IsInCombat() && (c->GetVictim() == bot || (master && c->GetVictim() == master)))
            {
                type = "threat";
            }
            else if (bot->IsValidAttackTarget(c))
            {
                type = c->IsInCombat() ? "threat" : "hostile";
            }
            else if (c->IsFriendlyTo(bot))
            {
                type = "friendly";
            }

            // In crowded areas (>8 candidates), filter out ambient low-value civilians and critters
            if (!isFocus && candidates.size() > 8 && type == "neutral" && c->IsCivilian() && dMaster > 10.0f)
                continue;

            EntityCandidate candidate;
            candidate.guid = c->GetGUID().GetCounter();
            candidate.name = c->GetName();
            candidate.type = type;
            candidate.distance = dist;
            candidate.distToMaster = dMaster;
            candidate.x = c->GetPositionX();
            candidate.y = c->GetPositionY();
            candidate.z = c->GetPositionZ();
            candidate.levelDiff = (int32)c->GetLevel() - (int32)bot->GetLevel();
            candidate.elite = c->isElite();
            candidate.focus = isFocus;
            candidates.push_back(candidate);
        }

        for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
        {
            GameObject* go = pair.second;
            if (!go || !go->IsInWorld() || !bot->IsWithinDistInMap(go, scanDist) || !bot->IsWithinLOSInMap(go))
                continue;

            EntityCandidate candidate;
            candidate.guid = go->GetGUID().GetCounter();
            candidate.name = go->GetName();
            candidate.type = "object";
            if (go->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER)
                candidate.type = "quest_object";
            else if (go->GetGoType() == GAMEOBJECT_TYPE_CHEST)
                candidate.type = "chest";
            candidate.distance = bot->GetDistance(go);
            candidate.distToMaster = master ? master->GetDistance(go) : candidate.distance;
            candidate.x = go->GetPositionX();
            candidate.y = go->GetPositionY();
            candidate.z = go->GetPositionZ();
            candidates.push_back(candidate);
        }

        for (auto const& ref : bot->GetMap()->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || p == bot || p == master || !p->IsInWorld() || !bot->IsWithinDistInMap(p, scanDist))
                continue;

            if (sAzerothFriendConfig->snapshotLineOfSight && !bot->IsWithinLOSInMap(p))
                continue;

            float dist = bot->GetDistance(p);
            float dMaster = master ? master->GetDistance(p) : dist;
            std::string type = "friendly_bot";
            bool isFocus = (masterSelected && masterSelected == p);

            if (isFocus)
            {
                type = "focus_target";
            }
            else if (bot->IsInSameGroupWith(p))
            {
                type = "party_bot";
            }
            else if (bot->IsValidAttackTarget(p))
            {
                type = "enemy_player";
            }
            else if (p->IsFriendlyTo(bot))
            {
                type = "friendly_bot";
            }

            if (!isFocus && candidates.size() > 8 && type == "friendly_bot" && dMaster > 15.0f)
                continue;

            EntityCandidate candidate;
            candidate.guid = p->GetGUID().GetCounter();
            candidate.name = p->GetName();
            candidate.type = type;
            candidate.distance = dist;
            candidate.distToMaster = dMaster;
            candidate.x = p->GetPositionX();
            candidate.y = p->GetPositionY();
            candidate.z = p->GetPositionZ();
            candidate.levelDiff = (int32)p->GetLevel() - (int32)bot->GetLevel();
            candidate.focus = isFocus;
            candidates.push_back(candidate);
        }

        std::sort(candidates.begin(), candidates.end(), [](EntityCandidate const& a, EntityCandidate const& b)
        {
            auto weight = [](std::string const& type)
            {
                if (type == "focus_target") return 0;
                if (type == "threat") return 1;
                if (type == "party_bot") return 2;
                if (type == "questgiver" || type == "quest_object") return 3;
                if (type == "dead_lootable") return 4;
                if (type == "hostile" || type == "enemy_player") return 5;
                if (type == "friendly_bot") return 6;
                return 7;
            };
            int wa = weight(a.type);
            int wb = weight(b.type);
            if (wa != wb)
                return wa < wb;
            return a.distToMaster < b.distToMaster;
        });

        return candidates;
    }

    std::string RenderNearbyJson(std::vector<EntityCandidate> const& candidates, uint32 maxEntities)
    {
        std::ostringstream ss;
        ss << "[";
        uint32 count = 0;
        for (size_t i = 0; i < candidates.size() && count < maxEntities; ++i)
        {
            if (count > 0)
                ss << ",";
            ss << RenderCandidate(candidates[i]);
            ++count;
        }
        ss << "]";
        return ss.str();
    }

    std::string JoinJsonObjects(std::vector<std::string> const& parts)
    {
        std::string joined = "{";
        bool first = true;
        for (std::string const& part : parts)
        {
            std::string body = part;
            if (!body.empty() && body.front() == '{')
                body.erase(body.begin());
            if (!body.empty() && body.back() == '}')
                body.pop_back();
            if (body.empty())
                continue;
            if (!first)
                joined += ",";
            joined += body;
            first = false;
        }
        joined += "}";
        return joined;
    }

    std::string DoubleQuote(std::string const& value)
    {
        return "\"" + AzerothFriendShared::EscapeJsonString(value) + "\"";
    }
}

std::string AzerothFriendEnvironment::BuildVitalsJson(Player* bot, uint32 /*controlRevision*/) const
{
    if (!bot || !bot->GetMap())
        return "{}";

    std::ostringstream ss;
    ss << "{";
    ss << "\"race\":" << (uint32)bot->getRace() << ","
       << "\"class\":" << (uint32)bot->getClass() << ","
       << "\"level\":" << (uint32)bot->GetLevel() << ","
       << "\"health_pct\":" << (uint32)bot->GetHealthPct() << ","
       << "\"power_pct\":" << (uint32)bot->GetPowerPct(bot->getPowerType()) << ","
       << "\"map_id\":" << bot->GetMapId() << ","
       << "\"zone_id\":" << bot->GetZoneId() << ","
       << "\"area_id\":" << bot->GetAreaId() << ","
       << "\"pos_x\":" << std::round(bot->GetPositionX() * 10.0f) / 10.0f << ","
       << "\"pos_y\":" << std::round(bot->GetPositionY() * 10.0f) / 10.0f << ","
       << "\"pos_z\":" << std::round(bot->GetPositionZ() * 10.0f) / 10.0f << ","
       << "\"orientation\":" << std::round(bot->GetOrientation() * 100.0f) / 100.0f << ","
       << "\"in_combat\":" << (bot->IsInCombat() ? "true" : "false") << ",";

    Unit* target = bot->GetSelectedUnit();
    if (target)
    {
        ss << "\"target\":{"
           << "\"guid\":" << target->GetGUID().GetCounter() << ","
           << "\"name\":" << DoubleQuote(target->GetName()) << ","
           << "\"hp_pct\":" << (uint32)target->GetHealthPct() << ","
           << "\"level_diff\":" << ((int32)target->GetLevel() - (int32)bot->GetLevel()) << ","
           << "\"is_enemy\":" << (bot->IsValidAttackTarget(target) ? "true" : "false")
           << "}";
    }
    else
    {
        ss << "\"target\":null";
    }

    Player* trader = bot->GetTrader();
    TradeData* pTrade = bot->GetTradeData();
    if (trader && pTrade)
    {
        ss << ",\"trade\":{"
           << "\"active\":true,"
           << "\"trader_guid\":" << trader->GetGUID().GetCounter() << ","
           << "\"trader_name\":" << DoubleQuote(trader->GetName()) << ","
           << "\"gold\":" << pTrade->GetMoney()
           << "}";
    }
    else
    {
        ss << ",\"trade\":null";
    }

    ss << "}";
    return ss.str();
}

std::string AzerothFriendEnvironment::BuildOwnerJson(Player* bot) const
{
    Player* master = FindMaster(bot);
    if (!master)
        return "{}";

    Unit* masterTarget = master->GetSelectedUnit();
    std::string targetName = masterTarget ? masterTarget->GetName() : "";
    uint32 targetHpPct = masterTarget ? (uint32)masterTarget->GetHealthPct() : 0;
    bool targetIsEnemy = masterTarget ? master->IsValidAttackTarget(masterTarget) : false;
    bool isCasting = master->IsNonMeleeSpellCast(false);

    std::ostringstream ss;
    ss << "{"
       << "\"guid\":" << master->GetGUID().GetCounter() << ","
       << "\"name\":" << DoubleQuote(master->GetName()) << ","
       << "\"dist\":" << std::round(bot->GetDistance(master) * 10.0f) / 10.0f << ","
       << "\"hp_pct\":" << (uint32)master->GetHealthPct() << ","
       << "\"in_combat\":" << (master->IsInCombat() ? "true" : "false") << ","
       << "\"is_casting\":" << (isCasting ? "true" : "false") << ","
       << "\"target_name\":" << DoubleQuote(targetName) << ","
       << "\"target_hp_pct\":" << targetHpPct << ","
       << "\"target_is_enemy\":" << (targetIsEnemy ? "true" : "false")
       << "}";
    return ss.str();
}

std::string AzerothFriendEnvironment::BuildSurroundingsJson(Player* bot) const
{
    if (!bot || !bot->GetMap())
        return "{}";

    Player* master = FindMaster(bot);
    uint32 zoneId = bot->GetZoneId();
    uint32 areaId = bot->GetAreaId();
    AreaTableEntry const* zoneEntry = sAreaTableStore.LookupEntry(zoneId);
    AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(areaId);
    char const* zp = zoneEntry ? zoneEntry->area_name[sWorld->GetDefaultDbcLocale()] : nullptr;
    char const* ap = areaEntry ? areaEntry->area_name[sWorld->GetDefaultDbcLocale()] : nullptr;
    std::string zoneName = zp ? zp : "";
    std::string areaName = ap ? ap : "";

    bool isResting = bot->HasPlayerFlag(PLAYER_FLAGS_RESTING);
    uint32 threatCount = (uint32)bot->getAttackers().size();

    std::vector<EntityCandidate> candidates = CollectNearbyCandidates(bot, master);

    std::ostringstream ss;
    ss << "{"
       << "\"zone_name\":" << DoubleQuote(zoneName) << ","
       << "\"area_name\":" << DoubleQuote(areaName) << ","
       << "\"is_outdoors\":" << (bot->IsOutdoors() ? "true" : "false") << ","
       << "\"in_inn\":" << (isResting ? "true" : "false") << ","
       << "\"is_resting\":" << (isResting ? "true" : "false") << ","
       << "\"is_swimming\":" << (bot->IsInWater() ? "true" : "false") << ","
       << "\"is_underwater\":" << (bot->IsUnderWater() ? "true" : "false") << ","
       << "\"is_mounted\":" << (bot->IsMounted() ? "true" : "false") << ","
       << "\"is_falling\":" << (bot->IsFalling() ? "true" : "false") << ","
       << "\"in_combat\":" << (bot->IsInCombat() ? "true" : "false") << ","
       << "\"threat_count\":" << threatCount << ","
       << "\"crowd_count\":" << candidates.size() << ","
       << "\"nearby\":" << RenderNearbyJson(candidates, sAzerothFriendConfig->maxVisibleEntities) << ",";

    // Active quests summary (IDs only to keep payload lightweight)
    ss << "\"active_quests\":[";
    bool firstQuest = true;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 questId = bot->GetQuestSlotQuestId(slot);
        if (!questId)
            continue;
        if (!firstQuest)
            ss << ",";
        QuestStatus status = bot->GetQuestStatus(questId);
        ss << "{\"id\":" << questId << ",\"complete\":" << (status == QUEST_STATUS_COMPLETE ? "true" : "false") << "}";
        firstQuest = false;
    }
    ss << "]}";
    return ss.str();
}

std::string AzerothFriendEnvironment::BuildCapabilitiesJson(Player* bot, uint32 /*controlRevision*/) const
{
    if (!bot || !sAzerothFriendConfig->selfContextEnable)
        return "{}";

    std::ostringstream ss;
    ss << "{\"self_context\":" << AzerothFriendBotController::CollectSelfContext(bot) << "}";
    return ss.str();
}

std::string AzerothFriendEnvironment::BuildLightEnvironmentJson(Player* bot)
{
    if (!bot || !bot->GetMap())
        return "{}";

    std::vector<std::string> parts = {
        BuildVitalsJson(bot, 0),
        BuildSurroundingsJson(bot),
    };

    Player* master = FindMaster(bot);
    if (master)
        parts.push_back("{\"master\":" + BuildOwnerJson(bot) + "}");
    else
        parts.push_back("{\"master\":null}");

    if (sAzerothFriendConfig->selfContextEnable)
        parts.push_back(BuildCapabilitiesJson(bot, 0));

    return JoinJsonObjects(parts);
}

/**
 * Publish the world-thread value snapshots to the RAM-first transport.
 *
 * Vitals, surroundings and owner state travel on every delta; control/goals on
 * revision change (or a slow heartbeat) and capabilities (spellbook, gear, bags)
 * only when they can have changed. Nothing here blocks: the transport copies the
 * strings and its worker serializes/transmits them.
 */
void AzerothFriendEnvironment::PublishLiveState(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !sAFLiveState->IsRunning())
        return;

    uint32 botGuid = bot->GetGUID().GetCounter();
    if (!sAFLiveState->IsBotBridgeEnabled(botGuid))
        return;
    auto& state = _botStates[botGuid];
    time_t now = time(nullptr);

    AzerothFriend::BotControlRecord control;
    bool const haveControl = sAFLiveState->GetBotControl(botGuid, control);
    uint32 const revision = haveControl ? control.controlRevision : 0;
    bool const fullRefresh = sAFLiveState->IsFullRefreshRequested(botGuid);

    sAFLiveState->Publish(botGuid, AzerothFriend::LiveSection::Vitals, BuildVitalsJson(bot, revision), revision);
    sAFLiveState->Publish(botGuid, AzerothFriend::LiveSection::Surroundings, BuildSurroundingsJson(bot), revision);
    sAFLiveState->Publish(botGuid, AzerothFriend::LiveSection::Owner, BuildOwnerJson(bot), revision);

    bool const controlChanged = haveControl &&
        (state.lastPublishedRevision != revision || now - state.lastControlPublishTime >= 10);
    if (haveControl && (fullRefresh || controlChanged || state.lastControlPublishTime == 0))
    {
        std::ostringstream controlJson;
        controlJson << "{"
                    << "\"control_revision\":" << control.controlRevision << ","
                    << "\"bridge_enabled\":" << (control.bridgeEnabled ? "true" : "false") << ","
                    << "\"autonomy_enabled\":" << (control.autonomyEnabled ? "true" : "false") << ","
                    << "\"goal_status\":" << DoubleQuote(control.goalStatus) << ","
                    << "\"current_goal\":" << DoubleQuote(control.currentGoal) << ","
                    << "\"long_term_goal\":" << DoubleQuote(control.longTermGoal) << ","
                    << "\"goal_progress\":" << DoubleQuote(control.goalProgress) << ","
                    << "\"goal_result\":" << DoubleQuote(control.goalResult) << ","
                    << "\"action_mode\":" << DoubleQuote(control.actionMode.empty() ? "travel" : control.actionMode) << ","
                    << "\"mode\":" << DoubleQuote(control.mode)
                    << "}";
        sAFLiveState->Publish(botGuid, AzerothFriend::LiveSection::Control, controlJson.str(), revision);
        state.lastPublishedRevision = revision;
        state.lastControlPublishTime = now;
    }

    if (fullRefresh || now - state.lastCapabilityPublishTime >= 30)
    {
        std::string capabilities = BuildCapabilitiesJson(bot, revision);
        if (capabilities != "{}")
        {
            // Capabilities carry a revision so the bridge can refresh its lazy
            // spell/catalogue cache only when something actually changed.
            std::string withRevision = capabilities;
            withRevision.pop_back();
            withRevision += ",\"revision\":" + std::to_string(revision) + "}";
            sAFLiveState->Publish(botGuid, AzerothFriend::LiveSection::Capabilities, withRevision, revision);
        }
        state.lastCapabilityPublishTime = now;
    }
}

void AzerothFriendEnvironment::UpdateBotEnvironment(Player* bot)
{
    if (!bot || !bot->IsInWorld() || !sAzerothFriendConfig->enable)
        return;

    uint32 botGuid = bot->GetGUID().GetCounter();
    auto& state = _botStates[botGuid];

    float dx = bot->GetPositionX() - state.lastPosX;
    float dy = bot->GetPositionY() - state.lastPosY;
    float dz = bot->GetPositionZ() - state.lastPosZ;
    float distSq = dx * dx + dy * dy + dz * dz;

    uint8 hpPct = (uint8)bot->GetHealthPct();
    uint8 pwrPct = (uint8)bot->GetPowerPct(bot->getPowerType());
    bool inCombat = bot->IsInCombat();
    Unit* target = bot->GetSelectedUnit();
    uint32 targetGuid = target ? target->GetGUID().GetCounter() : 0;
    time_t now = time(nullptr);

    bool deltaTriggered = (distSq >= (sAzerothFriendConfig->deltaDistance * sAzerothFriendConfig->deltaDistance)) ||
                          (std::abs((int)hpPct - (int)state.lastHealthPct) >= 10) ||
                          (inCombat != state.lastInCombat) ||
                          (targetGuid != state.lastTargetGuid) ||
                          (now - state.lastSnapshotTime >= 10); // heart-beat every 10s

    if (!deltaTriggered)
    {
        // A freshly connected bridge asks for a complete baseline; publish it even
        // when the bot is standing still so live state never waits for movement.
        if (sAFLiveState->IsRunning() && sAFLiveState->IsFullRefreshRequested(botGuid))
            PublishLiveState(bot);

        // The panel used to go quiet whenever the bot stood still, because the delta
        // gate also suppressed telemetry. Re-send on a fixed cadence so the HUD tracks
        // the world live; the expensive path (snapshot scan + checkpoint) stays gated.
        if (now - state.lastTelemetryTime >= (time_t)sAzerothFriendConfig->telemetryIntervalSeconds)
        {
            state.lastTelemetryTime = now;
            BroadcastTelemetry(bot);
        }
        return;
    }

    state.lastPosX = bot->GetPositionX();
    state.lastPosY = bot->GetPositionY();
    state.lastPosZ = bot->GetPositionZ();
    state.lastHealthPct = hpPct;
    state.lastPowerPct = pwrPct;
    state.lastInCombat = inCombat;
    state.lastTargetGuid = targetGuid;
    state.lastSnapshotTime = now;
    state.lastTelemetryTime = now;
    state.checkpointDirty = true;

    std::string envJson = BuildLightEnvironmentJson(bot);

    // 1. Store in the RAM ring buffer (voices storage mode 1)
    if (sAzerothFriendConfig->storageMode == 1)
    {
        AzerothFriend::SnapshotMemory::Instance().StoreSnapshot(
            botGuid, envJson,
            sAzerothFriendConfig->ramMaximumSnapshots,
            sAzerothFriendConfig->snapshotTtlMinutes);
    }

    // 2. Publish the delta to the live-state transport (RAM-first reads).
    PublishLiveState(bot);

    // 3. SQL checkpoint: recovery/debug data, at most once per configured cadence
    //    and only when the environment actually changed.
    if (state.checkpointDirty &&
        now - state.lastCheckpointTime >= (time_t)sAzerothFriendConfig->environmentCheckpointSeconds)
    {
        state.lastCheckpointTime = now;
        state.checkpointDirty = false;
        SaveEnvironmentCheckpoint(bot, envJson, hpPct, pwrPct, inCombat, targetGuid);
    }

    // Broadcast live telemetry (State, Context, Mindset) with zero extra tokens
    BroadcastTelemetry(bot);
}

void AzerothFriendEnvironment::SaveEnvironmentCheckpoint(Player* bot, std::string const& envJson,
                                                         uint8 hpPct, uint8 pwrPct, bool inCombat,
                                                         uint32 targetGuid)
{
    if (!bot)
        return;

    std::string targetName = "";
    if (Unit* target = bot->GetSelectedUnit())
        targetName = target->GetName();

    std::ostringstream sql;
    sql << "INSERT INTO azeroth_friend_state "
        << "(bot_guid, map_id, zone_id, area_id, pos_x, pos_y, pos_z, orientation, "
        << "level, health_pct, power_pct, in_combat, target_guid, target_name, environment_json) "
        << "VALUES ("
        << bot->GetGUID().GetCounter() << ", "
        << bot->GetMapId() << ", "
        << bot->GetZoneId() << ", "
        << bot->GetAreaId() << ", "
        << bot->GetPositionX() << ", "
        << bot->GetPositionY() << ", "
        << bot->GetPositionZ() << ", "
        << bot->GetOrientation() << ", "
        << (uint32)bot->GetLevel() << ", "
        << (uint32)hpPct << ", "
        << (uint32)pwrPct << ", "
        << (inCombat ? 1 : 0) << ", "
        << (targetGuid ? std::to_string(targetGuid) : "NULL") << ", "
        << (targetName.empty() ? "NULL" : "'" + AzerothFriendShared::EscapeSqlString(targetName) + "'") << ", "
        << "'" + AzerothFriendShared::EscapeSqlString(envJson) + "') "
        << "ON DUPLICATE KEY UPDATE "
        << "map_id = VALUES(map_id), "
        << "zone_id = VALUES(zone_id), "
        << "area_id = VALUES(area_id), "
        << "pos_x = VALUES(pos_x), "
        << "pos_y = VALUES(pos_y), "
        << "pos_z = VALUES(pos_z), "
        << "orientation = VALUES(orientation), "
        << "level = VALUES(level), "
        << "health_pct = VALUES(health_pct), "
        << "power_pct = VALUES(power_pct), "
        << "in_combat = VALUES(in_combat), "
        << "target_guid = VALUES(target_guid), "
        << "target_name = VALUES(target_name), "
        << "environment_json = VALUES(environment_json), "
        << "updated_at = NOW()";

    // Execute() enqueues onto the database worker pool: it does not block the
    // world thread and never waits for a result.
    CharacterDatabase.Execute(sql.str().c_str());
}

void AzerothFriendEnvironment::BroadcastTelemetry(Player* bot, Player* targetPlayer)
{
    if (!bot || !bot->IsInWorld())
        return;

    uint8 hpPct = (uint8)bot->GetHealthPct();
    uint8 pwrPct = (uint8)bot->GetPowerPct(bot->getPowerType());
    bool inCombat = bot->IsInCombat();
    Unit* target = bot->GetSelectedUnit();
    std::string targetName = target ? target->GetName() : "";

    uint32 zoneId = bot->GetZoneId();
    uint32 areaId = bot->GetAreaId();
    AreaTableEntry const* zoneEntry = sAreaTableStore.LookupEntry(zoneId);
    AreaTableEntry const* areaEntry = sAreaTableStore.LookupEntry(areaId);
    char const* zp = zoneEntry ? zoneEntry->area_name[sWorld->GetDefaultDbcLocale()] : nullptr;
    std::string zoneName = zp ? zp : "";
    char const* ap = areaEntry ? areaEntry->area_name[sWorld->GetDefaultDbcLocale()] : nullptr;
    std::string areaName = ap ? ap : "";

    bool isOutdoors = bot->IsOutdoors();
    bool isResting = bot->HasPlayerFlag(PLAYER_FLAGS_RESTING);
    bool inInn = isResting;
    bool isSwimming = bot->IsInWater();
    bool isMounted = bot->IsMounted();
    uint32 botGuid = bot->GetGUID().GetCounter();

    // 1. FRIEND_STATE
    std::ostringstream statePayload;
    statePayload << "Name: " << bot->GetName()
                 << "|Race: " << (uint32)bot->getRace()
                 << "|Class: " << (uint32)bot->getClass()
                 << "|Level: " << (uint32)bot->GetLevel()
                 << "|Health: " << (uint32)hpPct << "% (" << bot->GetHealth() << "/" << bot->GetMaxHealth() << ")"
                 << "|Power: " << (uint32)pwrPct << "% (" << bot->GetPower(bot->getPowerType()) << "/" << bot->GetMaxPower(bot->getPowerType()) << ")"
                 << "|Zone: " << (zoneName.empty() ? std::to_string(zoneId) : zoneName)
                 << "|Subzone: " << (areaName.empty() ? std::to_string(areaId) : areaName)
                 << "|Coordinates: " << (int)bot->GetPositionX() << ", " << (int)bot->GetPositionY() << ", " << (int)bot->GetPositionZ()
                 << "|Spellbook: " << (uint32)bot->GetSpellMap().size() << " known"
                 << "|Casting: " << ([&]() -> std::string {
                        for (uint32 slot = 0; slot < CURRENT_MAX_SPELL; ++slot)
                            if (Spell* cast = bot->GetCurrentSpell(CurrentSpellTypes(slot)))
                                if (SpellInfo const* info = cast->GetSpellInfo())
                                    return info->SpellName[0] ? info->SpellName[0] : "unknown";
                        return "idle";
                    })()
                 << "|Combat: " << (inCombat ? "IN COMBAT" : "NOT IN COMBAT")
                 << "|Target: " << (targetName.empty() ? "None" : targetName)
                 << "|ActionMode: " << ([&]() -> std::string {
                        AzerothFriend::BotControlRecord ctrl;
                        if (sAFLiveState->GetBotControl(botGuid, ctrl) && !ctrl.actionMode.empty())
                            return ctrl.actionMode;
                        return "travel";
                    })();
    AzerothFriendShared::SendTelemetry(bot, "FRIEND_STATE", statePayload.str(), targetPlayer);

    // 2. FRIEND_CONTEXT (Self-Context: gear avg ilvl, bags, consumables, quests, environment flags, master)
    std::string selfCtx = AzerothFriendBotController::CollectSelfContext(bot);
    std::ostringstream ctxPayload;
    ctxPayload << "Bot: " << bot->GetName()
               << "|SelfContext: " << selfCtx
               << "|ZoneName: " << (zoneName.empty() ? std::to_string(zoneId) : zoneName)
               << "|AreaName: " << (areaName.empty() ? std::to_string(areaId) : areaName)
               << "|Outdoors: " << (isOutdoors ? "Yes" : "No")
               << "|InInn: " << (inInn ? "Yes" : "No")
               << "|Resting: " << (isResting ? "Yes" : "No")
               << "|Swimming: " << (isSwimming ? "Yes" : "No")
               << "|Mounted: " << (isMounted ? "Yes" : "No");

    Player* master = FindMaster(bot);
    if (master)
    {
        Unit* masterTarget = master->GetSelectedUnit();
        std::string masterTargetName = masterTarget ? masterTarget->GetName() : "None";
        uint32 targetHpPct = masterTarget ? (uint32)masterTarget->GetHealthPct() : 0;
        bool targetEnemy = masterTarget ? master->IsValidAttackTarget(masterTarget) : false;
        bool isCasting = master->IsNonMeleeSpellCast(false);

        ctxPayload << "|MasterName: " << master->GetName()
                   << "|MasterDist: " << std::round(bot->GetDistance(master) * 10.0f) / 10.0f
                   << "|MasterHP: " << (uint32)master->GetHealthPct()
                   << "|MasterCombat: " << (master->IsInCombat() ? "In Combat" : "Out of Combat")
                   << "|MasterTarget: " << masterTargetName
                   << "|MasterTargetHP: " << targetHpPct
                   << "|MasterTargetEnemy: " << (targetEnemy ? "Enemy" : "Friendly")
                   << "|MasterCasting: " << (isCasting ? "Casting" : "None");
    }
    else
    {
        ctxPayload << "|MasterName: None|MasterDist: 0|MasterHP: 0|MasterCombat: None|MasterTarget: None|MasterTargetHP: 0|MasterTargetEnemy: None|MasterCasting: None";
    }
    AzerothFriendShared::SendTelemetry(bot, "FRIEND_CONTEXT", ctxPayload.str(), targetPlayer);

    // 3. FRIEND_MINDSET (Mindset latency, commitment timer, tokens saved).
    //    Counters come from the RAM cache refreshed asynchronously; SQL is only
    //    read when the live-state service is disabled (explicit rollback mode).
    uint32 tokensSaved = 0;
    uint32 coProc = 0;
    uint32 sensReused = 0;
    if (sAFLiveState->IsRunning())
    {
        AzerothFriend::TelemetryCounters counters = sAFLiveState->GetTelemetry();
        coProc = counters.coProcessed;
        sensReused = counters.sensoryReused;
        tokensSaved = counters.tokensSaved;
    }
    else
    {
        QueryResult telRes = CharacterDatabase.Query(
            "SELECT co_processed_events, sensory_reused_events, tokens_saved FROM azeroth_friend_telemetry WHERE id = 1");
        if (telRes)
        {
            Field* tf = telRes->Fetch();
            coProc = tf[0].Get<uint32>();
            sensReused = tf[1].Get<uint32>();
            tokensSaved = tf[2].Get<uint32>();
        }
    }

    bool isClaimed = AzerothFriendBotController::IsBotClaimed(bot);
    std::ostringstream mindsetPayload;
    mindsetPayload << "Bot: " << bot->GetName()
                   << "|Claimed: " << (isClaimed ? "CLAIMED" : "RELEASED")
                   << "|Bridge: " << (isClaimed ? "DISCONNECTED" : "CONNECTED")
                   << "|CommitmentWindow: " << sAzerothFriendConfig->mindsetCommitmentSeconds << "s"
                   << "|TokensSaved: " << tokensSaved
                   << "|CoProcessed: " << coProc
                   << "|SensoryReused: " << sensReused
                   << "|ThinkingCadence: " << sAzerothFriendConfig->thinkingCadence;
    AzerothFriendShared::SendTelemetry(bot, "FRIEND_MINDSET", mindsetPayload.str(), targetPlayer);

    // Bot API v1 overview. This is deliberately server-owned telemetry: the
    // addon renders the live configuration and registry counts but never
    // infers whether a capability is available or safe on its own.
    uint32 autonomousCapabilities = 0;
    uint32 ownerCapabilities = 0;
    uint32 gmCapabilities = 0;
    std::vector<AzerothFriendActionRegistry::ActionInfo> const& capabilities =
        AzerothFriendActionRegistry::Curated();
    for (AzerothFriendActionRegistry::ActionInfo const& info : capabilities)
    {
        if (info.authority == "owner_command")
            ++ownerCapabilities;
        else if (info.authority == "gm_manual")
            ++gmCapabilities;
        else
            ++autonomousCapabilities;
    }

    std::ostringstream apiPayload;
    apiPayload << "Bot: " << bot->GetName()
               << "|View: overview"
               << "|ApiVersion: 1"
               << "|CapabilityRevision: 1"
               << "|Execution: " << (sAzerothFriendConfig->botApiEnable ? "ENABLED" : "DISABLED")
               << "|OwnerTier: " << (sAzerothFriendConfig->botApiOwnerTierEnable ? "ENABLED" : "DISABLED")
               << "|RawPassthrough: " << (sAzerothFriendConfig->rawPassthroughEnable ? "ENABLED" : "DISABLED")
               << "|NativeDiscovery: " << (sAzerothFriendConfig->botApiNativeDiscoveryEnable ? "ENABLED" : "DISABLED")
               << "|Curated: " << capabilities.size()
               << "|Autonomous: " << autonomousCapabilities
               << "|OwnerCommand: " << ownerCapabilities
               << "|GmManual: " << gmCapabilities;
    AzerothFriendShared::SendTelemetry(bot, "FRIEND_API", apiPayload.str(), targetPlayer);

    // Goal telemetry: RAM control record first, SQL only in rollback mode.
    if (sAFLiveState->IsRunning())
    {
        AzerothFriend::BotControlRecord control;
        if (sAFLiveState->GetBotControl(botGuid, control))
        {
            std::ostringstream data;
            data << "{\"bot\":\"" << bot->GetName() << "\",\"enabled\":" << (control.autonomyEnabled ? "true" : "false")
                 << ",\"status\":\"" << AzerothFriendShared::EscapeJsonString(control.goalStatus) << "\""
                 << ",\"goal\":\"" << AzerothFriendShared::EscapeJsonString(control.currentGoal) << "\""
                 << ",\"progress\":\"" << AzerothFriendShared::EscapeJsonString(control.goalProgress) << "\""
                 << ",\"result\":\"" << AzerothFriendShared::EscapeJsonString(control.goalResult) << "\""
                 << ",\"long_goal\":\"" << AzerothFriendShared::EscapeJsonString(control.longTermGoal) << "\""
                 << "}";
            AzerothFriendShared::SendTelemetry(bot, "FRIEND_GOAL", data.str(), targetPlayer);
        }
    }
    else
    {
        QueryResult goal = CharacterDatabase.Query(("SELECT autonomy_enabled, goal_status, current_goal, "
            "COALESCE(goal_progress,''), COALESCE(goal_result,''), COALESCE(long_term_goal,'') "
            "FROM azeroth_friend_bots WHERE bot_guid=" + std::to_string(botGuid)).c_str());
        if (goal)
        {
            Field* fields = goal->Fetch();
            std::ostringstream data;
            data << "{\"bot\":\"" << bot->GetName() << "\",\"enabled\":" << (fields[0].Get<bool>() ? "true" : "false");
            char const* keys[] = { "status", "goal", "progress", "result", "long_goal" };
            for (uint8 i = 0; i < 5; ++i)
                data << ",\"" << keys[i] << "\":\"" << AzerothFriendShared::EscapeJsonString(fields[i + 1].Get<std::string>()) << "\"";
            data << "}";
            AzerothFriendShared::SendTelemetry(bot, "FRIEND_GOAL", data.str(), targetPlayer);
        }
    }

    // Recent action history: served from RAM (asynchronously refreshed) so the
    // world thread never blocks on a row scan.
    if (sAFLiveState->IsRunning())
    {
        AzerothFriendShared::SendTelemetry(bot, "FRIEND_HISTORY", sAFLiveState->GetHistory(botGuid), targetPlayer);
    }
    else
    {
        QueryResult history = CharacterDatabase.Query(("SELECT action_type,status,COALESCE(failure_reason,''), "
            "COALESCE(thought,'') FROM azeroth_friend_actions WHERE bot_guid=" +
            std::to_string(botGuid) + " ORDER BY id DESC LIMIT 5").c_str());
        std::ostringstream recent;
        if (history)
        {
            do
            {
                Field* fields = history->Fetch();
                recent << fields[0].Get<std::string>() << " -> " << fields[1].Get<std::string>() << " "
                    << fields[2].Get<std::string>() << "\n" << fields[3].Get<std::string>().substr(0, 240) << "\n\n";
            } while (history->NextRow());
        }
        AzerothFriendShared::SendTelemetry(bot, "FRIEND_HISTORY", recent.str(), targetPlayer);
    }

    std::ostringstream players;
    for (auto const& entry : ObjectAccessor::GetPlayers())
    {
        Player* other = entry.second;
        if (other && other != bot && other->IsInWorld() && bot->IsWithinDistInMap(other, sAzerothFriendConfig->scanRadius))
            players << other->GetName() << " | level " << other->GetLevel() << " | HP " << uint32(other->GetHealthPct())
                    << "% | " << uint32(bot->GetDistance(other)) << " yards\n";
    }
    AzerothFriendShared::SendTelemetry(bot, "FRIEND_PLAYERS", players.str(), targetPlayer);

    // 3b. FRIEND_THOUGHT (Real-time live thoughts & reasoning from cognitive planner)
    std::string curThought;
    if (sAFLiveState->IsRunning())
    {
        curThought = sAFLiveState->GetThought(botGuid);
    }
    else
    {
        QueryResult thoughtRes = CharacterDatabase.Query(
            ("SELECT COALESCE(last_thought, '') FROM azeroth_friend_state WHERE bot_guid = " +
             std::to_string(botGuid)).c_str());
        if (thoughtRes)
            curThought = thoughtRes->Fetch()[0].Get<std::string>();
    }

    if (!curThought.empty())
    {
        // [MAF-048] Only broadcast if thought changed or this is a targeted inspection
        if (curThought != _botStates[botGuid].lastSentThought || targetPlayer != nullptr)
        {
            _botStates[botGuid].lastSentThought = curThought;
            AzerothFriendShared::SendTelemetry(bot, "FRIEND_THOUGHT", "Bot: " + bot->GetName() + "|" + curThought, targetPlayer);
        }
    }

    // 4. FRIEND_ENV (Nearby surroundings radar)
    std::vector<EntityCandidate> candidates = CollectNearbyCandidates(bot, master);
    std::vector<std::pair<float, std::string>> nearby;
    float scanDist = sAzerothFriendConfig->scanRadius;

    if (bot->GetMap())
    {
        for (auto const& pair : bot->GetMap()->GetCreatureBySpawnIdStore())
        {
            Creature* c = pair.second;
            if (!c || !c->IsInWorld() || !bot->IsWithinDistInMap(c, scanDist))
                continue;
            if (sAzerothFriendConfig->snapshotLineOfSight && !bot->IsWithinLOSInMap(c))
                continue;

            float dist = bot->GetDistance(c);
            std::string tag = "NEUTRAL";
            std::string detail;
            if (c->isDead())
            {
                if (!c->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
                    continue;
                tag = "LOOT";
            }
            else if (c->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER))
            {
                tag = "QUESTGIVER";
            }
            else if (bot->IsValidAttackTarget(c))
            {
                tag = "HOSTILE";
                detail = " " + std::to_string((uint32)c->GetHealthPct()) + "%";
                if (c->GetVictim() == bot)
                    detail += " attacking-you";
                else if (master && c->GetVictim() == master)
                    detail += " attacking-master";
            }
            else if (c->IsFriendlyTo(bot))
            {
                tag = "FRIENDLY";
            }

            nearby.push_back({ dist, tag + ": " + c->GetName() + " (" +
                                     std::to_string((int)dist) + "y)" + detail });
        }

        for (auto const& pair : bot->GetMap()->GetGameObjectBySpawnIdStore())
        {
            GameObject* go = pair.second;
            if (!go || !go->IsInWorld() || !bot->IsWithinDistInMap(go, scanDist))
                continue;
            if (sAzerothFriendConfig->snapshotLineOfSight && !bot->IsWithinLOSInMap(go))
                continue;

            float dist = bot->GetDistance(go);
            std::string tag = "OBJECT";
            switch (go->GetGoType())
            {
                case GAMEOBJECT_TYPE_CHEST:       tag = "CHEST"; break;
                case GAMEOBJECT_TYPE_QUESTGIVER:  tag = "QUEST_OBJECT"; break;
                case GAMEOBJECT_TYPE_GOOBER:      tag = "NODE"; break;
                default: break;
            }
            nearby.push_back({ dist, tag + ": " + go->GetName() + " (" +
                                     std::to_string((int)dist) + "y)" });
        }

        for (auto const& ref : bot->GetMap()->GetPlayers())
        {
            Player* p = ref.GetSource();
            if (!p || p == bot || !p->IsInWorld() || !bot->IsWithinDistInMap(p, scanDist))
                continue;
            if (sAzerothFriendConfig->snapshotLineOfSight && !bot->IsWithinLOSInMap(p))
                continue;

            float dist = bot->GetDistance(p);
            std::string tag = (p == master) ? "MASTER"
                : (bot->IsInSameGroupWith(p) ? "PARTY"
                : (bot->IsFriendlyTo(p) ? "PLAYER" : "ENEMY_PLAYER"));
            nearby.push_back({ dist, tag + ": " + p->GetName() + " (" +
                                     std::to_string((int)dist) + "y)" });
        }
    }

    std::sort(nearby.begin(), nearby.end(),
              [](std::pair<float, std::string> const& a, std::pair<float, std::string> const& b) { return a.first < b.first; });

    size_t shown = std::min<size_t>(nearby.size(), sAzerothFriendConfig->maxVisibleEntities);
    std::ostringstream envPayload;
    envPayload << "Scan: " << (int)scanDist << "y | " << nearby.size() << " in range | showing " << shown;
    for (size_t i = 0; i < shown; ++i)
        envPayload << "|" << nearby[i].second;

    AzerothFriendShared::SendTelemetry(bot, "FRIEND_ENV", envPayload.str(), targetPlayer);

    // 5. FRIEND_DIAG: server-owned RAM freshness, transport status, context size
    //    and summary timestamps. The addon only renders these values.
    if (sAFLiveState->IsRunning())
    {
        AzerothFriend::LiveStateStatus status = sAFLiveState->GetStatus();
        AzerothFriend::BridgeDiagnostics diagnostics = sAFLiveState->GetBridgeDiagnostics();
        time_t now = time(nullptr);

        std::string transport = "OFFLINE";
        if (status.bridgeAuthorized)
            transport = (now - status.lastClientActivity <= 5) ? "LIVE" : "STALLED";
        else if (status.bridgeConnected)
            transport = "HANDSHAKE";

        uint32 cacheAge = diagnostics.receivedAt ? (uint32)(now - diagnostics.receivedAt) : 0;
        uint32 summaryAge = diagnostics.summaryTimestamp ? (uint32)(now - diagnostics.summaryTimestamp) : 0;

        std::ostringstream diagPayload;
        diagPayload << "Bot: " << bot->GetName()
                    << "|Transport: " << transport
                    << "|Session: " << status.sessionId
                    << "|FramesSent: " << status.framesSent
                    << "|Refreshes: " << status.refreshesRequested
                    << "|CacheAge: " << cacheAge
                    << "|ContextTokens: " << diagnostics.contextTokens
                    << "|ContextCeiling: " << sAzerothFriendConfig->contextMaxInputTokens
                    << "|RamBytes: " << diagnostics.cacheBytes
                    << "|RamCachedBots: " << diagnostics.cacheBots
                    << "|SummaryAge: " << summaryAge
                    << "|RolloutStage: " << sAzerothFriendConfig->rolloutStage
                    << "|SqlCompat: " << (sAzerothFriendConfig->sqlCompatibilityMode ? "ON" : "OFF");
        if (!diagnostics.lastDiagnostic.empty())
            diagPayload << "|Diagnostic: " << diagnostics.lastDiagnostic;
        AzerothFriendShared::SendTelemetry(bot, "FRIEND_DIAG", diagPayload.str(), targetPlayer);
    }
}
