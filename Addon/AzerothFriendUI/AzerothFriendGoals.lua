-- Goal controls and BotBuddy-inspired command/player inspectors.
local state = {status='Unknown', goal='', progress='', result='', history='', players=''}
local panel, body, goalEdit, longGoalEdit, autonomyButton

-- Shared with AzerothFriendUI.lua so the HUD autonomy switch reflects server
-- telemetry instead of guessing what the companion is doing.
AzerothFriendGoalState = state
function AFGetGoalState() return state end

local function settings()
    AzerothFriendUIDB = AzerothFriendUIDB or {}
    local key = (GetRealmName() or '') .. ':' .. (UnitName('player') or '')
    AzerothFriendUIDB.goalPanels = AzerothFriendUIDB.goalPanels or {}
    AzerothFriendUIDB.goalPanels[key] = AzerothFriendUIDB.goalPanels[key] or {}
    return AzerothFriendUIDB.goalPanels[key]
end
local function refresh()
    if not body then return end
    body:SetText('Companion: ' .. (state.bot or 'unavailable') .. '\nAutonomy: ' ..
        (state.enabled and 'ON' or 'OFF') .. ' | Goal: ' .. (state.status or 'Unknown') ..
        '\n\nCore purpose (long-term):\n' .. (state.long_goal or 'not set') ..
        '\n\n' .. (state.goal or '') .. '\n' .. (state.progress or '') .. '\n' .. (state.result or '') ..
        '\n\nLast five actions (dispatch is not completion):\n' .. (state.history or '') ..
        '\nNearby players:\n' .. (state.players or ''))
    body:SetHeight(math.max(300, body:GetStringHeight() + 20))
    body:GetParent():SetHeight(body:GetHeight())
    if autonomyButton then
        autonomyButton:SetText(state.enabled and 'Disable' or 'Enable')
    end
    -- Mirror server state into the editors without fighting the owner's typing.
    if goalEdit and not goalEdit:HasFocus() and (state.goal or '') ~= '' then
        goalEdit:SetText(state.goal)
    end
    if longGoalEdit and not longGoalEdit:HasFocus() and (state.long_goal or '') ~= '' then
        longGoalEdit:SetText(state.long_goal)
    end
end
function AFProcessExtension(message)
    local tag, data = message:match('^%[(FRIEND_%u+)%]%s*(.*)$')
    if tag == 'FRIEND_GOAL' then
        local decoded = AFDecodeObject(data)
        if not decoded then return true end
        for k,v in pairs(decoded) do state[k] = v end
    elseif tag == 'FRIEND_HISTORY' then state.history = data
    elseif tag == 'FRIEND_PLAYERS' then state.players = data
    else return false end
    refresh()
    return true
end
local function send(command)
    SendChatMessage('.af ' .. command, 'SAY')
end
local function create()
    if panel then return end
    panel = CreateFrame('Frame', 'AzerothFriendGoalPanel', UIParent)
    panel:SetWidth(600); panel:SetHeight(500); panel:SetPoint('CENTER')
    panel:SetMovable(true); panel:EnableMouse(true); panel:SetResizable(true)
    panel:SetMinResize(600, 350)
    panel:SetBackdrop({bgFile='Interface\\DialogFrame\\UI-DialogBox-Background', edgeFile='Interface\\Tooltips\\UI-Tooltip-Border', tile=true, tileSize=16, edgeSize=16, insets={left=4,right=4,top=4,bottom=4}})
    panel:RegisterForDrag('LeftButton')
    panel:SetScript('OnDragStart', function(self) self:StartMoving() end)
    local function save()
        panel:StopMovingOrSizing()
        local s = settings(); local point, _, relativePoint, x, y = panel:GetPoint()
        s.point, s.relativePoint, s.x, s.y = point, relativePoint, x, y
        s.width, s.height = panel:GetWidth(), panel:GetHeight()
    end
    panel:SetScript('OnDragStop', save)
    local title = panel:CreateFontString(nil, 'OVERLAY', 'GameFontNormalLarge')
    title:SetPoint('TOPLEFT', 16, -16); title:SetText('AzerothFriend: Goal and command history')
    local close = CreateFrame('Button', nil, panel, 'UIPanelCloseButton'); close:SetPoint('TOPRIGHT')
    close:SetScript('OnClick', function() settings().visible=false; panel:Hide() end)
    goalEdit = CreateFrame('EditBox', nil, panel, 'InputBoxTemplate')
    goalEdit:SetHeight(24); goalEdit:SetPoint('TOPLEFT', 20, -50); goalEdit:SetPoint('TOPRIGHT', -20, -50)
    goalEdit:SetAutoFocus(false); goalEdit:SetMaxLetters(255)
    goalEdit:SetScript('OnEscapePressed', function(self) self:ClearFocus() end)
    longGoalEdit = CreateFrame('EditBox', nil, panel, 'InputBoxTemplate')
    longGoalEdit:SetHeight(24); longGoalEdit:SetPoint('TOPLEFT', 20, -78); longGoalEdit:SetPoint('TOPRIGHT', -20, -78)
    longGoalEdit:SetAutoFocus(false); longGoalEdit:SetMaxLetters(2000)
    longGoalEdit:SetScript('OnEscapePressed', function(self) self:ClearFocus() end)
    local controls = {{'Set goal', function() send('goal set ' .. goalEdit:GetText()); goalEdit:ClearFocus() end},
        {'Set purpose', function() send('goal longterm ' .. longGoalEdit:GetText()); longGoalEdit:ClearFocus() end},
        {'Enable', function() send(state.enabled and 'autonomy off' or 'autonomy on') end},
        {'Pause', function() send('goal pause') end},
        {'Resume', function() send('goal resume') end}, {'Complete', function() send('goal complete') end},
        {'Clear', function() send('goal clear') end}, {'Sync', function() send('inspect') end}}
    for i, control in ipairs(controls) do
        local button = CreateFrame('Button', nil, panel, 'UIPanelButtonTemplate')
        button:SetWidth(68); button:SetHeight(24); button:SetPoint('TOPLEFT', 14+(i-1)*72, -112)
        button:SetText(control[1]); button:SetScript('OnClick', control[2])
        if i == 3 then autonomyButton = button end
    end
    local scroll = CreateFrame('ScrollFrame', 'AzerothFriendGoalScroll', panel, 'UIPanelScrollFrameTemplate')
    scroll:SetPoint('TOPLEFT', 18, -146); scroll:SetPoint('BOTTOMRIGHT', -32, 20)
    local child = CreateFrame('Frame', nil, scroll); child:SetWidth(540); child:SetHeight(1000)
    body = child:CreateFontString(nil, 'OVERLAY', 'GameFontHighlightSmall')
    body:SetPoint('TOPLEFT'); body:SetWidth(530); body:SetJustifyH('LEFT'); body:SetJustifyV('TOP')
    scroll:SetScrollChild(child)
    panel:SetScript('OnSizeChanged', function(self, width)
        child:SetWidth(width-60); body:SetWidth(width-65); refresh(); child:SetHeight(body:GetHeight())
    end)
    local grip = CreateFrame('Button', nil, panel)
    grip:SetWidth(20); grip:SetHeight(20); grip:SetPoint('BOTTOMRIGHT')
    grip:SetNormalTexture('Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up')
    grip:SetScript('OnMouseDown', function() panel:StartSizing('BOTTOMRIGHT') end)
    grip:SetScript('OnMouseUp', save)
    local s = settings()
    if s.point then panel:ClearAllPoints(); panel:SetPoint(s.point, UIParent, s.relativePoint, s.x, s.y) end
    if s.width then panel:SetWidth(s.width); panel:SetHeight(s.height) end
    panel:Hide(); refresh()
end
function AFHandleSlash(input)
    local command, rest = input:match('^%s*(%S+)%s*(.-)%s*$')
    command = command and command:lower() or ''
    if command == 'goals' then
        create(); if panel:IsShown() then panel:Hide() else panel:Show(); send('inspect') end
        settings().visible=panel:IsShown(); return true
    end
    if command == 'goal' or command == 'autonomy' or command == 'cast' then
        send(command .. ' ' .. rest); return true
    end
    return false
end
local events = CreateFrame('Frame')
events:RegisterEvent('PLAYER_ENTERING_WORLD')
events:SetScript('OnEvent', function() create(); if settings().visible then panel:Show() end end)
