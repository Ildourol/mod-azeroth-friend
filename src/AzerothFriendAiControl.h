#ifndef MOD_AZEROTH_FRIEND_AI_CONTROL_H
#define MOD_AZEROTH_FRIEND_AI_CONTROL_H

#include "Common.h"
#include "Player.h"
#include <string>
#include <vector>

// Owns the "companion lease" over a playerbot's own AI.
//
// Without this, playerbots keeps running its own strategies (follow, rpg, grind,
// travel, random movement) and simply overwrites whatever the companion module
// orders. Acquire() snapshots the bot's strategies, suppresses autonomous
// behaviour, and Release() puts the exact previous strategy set back.
namespace AzerothFriendAiControl
{
    struct LeaseInfo
    {
        bool leased = false;
        time_t expires = 0;
        uint32 durationSeconds = 0;
        std::string mode = "hold";
        std::vector<std::string> nonCombat;
        std::vector<std::string> combat;
        std::vector<std::string> dead;
    };

    // Snapshots strategies and suspends autonomy. Refreshes the lease when the bot
    // is already leased. Returns false (with a reason) when the bot has no AI.
    bool Acquire(Player* bot, uint32 durationSeconds, std::string* reason = nullptr);

    // Applies a long-lived behaviour while keeping the strategy snapshot:
    // "hold" (no autonomy, module drives everything), "follow", "grind", "guard",
    // "travel", "default".
    bool ApplyMode(Player* bot, std::string const& mode, std::string* reason = nullptr);

    // Restores the snapshotted strategies and forgets the lease.
    void Release(Player* bot, std::string const& reason = "", bool retainStrategies = false);

    bool IsLeased(Player* bot);
    bool IsLeasedGuid(uint32 botGuid);
    std::string GetMode(Player* bot);

    // Diagnostics for `.af diag`.
    std::string Describe(Player* bot);

    // Combat Supremacy: Checks if the bot or its linked master is engaged in active combat.
    // When active, external bridge plans yield 100% of actions to native mod-playerbots BOT_STATE_COMBAT.
    bool IsInCombatSupremacy(Player* bot);

    // Elastic Tether: Verifies the bot remains within acceptable range of its master.
    // If master is mounted or distance > maxDistance (default 35y), autonomous leases are canceled.
    bool CheckOwnerTether(Player* bot, float maxDistance = 35.0f);

    // Living Downtime tracker: monitors master idle duration in inns/cities and wilderness.
    // Returns 0 = none, 1 = tavern_rest (in inn/capital city), 2 = campfire_cook (wilderness).
    uint8 GetDowntimeRoutine(Player* bot);
    void NotifyMasterMoved(uint32 masterGuid);

    // Restores strategies for expired leases (called from the action tick).
    void ReleaseExpired();

    // Restores every lease (shutdown / module unload safety net).
    void ReleaseAll();
}

#endif // MOD_AZEROTH_FRIEND_AI_CONTROL_H
