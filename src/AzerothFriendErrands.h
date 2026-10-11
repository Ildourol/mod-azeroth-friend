#ifndef MOD_AZEROTH_FRIEND_ERRANDS_H
#define MOD_AZEROTH_FRIEND_ERRANDS_H

#include "AzerothFriendPlayerbot.h"
#include "AzerothFriendTravel.h"
#include <cstdint>
#include <string>
#include <vector>

class Player;

enum class AutopilotErrandKind : uint8_t
{
    Place,
    Service,
    QuestTurnIn,
    QuestObjective,
    Craft,
    Open,
    Mailbox,
    Npc
};

struct AutopilotErrand
{
    bool                active    = false;
    AutopilotErrandKind kind      = AutopilotErrandKind::Place;
    uint8_t             service   = 0;
    uint32_t            npcEntry  = 0;
    uint32_t            questId   = 0;
    uint8_t             rewardChoice = 0;
    std::string         label;
    uint32_t            startedAt = 0;
    AutopilotTrip       trip;

    bool                hunting   = false;
    uint32_t            huntUntil = 0;
    uint32_t            lastUseAt = 0;

    uint32_t            craftSpell = 0;
    uint32_t            craftLeft  = 0;
    uint32_t            craftDone  = 0;
    uint32_t            craftItem  = 0;
    uint32_t            craftHad   = 0;
    uint32_t            craftMisses = 0;
    bool                craftCasting = false;
    uint64_t            objectGuid = 0;
    uint32_t            objectTries = 0;

    bool                exploring    = false;
    uint8_t             exploreLeft  = 0;
    float               exploreAngle = 0.0f;
    std::vector<uint64_t> objectsGivenUp;
    std::vector<uint64_t> unreachable;
    uint8_t               talkTries = 0;
};

struct AutopilotErrandUpdate
{
    std::string note;
    bool        finished = false;
};

namespace AzerothFriendErrands
{
    void LoadConfig();
    const std::string& Reference();
    std::string Run(Player* bot, PlayerbotAI* ai, const std::string& command,
                    AutopilotErrand& errand, uint32_t now);
    bool IsStrategyChange(const std::string& command);
    bool IsErrand(const std::string& command);
    bool IsDenied(const std::string& command);
    std::string Normalize(const std::string& raw);
    AutopilotErrandUpdate UpdateErrand(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand,
                                       uint32_t now);
    std::string DescribeCraftable(Player* bot);
    std::string DescribeSurroundings(Player* bot, bool forChat = false);
    void HuntUnderAttack(Player* bot, PlayerbotAI* ai, AutopilotErrand& errand);
    void StopErrand(PlayerbotAI* ai, AutopilotErrand& errand);
}

#endif // MOD_AZEROTH_FRIEND_ERRANDS_H
