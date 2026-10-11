#ifndef MOD_AZEROTH_FRIEND_AUTOPILOT_H
#define MOD_AZEROTH_FRIEND_AUTOPILOT_H

#include "ChatCommand.h"
#include "ScriptMgr.h"

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------------------------------------
// Autopilot Subsystem for mod-azeroth-friend:
// Lifecycle enrollment, sweeps, bracket shield, anti-handouts, corpse runs,
// errand execution integration, progress diary telemetry, and in-game commands.
// Zero blocking HTTP/LLM calls: all cognitive planning flows through Python bridge.
// --------------------------------------------------------------------------

void Autopilot_LoadConfig();
void Autopilot_Load();
void Autopilot_Update(uint32_t diff);
void Autopilot_SaveAll();

bool Autopilot_IsActive();
std::vector<std::string> Autopilot_InactiveReasons();

bool Autopilot_MonitorCommand(Player* gm, const std::string& sub, const std::string& name);
std::string Autopilot_ChatContext(Player* bot);
int Autopilot_RequestMode(Player* bot, Player* player);
void Autopilot_NotePlayerRequest(uint64_t botGuid, uint64_t playerGuid, const std::string& what, const std::string& words);
void Autopilot_NoteWhisper(Player* from, Player* to, const std::string& text);

Acore::ChatCommands::ChatCommandTable const& Autopilot_CommandTable();

struct AutopilotMonitorRow
{
    uint64_t    guid  = 0;
    std::string name;
    uint8_t     level = 0;
    uint8_t     cls   = 0;
    std::string zone;
    std::string tier;
    std::string state;
    std::string doing;
};
std::vector<AutopilotMonitorRow> Autopilot_MonitorList();
bool Autopilot_MonitorPage(Player* bot, const std::string& page, std::vector<std::string>& lines);

class AutopilotPlayerScript : public PlayerScript
{
public:
    AutopilotPlayerScript();

    void OnPlayerLogin(Player* player) override;
    void OnPlayerLogout(Player* player) override;
    void OnPlayerCreatureKill(Player* killer, Creature* victim) override;
    void OnPlayerPVPKill(Player* killer, Player* killed) override;
    void OnPlayerJustDied(Player* player) override;
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override;
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override;
    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override;
    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override;
    void OnPlayerDeleteFromDB(CharacterDatabaseTransaction trans, uint32 lowGuid) override;
};

class AutopilotGuildScript : public GuildScript
{
public:
    AutopilotGuildScript();
    void OnAddMember(Guild* guild, Player* player, uint8& plRank) override;
};

class AutopilotServerScript : public ServerScript
{
public:
    AutopilotServerScript();
    void OnPacketSent(WorldSession* session, WorldPacket const& packet) override;
};

#endif // MOD_AZEROTH_FRIEND_AUTOPILOT_H
