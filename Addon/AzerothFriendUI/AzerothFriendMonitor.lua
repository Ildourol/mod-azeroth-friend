-- ============================================================================
-- AzerothFriendMonitor.lua - Compatibility Shim for AzerothFriend Monitor
-- All monitor capabilities are unified into AzerothFriendMasterFrame in AzerothFriendUI.lua.
-- This file provides backwards-compatibility for external addons, macros, and scripts.
-- ============================================================================

function ToggleAzerothFriendMonitor()
    if AzerothFriendMasterFrame and AzerothFriendMasterFrame:IsShown() and GetSettings and GetSettings().activeTab == 1 then
        AzerothFriendMasterFrame:Hide()
    else
        if GetSettings then GetSettings().activeTab = 1 end
        if AzerothFriendMasterFrame then AzerothFriendMasterFrame:Show() end
        if RefreshMasterTab then RefreshMasterTab() end
    end
end
