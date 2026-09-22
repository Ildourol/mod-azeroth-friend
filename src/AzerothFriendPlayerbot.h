#ifndef MOD_AZEROTH_FRIEND_PLAYERBOT_H
#define MOD_AZEROTH_FRIEND_PLAYERBOT_H

// Single place that decides whether the playerbots integration is compiled in.
// The modules build target puts every playerbots source directory on the include
// path, so these headers resolve whenever mod-playerbots is present.
#if __has_include("Playerbots.h")
    #include "Playerbots.h"
    #include "PlayerbotAI.h"
    #include "PlayerbotMgr.h"
    #include "AiObjectContext.h"
    #include "Action.h"
    #include "Event.h"
    #define AF_HAS_PLAYERBOTS 1
#else
    #define AF_HAS_PLAYERBOTS 0
#endif

#endif // MOD_AZEROTH_FRIEND_PLAYERBOT_H
