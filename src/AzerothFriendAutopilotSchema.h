#ifndef MOD_AZEROTH_FRIEND_AUTOPILOT_SCHEMA_H
#define MOD_AZEROTH_FRIEND_AUTOPILOT_SCHEMA_H

// --------------------------------------------------------------------------
// Schema self-heal for mod-azeroth-friend autopilot tables.
//
// At startup, checks that azeroth_friend_autopilot, _snapshots and _events
// exist with all required columns, creates any missing tables, and widens
// columns if needed.
//
// World thread only, at startup (before player logins).
// --------------------------------------------------------------------------

namespace AzerothFriendAutopilotSchema
{
    void EnsureTables();
}

#endif // MOD_AZEROTH_FRIEND_AUTOPILOT_SCHEMA_H
