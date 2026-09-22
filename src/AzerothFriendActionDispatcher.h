#ifndef MOD_AZEROTH_FRIEND_ACTION_DISPATCHER_H
#define MOD_AZEROTH_FRIEND_ACTION_DISPATCHER_H

#include "Common.h"
#include "Player.h"
#include <unordered_map>
#include <unordered_set>
#include <string>

enum class AFStepKind : uint8
{
    Immediate = 0,
    Move = 1,          // walk to coordinates
    Approach = 2,      // walk to an entity, then run an optional follow-up action
    TargetDead = 3,    // wait until the current target dies
    Wait = 4           // hold position for N seconds
};

struct BotActiveAction
{
    uint64 actionId = 0;
    std::string planId;
    uint8 stepIndex = 0;
    uint8 totalSteps = 1;
    std::string actionType;
    std::string paramsJson;
    AFStepKind kind = AFStepKind::Immediate;
    std::string followUpAction;
    std::string followUpParam;
    uint32 targetGuid = 0;
    std::string targetName;
    float targetX = 0.0f;
    float targetY = 0.0f;
    float targetZ = 0.0f;
    float targetRange = 2.0f;
    time_t startTime = 0;
    time_t deadline = 0;
    time_t lastProgressCheck = 0;
    float lastCheckX = 0.0f;
    float lastCheckY = 0.0f;
    uint8 attempts = 0;
    bool handedOff = false;   // long-lived mode action (follow/grind/...) took over
    bool verified = false;    // true only when a postcondition was observed
};

// One queue row claimed asynchronously from azeroth_friend_actions. Holds values
// only, so it survives being handed from the DB worker callback to the world
// thread without touching any engine object.
struct BotClaimedStep
{
    uint64 actionId = 0;
    uint32 botGuid = 0;
    std::string planId;
    uint8 stepIndex = 0;
    uint8 totalSteps = 1;
    std::string actionType;
    std::string paramsJson;
    std::string thought;
    uint32 controlRevision = 0;
    std::string authority = "autonomous";
    uint64 originEventId = 0;
    uint32 originSourceGuid = 0;
};

class AzerothFriendActionDispatcher
{
public:
    static AzerothFriendActionDispatcher* instance();

    // Schema self-heal, stale step recovery and catalogue export.
    void Initialize();
    void Shutdown();

    void Update(uint32 diff);
    void InterruptBotPlan(uint32 botGuid, std::string const& reason, bool emitEvent = true);

    // Used by `.af run` so a GM can fire a single catalogue action immediately.
    bool ExecuteManualAction(Player* bot, std::string const& actionType, std::string const& paramsJson,
                            std::string& outMessage);

    // Writes the live playerbots action/strategy catalogue for a bot into
    // azeroth_friend_action_catalog and returns a short summary.
    std::string ExportCatalog(Player* bot);

    // Unreachable corpse cooldown filter (MAF-051)
    bool IsCorpseUnreachable(uint32 corpseGuid) const;
    void MarkCorpseUnreachable(uint32 corpseGuid, uint32 cooldownSeconds = 60);

private:
    struct StepPlan
    {
        AFStepKind kind = AFStepKind::Immediate;
        std::string followUpAction;
        std::string followUpParam;
        uint32 targetGuid = 0;
        std::string targetName;
        float targetX = 0.0f;
        float targetY = 0.0f;
        float targetZ = 0.0f;
        float targetRange = 2.0f;
        uint32 waitSeconds = 0;
        std::string result;
        bool verified = false;
    };

    void ProcessBotActions(Player* bot, std::string const& botMode);
    // Executes a step whose ownership was claimed asynchronously from the queue.
    void StartClaimedStep(Player* bot, BotClaimedStep const& claimed);
    // Schedules an asynchronous claim of the next pending step (no world-thread SQL).
    void RequestNextStep(uint32 botGuid);
    // Legacy synchronous path, used only when the RAM-first service is disabled.
    void ProcessBotActionsLegacy(Player* bot, std::string const& botMode);
    void RefreshDispatcherCaches(bool force = false);
    void AttendActiveStep(Player* bot, BotActiveAction& action);
    void CheckEmergencyReflexes(Player* bot);
    void EmitAutonomyTick(Player* bot, std::string const& botMode);
    void RunStuckRecovery(Player* bot, BotActiveAction const& action);
    bool TryBuildStep(Player* bot, std::string const& actionType, std::string const& paramsJson,
                      StepPlan& plan, std::string& error);
    void CompleteStep(Player* bot, BotActiveAction const& action, std::string const& result,
                      bool success, std::string const& error);
    void CancelRemainingSteps(Player* bot, std::string const& planId, std::string const& reason);
    void PublishStepTelemetry(Player* bot, BotActiveAction const& action, std::string const& status,
                              std::string const& result, std::string const& error);

    uint32 _updateTimer = 0;
    bool _initialized = false;
    bool _schemaCheckInFlight = false;
    std::unordered_map<uint32, BotActiveAction> _activeActions;
    std::unordered_map<uint32, time_t> _lastAutonomyTick;
    std::unordered_map<uint32, time_t> _lastHealthAlert;
    std::unordered_set<uint32> _catalogExported;
    // Asynchronous step claiming: the queue row is fetched on the DB worker and
    // turned into an active action on the world thread.
    std::unordered_map<uint32, BotClaimedStep> _claimedSteps;
    std::unordered_set<uint32> _claimInFlight;
    std::unordered_map<uint32, uint32> _pendingEventCount;
    std::unordered_map<uint32, time_t> _unreachableCorpses;
    time_t _lastCacheRefresh = 0;
};

#define sAFDispatcher AzerothFriendActionDispatcher::instance()

#endif // MOD_AZEROTH_FRIEND_ACTION_DISPATCHER_H
