#ifndef MOD_AZEROTH_FRIEND_WORLD_INDEX_H
#define MOD_AZEROTH_FRIEND_WORLD_INDEX_H

#include <cstdint>
#include <string>
#include <vector>

class Player;

enum class AutopilotService : uint8_t
{
    Repair,
    Vendor,
    Trainer,        // class trainer usable by this bot
    Profession,     // profession trainer
    Inn,
    FlightMaster,
    Bank,
    Auction,
    Count
};

const char* AutopilotService_Name(AutopilotService s);
bool        AutopilotService_FromName(const std::string& name, AutopilotService& out);

struct AutopilotPlace
{
    uint32_t    map   = 0;
    float       x     = 0.0f;
    float       y     = 0.0f;
    float       z     = 0.0f;
    uint32_t    entry = 0;
    std::string name;
    float       distance = 0.0f;
};

namespace AzerothFriendWorldIndex
{
    void Build();
    bool NearestService(Player* bot, AutopilotService service, AutopilotPlace& out);
    uint32_t ProfessionSkill(const std::string& name, std::string& display);
    std::string ProfessionNames();
    bool NearestProfessionTrainer(Player* bot, uint32_t skillId, AutopilotPlace& out);
    bool ZonePlace(Player* bot, uint32_t zoneId, AutopilotPlace& out, std::string& why);
    bool NearestSpawn(Player* bot, uint32_t entry, AutopilotPlace& out);
    bool SpawnBeyond(Player* bot, const std::vector<uint32_t>& creatures,
                     const std::vector<uint32_t>& objects, float minDistance, AutopilotPlace& out);
    const std::vector<uint32_t>& CreaturesDropping(uint32_t item);
    const std::vector<uint32_t>& ObjectsHolding(uint32_t item);
    bool NearestSpellFocus(Player* bot, uint32_t focusId, AutopilotPlace& out);
    std::string SpellFocusName(uint32_t focusId);
    bool NearestMailbox(Player* bot, AutopilotPlace& out);
    bool FindNpc(Player* bot, const std::string& name, AutopilotPlace& out);
    bool PreyNear(Player* bot, float range, AutopilotPlace& out);
    bool HuntingGround(Player* bot, AutopilotPlace& out);
    const std::vector<uint32_t>& QuestEnders(uint32_t questId);
    uint32_t FindZone(const std::string& name, std::string& display);
    std::string DescribeServices(Player* bot);
    std::string ZonesForLevel(Player* bot);
    std::string QuestsOnOffer(Player* bot, float range);
    const std::vector<uint32_t>& CreditedBy(uint32_t entry);
}

#endif // MOD_AZEROTH_FRIEND_WORLD_INDEX_H
