#include "AzerothFriendAutopilot.h"
#include "AzerothFriendBuffs.h"
#include "AzerothFriendChatHook.h"
#include "AzerothFriendCommand.h"
#include "AzerothFriendConfig.h"
#include "AzerothFriendExpression.h"
#include "AzerothFriendLiveState.h"
#include "AzerothFriendMonitor.h"
#include "Log.h"

void Addmod_azeroth_friendScripts()
{
    new AzerothFriendWorldScript();
    new AzerothFriendPlayerScript();
    new AutopilotPlayerScript();
    new AutopilotGuildScript();
    new AutopilotServerScript();
    new AzerothFriendEmoteScript();
    new AzerothFriendMonitorScript();
    AzerothFriend::RegisterAzerothFriendCommand();

    LOG_INFO("server.loading", "Registering mod-azeroth-friend scripts.");
}

