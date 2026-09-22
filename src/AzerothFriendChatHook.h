#ifndef MOD_AZEROTH_FRIEND_CHAT_HOOK_H
#define MOD_AZEROTH_FRIEND_CHAT_HOOK_H

#include "ScriptMgr.h"
#include "Player.h"

class AzerothFriendPlayerScript : public PlayerScript
{
public:
    AzerothFriendPlayerScript();

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Player* receiver) override;
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg, Group* group) override;
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 language, std::string& msg) override;
    void OnPlayerEnterCombat(Player* player, Unit* enemy) override;
    void OnPlayerLeaveCombat(Player* player) override;
    void OnPlayerKilledByCreature(Creature* killer, Player* player) override;
    void OnPlayerDuelRequest(Player* target, Player* challenger) override;
    void OnPlayerDuelStart(Player* player1, Player* player2) override;
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override;
    bool OnPlayerCanInitTrade(Player* player, Player* target) override;
    void OnPlayerLogin(Player* player) override;
    void OnPlayerLogout(Player* player) override;
};

class AzerothFriendWorldScript : public WorldScript
{
public:
    AzerothFriendWorldScript();

    void OnAfterConfigLoad(bool reload) override;
    void OnUpdate(uint32 diff) override;
    void OnShutdown() override;
};

#endif // MOD_AZEROTH_FRIEND_CHAT_HOOK_H
