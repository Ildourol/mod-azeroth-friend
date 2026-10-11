#ifndef MOD_AZEROTH_FRIEND_PROGRESS_H
#define MOD_AZEROTH_FRIEND_PROGRESS_H

#include <cstdint>
#include <string>
#include <vector>

class Field;
class Player;

struct ProgressSnapshot
{
    uint64_t    botGuid        = 0;
    uint32_t    takenAt        = 0;
    uint8_t     level          = 0;
    uint32_t    xp             = 0;
    uint32_t    money          = 0;
    uint16_t    mapId          = 0;
    uint32_t    zoneId         = 0;
    uint32_t    areaId         = 0;
    uint8_t     durabilityPct  = 100;
    uint16_t    freeBagSlots   = 0;
    uint8_t     questsActive   = 0;
    uint32_t    questsRewarded = 0;
    uint32_t    killsTotal     = 0;
    uint32_t    deathsTotal    = 0;
    uint32_t    questsTotal    = 0;
    std::string professions;
};

struct ProgressEvent
{
    uint64_t    botGuid = 0;
    uint32_t    at      = 0;
    std::string type;
    std::string detail;
};

namespace AzerothFriendProgress
{
    ProgressSnapshot Capture(Player* bot);
    uint8_t DurabilityPct(Player* bot);
    const char* SnapshotColumns();
    ProgressSnapshot ReadSnapshot(Field* fields, uint64_t botGuid);
    void QueueSnapshot(ProgressSnapshot snapshot);
    void QueueEvent(uint64_t botGuid, std::string type, std::string detail);
    void Forget(uint64_t botGuid);
    void Flush(uint32_t maxSnapshotsPerBot, uint32_t maxEventsPerBot);
    std::vector<ProgressEvent> LoadEvents(uint64_t botGuid, uint32_t limit);
    std::vector<ProgressSnapshot> LoadSnapshots(uint64_t botGuid, uint32_t sinceUnix, uint32_t limit);
    std::string DescribeProfessions(const std::string& compact);
    std::string Summarize(const std::vector<ProgressSnapshot>& oldestFirst);
    std::string ZoneName(uint32_t zoneId);
}

#endif // MOD_AZEROTH_FRIEND_PROGRESS_H
