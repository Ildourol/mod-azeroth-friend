-- ============================================================================
-- AzerothFriendMonitor.lua - Compatibility Shim for AzerothFriend Monitor
-- All monitor capabilities are unified into AzerothFriendMasterFrame in AzerothFriendUI.lua.
-- This file provides backwards-compatibility for external addons, macros, and scripts.
-- ============================================================================

function ToggleAzerothFriendMonitor()
    if AFShowMonitorOverview then AFShowMonitorOverview() end
end
