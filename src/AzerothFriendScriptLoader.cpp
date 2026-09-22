#include "AzerothFriendChatHook.h"
#include "AzerothFriendCommand.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendLiveState.h"
#include "Log.h"

void Addmod_azeroth_friendScripts()
{
    new AzerothFriendWorldScript();
    new AzerothFriendPlayerScript();
    AzerothFriend::RegisterAzerothFriendCommand();

    LOG_INFO("server.loading", "Registering mod-azeroth-friend scripts.");
}
