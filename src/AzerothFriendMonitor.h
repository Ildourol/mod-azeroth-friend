#ifndef MOD_AZEROTH_FRIEND_MONITOR_H
#define MOD_AZEROTH_FRIEND_MONITOR_H

#include "ScriptMgr.h"
#include <cstdint>

// --------------------------------------------------------------------------
// Monitor: server-side communication for the in-game companion HUD & monitor.
// Listens for AFUI / OAPM addon whisper packets and responds with bot rosters,
// status telemetry, and farsight camera viewpoints.
// --------------------------------------------------------------------------

void Monitor_LoadConfig();
void Monitor_Update(uint32_t diff);

class AzerothFriendMonitorScript : public PlayerScript
{
public:
    AzerothFriendMonitorScript();

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override;
    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float x, float y, float z, float orientation,
                                uint32 options, Unit* target) override;
    void OnPlayerLogout(Player* player) override;
};

#endif // MOD_AZEROTH_FRIEND_MONITOR_H
