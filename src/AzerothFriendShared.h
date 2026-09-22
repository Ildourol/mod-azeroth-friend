#ifndef MOD_AZEROTH_FRIEND_SHARED_H
#define MOD_AZEROTH_FRIEND_SHARED_H

#include "Common.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "Creature.h"
#include "GameObject.h"
#include <string>
#include <string_view>

namespace AzerothFriendShared
{
    bool EqualCaseInsensitive(std::string_view a, std::string_view b);
    bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle);
    bool CanControl(Player* issuer, Player* bot);
    void InvalidateControl(Player* bot);
    std::string EscapeJsonString(std::string const& input);
    std::string EscapeSqlString(std::string const& input);
    Creature* FindCreatureByCounter(Map* map, uint32 lowGuid);
    GameObject* FindGameObjectByCounter(Map* map, uint32 lowGuid);
    Player* FindPlayerByCounter(uint32 lowGuid);
    void QueueEvent(uint32 botGuid, std::string const& eventType, uint8 priority,
                    uint32 sourceGuid, std::string const& sourceName, std::string const& payloadJson);
    void SendTelemetry(Player* bot, std::string const& tag, std::string const& payload, Player* targetPlayer = nullptr);
    void SendAddonError(Player* recipient, std::string const& errorMsg, std::string const& botName = "");
}

#endif // MOD_AZEROTH_FRIEND_SHARED_H
