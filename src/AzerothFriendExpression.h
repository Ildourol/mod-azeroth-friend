#ifndef MOD_AZEROTH_FRIEND_EXPRESSION_H
#define MOD_AZEROTH_FRIEND_EXPRESSION_H

#include "ObjectGuid.h"
#include "ScriptMgr.h"
#include <string>
#include <cstdint>

class Player;

// --------------------------------------------------------------------------
// Bot embodiment: facing, gestures, and reactions to inbound emotes.
//
// THREADING: ScheduleBotExpression() and BotPlayTextEmote() mutate world state
// and must only be called from the world/map thread.
// --------------------------------------------------------------------------

uint32_t ExtractEmoteTag(std::string& response);
std::string Expression_BuildGesturePrompt();
uint32_t LookupTextEmoteId(const std::string& name);
std::string LookupTextEmoteName(uint32_t id);
void BotPlayTextEmote(Player* bot, uint32_t textEmoteId, ObjectGuid targetGuid);
void ScheduleBotExpression(Player* bot, ObjectGuid targetGuid, uint32_t textEmoteId, uint32_t delayMs);
bool IsSafeForFacing(Player* bot);
uint32_t GetMirrorEmote(uint32_t inbound);
uint32_t GetCounterEmote(uint32_t inbound);

class AzerothFriendEmoteScript : public PlayerScript
{
public:
    AzerothFriendEmoteScript();
    void OnPlayerTextEmote(Player* player, uint32 textEmote, uint32 emoteNum, ObjectGuid guid) override;
};

#endif // MOD_AZEROTH_FRIEND_EXPRESSION_H
