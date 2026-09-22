#ifndef MOD_AZEROTH_FRIEND_CONFIG_H
#define MOD_AZEROTH_FRIEND_CONFIG_H

#include "Common.h"
#include <string>
#include <vector>

struct AzerothFriendConfig
{
    static AzerothFriendConfig* instance();

    bool enable = true;
    bool debug = false;
    std::vector<std::string> controlledBots;
    uint32 maxControlledBots = 1;
    uint32 tickIntervalMs = 500;

    // Light Environment & Storage Modes (RAM vs SQL)
    float deltaDistance = 5.0f;
    uint32 maxVisibleEntities = 6;
    float scanRadius = 30.0f;
    uint32 telemetryIntervalSeconds = 3;
    uint32 storageMode = 1; // 1 = RAM ring buffer, 2 = SQL persistent
    uint32 ramMaximumSnapshots = 3;
    uint32 sqlMaximumSnapshots = 10;
    uint32 snapshotTtlMinutes = 30;
    bool snapshotLineOfSight = true;

    // Self-Context & Bot Claiming (Autonomous Bot Control)
    bool selfContextEnable = true;

    // Playerbot Control Lease (real suspension of the bot's own AI)
    bool leaseEnable = true;
    std::string leaseStrategyMode = "suspend"; // suspend | clear | passive_stay
    uint32 leaseRefreshSeconds = 20;

    // Step execution
    uint32 actionStepTimeoutSeconds = 12;
    uint32 actionStepRetries = 1;

    // Raw playerbot passthrough (any native action / chat command)
    bool rawPassthroughEnable = false;
    std::string deniedActions;
    std::string deniedCommands;

    // Versioned semantic Bot API. Consequential calls additionally require a
    // queue row whose provenance matches the bot's current owner.
    bool botApiEnable = true;
    bool botApiOwnerTierEnable = true;
    bool botApiNativeDiscoveryEnable = true;

    // Speech fallback when mod-llm-chatter is absent/disabled
    bool sayFallback = false;
    std::string sayFallbackChannel = "party";

    // Autonomous ambient action ticks
    bool autonomyEnable = true;
    uint32 autonomyTickSeconds = 5;
    std::string thinkingCadence = "normal"; // "low" (20s), "normal" (10s), "high" (4s)
    std::string autonomyModes = "companion,guard,autonomous";
    uint32 autonomyLootRadius = 20;
    uint32 autonomyHealthPct = 40;
    bool autonomyCombatTicks = false;

    // Multi-Action Planning
    bool multiActionPlanEnable = true;
    uint32 multiActionPlanMaxSteps = 5;
    bool sessionContextReuseEnable = true;

    // RAM-first live state transport (loopback socket between worker and bridge)
    bool liveStateEnable = false;
    bool liveStateShadowMode = false;
    std::string liveStateHost = "127.0.0.1";
    uint32 liveStatePort = 8377;
    std::string liveStateSecret;
    uint32 liveStateHeartbeatSeconds = 2;
    uint32 liveStateFrameCapBytes = 256 * 1024;
    uint32 liveStatePublishIntervalMs = 250;
    uint32 liveStateQueueLimit = 1024;
    bool liveStateRequireFresh = true;

    // Compact planning context & summaries (bridge reads the same keys)
    bool contextCompactEnable = true;
    uint32 contextTargetInputTokens = 2000;
    uint32 contextMaxInputTokens = 3500;
    uint32 summaryIntervalSeconds = 60;
    bool sqlCompatibilityMode = false;
    uint32 cacheBotRefreshSeconds = 15;
    uint32 environmentCheckpointSeconds = 60;
    uint32 telemetryCheckpointSeconds = 10;
    uint32 diagnosticRetentionDays = 7;
    uint32 rolloutStage = 3; // 1 = instrument, 2 = shadow, 3 = live reads, 4 = summaries

    // mod-llm-chatter integration. Token sharing, sensory reuse and ambient speech
    // delegation are implemented in the Python bridge, which reads those keys from the
    // same config file; only the delivery channel is needed on the native side.
    std::string defaultChannel = "party";

    // Dual-Mind & Reflexes
    bool combatReflexes = true;
    uint32 emergencyThresholdPct = 20;

    // Stuck Detection
    bool stuckDetectionEnable = true;
    uint32 stuckTimeoutSeconds = 4;
    std::string stuckStrategy = "jump_repath_teleport";

    // Memory & Affinity
    bool affinitySystem = true;
    bool episodicMemory = true;
    uint32 maxMemoriesPerPrompt = 4;

    // Autonomous Downtime
    bool autoRepairSell = true;
    bool autoEquipUpgrades = true;
    bool downtimeInns = true;

    // Mindset Latency & Cognitive Inertia
    bool mindsetEnable = true;
    uint32 mindsetCommitmentSeconds = 15;
    bool mindsetAllowOpportunistic = true;

    // Dialogue & Overhearing
    bool hearOtherBots = true;
    bool hearNPCs = true;
    uint32 maxRecentDialogue = 6;

    void Load();
    bool IsBotControlled(std::string const& botName) const;
    bool IsAutonomyMode(std::string const& mode) const;
    std::string GetPrimaryBotName() const;
    uint32 GetThinkingCadenceSeconds() const;
    void SetThinkingCadence(std::string const& cadence);
};

#define sAzerothFriendConfig AzerothFriendConfig::instance()

#endif // MOD_AZEROTH_FRIEND_CONFIG_H
