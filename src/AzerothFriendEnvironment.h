#ifndef MOD_AZEROTH_FRIEND_ENVIRONMENT_H
#define MOD_AZEROTH_FRIEND_ENVIRONMENT_H

#include "Common.h"
#include "Player.h"
#include <unordered_map>

struct BotEnvironmentState
{
    float lastPosX = 0.0f;
    float lastPosY = 0.0f;
    float lastPosZ = 0.0f;
    uint8 lastHealthPct = 0;
    uint8 lastPowerPct = 0;
    bool lastInCombat = false;
    uint32 lastTargetGuid = 0;
    time_t lastSnapshotTime = 0;
    time_t lastTelemetryTime = 0;
    // RAM-first checkpointing: SQL only sees the environment once per minute
    // (and only when it changed), while the live socket carries every delta.
    time_t lastCheckpointTime = 0;
    time_t lastCapabilityPublishTime = 0;
    time_t lastControlPublishTime = 0;
    uint32 lastPublishedRevision = 0;
    bool checkpointDirty = false;
    std::string lastSentThought; // [MAF-048] Suppress identical thought broadcasts
};

class AzerothFriendEnvironment
{
public:
    static AzerothFriendEnvironment* instance();

    void UpdateBotEnvironment(Player* bot);
    void BroadcastTelemetry(Player* bot, Player* targetPlayer = nullptr);
    std::string BuildLightEnvironmentJson(Player* bot);
    // Sectioned value snapshots for the RAM-first live-state transport.
    void PublishLiveState(Player* bot);
    std::string BuildVitalsJson(Player* bot, uint32 controlRevision) const;
    std::string BuildOwnerJson(Player* bot) const;
    std::string BuildSurroundingsJson(Player* bot) const;
    std::string BuildCapabilitiesJson(Player* bot, uint32 controlRevision) const;

private:
    void SaveEnvironmentCheckpoint(Player* bot, std::string const& envJson, uint8 hpPct,
                                   uint8 pwrPct, bool inCombat, uint32 targetGuid);
    std::unordered_map<uint32, BotEnvironmentState> _botStates;
};

#define sAFEnvironment AzerothFriendEnvironment::instance()

#endif // MOD_AZEROTH_FRIEND_ENVIRONMENT_H
