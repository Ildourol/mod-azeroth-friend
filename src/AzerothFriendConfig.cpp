#include "AzerothFriendConfig.h"
#include "Config.h"
#include "Log.h"
#include "StringConvert.h"
#include <sstream>
#include <algorithm>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

AzerothFriendConfig* AzerothFriendConfig::instance()
{
    static AzerothFriendConfig instance;
    return &instance;
}

void AzerothFriendConfig::Load()
{
    enable = sConfigMgr->GetOption<bool>("AzerothFriend.Enable", true);
    debug = sConfigMgr->GetOption<bool>("AzerothFriend.Debug", false);
    tickIntervalMs = sConfigMgr->GetOption<uint32>("AzerothFriend.TickIntervalMs", 500);

    maxControlledBots = sConfigMgr->GetOption<uint32>("AzerothFriend.ControlledBots.MaxCount", 1);
    if (maxControlledBots == 1 && sConfigMgr->GetOption<uint32>("AzerothFriend.MaxControlledBots", 0) > 0)
        maxControlledBots = sConfigMgr->GetOption<uint32>("AzerothFriend.MaxControlledBots", 1);

    // Controlled bots parsing
    controlledBots.clear();
    std::string botsStr = sConfigMgr->GetOption<std::string>("AzerothFriend.ControlledBots", "Friendbot,Ollamatest");
    std::stringstream ss(botsStr);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        // trim whitespace
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), item.end());
        if (!item.empty())
        {
            controlledBots.push_back(item);
        }
    }

    // If more names in the list than maxControlledBots, just use the first ones
    if (maxControlledBots > 0 && controlledBots.size() > maxControlledBots)
    {
        controlledBots.resize(maxControlledBots);
    }

    // Light Environment & Storage Modes (RAM vs SQL)
    deltaDistance = sConfigMgr->GetOption<float>("AzerothFriend.Environment.DeltaDistance", 5.0f);
    maxVisibleEntities = sConfigMgr->GetOption<uint32>("AzerothFriend.Environment.MaxVisibleEntities", 6);
    scanRadius = sConfigMgr->GetOption<float>("AzerothFriend.Environment.ScanRadius", 30.0f);
    telemetryIntervalSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.Environment.TelemetryIntervalSeconds", 3);
    storageMode = sConfigMgr->GetOption<uint32>("AzerothFriend.Environment.StorageMode", 1);
    ramMaximumSnapshots = sConfigMgr->GetOption<uint32>("AzerothFriend.Environment.RamMaximumSnapshots", 3);
    sqlMaximumSnapshots = sConfigMgr->GetOption<uint32>("AzerothFriend.Environment.SqlMaximumSnapshots", 10);
    snapshotTtlMinutes = sConfigMgr->GetOption<uint32>("AzerothFriend.Environment.SnapshotTTLMinutes", 30);
    snapshotLineOfSight = sConfigMgr->GetOption<bool>("AzerothFriend.Environment.SnapshotLineOfSight", true);

    // Self-Context & Bot Claiming (Autonomous Bot Control)
    selfContextEnable = sConfigMgr->GetOption<bool>("AzerothFriend.SelfContext.Enable", true);

    // Playerbot Control Lease
    leaseEnable = sConfigMgr->GetOption<bool>("AzerothFriend.Control.Lease.Enable", true);
    leaseStrategyMode = sConfigMgr->GetOption<std::string>("AzerothFriend.Control.Lease.StrategyMode", "suspend");
    leaseRefreshSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.Control.Lease.RefreshSeconds", 20);
    if (leaseRefreshSeconds < 5)
        leaseRefreshSeconds = 5;

    // Step execution
    actionStepTimeoutSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.ActionStep.TimeoutSeconds", 12);
    if (actionStepTimeoutSeconds < 3)
        actionStepTimeoutSeconds = 3;
    actionStepRetries = sConfigMgr->GetOption<uint32>("AzerothFriend.ActionStep.RetryCount", 1);

    // Raw passthrough
    rawPassthroughEnable = sConfigMgr->GetOption<bool>("AzerothFriend.PlayerbotActions.AllowRawPassthrough", false);
    deniedActions = sConfigMgr->GetOption<std::string>("AzerothFriend.PlayerbotActions.Denylist",
        "logout,reset,leave,uninvite,destroy,cheat,repop,teleport,guild remove,guild demote,guild leave,wipe,bg join,bg leave,lfg leave,self resurrect");
    deniedCommands = sConfigMgr->GetOption<std::string>("AzerothFriend.PlayerbotActions.DeniedCommands",
        "logout,reset,leave,destroy,cheat,repop,teleport,uninvite,wipe,debug,cdebug,cs,guild remove,guild demote,guild leave");
    botApiEnable = sConfigMgr->GetOption<bool>("AzerothFriend.BotApi.Enable", true);
    botApiOwnerTierEnable = sConfigMgr->GetOption<bool>("AzerothFriend.BotApi.OwnerTierEnable", true);
    botApiNativeDiscoveryEnable = sConfigMgr->GetOption<bool>("AzerothFriend.BotApi.NativeDiscoveryEnable", true);

    // Speech fallback
    sayFallback = sConfigMgr->GetOption<bool>("AzerothFriend.Chat.SayFallback", false);
    sayFallbackChannel = sConfigMgr->GetOption<std::string>("AzerothFriend.Chat.SayFallbackChannel", "party");

    // Autonomous ambient ticks
    autonomyEnable = sConfigMgr->GetOption<bool>("AzerothFriend.Autonomy.Enable", true);
    autonomyTickSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.Autonomy.TickSeconds", 5);
    if (autonomyTickSeconds < 3)
        autonomyTickSeconds = 3;
    thinkingCadence = sConfigMgr->GetOption<std::string>("AzerothFriend.Autonomy.ThinkingCadence", "normal");
    std::transform(thinkingCadence.begin(), thinkingCadence.end(), thinkingCadence.begin(), ::tolower);
    if (thinkingCadence != "low" && thinkingCadence != "high")
        thinkingCadence = "normal";
    autonomyModes = sConfigMgr->GetOption<std::string>("AzerothFriend.Autonomy.Modes", "companion,guard,autonomous");
    autonomyLootRadius = sConfigMgr->GetOption<uint32>("AzerothFriend.Autonomy.LootRadius", 20);
    autonomyHealthPct = sConfigMgr->GetOption<uint32>("AzerothFriend.Autonomy.HealthPct", 40);
    autonomyCombatTicks = sConfigMgr->GetOption<bool>("AzerothFriend.Autonomy.CombatTicks", false);

    // Multi-Action Planning
    multiActionPlanEnable = sConfigMgr->GetOption<bool>("AzerothFriend.MultiActionPlan.Enable", true);
    multiActionPlanMaxSteps = sConfigMgr->GetOption<uint32>("AzerothFriend.MultiActionPlan.MaxSteps", 5);
    sessionContextReuseEnable = sConfigMgr->GetOption<bool>("AzerothFriend.SessionContextReuse.Enable", true);

    // RAM-first live state transport. The listener is opt-in because it opens a
    // loopback socket; the shared secret is never logged.
    liveStateEnable = sConfigMgr->GetOption<bool>("AzerothFriend.LiveState.Enable", false);
    liveStateShadowMode = sConfigMgr->GetOption<bool>("AzerothFriend.LiveState.ShadowMode", false);
    liveStateHost = sConfigMgr->GetOption<std::string>("AzerothFriend.LiveState.Host", "127.0.0.1");
    liveStatePort = sConfigMgr->GetOption<uint32>("AzerothFriend.LiveState.Port", 8377);
    liveStateSecret = sConfigMgr->GetOption<std::string>("AzerothFriend.LiveState.Secret", "");
    liveStateHeartbeatSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.LiveState.HeartbeatSeconds", 2);
    if (liveStateHeartbeatSeconds < 1)
        liveStateHeartbeatSeconds = 1;
    liveStateFrameCapBytes = sConfigMgr->GetOption<uint32>("AzerothFriend.LiveState.FrameCapBytes", 256 * 1024);
    liveStatePublishIntervalMs = sConfigMgr->GetOption<uint32>("AzerothFriend.LiveState.PublishIntervalMs", 250);
    if (liveStatePublishIntervalMs < 50)
        liveStatePublishIntervalMs = 50;
    liveStateQueueLimit = sConfigMgr->GetOption<uint32>("AzerothFriend.LiveState.QueueLimit", 1024);
    liveStateRequireFresh = sConfigMgr->GetOption<bool>("AzerothFriend.LiveState.RequireFresh", true);

    // Compact context, summaries and SQL ownership
    contextCompactEnable = sConfigMgr->GetOption<bool>("AzerothFriend.Context.Compact.Enable", true);
    contextTargetInputTokens = sConfigMgr->GetOption<uint32>("AzerothFriend.Context.TargetInputTokens", 2000);
    contextMaxInputTokens = sConfigMgr->GetOption<uint32>("AzerothFriend.Context.MaxInputTokens", 3500);
    if (contextMaxInputTokens < contextTargetInputTokens)
        contextMaxInputTokens = contextTargetInputTokens;
    summaryIntervalSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.Summary.IntervalSeconds", 60);
    sqlCompatibilityMode = sConfigMgr->GetOption<bool>("AzerothFriend.Context.SqlCompatibilityMode", false);
    cacheBotRefreshSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.Cache.BotRefreshSeconds", 15);
    if (cacheBotRefreshSeconds < 1)
        cacheBotRefreshSeconds = 1;
    environmentCheckpointSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.Environment.CheckpointSeconds", 60);
    if (environmentCheckpointSeconds < 1)
        environmentCheckpointSeconds = 1;
    telemetryCheckpointSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.Telemetry.CheckpointSeconds", 10);
    diagnosticRetentionDays = sConfigMgr->GetOption<uint32>("AzerothFriend.Diagnostics.RetentionDays", 7);
    rolloutStage = sConfigMgr->GetOption<uint32>("AzerothFriend.Rollout.Stage", 3);
    if (rolloutStage < 1)
        rolloutStage = 1;
    if (rolloutStage > 4)
        rolloutStage = 4;

    // mod-llm-chatter integration. Token sharing, sensory reuse and ambient speech
    // delegation live in the Python bridge (same config file); the native side only
    // needs the channel used when it delivers a line itself.
    defaultChannel = sConfigMgr->GetOption<std::string>("AzerothFriend.Chat.DefaultChannel", "party");

    // Dual-Mind & Reflexes
    combatReflexes = sConfigMgr->GetOption<bool>("AzerothFriend.DualMind.CombatReflexes", true);
    emergencyThresholdPct = sConfigMgr->GetOption<uint32>("AzerothFriend.DualMind.EmergencyThresholdPct", 20);

    // Stuck Detection
    stuckDetectionEnable = sConfigMgr->GetOption<bool>("AzerothFriend.StuckDetection.Enable", true);
    stuckTimeoutSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.StuckDetection.TimeoutSeconds", 4);
    stuckStrategy = sConfigMgr->GetOption<std::string>("AzerothFriend.StuckDetection.Strategy", "jump_repath_teleport");

    // Memory
    affinitySystem = sConfigMgr->GetOption<bool>("AzerothFriend.Memory.AffinitySystem", true);
    episodicMemory = sConfigMgr->GetOption<bool>("AzerothFriend.Memory.EpisodicMemory", true);
    maxMemoriesPerPrompt = sConfigMgr->GetOption<uint32>("AzerothFriend.Memory.MaxMemoriesPerPrompt", 4);

    // Autonomous Downtime
    autoRepairSell = sConfigMgr->GetOption<bool>("AzerothFriend.Autonomous.AutoRepairSell", true);
    autoEquipUpgrades = sConfigMgr->GetOption<bool>("AzerothFriend.Autonomous.AutoEquipUpgrades", true);
    downtimeInns = sConfigMgr->GetOption<bool>("AzerothFriend.Autonomous.DowntimeInns", true);

    // Mindset Latency & Cognitive Inertia
    mindsetEnable = sConfigMgr->GetOption<bool>("AzerothFriend.Mindset.Enable", true);
    mindsetCommitmentSeconds = sConfigMgr->GetOption<uint32>("AzerothFriend.Mindset.CommitmentSeconds", 15);
    mindsetAllowOpportunistic = sConfigMgr->GetOption<bool>("AzerothFriend.Mindset.AllowOpportunistic", true);

    // Dialogue & Overhearing
    hearOtherBots = sConfigMgr->GetOption<bool>("AzerothFriend.Chatter.HearOtherBots", true);
    hearNPCs = sConfigMgr->GetOption<bool>("AzerothFriend.Chatter.HearNPCs", true);
    maxRecentDialogue = sConfigMgr->GetOption<uint32>("AzerothFriend.Chatter.MaxRecentDialogue", 6);

    LOG_INFO("server.loading", "[AzerothFriend] Configuration loaded (Enabled: {}, StorageMode: {}, Controlled bots: {} (max: {}), Primary: '{}')",
             enable ? "Yes" : "No", storageMode == 1 ? "RAM" : "SQL", controlledBots.size(), maxControlledBots, GetPrimaryBotName());
    LOG_INFO("server.loading", "[AzerothFriend] RAM-first rollout: stage={} live-state={} ({}:{}) shadow={} sql-compat={}",
             rolloutStage, liveStateEnable ? "on" : "off", liveStateHost, liveStatePort,
             liveStateShadowMode ? "yes" : "no", sqlCompatibilityMode ? "yes" : "no");
}

std::string AzerothFriendConfig::GetPrimaryBotName() const
{
    return !controlledBots.empty() ? controlledBots.front() : "Friendbot";
}

bool AzerothFriendConfig::IsBotControlled(std::string const& botName) const
{
    if (controlledBots.empty())
        return false;

    for (const auto& name : controlledBots)
    {
        if (strcasecmp(name.c_str(), botName.c_str()) == 0)
            return true;
    }
    return false;
}

bool AzerothFriendConfig::IsAutonomyMode(std::string const& mode) const
{
    if (autonomyModes.empty())
        return true;

    std::stringstream ss(autonomyModes);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) { return !std::isspace(ch); }));
        item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), item.end());
        if (!item.empty() && strcasecmp(item.c_str(), mode.c_str()) == 0)
            return true;
    }
    return false;
}

uint32 AzerothFriendConfig::GetThinkingCadenceSeconds() const
{
    if (thinkingCadence == "low")
        return 20;
    if (thinkingCadence == "high")
        return 4;
    return 10; // normal
}

void AzerothFriendConfig::SetThinkingCadence(std::string const& cadence)
{
    std::string c = cadence;
    std::transform(c.begin(), c.end(), c.begin(), ::tolower);
    if (c == "low" || c == "normal" || c == "high")
        thinkingCadence = c;
}
