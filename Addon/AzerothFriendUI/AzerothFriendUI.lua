-- ============================================================================
-- AzerothFriendUI - Real-time Autonomous Bot HUD for WoW 3.3.5 (AzerothCore)
-- Features: Zero-Token Context Inspector, Mindset/Latency Monitor,
--           Token Synergy Telemetry, Remote Control Bar, and Minimap Icon.
-- ============================================================================

local ADDON_NAME = "AzerothFriendUI"
local ADDON_PREFIX = "AZEROTH_FRIEND"
local NativeSendChatMessage = SendChatMessage
local function SendChatMessage(message, channel, language, target)
    -- One authoritative route per click. Legacy paired whispers are redundant.
    if channel == "WHISPER" then return end
    if not message or not message:match("^%.af ") then return end
    NativeSendChatMessage(message, "SAY")
end

-- Color formatting constants
local COLORS = {
    HEADER    = "|cFFFFD700", -- Gold
    GOLD      = "|cFFFFD700", -- Explicit alias used by focus/token highlights
    SUBHEADER = "|cFF00BFFF", -- Deep Sky Blue
    LABEL     = "|cFF66D9EF", -- Light Cyan
    VALUE     = "|cFFFFFFFF", -- Pure White
    SUCCESS   = "|cFF55FF55", -- Soft Green
    WARNING   = "|cFFFFFF00", -- Yellow
    ALERT     = "|cFFFF4444", -- Bright Red
    MUTED     = "|cFF999999", -- Gray
    ORANGE    = "|cFFFFA500", -- Orange
    PURPLE    = "|cFFDA70D6", -- Orchid Purple
    CYAN      = "|cFF00FFFF", -- Cyan
    RESET     = "|r"
}

-- Default Configuration
local DEFAULT_SETTINGS = {
    showMaster = true,
    activeTab = 1, -- 1: Context, 2: Mindset/Debug, 3: Actions, 4: Thoughts, 5: Bot API
    layoutMode = "tabbed", -- "tabbed" or "floating"
    showMinimap = true,
    minimapPos = 45, -- angle in degrees
    showState = false,
    showThought = false,
    showActions = false,
    showEnv = false,
    showMemory = false,
    filterChat = true,
    locked = false,
    frames = {}
}

-- Live Companion Bot Cache
local BotCache = {
    name = "Friendbot",
    race = "Orc",
    class = "Rogue",
    level = 40,
    health = "100%",
    power = "100%",
    zone = "Unknown",
    subzone = "Unknown",
    coords = "0, 0, 0",
    combat = "NOT IN COMBAT",
    target = "None",
    -- Context
    ilvl = 0,
    freeBags = 0,
    totalBags = 0,
    waterCount = 0,
    foodCount = 0,
    potionsCount = 0,
    activeQuests = {},
    combatSpells = {},
    combatStrategies = {},
    nonCombatStrategies = {},
    outdoors = "Yes",
    inInn = "No",
    resting = "No",
    swimming = "No",
    mounted = "No",
    masterName = "None",
    masterDist = 0,
    masterHP = 100,
    masterCombat = "Out of Combat",
    masterTarget = "None",
    masterTargetHP = 0,
    masterTargetEnemy = "None",
    masterCasting = "None",
    -- Mindset & Debug
    mindset = "IDLE",
    actionMode = "travel",
    claimed = "RELEASED",
    bridgeEnabled = true,
    commitmentWindow = "15s",
    commitmentRemaining = 0,
    tokensSaved = 0,
    coProcessed = 0,
    sensoryReused = 0,
    -- RAM-first diagnostics (server-broadcast values, never computed locally)
    transport = "UNKNOWN",
    transportSession = "",
    transportFrames = 0,
    transportRefreshes = 0,
    cacheAge = 0,
    contextTokens = 0,
    contextCeiling = 0,
    ramBytes = 0,
    ramCachedBots = 0,
    summaryAge = 0,
    rolloutStage = 0,
    sqlCompat = "OFF",
    diagNote = "",
    lastActionError = "",
    lastActionErrorTime = "",
    errorCount = 0,
    lastEmoteTimes = {},
    activeAction = "None",
    actionStatus = "IDLE",
    actionStep = "0 of 1",
    movementTarget = "None",
    nextAction = "None",
    actionParams = "{}",
    actionResult = "",
    actionError = "",
    actionVerified = "pending",
    actionAuthority = "unknown",
    actionBinding = "unknown",
    actionNativeBinding = "none",
    actionCompletion = "unknown",
    -- Playerbot Semantic Bot API v1 (all values are server-broadcast)
    apiVersion = 0,
    capabilityRevision = 0,
    apiExecution = "UNKNOWN",
    apiOwnerTier = "UNKNOWN",
    apiRawPassthrough = "UNKNOWN",
    apiNativeDiscovery = "UNKNOWN",
    apiCuratedCount = 0,
    apiAutonomousCount = 0,
    apiOwnerCount = 0,
    apiGmCount = 0,
    apiCatalog = {},
    apiCatalogLoaded = false,
    apiCatalogFilter = "all",
    apiCatalogTruncated = false,
    apiContract = {},
    apiError = "",
    catalogSilenceUntil = 0,
    -- Thoughts & Memory
    thought = "Awaiting companion guidance...",
    speech = "",
    deepReasoning = "",
    model = "Default",
    tokenUsage = "None",
    lastTokens = { prompt = 0, completion = 0, reasoning = 0, total = 0 },
    sessionTokens = 0,
    sessionCalls = 0,
    activityLevel = "NORMAL",
    activityReason = "Normal activity rate",
    callTimestamps = {},
    errorHistory = {},
    planId = "None",
    triggerEvent = "None",
    memories = {},
    heardLog = {},
    thoughtHistory = {},
    surroundings = {},
}

-- Forward declarations used by telemetry handlers and UI callbacks.
local RefreshMasterTab
local RequestRefresh
local HandleInterceptedActionError
local RefreshBotApiPanel

-- Real-time dialogue tracker (Master, NPCs, other bots)

-- MultiBotBridge / AI addons talk in machine tokens over public chat. Those frames
-- are transport, not conversation, and must never reach the thoughts feed.
local PROTOCOL_TOKENS = {
    "mbot", "aio\t", "caps-", "caps_", "hello-aok", "roster:", "pong ", "dialog "
}

local function IsProtocolChat(text, sender)
    local who = (sender or ""):lower()
    if who:match("^%s*mbot") or who:match("^%s*aio") then return true end
    local body = (text or ""):lower()
    for _, token in ipairs(PROTOCOL_TOKENS) do
        if body:find(token, 1, true) then return true end
    end
    return false
end

local BOT_COMMAND_ACKS = {
    ["following"] = true,
    ["staying"] = true,
    ["holding"] = true,
    ["fleeing"] = true,
    ["attacking"] = true,
    ["drinking"] = true,
    ["eating"] = true,
    ["looting"] = true,
    ["casting"] = true,
    ["moving to"] = true,
    ["resting"] = true,
    ["ready"] = true,
    ["waiting"] = true,
}

local function IsBotAckText(text)
    if not text then return false end
    local lower = text:lower():gsub("^%s+", ""):gsub("%s+$", "")
    lower = lower:gsub("[%p%s]+$", "")
    if BOT_COMMAND_ACKS[lower] then return true end
    for ack, _ in pairs(BOT_COMMAND_ACKS) do
        if lower:find("^" .. ack) then
            return true
        end
    end
    return false
end

local recentChatTimestamps = {}
local function RecordChat(sender, channel, text)
    if not text or text == "" then return end
    sender = sender or "Unknown"
    local cleanSender = sender:match("^([^%-]+)") or sender

    -- Real chat lines are capped at 255 characters by the client, a speaker name at
    -- 12. Anything larger, or packed with separators/quotes/newlines, is another
    -- addon's data payload (localisation tables, protocol frames) - not speech the
    -- companion should be shown as having overheard.
    if #cleanSender > 24 then return end
    if #text > 255 then return end
    if text:find("\n") then return end

    -- Normalize text for deduplication
    local cleanText = text:gsub("^%s+", ""):gsub("%s+$", "")
    -- Extract underlying message if prefixed like "[PARTY] Master: text"
    local rawText = cleanText:match("^%[[^%]]+%]%s*[^:]+:%s*(.+)$") or cleanText

    local now = GetTime()
    -- Expire rolling window cache older than 15 seconds
    for k, t in pairs(recentChatTimestamps) do
        if (now - t) > 15 then
            recentChatTimestamps[k] = nil
        end
    end

    local dedupKey = (channel or "say"):lower() .. ":" .. cleanSender:lower() .. ":" .. rawText:lower()
    if recentChatTimestamps[dedupKey] and (now - recentChatTimestamps[dedupKey]) < 1.0 then
        return
    end
    recentChatTimestamps[dedupKey] = now

    if IsProtocolChat(cleanText, cleanSender) then
        return
    end

    if HandleInterceptedActionError and (HandleInterceptedActionError(cleanText) or HandleInterceptedActionError(rawText)) then
        return
    end

    local isAck = false
    local isBotSender = (BotCache.name and cleanSender:lower() == BotCache.name:lower())
    if isBotSender or IsBotAckText(rawText) then
        if IsBotAckText(rawText) then
            isAck = true
            channel = "ack"
        end
    end

    local timestamp = date("%H:%M:%S")
    table.insert(BotCache.heardLog, {
        time = timestamp,
        sender = cleanSender,
        channel = channel or "say",
        text = cleanText,
        isAck = isAck
    })
    if #BotCache.heardLog > 16 then
        table.remove(BotCache.heardLog, 1)
    end
    if RequestRefresh then
        RequestRefresh()
    end
end

-- Get character specific settings from SavedVariables
local function GetSettings()
    local playerName = UnitName("player") or "Default"
    if not AzerothFriendUIDB then
        AzerothFriendUIDB = {}
    end
    if not AzerothFriendUIDB.characters then
        AzerothFriendUIDB.characters = {}
    end
    if not AzerothFriendUIDB.characters[playerName] then
        AzerothFriendUIDB.characters[playerName] = CopyTable(DEFAULT_SETTINGS)
    end
    return AzerothFriendUIDB.characters[playerName]
end

-- Save frame position and dimensions
local function SaveFramePosition(frame)
    if not frame or not frame:GetName() then return end
    local settings = GetSettings()
    local point, _, relPoint, x, y = frame:GetPoint()
    local width, height = frame:GetWidth(), frame:GetHeight()
    settings.frames[frame:GetName()] = {
        point = point,
        relPoint = relPoint,
        x = x,
        y = y,
        width = width,
        height = height
    }
end

-- Restore frame position and dimensions
local function RestoreFramePosition(frame)
    if not frame or not frame:GetName() then return end
    local settings = GetSettings()
    local pos = settings.frames[frame:GetName()]
    if pos then
        frame:ClearAllPoints()
        frame:SetPoint(pos.point, UIParent, pos.relPoint, pos.x, pos.y)
        if pos.width and pos.height then
            frame:SetSize(pos.width, pos.height)
        end
    end
end

-- Safe FontString or Frame text setter with dynamic scrolling height calculation
local function SetContainerText(container, text)
    if not container then return end
    text = text or ""
    if container.__afLastText == text then return end
    container.__afLastText = text

    local name = container.GetName and container:GetName() or ""
    local fs = (name ~= "" and _G[name .. "Text"]) or (container.GetFontString and container:GetFontString()) or container.text

    if fs and fs.SetText then
        fs:SetText(text)
        local contentHeight = (fs.GetStringHeight and fs:GetStringHeight()) or 0
        local parent = container:GetParent()
        local minHeight = (parent and parent.GetHeight and parent:GetHeight()) or 330
        if container.SetHeight then
            container:SetHeight(math.max(contentHeight + 24, minHeight))
        end
        if parent and parent.UpdateScrollChildRect then
            parent:UpdateScrollChildRect()
        end
        return
    end

    if container.SetText then
        container:SetText(text)
    end
    if container.GetContentHeight then
        local contentHeight = container:GetContentHeight() or 0
        local parent = container:GetParent()
        local minHeight = (parent and parent.GetHeight and parent:GetHeight()) or 330
        if container.SetHeight then
            container:SetHeight(math.max(contentHeight + 24, minHeight))
        end
        if parent and parent.UpdateScrollChildRect then
            parent:UpdateScrollChildRect()
        end
    end
end

-- Multiline unflattening (converts "|" delimiters into real lines)
local function UnflattenLines(text)
    if not text then return {} end
    local lines = {}
    for line in string.gmatch(text, "([^|]+)") do
        local trimmed = line:gsub("^%s+", ""):gsub("%s+$", "")
        if trimmed ~= "" then
            table.insert(lines, trimmed)
        end
    end
    return lines
end

local function Trim(text)
    return tostring(text or ""):gsub("^%s+", ""):gsub("%s+$", "")
end

local function RequestBotApiCatalog()
    BotCache.catalogSilenceUntil = GetTime() + 6
    local botName = BotCache.name
    if not botName or botName == "" then
        local targetName = UnitName("target")
        if targetName and UnitIsPlayer("target") and not UnitIsUnit("target", "player") then
            botName = targetName
            BotCache.name = targetName
        end
    end
    if botName and botName ~= "" then
        SendChatMessage(".af catalog all " .. botName)
    else
        SendChatMessage(".af catalog all")
    end
end

-- ----------------------------------------------------------------------------
-- Content Formatters
-- ----------------------------------------------------------------------------

-- 1. Format Context & Surroundings (Zero Extra Tokens)
local function FormatContextText()
    local out = {}
    table.insert(out, COLORS.HEADER .. "Bot Character Self-Context & Surroundings Radar" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "Extracted natively from C++ memory (0 LLM Tokens)" .. COLORS.RESET)
    table.insert(out, COLORS.MUTED .. string.rep("-", 46) .. COLORS.RESET)

    -- Bot Profile
    table.insert(out, COLORS.LABEL .. "Companion: " .. COLORS.RESET .. COLORS.SUCCESS .. BotCache.name .. COLORS.RESET ..
                 COLORS.MUTED .. " | Level " .. COLORS.WARNING .. BotCache.level .. " " .. BotCache.race .. " " .. BotCache.class .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Health: " .. COLORS.RESET .. COLORS.VALUE .. BotCache.health .. COLORS.RESET ..
                 COLORS.MUTED .. " | Power: " .. COLORS.RESET .. COLORS.VALUE .. BotCache.power .. COLORS.RESET)

    -- Companion Master Tracking (Primary Focus)
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Companion Master Status (Primary Focus):" .. COLORS.RESET)
    if BotCache.masterName and BotCache.masterName ~= "None" then
        local distColor = BotCache.masterDist < 10 and COLORS.SUCCESS or (BotCache.masterDist < 30 and COLORS.WARNING or COLORS.ALERT)
        table.insert(out, COLORS.LABEL .. "Master: " .. COLORS.RESET .. COLORS.PURPLE .. BotCache.masterName .. COLORS.RESET ..
                     COLORS.LABEL .. " | Distance: " .. COLORS.RESET .. distColor .. BotCache.masterDist .. "y" .. COLORS.RESET)
        local castTag = (BotCache.masterCasting == "Casting") and (COLORS.ALERT .. " [CASTING]" .. COLORS.RESET) or ""
        table.insert(out, COLORS.LABEL .. "Master HP: " .. COLORS.RESET .. COLORS.VALUE .. BotCache.masterHP .. "%" .. COLORS.RESET ..
                     COLORS.LABEL .. " | State: " .. COLORS.RESET .. (BotCache.masterCombat:find("Combat") and (COLORS.ALERT .. BotCache.masterCombat .. COLORS.RESET) or (COLORS.SUCCESS .. BotCache.masterCombat .. COLORS.RESET)) .. castTag)
        if BotCache.masterTarget and BotCache.masterTarget ~= "None" then
            local enemyColor = (BotCache.masterTargetEnemy == "Enemy") and COLORS.ALERT or COLORS.SUCCESS
            table.insert(out, COLORS.LABEL .. "Master Target: " .. COLORS.RESET .. enemyColor .. BotCache.masterTarget .. COLORS.RESET ..
                         COLORS.MUTED .. " (" .. BotCache.masterTargetHP .. "% HP, " .. BotCache.masterTargetEnemy .. ")" .. COLORS.RESET)
        else
            table.insert(out, COLORS.LABEL .. "Master Target: " .. COLORS.RESET .. COLORS.MUTED .. "None (No Target)" .. COLORS.RESET)
        end
    else
        table.insert(out, COLORS.MUTED .. "  No party master linked. Operating independently." .. COLORS.RESET)
    end

    -- Surroundings Radar (30y LOS)
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Nearby Entities & Surroundings Radar (30y LOS):" .. COLORS.RESET)
    if BotCache.crowdCount and BotCache.crowdCount > #BotCache.surroundings then
        table.insert(out, COLORS.WARNING .. "  [Crowd Filter: Active - Showing " .. #BotCache.surroundings .. " of " .. BotCache.crowdCount .. " entities]" .. COLORS.RESET)
    end

    if #BotCache.surroundings == 0 then
        if UnitExists("target") then
            local tName = UnitName("target")
            local tLvl = UnitLevel("target")
            local isEnemy = UnitCanAttack("player", "target")
            local tag = isEnemy and "HOSTILE" or "FRIENDLY"
            table.insert(out, "  " .. (isEnemy and COLORS.ALERT or COLORS.SUCCESS) .. "[" .. (isEnemy and "!" or "+") .. "] " .. tag .. ": " .. tName .. " (Level " .. tLvl .. ")" .. COLORS.RESET)
        end
        table.insert(out, COLORS.MUTED .. "  No surroundings radar telemetry received yet. Click [Sync (0 Tok)] below to refresh." .. COLORS.RESET)
    else
        for _, line in ipairs(BotCache.surroundings) do
            if line:find("FOCUS_TARGET") or line:find("focus_target") then
                table.insert(out, COLORS.GOLD .. "  [* FOCUS] " .. line .. COLORS.RESET)
            elseif line:find("PARTY_BOT") then
                table.insert(out, COLORS.CYAN .. "  [B] " .. line .. COLORS.RESET)
            elseif line:find("FRIENDLY_BOT") then
                table.insert(out, COLORS.PURPLE .. "  [B] " .. line .. COLORS.RESET)
            elseif line:find("ENEMY_PLAYER") then
                table.insert(out, COLORS.ALERT .. "  [P] " .. line .. COLORS.RESET)
            elseif line:find("HOSTILE") or line:find("threat") then
                table.insert(out, COLORS.ALERT .. "  [!] " .. line .. COLORS.RESET)
            elseif line:find("NEUTRAL") then
                table.insert(out, COLORS.WARNING .. "  [-] " .. line .. COLORS.RESET)
            elseif line:find("FRIENDLY") or line:find("questgiver") then
                table.insert(out, COLORS.SUCCESS .. "  [+] " .. line .. COLORS.RESET)
            elseif line:find("LOOT") or line:find("dead_lootable") or line:find("chest") then
                table.insert(out, COLORS.ORANGE .. "  [$] " .. line .. COLORS.RESET)
            elseif line:find("PLAYER") then
                table.insert(out, COLORS.PURPLE .. "  [P] " .. line .. COLORS.RESET)
            else
                table.insert(out, COLORS.LABEL .. "  " .. line .. COLORS.RESET)
            end
        end
    end

    -- Equipment & Bags
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Gear & Inventory:" .. COLORS.RESET)
    local ilvlColor = BotCache.ilvl > 200 and COLORS.PURPLE or (BotCache.ilvl > 100 and COLORS.SUBHEADER or COLORS.SUCCESS)
    table.insert(out, COLORS.LABEL .. "Average Item Level: " .. COLORS.RESET .. ilvlColor .. (BotCache.ilvl > 0 and BotCache.ilvl or "N/A") .. COLORS.RESET)
    if BotCache.spellbook or BotCache.casting then
        table.insert(out, COLORS.LABEL .. "Spellbook: " .. COLORS.RESET .. COLORS.VALUE ..
                     (BotCache.spellbook or "unknown") .. COLORS.RESET ..
                     COLORS.MUTED .. " | Casting: " .. COLORS.RESET ..
                     ((BotCache.casting == "idle") and COLORS.MUTED or COLORS.ORANGE) ..
                     (BotCache.casting or "idle") .. COLORS.RESET)
    end
    local bagColor = BotCache.freeBags > 5 and COLORS.SUCCESS or (BotCache.freeBags > 0 and COLORS.WARNING or COLORS.ALERT)
    table.insert(out, COLORS.LABEL .. "Free Bag Space: " .. COLORS.RESET .. bagColor .. BotCache.freeBags .. " slots" .. COLORS.RESET ..
                 COLORS.MUTED .. " (Total: " .. BotCache.totalBags .. " slots)" .. COLORS.RESET)

    -- Consumables
    table.insert(out, COLORS.LABEL .. "Consumables: " .. COLORS.RESET ..
                 COLORS.VALUE .. "Water: " .. COLORS.CYAN .. BotCache.waterCount .. COLORS.RESET .. COLORS.MUTED .. " | " ..
                 COLORS.VALUE .. "Food: " .. COLORS.ORANGE .. BotCache.foodCount .. COLORS.RESET .. COLORS.MUTED .. " | " ..
                 COLORS.VALUE .. "Potions: " .. COLORS.ALERT .. BotCache.potionsCount .. COLORS.RESET)

    -- Learned Combat Spells
    if BotCache.combatSpells and #BotCache.combatSpells > 0 then
        table.insert(out, "\n" .. COLORS.SUBHEADER .. "Learned Combat Spells (" .. #BotCache.combatSpells .. " known):" .. COLORS.RESET)
        local spellParts = {}
        for _, sp in ipairs(BotCache.combatSpells) do
            local rankText = (sp.rank and sp.rank ~= "") and (" " .. COLORS.MUTED .. sp.rank .. COLORS.RESET) or ""
            table.insert(spellParts, COLORS.LABEL .. "[" .. sp.id .. "] " .. COLORS.VALUE .. sp.name .. rankText)
        end
        table.insert(out, "  " .. table.concat(spellParts, ", "))
    end

    -- Active Quests
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Active Quests in Log:" .. COLORS.RESET)
    if #BotCache.activeQuests == 0 then
        table.insert(out, COLORS.MUTED .. "  No active quests currently tracked." .. COLORS.RESET)
    else
        for _, q in ipairs(BotCache.activeQuests) do
            local tag = q.complete and (COLORS.SUCCESS .. "[COMPLETE] " .. COLORS.RESET) or (COLORS.WARNING .. "[IN PROGRESS] " .. COLORS.RESET)
            local titleStr = q.title or ("Quest ID #" .. q.id)
            table.insert(out, "  " .. tag .. COLORS.VALUE .. titleStr .. COLORS.RESET)
        end
    end

    -- World Environment
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Local World Environment:" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Zone / Subzone: " .. COLORS.RESET .. COLORS.VALUE .. BotCache.zone .. " (" .. BotCache.subzone .. ")" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Resting Area (Inn): " .. COLORS.RESET .. (BotCache.inInn == "Yes" and (COLORS.SUCCESS .. "Yes (Tavern Rest)" .. COLORS.RESET) or (COLORS.MUTED .. "No" .. COLORS.RESET)))
    table.insert(out, COLORS.LABEL .. "Environment Flags: " .. COLORS.RESET ..
                 COLORS.VALUE .. "Outdoors: " .. BotCache.outdoors .. COLORS.RESET .. COLORS.MUTED .. " | " ..
                 COLORS.VALUE .. "Swimming: " .. BotCache.swimming .. COLORS.RESET .. COLORS.MUTED .. " | " ..
                 COLORS.VALUE .. "Mounted: " .. BotCache.mounted .. COLORS.RESET)

    return table.concat(out, "\n")
end

-- 2. Format Mindset & Cognitive Debug Dashboard (Zero Extra Tokens)
local function FormatMindsetDebugText()
    local out = {}
    table.insert(out, COLORS.HEADER .. "Cognitive Mindset & Token Telemetry" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "Real-time AI momentum and token savings tracking" .. COLORS.RESET)
    table.insert(out, COLORS.MUTED .. string.rep("-", 46) .. COLORS.RESET)

    -- Mindset Status
    table.insert(out, COLORS.SUBHEADER .. "Cognitive Mindset (AI ADHD Prevention):" .. COLORS.RESET)
    local curMindset = tostring(BotCache.mindset or "IDLE")
    local mindsetColor = curMindset == "COMBAT" and COLORS.ALERT or
                         (curMindset == "RESTING" and COLORS.CYAN or
                         (curMindset == "LOOTING" and COLORS.ORANGE or
                         (curMindset == "FOLLOWING" and COLORS.SUCCESS or COLORS.VALUE)))
    table.insert(out, COLORS.LABEL .. "Active Mindset: " .. COLORS.RESET .. mindsetColor .. curMindset .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Commitment Window: " .. COLORS.RESET .. COLORS.WARNING .. tostring(BotCache.commitmentWindow or "15s") .. COLORS.RESET ..
                 COLORS.MUTED .. " (locks goal against ambient distractions)" .. COLORS.RESET)

    local curClaimed = tostring(BotCache.claimed or "RELEASED")
    local claimColor = curClaimed == "CLAIMED" and COLORS.SUCCESS or COLORS.MUTED
    table.insert(out, COLORS.LABEL .. "Bot Agency Lock: " .. COLORS.RESET .. claimColor .. curClaimed .. COLORS.RESET ..
                 COLORS.MUTED .. " (" .. (curClaimed == "CLAIMED" and "AzerothFriend bridge disconnected" or "bridge connected") .. ")" .. COLORS.RESET)

    -- Token Telemetry & Activity Level
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Real-Time Token Telemetry & Activity Level:" .. COLORS.RESET)
    local actBadge = COLORS.SUCCESS .. "[NORMAL]" .. COLORS.RESET
    if BotCache.activityLevel == "HIGH" then
        actBadge = COLORS.ALERT .. "[HIGH ACTIVITY SPIKE]" .. COLORS.RESET
    elseif BotCache.activityLevel == "ELEVATED" then
        actBadge = COLORS.WARNING .. "[ELEVATED ACTIVITY]" .. COLORS.RESET
    end
    table.insert(out, COLORS.LABEL .. "Activity Status: " .. COLORS.RESET .. actBadge .. COLORS.MUTED .. " (" .. tostring(BotCache.activityReason or "Stable") .. ")" .. COLORS.RESET)
    local tok = BotCache.lastTokens or {}
    local tTotal = tonumber(tok.total) or 0
    local tPrompt = tonumber(tok.prompt) or 0
    local tComp = tonumber(tok.completion) or 0
    local tReas = tonumber(tok.reasoning) or 0
    local reasText = (tReas > 0) and (", Reasoning: " .. tReas) or ""
    table.insert(out, COLORS.LABEL .. "Latest Call Tokens: " .. COLORS.RESET .. COLORS.SUCCESS .. tTotal .. COLORS.RESET ..
                 COLORS.MUTED .. " (Prompt: " .. tPrompt .. ", Completion: " .. tComp .. reasText .. ")" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Session Cumulative: " .. COLORS.RESET .. COLORS.GOLD .. (tonumber(BotCache.sessionTokens) or 0) .. " tokens" .. COLORS.RESET ..
                 COLORS.MUTED .. " (" .. (tonumber(BotCache.sessionCalls) or 0) .. " calls total | Model: " .. tostring(BotCache.model or "Default") .. ")" .. COLORS.RESET)

    -- Token Synergy with mod-llm-chatter
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Token Sharing & Synergy Dashboard:" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Estimated Tokens Saved: " .. COLORS.RESET .. COLORS.SUCCESS .. "~" .. tostring(BotCache.tokensSaved or 0) .. " tokens" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Compound Dialogue Calls: " .. COLORS.RESET .. COLORS.CYAN .. tostring(BotCache.coProcessed or 0) .. COLORS.RESET ..
                 COLORS.MUTED .. " (1 request = actions + speech)" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Zero-Token Sensory Actions: " .. COLORS.RESET .. COLORS.ORANGE .. tostring(BotCache.sensoryReused or 0) .. COLORS.RESET ..
                 COLORS.MUTED .. " (chests/nodes/loot reused)" .. COLORS.RESET)

    -- RAM-First Transport (server-owned values: the UI performs no calculations)
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "RAM-First Live State & Context:" .. COLORS.RESET)
    local transport = tostring(BotCache.transport or "UNKNOWN")
    local transportColor = (transport == "LIVE" and COLORS.SUCCESS) or
                           (transport == "STALLED" and COLORS.ALERT) or
                           (transport == "HANDSHAKE" and COLORS.WARNING) or COLORS.MUTED
    table.insert(out, COLORS.LABEL .. "Transport: " .. COLORS.RESET .. transportColor .. transport .. COLORS.RESET ..
                 COLORS.MUTED .. " (session " .. tostring(BotCache.transportSession or "n/a") ..
                 ", frames " .. tostring(BotCache.transportFrames or 0) ..
                 ", refreshes " .. tostring(BotCache.transportRefreshes or 0) .. ")" .. COLORS.RESET)
    local cacheAge = tonumber(BotCache.cacheAge) or 0
    local cacheColor = (cacheAge <= 5) and COLORS.SUCCESS or ((cacheAge <= 15) and COLORS.WARNING or COLORS.ALERT)
    table.insert(out, COLORS.LABEL .. "RAM Cache Age: " .. COLORS.RESET .. cacheColor .. tostring(cacheAge) .. "s" .. COLORS.RESET ..
                 COLORS.MUTED .. " (freshness window: 5s; stale suspends new planning)" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Planning Context: " .. COLORS.RESET .. COLORS.CYAN .. tostring(BotCache.contextTokens or 0) ..
                 COLORS.RESET .. COLORS.MUTED .. " / " .. tostring(BotCache.contextCeiling or 0) .. " estimated input tokens" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "RAM Footprint: " .. COLORS.RESET .. COLORS.VALUE .. tostring(BotCache.ramBytes or 0) .. " bytes" .. COLORS.RESET ..
                 COLORS.MUTED .. " across " .. tostring(BotCache.ramCachedBots or 0) .. " bot cache(s)" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Latest Summary Age: " .. COLORS.RESET .. COLORS.WARNING .. tostring(BotCache.summaryAge or 0) .. "s" .. COLORS.RESET ..
                 COLORS.MUTED .. " (goal-bound, max 2 per prompt)" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Rollout Stage: " .. COLORS.RESET .. COLORS.HEADER .. tostring(BotCache.rolloutStage or 0) .. COLORS.RESET ..
                 COLORS.MUTED .. " (SQL compatibility: " .. tostring(BotCache.sqlCompat or "OFF") .. ")" .. COLORS.RESET)
    if BotCache.diagNote and BotCache.diagNote ~= "" then
        table.insert(out, COLORS.LABEL .. "Context Diagnostic: " .. COLORS.RESET .. COLORS.ALERT .. tostring(BotCache.diagNote) .. COLORS.RESET)
    end

    -- Live Playerbot Strategies (Memory Sampled from Engine)
    if (BotCache.combatStrategies and #BotCache.combatStrategies > 0) or
       (BotCache.nonCombatStrategies and #BotCache.nonCombatStrategies > 0) then
        table.insert(out, "\n" .. COLORS.SUBHEADER .. "Playerbot Active Strategies (Live Engine Cache):" .. COLORS.RESET)
        if BotCache.combatStrategies and #BotCache.combatStrategies > 0 then
            table.insert(out, COLORS.LABEL .. "Combat Strategies: " .. COLORS.RESET .. COLORS.ALERT .. table.concat(BotCache.combatStrategies, ", ") .. COLORS.RESET)
        end
        if BotCache.nonCombatStrategies and #BotCache.nonCombatStrategies > 0 then
            table.insert(out, COLORS.LABEL .. "Non-Combat Strategies: " .. COLORS.RESET .. COLORS.SUCCESS .. table.concat(BotCache.nonCombatStrategies, ", ") .. COLORS.RESET)
        end
    end

    -- AI Policy & Configuration
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "AI Model & Reasoning Profile:" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Reasoning Engine: " .. COLORS.RESET .. COLORS.VALUE .. "OpenAI Thinking Policy (Legacy & Newer Models)" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Latency Filter: " .. COLORS.RESET .. COLORS.SUCCESS .. "Active (Suppresses routine ambient interruptions)" .. COLORS.RESET)

    -- Action Diagnostics & Suppressed Errors
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Action Diagnostics (Suppressed from Game Chat):" .. COLORS.RESET)
    if BotCache.lastActionError and BotCache.lastActionError ~= "" then
        table.insert(out, COLORS.LABEL .. "Latest Error: " .. COLORS.RESET .. COLORS.ALERT .. tostring(BotCache.lastActionError) .. COLORS.RESET)
        if BotCache.lastActionErrorTime and BotCache.lastActionErrorTime ~= "" then
            table.insert(out, COLORS.LABEL .. "Timestamp: " .. COLORS.RESET .. COLORS.MUTED .. tostring(BotCache.lastActionErrorTime) .. COLORS.RESET ..
                         COLORS.MUTED .. " (Suppressed count: " .. tostring(BotCache.errorCount or 1) .. ")" .. COLORS.RESET)
        end
    else
        table.insert(out, COLORS.MUTED .. "No action errors recorded. Chat filter active." .. COLORS.RESET)
    end
    if BotCache.errorHistory and #BotCache.errorHistory > 0 then
        table.insert(out, "\n" .. COLORS.LABEL .. "Recent Error History (Addon-Only):" .. COLORS.RESET)
        for i = #BotCache.errorHistory, math.max(1, #BotCache.errorHistory - 4), -1 do
            local e = BotCache.errorHistory[i]
            if e then
                local bTag = (e.bot and e.bot ~= "") and (" [" .. tostring(e.bot) .. "]") or ""
                table.insert(out, "  " .. COLORS.MUTED .. "[" .. tostring(e.time or "") .. "]" .. COLORS.RESET .. COLORS.ALERT .. bTag .. " " .. tostring(e.error or "") .. COLORS.RESET)
            end
        end
    end

    return table.concat(out, "\n")
end

-- 3. Format Actions & Execution Pipeline
local function FormatActionsText()
    local out = {}
    table.insert(out, COLORS.HEADER .. "Action Execution Pipeline & Mindset Matrix" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "Real-time action status, multi-stance composition, and non-overlapping arbitration" .. COLORS.RESET)
    table.insert(out, COLORS.MUTED .. string.rep("-", 46) .. COLORS.RESET)

    -- Live Pipeline Telemetry
    table.insert(out, COLORS.SUBHEADER .. "Live Execution Status:" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Active Action: " .. COLORS.RESET .. COLORS.SUCCESS .. BotCache.activeAction:upper() .. COLORS.RESET)
    local stColor = BotCache.actionStatus:lower():find("progress") and COLORS.WARNING or
                    (BotCache.actionStatus:lower():find("complete") and COLORS.SUCCESS or COLORS.MUTED)
    table.insert(out, COLORS.LABEL .. "Execution Status: " .. COLORS.RESET .. stColor .. BotCache.actionStatus .. COLORS.RESET)
    if BotCache.lastActionError and BotCache.lastActionError ~= "" then
        table.insert(out, COLORS.LABEL .. "Action Diagnostic: " .. COLORS.RESET .. COLORS.ALERT .. "[SUPPRESSED ERROR] " .. BotCache.lastActionError .. COLORS.RESET)
        table.insert(out, COLORS.MUTED .. "  Logged at " .. (BotCache.lastActionErrorTime or "recent") .. " | Blocked from chat (" .. (BotCache.errorCount or 1) .. " times)" .. COLORS.RESET)
    end

    -- Dynamic Subsumption & Autonomy Tier
    local combatState = (BotCache.combat == "In Combat" or BotCache.masterCombat:find("Combat"))
    local tetherState = (BotCache.masterDist and BotCache.masterDist > 35) and (COLORS.ALERT .. "EXCEEDED (>35y - Leash Active)" .. COLORS.RESET) or (COLORS.SUCCESS .. "OK (<35y)" .. COLORS.RESET)
    local autonomyTier = combatState and (COLORS.ALERT .. "COMBAT SUPREMACY (Native Playerbot Rotation Active)" .. COLORS.RESET) or
                         ((BotCache.activeAction:find("tavern_rest") or BotCache.activeAction:find("campfire_cook")) and (COLORS.CYAN .. "LIVING DOWNTIME ROUTINE" .. COLORS.RESET) or
                         (COLORS.SUCCESS .. "DYNAMIC SUBSUMPTION (Tethered Autonomy)" .. COLORS.RESET))
    table.insert(out, COLORS.LABEL .. "Autonomy Posture: " .. COLORS.RESET .. autonomyTier)
    table.insert(out, COLORS.LABEL .. "Elastic Tether: " .. COLORS.RESET .. tetherState)

    table.insert(out, COLORS.LABEL .. "Plan Step: " .. COLORS.RESET .. COLORS.ORANGE .. BotCache.actionStep .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Movement Target: " .. COLORS.RESET .. COLORS.VALUE .. BotCache.movementTarget .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Next Queued Step: " .. COLORS.RESET .. COLORS.SUBHEADER .. BotCache.nextAction .. COLORS.RESET)

    -- Multi-Stance Configuration (Mix & Match)
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Multi-Stance Composition (Mix & Match):" .. COLORS.RESET)
    local curM = (BotCache.mindset or "IDLE"):upper()
    local mColor = curM == "COMBAT" and COLORS.ALERT or (curM == "FOLLOWING" and COLORS.SUCCESS or COLORS.WARNING)
    table.insert(out, COLORS.LABEL .. "Primary Mindset: " .. COLORS.RESET .. mColor .. curM .. COLORS.RESET .. COLORS.MUTED .. " (Active Momentum)" .. COLORS.RESET)

    local moveStance = (curM == "FOLLOWING") and (COLORS.SUCCESS .. "FOLLOWING (Keep formation)" .. COLORS.RESET) or
                       ((BotCache.actionStatus:lower():find("stay") or BotCache.activeAction:lower():find("stay")) and (COLORS.WARNING .. "HOLD (Position locked)" .. COLORS.RESET) or (COLORS.VALUE .. "AUTO (Contextual)" .. COLORS.RESET))
    table.insert(out, COLORS.LABEL .. "  [1] Movement Layer: " .. COLORS.RESET .. moveStance)

    local combatStance = (curM == "COMBAT") and (COLORS.ALERT .. "ENGAGED (Threat rotation active)" .. COLORS.RESET) or (COLORS.MUTED .. "READY (Assists master on pull)" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "  [2] Combat Layer:   " .. COLORS.RESET .. combatStance)

    local utilStance = (curM == "RESTING") and (COLORS.CYAN .. "RESTING (Food & Drink active)" .. COLORS.RESET) or
                       ((curM == "LOOTING") and (COLORS.ORANGE .. "LOOTING (Corpse / Node gather)" .. COLORS.RESET) or (COLORS.MUTED .. "STANDBY (Automatic post-combat)" .. COLORS.RESET))
    table.insert(out, COLORS.LABEL .. "  [3] Utility Layer:  " .. COLORS.RESET .. utilStance)

    table.insert(out, COLORS.LABEL .. "  [4] Social Layer:   " .. COLORS.RESET .. COLORS.PURPLE .. "CHATTER (LLM Dialogue + Rate-limited Emotes)" .. COLORS.RESET)

    -- Complete List of Available Mindsets
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Available Mindset Catalog:" .. COLORS.RESET)
    local function MindsetTag(name, desc)
        local isActive = (curM == name)
        local prefix = isActive and (COLORS.SUCCESS .. " [>] " .. COLORS.RESET) or (COLORS.MUTED .. " [ ] " .. COLORS.RESET)
        local tagCol = isActive and COLORS.WARNING or COLORS.VALUE
        return prefix .. tagCol .. name .. COLORS.RESET .. COLORS.MUTED .. " - " .. desc .. COLORS.RESET
    end
    table.insert(out, MindsetTag("FOLLOWING", "Anchor to master, guard flank, dynamic crowd deadband."))
    table.insert(out, MindsetTag("COMBAT",    "Assist master, auto threat focus (12s stickiness), class rotation."))
    table.insert(out, MindsetTag("RESTING",   "Eat food & drink water when low on health/mana."))
    table.insert(out, MindsetTag("LOOTING",   "Harvest defeatable corpses, chests, and resource nodes."))
    table.insert(out, MindsetTag("EXPLORING", "Patrol local perimeter and scout uncharted surroundings."))
    table.insert(out, MindsetTag("SOCIAL",    "Ambient reactions, synchronised emotes, chatter dialogue."))
    table.insert(out, MindsetTag("IDLE",      "Passive observation, hold ground, agency lock hold."))

    -- Non-Overlapping Conflict Resolution Rules
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Non-Overlapping Arbitration Rules:" .. COLORS.RESET)
    table.insert(out, COLORS.MUTED .. "Mindsets can be mixed and matched. Priority arbitration ensures zero overlap:" .. COLORS.RESET)
    table.insert(out, COLORS.ALERT .. " 1. Combat Override: " .. COLORS.RESET .. COLORS.VALUE .. "Hostile pulls instantly suspend Resting, Looting & Patrol." .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. " 2. Vital Recovery: " .. COLORS.RESET .. COLORS.VALUE .. "Low HP/Mana (under 40%) out-of-combat pauses Following for Rest." .. COLORS.RESET)
    table.insert(out, COLORS.ORANGE .. " 3. Loot Sequence: " .. COLORS.RESET .. COLORS.VALUE .. "Nearby corpses are cleared before resuming formation travel." .. COLORS.RESET)
    table.insert(out, COLORS.SUCCESS .. " 4. Travel Baseline: " .. COLORS.RESET .. COLORS.VALUE .. "Following stays active as the default companion anchor." .. COLORS.RESET)
    table.insert(out, COLORS.PURPLE .. " 5. Ambient Speech: " .. COLORS.RESET .. COLORS.VALUE .. "Social chatter operates concurrently without interrupting movement." .. COLORS.RESET)

    -- In-Game Quick Action Triggers
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Cognitive Action & Command Triggers:" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af attack" .. COLORS.RESET .. COLORS.MUTED .. "        - Attack player's current target" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af follow" .. COLORS.RESET .. COLORS.MUTED .. "        - Force formation follow" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af stay / hold" .. COLORS.RESET .. COLORS.MUTED .. "   - Hold ground and stay" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af flee" .. COLORS.RESET .. COLORS.MUTED .. "          - Disengage and retreat to master" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af loot [all]" .. COLORS.RESET .. COLORS.MUTED .. "    - Harvest corpses (all/normal/gray)" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af rest" .. COLORS.RESET .. COLORS.MUTED .. "          - Eat food & drink water" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af rpg [id|?]" .. COLORS.RESET .. COLORS.MUTED .. "    - Execute/query quest objectives" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af accept" .. COLORS.RESET .. COLORS.MUTED .. "        - Accept offered quest from NPC" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af reward [1-6]" .. COLORS.RESET .. COLORS.MUTED .. "  - Select quest completion reward" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af co" .. COLORS.RESET .. COLORS.MUTED .. "            - Toggle major offensive cooldowns" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af open" .. COLORS.RESET .. COLORS.MUTED .. "          - Open lockboxes, clams, containers" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af trainer" .. COLORS.RESET .. COLORS.MUTED .. "       - Learn available class spells" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af claim / release" .. COLORS.RESET .. COLORS.MUTED .. " - Toggle companion agency lock" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af action [cmd]" .. COLORS.RESET .. COLORS.MUTED .. "   - Dispatch any catalog action directly" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af goal set [text]" .. COLORS.RESET .. COLORS.MUTED .. " - Set the focused goal (panel: /af goals)" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af autonomy on|off" .. COLORS.RESET .. COLORS.MUTED .. " - Opt in to self-directed operation" .. COLORS.RESET)
    table.insert(out, COLORS.CYAN .. "  /af cast [spell] [target]" .. COLORS.RESET .. COLORS.MUTED .. " - Request a spell in natural language" .. COLORS.RESET)

    return table.concat(out, "\n")
end

-- Playerbot Semantic Bot API v1. The addon renders only server-reported
-- contracts and outcomes; it does not maintain a client-side action schema.
local function FormatBotApiText()
    local out = {}
    local contract = BotCache.apiContract or {}

    table.insert(out, COLORS.SUBHEADER .. "Latest Capability Execution" .. COLORS.RESET)
    local status = tostring(BotCache.actionStatus or "IDLE")
    local statusLower = status:lower()
    local statusColor = statusLower:find("fail") and COLORS.ALERT or
                        (statusLower:find("complete") and COLORS.SUCCESS or COLORS.WARNING)
    table.insert(out, COLORS.LABEL .. "Capability: " .. COLORS.RESET .. COLORS.VALUE ..
                 tostring(BotCache.activeAction or "None") .. COLORS.RESET ..
                 COLORS.MUTED .. " | Status: " .. COLORS.RESET .. statusColor .. status .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Verified: " .. COLORS.RESET ..
                 ((BotCache.actionVerified == "true") and COLORS.SUCCESS or
                  ((BotCache.actionVerified == "false") and COLORS.WARNING or COLORS.MUTED)) ..
                 tostring(BotCache.actionVerified or "pending") .. COLORS.RESET ..
                 COLORS.MUTED .. " | Authority: " .. COLORS.RESET .. COLORS.VALUE ..
                 tostring(BotCache.actionAuthority or "unknown") .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Binding: " .. COLORS.RESET .. COLORS.CYAN ..
                 tostring(BotCache.actionBinding or "unknown") .. COLORS.RESET ..
                 COLORS.MUTED .. " -> " .. tostring(BotCache.actionNativeBinding or "none") .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Completion: " .. COLORS.RESET .. COLORS.VALUE ..
                 tostring(BotCache.actionCompletion or "unknown") .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Parameters: " .. COLORS.RESET .. COLORS.MUTED ..
                 tostring(BotCache.actionParams or "{}") .. COLORS.RESET)
    if BotCache.actionResult and BotCache.actionResult ~= "" then
        table.insert(out, COLORS.LABEL .. "Effects: " .. COLORS.RESET .. COLORS.SUCCESS ..
                     tostring(BotCache.actionResult) .. COLORS.RESET)
    end
    if BotCache.actionError and BotCache.actionError ~= "" then
        table.insert(out, COLORS.LABEL .. "Error: " .. COLORS.RESET .. COLORS.ALERT ..
                     tostring(BotCache.actionError) .. COLORS.RESET)
    end

    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Selected Capability Contract" .. COLORS.RESET)
    if contract.name and contract.name ~= "" then
        table.insert(out, COLORS.HEADER .. contract.name .. COLORS.RESET ..
                     COLORS.MUTED .. " | v" .. tostring(contract.apiVersion or BotCache.apiVersion or 1) ..
                     " r" .. tostring(contract.capabilityRevision or BotCache.capabilityRevision or 1) ..
                     " | " .. tostring(contract.category or "uncategorized") .. COLORS.RESET)
        table.insert(out, COLORS.VALUE .. tostring(contract.description or "") .. COLORS.RESET)
        table.insert(out, COLORS.LABEL .. "Authority: " .. COLORS.RESET .. COLORS.WARNING ..
                     tostring(contract.authority or "unknown") .. COLORS.RESET)
        table.insert(out, COLORS.LABEL .. "Native binding: " .. COLORS.RESET .. COLORS.CYAN ..
                     tostring(contract.bindingKind or "unknown") .. COLORS.RESET .. COLORS.MUTED ..
                     " -> " .. tostring(contract.nativeBinding or "none") .. COLORS.RESET)
        table.insert(out, COLORS.LABEL .. "Completion policy: " .. COLORS.RESET .. COLORS.VALUE ..
                     tostring(contract.completion or "unknown") .. COLORS.RESET)
        table.insert(out, COLORS.LABEL .. "Preconditions: " .. COLORS.RESET .. COLORS.VALUE ..
                     tostring(contract.preconditions or "none") .. COLORS.RESET)
        table.insert(out, COLORS.LABEL .. "Parameters: " .. COLORS.RESET .. COLORS.VALUE ..
                     tostring(contract.parameters or "none") .. COLORS.RESET)
        table.insert(out, COLORS.MUTED .. "Parameter schema: " .. tostring(contract.parameterSchema or "not received") .. COLORS.RESET)
        table.insert(out, COLORS.MUTED .. "Result schema: " .. tostring(contract.resultSchema or "not received") .. COLORS.RESET)
    else
        table.insert(out, COLORS.MUTED .. "Enter a capability above and click Describe to load its server-owned contract." .. COLORS.RESET)
    end
    if BotCache.apiError and BotCache.apiError ~= "" then
        table.insert(out, COLORS.ALERT .. BotCache.apiError .. COLORS.RESET)
    end

    table.insert(out, "\n" .. COLORS.SUBHEADER .. "Curated Capability Index" .. COLORS.RESET)
    table.insert(out, COLORS.MUTED .. "The list is diagnostic. Raw native passthrough stays hidden unless enabled server-side." .. COLORS.RESET)
    local catalog = BotCache.apiCatalog or {}
    if #catalog == 0 then
        table.insert(out, COLORS.MUTED .. "No catalogue response yet. Click Refresh Catalog." .. COLORS.RESET)
    else
        local lastCategory = ""
        for _, entry in ipairs(catalog) do
            if entry.category ~= lastCategory then
                lastCategory = entry.category
                table.insert(out, "\n" .. COLORS.CYAN .. "[" .. tostring(lastCategory):upper() .. "]" .. COLORS.RESET)
            end
            local authorityColor = entry.authority == "owner_command" and COLORS.WARNING or COLORS.SUCCESS
            table.insert(out, "  " .. COLORS.VALUE .. tostring(entry.name) .. COLORS.RESET ..
                         COLORS.MUTED .. " - " .. COLORS.RESET .. authorityColor ..
                         tostring(entry.authority) .. COLORS.RESET)
        end
        if BotCache.apiCatalogTruncated then
            table.insert(out, COLORS.WARNING .. "List truncated by the server; use Describe with an exact capability name." .. COLORS.RESET)
        end
    end

    return table.concat(out, "\n")
end

RefreshBotApiPanel = function()
    local statusText = _G["AzerothFriendApiStatusText"]
    if statusText then
        local executionColor = BotCache.apiExecution == "ENABLED" and COLORS.SUCCESS or COLORS.ALERT
        statusText:SetText(
            COLORS.LABEL .. "Bot API: " .. COLORS.RESET .. executionColor .. tostring(BotCache.apiExecution) .. COLORS.RESET ..
            COLORS.MUTED .. " | v" .. tostring(BotCache.apiVersion or 0) ..
            " r" .. tostring(BotCache.capabilityRevision or 0) .. COLORS.RESET .. "\n" ..
            COLORS.LABEL .. "Capabilities: " .. COLORS.RESET .. COLORS.VALUE .. tostring(BotCache.apiCuratedCount or 0) .. COLORS.RESET ..
            COLORS.MUTED .. " (autonomous " .. tostring(BotCache.apiAutonomousCount or 0) ..
            ", owner " .. tostring(BotCache.apiOwnerCount or 0) .. ") | Owner tier: " ..
            tostring(BotCache.apiOwnerTier or "UNKNOWN") .. " | Raw: " ..
            tostring(BotCache.apiRawPassthrough or "UNKNOWN") .. " | Discovery: " ..
            tostring(BotCache.apiNativeDiscovery or "UNKNOWN") .. COLORS.RESET)
    end
    if AzerothFriendApiContractBodyFrame then
        SetContainerText(AzerothFriendApiContractBodyFrame, FormatBotApiText())
    end
    if AzerothFriendApiRunButton then
        if BotCache.apiExecution == "ENABLED" then
            AzerothFriendApiRunButton:Enable()
        else
            AzerothFriendApiRunButton:Disable()
        end
    end
end

-- 4. Format Surroundings & Radar
local function FormatSurroundingsText()
    local out = {}
    table.insert(out, COLORS.HEADER .. "Nearby Entities & Objects (30y LOS Radar)" .. COLORS.RESET)
    table.insert(out, COLORS.MUTED .. string.rep("-", 46) .. COLORS.RESET)

    if BotCache.crowdCount and BotCache.crowdCount > #BotCache.surroundings then
        table.insert(out, COLORS.WARNING .. "[Crowd Filter: Active - Showing " .. #BotCache.surroundings .. " of " .. BotCache.crowdCount .. " entities]" .. COLORS.RESET)
    end

    if #BotCache.surroundings == 0 then
        if UnitExists("target") then
            local tName = UnitName("target")
            local tLvl = UnitLevel("target")
            local isEnemy = UnitCanAttack("player", "target")
            local tag = isEnemy and "HOSTILE" or "FRIENDLY"
            table.insert(out, (isEnemy and COLORS.ALERT or COLORS.SUCCESS) .. "[" .. (isEnemy and "!" or "+") .. "] " .. tag .. ": " .. tName .. " (Level " .. tLvl .. ")" .. COLORS.RESET)
        end
        table.insert(out, COLORS.MUTED .. "No surroundings radar telemetry received yet." .. COLORS.RESET)
        table.insert(out, COLORS.CYAN .. "\nTip: Click [Sync (0 Tok)] below to request a fresh surroundings radar scan from the server." .. COLORS.RESET)
    else
        for _, line in ipairs(BotCache.surroundings) do
            if line:find("FOCUS_TARGET") or line:find("focus_target") then
                table.insert(out, COLORS.GOLD .. "[* FOCUS] " .. line .. COLORS.RESET)
            elseif line:find("PARTY_BOT") then
                table.insert(out, COLORS.CYAN .. "[B] " .. line .. COLORS.RESET)
            elseif line:find("FRIENDLY_BOT") then
                table.insert(out, COLORS.PURPLE .. "[B] " .. line .. COLORS.RESET)
            elseif line:find("ENEMY_PLAYER") then
                table.insert(out, COLORS.ALERT .. "[P] " .. line .. COLORS.RESET)
            elseif line:find("HOSTILE") or line:find("threat") then
                table.insert(out, COLORS.ALERT .. "[!] " .. line .. COLORS.RESET)
            elseif line:find("NEUTRAL") then
                table.insert(out, COLORS.WARNING .. "[-] " .. line .. COLORS.RESET)
            elseif line:find("FRIENDLY") or line:find("questgiver") then
                table.insert(out, COLORS.SUCCESS .. "[+] " .. line .. COLORS.RESET)
            elseif line:find("LOOT") or line:find("dead_lootable") or line:find("chest") then
                table.insert(out, COLORS.ORANGE .. "[$] " .. line .. COLORS.RESET)
            elseif line:find("PLAYER") then
                table.insert(out, COLORS.PURPLE .. "[P] " .. line .. COLORS.RESET)
            else
                table.insert(out, COLORS.LABEL .. line .. COLORS.RESET)
            end
        end
    end

    return table.concat(out, "\n")
end

-- 5. Format Thoughts, Speech & Overheard Dialogue Stream
local function FormatThoughtsText()
    local out = {}
    table.insert(out, COLORS.HEADER .. "Cognitive Reasoning, Speech & Dialogue Stream" .. COLORS.RESET)
    table.insert(out, COLORS.MUTED .. string.rep("-", 50) .. COLORS.RESET)

    -- The agent's enduring purpose and current objective: the basis for everything
    -- it decides, so it stays visible next to the reasoning it produces.
    local goalState = AFGetGoalState and AFGetGoalState() or nil
    if goalState then
        local longGoal = (goalState.long_goal and goalState.long_goal ~= "") and goalState.long_goal or "not set"
        local shortGoal = (goalState.goal and goalState.goal ~= "") and goalState.goal or "not set"
        table.insert(out, COLORS.SUBHEADER .. "[Core Purpose - long-term goal]:" .. COLORS.RESET)
        table.insert(out, COLORS.VALUE .. tostring(longGoal) .. COLORS.RESET)
        table.insert(out, COLORS.SUBHEADER .. "[Current Objective - short-term goal]:" .. COLORS.RESET)
        table.insert(out, COLORS.VALUE .. tostring(shortGoal) .. COLORS.RESET ..
                     COLORS.MUTED .. " (status: " .. tostring(goalState.status or "Unknown") .. ")" .. COLORS.RESET)
        table.insert(out, COLORS.MUTED .. string.rep("-", 42) .. COLORS.RESET)
    end

    table.insert(out, COLORS.LABEL .. "Trigger Event: " .. COLORS.RESET .. COLORS.SUBHEADER .. tostring(BotCache.triggerEvent or "None") .. COLORS.RESET ..
                 COLORS.MUTED .. " | Plan ID: " .. COLORS.RESET .. COLORS.WARNING .. tostring(BotCache.planId or "0") .. COLORS.RESET)

    if BotCache.model and BotCache.model ~= "Default" then
        table.insert(out, COLORS.LABEL .. "LLM Engine: " .. COLORS.RESET .. COLORS.CYAN .. tostring(BotCache.model) .. COLORS.RESET ..
                     COLORS.MUTED .. " | Usage: " .. COLORS.RESET .. COLORS.SUCCESS .. tostring(BotCache.tokenUsage or "None") .. COLORS.RESET)
    end

    -- Real-Time Inner Thought Reasoning
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "[Current Cognitive Reasoning / Thought]:" .. COLORS.RESET)
    table.insert(out, COLORS.VALUE .. "\"" .. tostring(BotCache.thought or "Awaiting companion guidance...") .. "\"" .. COLORS.RESET)

    -- Deep Reasoning / Chain of Thought if available from thinking models (0 extra tokens)
    if BotCache.deepReasoning and BotCache.deepReasoning ~= "" and BotCache.deepReasoning ~= "None" then
        table.insert(out, "\n" .. COLORS.PURPLE .. "[Deep Chain-of-Thought (0 Extra Tokens)]:" .. COLORS.RESET)
        table.insert(out, COLORS.MUTED .. tostring(BotCache.deepReasoning) .. COLORS.RESET)
    end

    -- Companion Spoken Response (via Chatter)
    if BotCache.speech and BotCache.speech ~= "" and BotCache.speech ~= "None" then
        table.insert(out, "\n" .. COLORS.SUCCESS .. "[Companion Spoken Response (Chatter)]:" .. COLORS.RESET)
        table.insert(out, COLORS.CYAN .. "\"" .. tostring(BotCache.speech) .. "\"" .. COLORS.RESET)
    end

    -- Token Usage & Activity Telemetry Block
    table.insert(out, "\n" .. COLORS.HEADER .. "[Token Usage & LLM Activity Telemetry]:" .. COLORS.RESET)
    local tok = BotCache.lastTokens or {}
    local tTotal = tonumber(tok.total) or 0
    local tPrompt = tonumber(tok.prompt) or 0
    local tComp = tonumber(tok.completion) or 0
    local tReas = tonumber(tok.reasoning) or 0
    local reasText = (tReas > 0) and (", Reasoning: " .. tReas) or ""
    local actBadge = COLORS.SUCCESS .. "[NORMAL]" .. COLORS.RESET
    if BotCache.activityLevel == "HIGH" then
        actBadge = COLORS.ALERT .. "[HIGH ACTIVITY SPIKE]" .. COLORS.RESET
    elseif BotCache.activityLevel == "ELEVATED" then
        actBadge = COLORS.WARNING .. "[ELEVATED ACTIVITY]" .. COLORS.RESET
    end
    table.insert(out, COLORS.LABEL .. "Activity Status: " .. COLORS.RESET .. actBadge ..
                 COLORS.MUTED .. " | Rate: " .. COLORS.RESET .. COLORS.VALUE .. tostring(BotCache.activityReason or "Stable") .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Latest Call: " .. COLORS.RESET .. COLORS.SUCCESS .. "Total: " .. tTotal .. " tokens" .. COLORS.RESET ..
                 COLORS.MUTED .. " (Prompt: " .. tPrompt .. ", Completion: " .. tComp .. reasText .. ")" .. COLORS.RESET)
    table.insert(out, COLORS.LABEL .. "Session Total: " .. COLORS.RESET .. COLORS.GOLD .. (tonumber(BotCache.sessionTokens) or 0) .. " tokens" .. COLORS.RESET ..
                 COLORS.MUTED .. " across " .. COLORS.RESET .. COLORS.VALUE .. (tonumber(BotCache.sessionCalls) or 0) .. " calls" .. COLORS.RESET ..
                 COLORS.MUTED .. " | Engine: " .. COLORS.RESET .. COLORS.CYAN .. tostring(BotCache.model or "Default") .. COLORS.RESET)

    -- Overheard dialogue: only real speech. Server telemetry frames, addon-channel
    -- payloads, localisation dumps and oversized machine blobs are rejected upstream
    -- in RecordChat, so everything listed here is something a player or NPC said.
    table.insert(out, "\n" .. COLORS.HEADER .. "[Overheard Dialogue & Messages]:" .. COLORS.RESET)
    table.insert(out, COLORS.MUTED .. "  Sources: party / say / yell / whisper / emotes the companion can hear." .. COLORS.RESET)
    table.insert(out, COLORS.MUTED .. "  Tags: " .. COLORS.RESET .. COLORS.PURPLE .. "[MASTER]" .. COLORS.RESET ..
                 COLORS.MUTED .. " you, " .. COLORS.RESET .. COLORS.SUCCESS .. "[COMPANION]" .. COLORS.RESET ..
                 COLORS.MUTED .. " active bot, " .. COLORS.RESET .. COLORS.CYAN .. "[PARTY BOT]" .. COLORS.RESET ..
                 COLORS.MUTED .. " group bot, " .. COLORS.RESET .. COLORS.ORANGE .. "[NPC]" .. COLORS.RESET ..
                 COLORS.MUTED .. " world NPC, " .. COLORS.RESET .. COLORS.MUTED .. "[BOT ACK]" .. COLORS.RESET ..
                 COLORS.MUTED .. " reply." .. COLORS.RESET)
    local heard = BotCache.heardLog or {}
    if #heard == 0 then
        table.insert(out, COLORS.MUTED .. "  No dialogue overheard yet. Say something in party or say!" .. COLORS.RESET)
    else
        for _, d in ipairs(heard) do
            local shownText = tostring(d.text or "")
            if #shownText > 200 then
                shownText = shownText:sub(1, 197) .. "..."
            end
            local dChan = tostring(d.channel or "say")
            local chanTag = COLORS.CYAN .. "[" .. dChan:upper() .. "]" .. COLORS.RESET
            local pName = (UnitName("player") or ""):lower()
            local sName = tostring(d.sender or ""):lower()
            local bName = tostring(BotCache.name or ""):lower()
            local isMaster = (sName == pName) or (BotCache.masterName and sName == BotCache.masterName:lower())
            local isCompanion = (sName == bName) and (bName ~= "")
            local spkTag
            local senderDisplay = tostring(d.sender or "Unknown")
            if d.isAck or dChan == "ack" then
                spkTag = COLORS.MUTED .. "[BOT ACK] " .. senderDisplay .. COLORS.RESET
            elseif isMaster then
                spkTag = COLORS.PURPLE .. "[MASTER] " .. senderDisplay .. COLORS.RESET
            elseif isCompanion then
                spkTag = COLORS.SUCCESS .. "[COMPANION] " .. senderDisplay .. COLORS.RESET
            elseif dChan:find("npc") or shownText:find("%[NPC%]") then
                spkTag = COLORS.ORANGE .. "[NPC] " .. senderDisplay .. COLORS.RESET
            elseif dChan == "party" or shownText:find("%[PARTY BOT%]") then
                spkTag = COLORS.CYAN .. "[PARTY BOT] " .. senderDisplay .. COLORS.RESET
            else
                spkTag = COLORS.WARNING .. senderDisplay .. COLORS.RESET
            end
            local timeTag = COLORS.MUTED .. "[" .. tostring(d.time or "") .. "]" .. COLORS.RESET
            table.insert(out, timeTag .. " " .. chanTag .. " " .. spkTag .. ": " .. COLORS.VALUE .. shownText .. COLORS.RESET)
        end
    end

    -- Real-Time Thought Timeline (Last 8 entries)
    local thHistory = BotCache.thoughtHistory or {}
    if #thHistory > 0 then
        table.insert(out, "\n" .. COLORS.LABEL .. "[Recent Thought Timeline]:" .. COLORS.RESET)
        for i = #thHistory, 1, -1 do
            local h = thHistory[i]
            if h then
                local mColor = COLORS.WARNING
                local hMindset = tostring(h.mindset or "IDLE")
                if hMindset == "COMBAT" then mColor = COLORS.ALERT
                elseif hMindset == "SOCIAL" then mColor = COLORS.SUCCESS
                elseif hMindset == "RESTING" then mColor = COLORS.CYAN
                end

                local timeRange = tostring(h.time or "")
                if h.lastTime and h.lastTime ~= h.time then
                    timeRange = timeRange .. " - " .. tostring(h.lastTime)
                end

                if h.isEncounter and h.actions and #h.actions > 1 then
                    local tgtStr = (h.target and h.target ~= "None") and (" on " .. tostring(h.target)) or ""
                    local actCount = h.count or #h.actions
                    table.insert(out, COLORS.MUTED .. "[" .. timeRange .. "] " .. COLORS.ALERT .. "[COMBAT ENCOUNTER" .. tgtStr .. "] " .. COLORS.GOLD .. "(" .. actCount .. " actions):" .. COLORS.RESET)
                    for _, act in ipairs(h.actions) do
                        if act then
                            table.insert(out, "    " .. COLORS.MUTED .. "[" .. tostring(act.time or "") .. "] " .. COLORS.RESET .. COLORS.VALUE .. tostring(act.text or "") .. COLORS.RESET)
                        end
                    end
                else
                    local repeatTag = (h.count and h.count > 1) and (" " .. COLORS.GOLD .. "(x" .. h.count .. ")" .. COLORS.RESET) or ""
                    table.insert(out, COLORS.MUTED .. "[" .. timeRange .. "] " .. mColor .. "[" .. hMindset .. "]" .. COLORS.RESET .. " " .. COLORS.VALUE .. tostring(h.thought or "") .. repeatTag .. COLORS.RESET)
                end
            end
        end
    end

    -- Episodic Memories
    table.insert(out, "\n" .. COLORS.SUBHEADER .. "[Recent Episodic Memories]:" .. COLORS.RESET)
    local mems = BotCache.memories or {}
    if #mems == 0 then
        table.insert(out, COLORS.MUTED .. "  No episodic memories logged yet." .. COLORS.RESET)
    else
        for _, m in ipairs(mems) do
            table.insert(out, COLORS.MUTED .. "- " .. COLORS.VALUE .. tostring(m or "") .. COLORS.RESET)
        end
    end

    return table.concat(out, "\n")
end

-- Refresh Master Frame and floating frames
RefreshMasterTab = function()
    -- Update floating panels if shown
    if AzerothFriendThoughtFrame and AzerothFriendThoughtFrame:IsShown() then
        SetContainerText(AzerothFriendThoughtFrameHTML, FormatThoughtsText())
    end
    if AzerothFriendStateFrame and AzerothFriendStateFrame:IsShown() then
        SetContainerText(AzerothFriendStateFrameHTML, FormatContextText())
    end
    if AzerothFriendActionFrame and AzerothFriendActionFrame:IsShown() then
        SetContainerText(AzerothFriendActionFrameHTML, FormatActionsText())
    end
    if AzerothFriendEnvFrame and AzerothFriendEnvFrame:IsShown() then
        SetContainerText(AzerothFriendEnvFrameHTML, FormatSurroundingsText())
    end
    if AzerothFriendMemoryFrame and AzerothFriendMemoryFrame:IsShown() then
        SetContainerText(AzerothFriendMemoryFrameHTML, FormatMindsetDebugText())
    end

    if not AzerothFriendMasterFrame or not AzerothFriendMasterFrame:IsShown() then return end

    local s = GetSettings()
    local tab = s.activeTab or 1
    if tab > 5 then tab = 5 end

    -- Reset button highlights
    for i = 1, 5 do
        local btn = _G["AzerothFriendMasterTab" .. i]
        if btn then
            if i == tab then
                btn:LockHighlight()
            else
                btn:UnlockHighlight()
            end
        end
    end
    -- Update Title & Subtitle Badge
    if AzerothFriendMasterFrameTitle then
        AzerothFriendMasterFrameTitle:SetText("AzerothFriend: " .. tostring(BotCache.name or "Companion") .. " (" .. tostring(BotCache.mindset or "IDLE") .. ")")
    end
    if AzerothFriendMasterFrameSubtitle then
        local tok = (BotCache.lastTokens and tonumber(BotCache.lastTokens.total)) or 0
        local badge
        if BotCache.activityLevel == "HIGH" then
            badge = string.format("|cFFFF2020[Tokens: %d | HIGH ACTIVITY SPIKE]|r", tok)
        elseif BotCache.activityLevel == "ELEVATED" then
            badge = string.format("|cFFFFFF00[Tokens: %d | Elevated Activity]|r", tok)
        else
            badge = string.format("|cFF00FF00[Tokens: %d | Normal]|r", tok)
        end
        AzerothFriendMasterFrameSubtitle:SetText(badge)
    end

    -- Update Claim switch from server telemetry (BotCache.claimed)
    if AzerothFriendBtnClaim then
        local isClaimed = (tostring(BotCache.claimed):upper() == "CLAIMED")
        if AzerothFriendBtnClaim.SetChecked then
            AzerothFriendBtnClaim:SetChecked(isClaimed and 1 or nil)
        end
        AzerothFriendBtnClaim:SetText(isClaimed and "|cff00ff00Claim: ON|r" or "Claim: OFF")
    end

    -- Update Autonomy switch from server telemetry. The switch never guesses:
    -- FRIEND_GOAL carries autonomy_enabled and goal_status straight from the database.
    if AzerothFriendBtnAutonomy then
        local goal = AFGetGoalState and AFGetGoalState() or nil
        local enabled = (goal and goal.enabled) and true or false
        if AzerothFriendBtnAutonomy.SetChecked then
            AzerothFriendBtnAutonomy:SetChecked(enabled and 1 or nil)
        end
        AzerothFriendBtnAutonomy:SetText(enabled and "|cff00e5ffAuto: ON|r" or "Auto: OFF")
    end

    -- Update Action Mode toggles from server telemetry (BotCache.actionMode)
    local actMode = (BotCache.actionMode or "travel"):lower()
    if AzerothFriendBtnModeCombat then
        if actMode == "combat" then
            AzerothFriendBtnModeCombat:SetText("|cFFFF4444[Combat]|r")
            AzerothFriendBtnModeCombat:LockHighlight()
        else
            AzerothFriendBtnModeCombat:SetText("Combat")
            AzerothFriendBtnModeCombat:UnlockHighlight()
        end
    end
    if AzerothFriendBtnModeTravel then
        if actMode == "travel" then
            AzerothFriendBtnModeTravel:SetText("|cFF00BFFF[Travel]|r")
            AzerothFriendBtnModeTravel:LockHighlight()
        else
            AzerothFriendBtnModeTravel:SetText("Travel")
            AzerothFriendBtnModeTravel:UnlockHighlight()
        end
    end
    if AzerothFriendBtnModeIdle then
        if actMode == "idle" then
            AzerothFriendBtnModeIdle:SetText("|cFF55FF55[Idle]|r")
            AzerothFriendBtnModeIdle:LockHighlight()
        else
            AzerothFriendBtnModeIdle:SetText("Idle")
            AzerothFriendBtnModeIdle:UnlockHighlight()
        end
    end
    if AzerothFriendBtnModeSocial then
        if actMode == "social" then
            AzerothFriendBtnModeSocial:SetText("|cFFDA70D6[Social]|r")
            AzerothFriendBtnModeSocial:LockHighlight()
        else
            AzerothFriendBtnModeSocial:SetText("Social")
            AzerothFriendBtnModeSocial:UnlockHighlight()
        end
    end

    -- Render active tab content
    if tab == 3 then
        if AzerothFriendMasterScrollFrame then AzerothFriendMasterScrollFrame:Hide() end
        if AzerothFriendApiPanel then AzerothFriendApiPanel:Hide() end
        if AzerothFriendActionsPanel then
            AzerothFriendActionsPanel:Show()
            local pStatus = _G["AzerothFriendActionsPipelineStatus"]
            if pStatus then
                local stLines = {}
                table.insert(stLines, COLORS.LABEL .. "Active Action: " .. COLORS.RESET .. COLORS.SUCCESS .. BotCache.activeAction:upper() .. COLORS.RESET ..
                             COLORS.MUTED .. " | Status: " .. COLORS.RESET .. COLORS.VALUE .. BotCache.actionStatus .. COLORS.RESET ..
                             COLORS.MUTED .. " | Plan Step: " .. COLORS.RESET .. COLORS.ORANGE .. BotCache.actionStep .. COLORS.RESET)
                if BotCache.lastActionError and BotCache.lastActionError ~= "" then
                    table.insert(stLines, COLORS.ALERT .. "[SUPPRESSED ERROR]: " .. COLORS.RESET .. COLORS.VALUE .. BotCache.lastActionError .. COLORS.RESET ..
                                 COLORS.MUTED .. " (" .. (BotCache.lastActionErrorTime or "recent") .. " | Blocked from chat: " .. (BotCache.errorCount or 1) .. "x)" .. COLORS.RESET)
                end
                table.insert(stLines, COLORS.LABEL .. "Movement Target: " .. COLORS.RESET .. COLORS.VALUE .. BotCache.movementTarget .. COLORS.RESET ..
                             COLORS.MUTED .. " | Next Queued: " .. COLORS.RESET .. COLORS.SUBHEADER .. BotCache.nextAction .. COLORS.RESET)
                pStatus:SetText(table.concat(stLines, "\n"))
            end
            -- Goals replace the old mindset toggles: the owner sets purpose, the
            -- planner picks the mindset and reports it read-only.
            local goal = AFGetGoalState and AFGetGoalState() or nil
            local goalStatus = _G["AzerothFriendGoalStatusText"]
            if goalStatus then
                local longGoal = (goal and goal.long_goal and goal.long_goal ~= "") and goal.long_goal or "not set"
                local shortGoal = (goal and goal.goal and goal.goal ~= "") and goal.goal or "not set"
                local status = (goal and goal.status) or "Unknown"
                local enabled = (goal and goal.enabled) and "ON" or "OFF"
                local actColor = COLORS.CYAN
                local actDesc = "+travel,+follow"
                if actMode == "combat" then
                    actColor = COLORS.ALERT
                    actDesc = "+grind,+combat,+loot"
                elseif actMode == "idle" then
                    actColor = COLORS.SUCCESS
                    actDesc = "+stay,-follow"
                elseif actMode == "social" then
                    actColor = "|cFFDA70D6"
                    actDesc = "+rpg,-grind"
                end
                goalStatus:SetText(
                    COLORS.SUBHEADER .. "ACTION MODE (Tactical Posture): " .. COLORS.RESET ..
                    actColor .. actMode:upper() .. COLORS.RESET .. COLORS.MUTED .. " (" .. actDesc .. ")" .. COLORS.RESET .. "\n" ..
                    COLORS.SUBHEADER .. "CORE PURPOSE (Long-Term): " .. COLORS.RESET .. "\n" ..
                    COLORS.VALUE .. longGoal .. COLORS.RESET .. "\n" ..
                    COLORS.SUBHEADER .. "CURRENT OBJECTIVE (Short-Term): " .. COLORS.RESET .. "\n" ..
                    COLORS.VALUE .. shortGoal .. COLORS.RESET .. "\n" ..
                    COLORS.MUTED .. "Status: " .. COLORS.RESET .. COLORS.VALUE .. status .. COLORS.RESET ..
                    COLORS.MUTED .. " | Autonomy: " .. COLORS.RESET .. COLORS.VALUE .. enabled .. COLORS.RESET ..
                    COLORS.MUTED .. " | Mindset (read-only): " .. COLORS.RESET .. COLORS.VALUE .. (BotCache.mindset or "IDLE") .. COLORS.RESET)
            end
        end
    elseif tab == 5 then
        if AzerothFriendActionsPanel then AzerothFriendActionsPanel:Hide() end
        if AzerothFriendMasterScrollFrame then AzerothFriendMasterScrollFrame:Hide() end
        if AzerothFriendApiPanel then AzerothFriendApiPanel:Show() end
        RefreshBotApiPanel()
    else
        if AzerothFriendActionsPanel then AzerothFriendActionsPanel:Hide() end
        if AzerothFriendApiPanel then AzerothFriendApiPanel:Hide() end
        if AzerothFriendMasterScrollFrame then AzerothFriendMasterScrollFrame:Show() end
        if tab == 1 then
            SetContainerText(AzerothFriendMasterHTML, FormatContextText())
        elseif tab == 2 then
            SetContainerText(AzerothFriendMasterHTML, FormatMindsetDebugText())
        elseif tab == 4 then
            SetContainerText(AzerothFriendMasterHTML, FormatThoughtsText())
        end
    end
end

-- Telemetry arrives in bursts (state, context, mindset, actions, env, thought,
-- memory, goal). Repainting the whole HUD for every packet is what made the
-- thoughts panel stutter, so message-driven repaints are coalesced into at most
-- one pass every 0.2s. User actions still refresh immediately.
local refreshQueued = false
local refreshElapsed = 0
RequestRefresh = function()
    refreshQueued = true
end

-- ----------------------------------------------------------------------------
-- Packet Processing
-- ----------------------------------------------------------------------------

local function ProcessMessage(msg)
    if type(msg) ~= "string" then return false end
    -- Shadow for the rest of this function: telemetry-driven repaints are coalesced.
    local function RefreshMasterTab()
        RequestRefresh()
    end
    if msg:match("^%[AF1%] ") then
        local decoded = AFDecodePacket and AFDecodePacket(msg)
        if decoded then ProcessMessage(decoded) end
        return true
    end
    if AFProcessExtension and AFProcessExtension(msg) then
        -- Goal/autonomy telemetry also drives the HUD switch, so repaint the master frame.
        RefreshMasterTab()
        return true
    end

    -- 1. [FRIEND_STATE]
    if msg:match("^%[FRIEND_STATE%]") then
        local raw = msg:sub(15):gsub("^%s+", "")
        for _, line in ipairs(UnflattenLines(raw)) do
            local k, v = line:match("^([^:]+):%s*(.+)$")
            if k and v then
                local kl = k:lower()
                if kl == "name" then BotCache.name = v
                elseif kl == "race" then BotCache.race = v
                elseif kl == "class" then BotCache.class = v
                elseif kl == "level" then BotCache.level = tonumber(v) or BotCache.level
                elseif kl == "health" then BotCache.health = v
                elseif kl == "power" then BotCache.power = v
                elseif kl == "zone" then BotCache.zone = v
                elseif kl == "subzone" then BotCache.subzone = v
                elseif kl == "coordinates" then BotCache.coords = v
                elseif kl == "combat" then BotCache.combat = v
                elseif kl == "target" then BotCache.target = v
                elseif kl == "spellbook" then BotCache.spellbook = v
                elseif kl == "casting" then BotCache.casting = v
                elseif kl == "actionmode" then BotCache.actionMode = v:lower()
                end
            end
        end
        RefreshMasterTab()
        return true

    -- 2. [FRIEND_CONTEXT] (Zero Token Self-Context)
    elseif msg:match("^%[FRIEND_CONTEXT%]") then
        local raw = msg:sub(17):gsub("^%s+", "")
        for _, line in ipairs(UnflattenLines(raw)) do
            local k, v = line:match("^([^:]+):%s*(.+)$")
            if k and v then
                local kl = k:lower()
                if kl == "bot" then BotCache.name = v
                elseif kl == "zonename" then BotCache.zone = v
                elseif kl == "areaname" then BotCache.subzone = v
                elseif kl == "outdoors" then BotCache.outdoors = v
                elseif kl == "ininn" then BotCache.inInn = v
                elseif kl == "resting" then BotCache.resting = v
                elseif kl == "swimming" then BotCache.swimming = v
                elseif kl == "mounted" then BotCache.mounted = v
                elseif kl == "mastername" then BotCache.masterName = v
                elseif kl == "masterdist" then BotCache.masterDist = tonumber(v) or 0
                elseif kl == "masterhp" then BotCache.masterHP = tonumber(v) or 100
                elseif kl == "mastercombat" then BotCache.masterCombat = v
                elseif kl == "mastertarget" then BotCache.masterTarget = v
                elseif kl == "mastertargethp" then BotCache.masterTargetHP = tonumber(v) or 0
                elseif kl == "mastertargetenemy" then BotCache.masterTargetEnemy = v
                elseif kl == "mastercasting" then BotCache.masterCasting = v
                elseif kl == "selfcontext" then
                    -- Parse item level (supports both avg_ilvl and avg_item_level)
                    local ilvl = v:match('"avg_ilvl":%s*(%d+)') or v:match('"avg_item_level":%s*(%d+)')
                    if ilvl then BotCache.ilvl = tonumber(ilvl) or 0 end

                    local free = v:match('"free_bag_slots":%s*(%d+)')
                    if free then BotCache.freeBags = tonumber(free) or 0 end
                    local tot = v:match('"total_bag_slots":%s*(%d+)')
                    if tot then BotCache.totalBags = tonumber(tot) or 0 end
                    local wat = v:match('"water_count":%s*(%d+)')
                    if wat then BotCache.waterCount = tonumber(wat) or 0 end
                    local fd = v:match('"food_count":%s*(%d+)')
                    if fd then BotCache.foodCount = tonumber(fd) or 0 end
                    local pot = v:match('"potions_count":%s*(%d+)')
                    if pot then BotCache.potionsCount = tonumber(pot) or 0 end

                    -- If explicit consumable counts were not provided, parse bag_items array
                    local bagJson = v:match('"bag_items":%s*(%b[])')
                    if bagJson and (not wat or not fd or not pot) then
                        local countedWater = 0
                        local countedFood = 0
                        local countedPot = 0
                        for iname, icnt in bagJson:gmatch('"name":%s*"([^"]+)",%s*"count":%s*(%d+)') do
                            local cnt = tonumber(icnt) or 1
                            local nl = iname:lower()
                            if nl:find("water") or nl:find("drink") or nl:find("milk") or nl:find("juice") or nl:find("tea") or nl:find("ale") or nl:find("wine") or nl:find("brew") then
                                countedWater = countedWater + cnt
                            elseif nl:find("potion") or nl:find("elixir") or nl:find("bandage") or nl:find("flask") or nl:find("remedy") then
                                countedPot = countedPot + cnt
                            else
                                countedFood = countedFood + cnt
                            end
                        end
                        if not wat then BotCache.waterCount = countedWater end
                        if not fd then BotCache.foodCount = countedFood end
                        if not pot then BotCache.potionsCount = countedPot end
                        if not tot or BotCache.totalBags == 0 then
                            BotCache.totalBags = 16
                            BotCache.freeBags = math.max(0, 16 - (countedWater + countedFood + countedPot > 0 and 4 or 0))
                        end
                    end

                    -- Extract quests (supports both complete boolean format and title string format)
                    BotCache.activeQuests = {}
                    for qid, qcomp in v:gmatch('{"id":%s*(%d+),%s*"complete":%s*(%a+)}') do
                        table.insert(BotCache.activeQuests, {id = qid, title = "Quest #" .. qid, complete = (qcomp == "true")})
                    end
                    if #BotCache.activeQuests == 0 then
                        for qid, qtitle in v:gmatch('{"id":%s*(%d+),%s*"title":%s*"([^"]+)"}') do
                            table.insert(BotCache.activeQuests, {id = qid, title = qtitle, complete = false})
                        end
                    end

                    -- Extract crowd_count if present
                    local crowd = v:match('"crowd_count":%s*(%d+)')
                    if crowd then
                        BotCache.crowdCount = tonumber(crowd)
                    end

                    -- Extract surroundings if nearby entities array is included in json
                    local nearbyJson = v:match('"nearby":%s*(%b[])')
                    if nearbyJson then
                        local foundSurroundings = {}
                        for itemStr in nearbyJson:gmatch('(%b{})') do
                            local ntype = itemStr:match('"type":%s*"([^"]+)"') or "ENTITY"
                            local nname = itemStr:match('"name":%s*"([^"]+)"') or "Unknown"
                            local ndist = itemStr:match('"dist":%s*([%d%.]+)') or "0"
                            local isFocus = itemStr:match('"focus":%s*true')
                            local prefix = isFocus and "FOCUS_TARGET: " or (ntype:upper() .. ": ")
                            table.insert(foundSurroundings, prefix .. nname .. " (" .. ndist .. "y)")
                        end
                        if #foundSurroundings > 0 then
                            BotCache.surroundings = foundSurroundings
                        end
                    end

                    -- Extract combat_spells (with SpellID and rank)
                    local spellsJson = v:match('"combat_spells":%s*(%b[])')
                    if spellsJson then
                        local foundSpells = {}
                        for itemStr in spellsJson:gmatch('(%b{})') do
                            local sid = itemStr:match('"id":%s*(%d+)')
                            local sname = itemStr:match('"name":%s*"([^"]+)"')
                            local srank = itemStr:match('"rank":%s*"([^"]+)"') or ""
                            if sid and sname then
                                table.insert(foundSpells, {
                                    id = tonumber(sid),
                                    name = sname,
                                    rank = srank
                                })
                            end
                        end
                        if #foundSpells > 0 then
                            BotCache.combatSpells = foundSpells
                        end
                    end

                    -- Extract playerbot strategies (combat and non-combat)
                    local pbJson = v:match('"playerbot":%s*(%b{})')
                    if pbJson then
                        local cbtJson = pbJson:match('"strategies_combat":%s*(%b[])')
                        if cbtJson then
                            local cbtList = {}
                            for sname in cbtJson:gmatch('"([^"]+)"') do
                                table.insert(cbtList, sname)
                            end
                            BotCache.combatStrategies = cbtList
                        end
                        local ncJson = pbJson:match('"strategies_non_combat":%s*(%b[])')
                        if ncJson then
                            local ncList = {}
                            for sname in ncJson:gmatch('"([^"]+)"') do
                                table.insert(ncList, sname)
                            end
                            BotCache.nonCombatStrategies = ncList
                        end
                    end
                end
            end
        end
        RefreshMasterTab()
        return true

    -- 3. [FRIEND_MINDSET]
    elseif msg:match("^%[FRIEND_MINDSET%]") then
        local raw = msg:sub(17):gsub("^%s+", "")
        for _, line in ipairs(UnflattenLines(raw)) do
            local k, v = line:match("^([^:]+):%s*(.+)$")
            if k and v then
                local kl = k:lower()
                if kl == "bot" then BotCache.name = v
                elseif kl == "claimed" then BotCache.claimed = v
                elseif kl == "bridge" then BotCache.bridgeEnabled = (v:upper() == "CONNECTED" or v == "1" or v:lower() == "true")
                elseif kl == "commitmentwindow" then BotCache.commitmentWindow = v
                elseif kl == "tokenssaved" then BotCache.tokensSaved = tonumber(v) or 0
                elseif kl == "coprocessed" then BotCache.coProcessed = tonumber(v) or 0
                elseif kl == "sensoryreused" then BotCache.sensoryReused = tonumber(v) or 0
                elseif kl == "thinkingcadence" then
                    BotCache.thinkingCadence = v:lower()
                    if AzerothFriendBtnThinking then
                        local tc = BotCache.thinkingCadence or "normal"
                        if tc == "low" then
                            AzerothFriendBtnThinking:SetText("Think: LOW")
                        elseif tc == "high" then
                            AzerothFriendBtnThinking:SetText("Think: HIGH")
                        else
                            AzerothFriendBtnThinking:SetText("Think: NORM")
                        end
                    end
                end
            end
        end
        RefreshMasterTab()
        return true

    -- 3b. [FRIEND_DIAG] (RAM-first transport, cache freshness, context size)
    elseif msg:match("^%[FRIEND_DIAG%]") then
        local raw = msg:sub(14):gsub("^%s+", "")
        for _, line in ipairs(UnflattenLines(raw)) do
            local k, v = line:match("^([^:]+):%s*(.+)$")
            if k and v then
                local kl = k:lower()
                if kl == "bot" then BotCache.name = v
                elseif kl == "transport" then BotCache.transport = v
                elseif kl == "session" then BotCache.transportSession = v
                elseif kl == "framessent" then BotCache.transportFrames = tonumber(v) or 0
                elseif kl == "refreshes" then BotCache.transportRefreshes = tonumber(v) or 0
                elseif kl == "cacheage" then BotCache.cacheAge = tonumber(v) or 0
                elseif kl == "contexttokens" then BotCache.contextTokens = tonumber(v) or 0
                elseif kl == "contextceiling" then BotCache.contextCeiling = tonumber(v) or 0
                elseif kl == "rambytes" then BotCache.ramBytes = tonumber(v) or 0
                elseif kl == "ramcachedbots" then BotCache.ramCachedBots = tonumber(v) or 0
                elseif kl == "summaryage" then BotCache.summaryAge = tonumber(v) or 0
                elseif kl == "rolloutstage" then BotCache.rolloutStage = tonumber(v) or 0
                elseif kl == "sqlcompat" then BotCache.sqlCompat = v
                elseif kl == "diagnostic" then BotCache.diagNote = v
                end
            end
        end
        RefreshMasterTab()
        return true

    -- 3c. [FRIEND_API] (server-owned Bot API v1 overview/catalogue/contract)
    elseif msg:match("^%[FRIEND_API%]") then
        local raw = msg:sub(13):gsub("^%s+", "")
        local fields = {}
        for _, line in ipairs(UnflattenLines(raw)) do
            local k, v = line:match("^([^:]+):%s*(.+)$")
            if k and v then fields[k:lower()] = v end
        end

        local view = (fields.view or "overview"):lower()
        if fields.bot then BotCache.name = fields.bot end
        if fields.apiversion then BotCache.apiVersion = tonumber(fields.apiversion) or BotCache.apiVersion end
        if fields.capabilityrevision then BotCache.capabilityRevision = tonumber(fields.capabilityrevision) or BotCache.capabilityRevision end
        if fields.execution then BotCache.apiExecution = fields.execution:upper() end
        if fields.ownertier then BotCache.apiOwnerTier = fields.ownertier:upper() end
        if fields.rawpassthrough then BotCache.apiRawPassthrough = fields.rawpassthrough:upper() end
        if fields.nativediscovery then BotCache.apiNativeDiscovery = fields.nativediscovery:upper() end
        if fields.curated then BotCache.apiCuratedCount = tonumber(fields.curated) or 0 end
        if fields.autonomous then BotCache.apiAutonomousCount = tonumber(fields.autonomous) or 0 end
        if fields.ownercommand then BotCache.apiOwnerCount = tonumber(fields.ownercommand) or 0 end
        if fields.gmmanual then BotCache.apiGmCount = tonumber(fields.gmmanual) or 0 end

        if view == "catalog" then
            BotCache.apiCatalog = {}
            BotCache.apiCatalogLoaded = true
            BotCache.apiCatalogFilter = fields.filter or "all"
            BotCache.apiCatalogTruncated = (fields.truncated or "NO"):upper() == "YES"
            for item in tostring(fields.catalog or ""):gmatch("[^;]+") do
                local name, category, authority = item:match("^([^~]+)~([^~]+)~([^~]+)$")
                if name then
                    table.insert(BotCache.apiCatalog, {
                        name = name,
                        category = category or "other",
                        authority = authority or "unknown"
                    })
                end
            end
            table.sort(BotCache.apiCatalog, function(a, b)
                if a.category == b.category then return a.name < b.name end
                return a.category < b.category
            end)
            BotCache.apiError = ""
        elseif view == "contract" then
            BotCache.apiContract = {
                apiVersion = tonumber(fields.apiversion) or BotCache.apiVersion,
                capabilityRevision = tonumber(fields.capabilityrevision) or BotCache.capabilityRevision,
                name = fields.name or "",
                category = fields.category or "",
                description = fields.description or "",
                authority = fields.authority or "unknown",
                bindingKind = fields.bindingkind or "unknown",
                nativeBinding = fields.nativebinding or "none",
                completion = fields.completion or "unknown",
                preconditions = fields.preconditions or "none",
                parameters = fields.parameters or "none",
                parameterSchema = fields.parameterschema or "",
                resultSchema = fields.resultschema or ""
            }
            BotCache.apiError = ""
            if AzerothFriendApiCapabilityEdit and fields.name then
                AzerothFriendApiCapabilityEdit:SetText(fields.name)
            end
        elseif view == "error" then
            BotCache.apiError = fields.error or "Bot API request failed."
        end

        RefreshMasterTab()
        return true

    -- 4. [FRIEND_THOUGHT]
    elseif msg:match("^%[FRIEND_THOUGHT%]") then
        local raw = msg:sub(17):gsub("^%s+", "")
        local newThought = nil
        local newMindset = nil
        local sawKnownField = false
        for _, line in ipairs(UnflattenLines(raw)) do
            local k, v = line:match("^([^:]+):%s*(.+)$")
            if k and v then
                local kl = k:lower()
                if kl:find("thought") then
                    BotCache.thought = v
                    newThought = v
                    sawKnownField = true
                elseif kl:find("speech") then
                    BotCache.speech = v
                    sawKnownField = true
                elseif kl:find("model") then
                    BotCache.model = v
                    sawKnownField = true
                elseif kl:find("token") then
                    BotCache.tokenUsage = v
                    local tot = tonumber(v:match("Total=(%d+)")) or 0
                    local p = tonumber(v:match("P=(%d+)")) or 0
                    local c = tonumber(v:match("C=(%d+)")) or 0
                    local r = tonumber(v:match("R=(%d+)")) or 0
                    if tot > 0 then
                        BotCache.lastTokens = { prompt = p, completion = c, reasoning = r, total = tot }
                        BotCache.sessionTokens = (BotCache.sessionTokens or 0) + tot
                        BotCache.sessionCalls = (BotCache.sessionCalls or 0) + 1
                        local now = GetTime()
                        if not BotCache.callTimestamps then BotCache.callTimestamps = {} end
                        table.insert(BotCache.callTimestamps, { time = now, tokens = tot })
                        for i = #BotCache.callTimestamps, 1, -1 do
                            if (now - BotCache.callTimestamps[i].time) > 60 then
                                table.remove(BotCache.callTimestamps, i)
                            end
                        end
                        local tokensLastMin = 0
                        for _, call in ipairs(BotCache.callTimestamps) do
                            tokensLastMin = tokensLastMin + (call.tokens or 0)
                        end
                        local callsLastMin = #BotCache.callTimestamps
                        if tot > 3000 or tokensLastMin > 5000 or callsLastMin >= 5 then
                            BotCache.activityLevel = "HIGH"
                            BotCache.activityReason = string.format("Spike: %d tok/min (%d calls/min)", tokensLastMin, callsLastMin)
                        elseif tokensLastMin > 2500 or callsLastMin >= 3 then
                            BotCache.activityLevel = "ELEVATED"
                            BotCache.activityReason = string.format("Elevated: %d tok/min (%d calls/min)", tokensLastMin, callsLastMin)
                        else
                            BotCache.activityLevel = "NORMAL"
                            BotCache.activityReason = string.format("Normal: %d tok/min (%d calls/min)", tokensLastMin, callsLastMin)
                        end
                    end
                    sawKnownField = true
                elseif kl:find("activity") then
                    local act = v:upper()
                    if act:find("HIGH") or act:find("SPIKE") then
                        BotCache.activityLevel = "HIGH"
                    elseif act:find("ELEVATED") then
                        BotCache.activityLevel = "ELEVATED"
                    end
                    sawKnownField = true
                elseif kl:find("heard") then
                    local chan, who, txt = v:match("^%[([^%]]+)%]%s*([^:]+):%s*(.+)$")
                    if chan and who and txt then
                        RecordChat(who, chan:lower(), txt)
                    else
                        RecordChat("Overheard", "say", v)
                    end
                    sawKnownField = true
                elseif kl:find("reasoning") or kl:find("deep") then
                    BotCache.deepReasoning = v
                    sawKnownField = true
                elseif kl:find("mindset") then
                    BotCache.mindset = v
                    newMindset = v
                    sawKnownField = true
                elseif kl:find("plan") then
                    BotCache.planId = v
                    sawKnownField = true
                elseif kl:find("event") then
                    BotCache.triggerEvent = v
                    sawKnownField = true
                end
            end
        end

        -- Deterministic replies are sometimes plain prose without field keys. They
        -- used to be dropped, which left the panel frozen on an old plan thought.
        if not sawKnownField then
            local prose = raw:gsub("^Bot:%s*[^|]*|", ""):gsub("^%s+", ""):gsub("%s+$", "")
            if prose ~= "" then
                BotCache.thought = prose
                newThought = prose
            end
        end

        -- Record into real-time thought timeline with grouping & encounter awareness
        if newThought and newThought ~= "" and newThought ~= "None" then
            local curMindset = (newMindset or BotCache.mindset or "IDLE"):upper()
            local nowTime = date("%H:%M:%S")
            local lastH = BotCache.thoughtHistory[#BotCache.thoughtHistory]
            -- [MAF-048] Clean and normalize thought text for deduplication comparison
            local cleanNew = newThought:gsub("^%s+", ""):gsub("[%s%.]+$", ""):lower()
            local cleanLast = lastH and tostring(lastH.thought or ""):gsub("^%s+", ""):gsub("[%s%.]+$", ""):lower() or ""

            if lastH and (lastH.thought == newThought or cleanNew == cleanLast) then
                -- Same thought repeated: increment repeat counter and update last observed timestamp
                lastH.count = (lastH.count or 1) + 1
                lastH.lastTime = nowTime
            elseif curMindset == "COMBAT" and lastH and lastH.mindset == "COMBAT" and lastH.isEncounter and (BotCache.target and BotCache.target ~= "None" and lastH.target == BotCache.target) then
                -- Same combat encounter target: group into encounter sequence
                lastH.count = (lastH.count or 1) + 1
                lastH.lastTime = nowTime
                if not lastH.actions then lastH.actions = {} end
                if #lastH.actions < 5 then
                    table.insert(lastH.actions, { time = nowTime, text = newThought })
                else
                    lastH.actions[#lastH.actions] = { time = nowTime, text = newThought }
                end
                lastH.thought = newThought
            else
                -- New timeline item
                local isCombat = (curMindset == "COMBAT")
                local entry = {
                    time = nowTime,
                    lastTime = nowTime,
                    mindset = curMindset,
                    thought = newThought,
                    count = 1,
                    isEncounter = isCombat,
                    target = isCombat and (BotCache.target or "Enemy") or nil,
                    actions = isCombat and { { time = nowTime, text = newThought } } or nil
                }
                table.insert(BotCache.thoughtHistory, entry)
                if #BotCache.thoughtHistory > 8 then
                    table.remove(BotCache.thoughtHistory, 1)
                end
            end
        end

        RefreshMasterTab()
        return true

    -- 5. [FRIEND_ACTIONS]
    elseif msg:match("^%[FRIEND_ACTIONS%]") then
        local raw = msg:sub(17):gsub("^%s+", "")
        BotCache.actionResult = ""
        BotCache.actionError = ""
        for _, line in ipairs(UnflattenLines(raw)) do
            local k, v = line:match("^([^:]+):%s*(.+)$")
            if k and v then
                local kl = k:lower()
                if kl == "active" or kl == "command" or kl == "capability" then BotCache.activeAction = v
                elseif kl == "status" then BotCache.actionStatus = v
                elseif kl == "step" then BotCache.actionStep = v
                elseif kl == "movement" then BotCache.movementTarget = v
                elseif kl == "next" then BotCache.nextAction = v
                elseif kl == "params" then BotCache.actionParams = v
                elseif kl == "result" then BotCache.actionResult = v
                elseif kl == "error" then
                    BotCache.actionError = v
                    BotCache.lastActionError = v
                    BotCache.lastActionErrorTime = date("%H:%M:%S")
                elseif kl == "apiversion" then BotCache.apiVersion = tonumber(v) or BotCache.apiVersion
                elseif kl == "authority" then BotCache.actionAuthority = v
                elseif kl == "bindingkind" then BotCache.actionBinding = v
                elseif kl == "nativebinding" then BotCache.actionNativeBinding = v
                elseif kl == "completion" then BotCache.actionCompletion = v
                elseif kl == "verified" then BotCache.actionVerified = v:lower()
                end
            end
        end
        RefreshMasterTab()
        return true

    -- 6. [FRIEND_ENV]
    elseif msg:match("^%[FRIEND_ENV%]") then
        local raw = msg:sub(13):gsub("^%s+", "")
        BotCache.surroundings = UnflattenLines(raw)
        RefreshMasterTab()
        return true

    -- 7. [FRIEND_MEMORY]
    elseif msg:match("^%[FRIEND_MEMORY%]") then
        local raw = msg:sub(16):gsub("^%s+", "")
        BotCache.memories = UnflattenLines(raw)
        RefreshMasterTab()
        return true

    -- 8. [FRIEND_CONFIG] (Server bot configuration sync)
    elseif msg:match("^%[FRIEND_CONFIG%]") then
        local raw = msg:match("^%[[^%]]+%]%s*(.*)$")
        for _, line in ipairs(UnflattenLines(raw)) do
            local k, v = line:match("^([^:]+):%s*(.+)$")
            if k and v then
                local kl = k:lower()
                if kl == "bot" or kl == "name" or kl == "controlledbot" or kl == "primarybot" then
                    BotCache.name = v
                end
            end
        end
        RefreshMasterTab()
        return true

    -- 9. [FRIEND_ERROR] (Direct Action & Assist Diagnostics from Server)
    elseif msg:match("^%[FRIEND_ERROR%]") then
        local raw = msg:match("^%[[^%]]+%]%s*(.*)$")
        for _, line in ipairs(UnflattenLines(raw)) do
            local k, v = line:match("^([^:]+):%s*(.+)$")
            if k and v then
                local kl = k:lower()
                if kl == "error" or kl == "err" or kl == "msg" then
                    BotCache.lastActionError = v
                    BotCache.lastActionErrorTime = date("%H:%M:%S")
                    BotCache.errorCount = (BotCache.errorCount or 0) + 1
                    if not BotCache.errorHistory then BotCache.errorHistory = {} end
                    table.insert(BotCache.errorHistory, { time = BotCache.lastActionErrorTime, error = v, bot = BotCache.name })
                    if #BotCache.errorHistory > 10 then table.remove(BotCache.errorHistory, 1) end
                elseif kl == "bot" and v ~= "" then
                    BotCache.name = v
                end
            end
        end
        RefreshMasterTab()
        return true
    end

    return false
end

-- ----------------------------------------------------------------------------
-- Bulletproof Chat Error Interceptor & AddMessage Hook
-- ----------------------------------------------------------------------------

HandleInterceptedActionError = function(rawText)
    if not rawText or type(rawText) ~= "string" then return false end
    local lower = rawText:lower()
    if (lower:find("assist") and lower:find("failed")) or
       lower:find(": failed") or lower:find("dps assist") or lower:find("tank assist") or
       lower:find(": impossible") or lower:find(": useless") or lower:find(": unknown action") or
       lower:find(": not enough mana") or lower:find(": out of range") or lower:find(": cannot cast") or
       lower:find(": spell not learned") or lower:find("unknown companion command") or
       lower:find("companion unavailable or not owned") or lower:find("invalid action") then
        local clean = rawText:gsub("|c%x%x%x%x%x%x%x%x", ""):gsub("|r", "")
        clean = clean:gsub("^%s+", ""):gsub("%s+$", "")
        BotCache.lastActionError = clean
        BotCache.lastActionErrorTime = date("%H:%M:%S")
        BotCache.errorCount = (BotCache.errorCount or 0) + 1
        if not BotCache.errorHistory then BotCache.errorHistory = {} end
        table.insert(BotCache.errorHistory, { time = BotCache.lastActionErrorTime, error = clean, bot = BotCache.name })
        if #BotCache.errorHistory > 10 then table.remove(BotCache.errorHistory, 1) end
        RefreshMasterTab()
        return true -- Suppress!
    end
    return false
end

-- Hook AddMessage on all active chat frames to guarantee suppression
local function HookChatFrame(frame)
    if not frame or not frame.AddMessage or frame._afHooked then return end
    frame._afHooked = true
    local orig = frame.AddMessage
    frame.AddMessage = function(self, text, r, g, b, id, ...)
        if HandleInterceptedActionError(text) then
            return -- Block completely from chat frame!
        end
        return orig(self, text, r, g, b, id, ...)
    end
end

local function HookAllChatFrames()
    for i = 1, NUM_CHAT_WINDOWS or 7 do
        local cf = _G["ChatFrame" .. i]
        if cf then HookChatFrame(cf) end
    end
end

local function ChatFilter(self, event, msg, sender, ...)
    if not msg then return false, msg, sender, ... end

    -- 1. Intercept and suppress playerbot action errors and companion diagnostics from ANY chat event
    if HandleInterceptedActionError(msg) then
        return true -- Completely suppress from game chat frame
    end

    -- 2. Anti-Spam: Throttle repeated bot emotes in chat (max 1 per 15s per sender in chat)
    if event == "CHAT_MSG_EMOTE" or event == "CHAT_MSG_TEXT_EMOTE" or event == "CHAT_MSG_MONSTER_EMOTE" then
        local now = GetTime()
        local senderKey = tostring(sender or "Unknown")
        if not BotCache.lastEmoteTimes then BotCache.lastEmoteTimes = {} end
        local lastTime = BotCache.lastEmoteTimes[senderKey] or 0
        if (now - lastTime) < 15 then
            return true -- Suppress rapid emote spam from chat
        end
        BotCache.lastEmoteTimes[senderKey] = now
    end

    if event ~= "CHAT_MSG_SYSTEM" then
        return false, msg, sender, ...
    end

    -- The server keeps its text-mode `.af catalog` output for console and
    -- non-addon users. When this HUD requested that same data, hide only the
    -- matching catalogue lines; the structured FRIEND_API copy is rendered in
    -- the Bot API tab instead.
    if GetTime() <= (BotCache.catalogSilenceUntil or 0) then
        local clean = msg:gsub("|c%x%x%x%x%x%x%x%x", ""):gsub("|r", "")
        if clean:find("^=== AzerothFriend Action Catalogue") or
           clean:find("^[%w_%-]+%(") or clean:find("^[%w_%-]+ v%d") or
           clean:find("^Authority:") or clean:find("^Preconditions:") or
           clean:find("^Parameters:") or clean:find("^Result:") or
           clean:find("^Tip: %.af catalog") or clean:find("^Tip: %.af run") or
           clean:find("^%.%.%. %(") or clean:find("^Unknown capability '") then
            return true
        end
    end

    if not (msg:match("^%[AF1%] ") or msg:match("^%[FRIEND_%u+%] ")) then
        return false, msg, sender, ...
    end

    local isTelemetry = ProcessMessage(msg)
    local settings = GetSettings()
    if isTelemetry and settings.filterChat then
        return true -- Clean chat: suppress raw telemetry packet
    end
    return false, msg, sender, ...
end

-- ----------------------------------------------------------------------------
-- UI Setup & Wire-up
-- ----------------------------------------------------------------------------

local function MakeDraggable(frame, titleBar)
    if not frame or not titleBar then return end
    titleBar:EnableMouse(true)
    titleBar:SetScript("OnMouseDown", function(self, btn)
        if btn == "LeftButton" and not GetSettings().locked then
            frame:StartMoving()
        end
    end)
    titleBar:SetScript("OnMouseUp", function(self, btn)
        frame:StopMovingOrSizing()
        SaveFramePosition(frame)
    end)
end

local function MakeResizable(frame, minW, minH)
    if not frame then return end
    frame:SetResizable(true)
    frame:SetMinResize(minW or 320, minH or 200)
    frame:SetClampedToScreen(true)

    local grip = CreateFrame("Button", nil, frame)
    grip:SetSize(16, 16)
    grip:SetPoint("BOTTOMRIGHT", -2, 2)
    grip:EnableMouse(true)

    grip:SetNormalTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Up")
    grip:SetHighlightTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Highlight")
    grip:SetPushedTexture("Interface\\ChatFrame\\UI-ChatIM-SizeGrabber-Down")

    grip:SetScript("OnMouseDown", function(self, btn)
        if btn == "LeftButton" and not GetSettings().locked then
            frame:StartSizing("BOTTOMRIGHT")
        end
    end)
    grip:SetScript("OnMouseUp", function(self, btn)
        frame:StopMovingOrSizing()
        SaveFramePosition(frame)
    end)
end

local function SetupFrame(frame, titleBar, minW, minH)
    if not frame then return end
    frame:SetMovable(true)
    frame:SetClampedToScreen(true)
    RestoreFramePosition(frame)
    MakeDraggable(frame, titleBar)
    MakeResizable(frame, minW, minH)
end

local function ApplyLayoutMode()
    local s = GetSettings()
    if s.layoutMode == "floating" then
        if AzerothFriendMasterFrame then AzerothFriendMasterFrame:Hide() end
        if AzerothFriendStateFrame then AzerothFriendStateFrame:Show() end
        if AzerothFriendThoughtFrame then AzerothFriendThoughtFrame:Show() end
        if AzerothFriendActionFrame then AzerothFriendActionFrame:Show() end
        if AzerothFriendEnvFrame then AzerothFriendEnvFrame:Show() end
        if AzerothFriendMemoryFrame then AzerothFriendMemoryFrame:Show() end
    else
        if AzerothFriendStateFrame then AzerothFriendStateFrame:Hide() end
        if AzerothFriendThoughtFrame then AzerothFriendThoughtFrame:Hide() end
        if AzerothFriendActionFrame then AzerothFriendActionFrame:Hide() end
        if AzerothFriendEnvFrame then AzerothFriendEnvFrame:Hide() end
        if AzerothFriendMemoryFrame then AzerothFriendMemoryFrame:Hide() end
        if AzerothFriendMasterFrame then
            AzerothFriendMasterFrame:Show()
        end
    end
    RefreshMasterTab()
end

-- Minimap Button Creation & Dragging
local function SetupMinimapButton()
    local btn = CreateFrame("Button", "AzerothFriendMinimapButton", Minimap)
    btn:SetSize(32, 32)
    btn:SetFrameStrata("MEDIUM")
    btn:SetFrameLevel(8)
    btn:EnableMouse(true)
    btn:SetMovable(true)

    -- Icon
    local icon = btn:CreateTexture(nil, "BACKGROUND")
    icon:SetSize(20, 20)
    icon:SetPoint("CENTER", 0, 0)
    icon:SetTexture("Interface\\Icons\\INV_Misc_Book_09")
    btn.icon = icon

    -- Border
    local border = btn:CreateTexture(nil, "OVERLAY")
    border:SetSize(52, 52)
    border:SetPoint("TOPLEFT", 0, 0)
    border:SetTexture("Interface\\Minimap\\MiniMap-TrackingBorder")

    local function UpdateMinimapPos()
        local s = GetSettings()
        local angle = math.rad(s.minimapPos or 45)
        local x = math.cos(angle) * 80
        local y = math.sin(angle) * 80
        btn:ClearAllPoints()
        btn:SetPoint("CENTER", Minimap, "CENTER", x, y)
    end

    btn:RegisterForDrag("LeftButton")
    btn:SetScript("OnDragStart", function(self)
        self.isDragging = true
        self:SetScript("OnUpdate", function(self)
            local mx, my = Minimap:GetCenter()
            local cx, cy = GetCursorPosition()
            local scale = UIParent:GetEffectiveScale()
            cx, cy = cx / scale, cy / scale
            local angle = math.deg(math.atan2(cy - my, cx - mx))
            if angle < 0 then angle = angle + 360 end
            GetSettings().minimapPos = angle
            UpdateMinimapPos()
        end)
    end)
    btn:SetScript("OnDragStop", function(self)
        self.isDragging = false
        self:SetScript("OnUpdate", nil)
    end)

    btn:SetScript("OnClick", function(self, button)
        if button == "RightButton" then
            -- Toggle layout mode
            local s = GetSettings()
            s.layoutMode = (s.layoutMode == "tabbed") and "floating" or "tabbed"
            print(COLORS.HEADER .. "AzerothFriend UI: " .. COLORS.RESET .. "Layout mode set to " .. COLORS.WARNING .. s.layoutMode .. COLORS.RESET)
            ApplyLayoutMode()
        else
            -- Toggle Master Frame
            if AzerothFriendMasterFrame:IsShown() then
                AzerothFriendMasterFrame:Hide()
            else
                AzerothFriendMasterFrame:Show()
                RefreshMasterTab()
            end
        end
    end)

    btn:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_LEFT")
        GameTooltip:AddLine("AzerothFriend Companion HUD", 1, 0.82, 0)
        GameTooltip:AddLine("Active Bot: " .. COLORS.SUCCESS .. BotCache.name .. COLORS.RESET, 1, 1, 1)
        GameTooltip:AddLine("Mindset: " .. COLORS.CYAN .. BotCache.mindset .. COLORS.RESET, 1, 1, 1)
        GameTooltip:AddLine("Tokens Saved: " .. COLORS.SUCCESS .. "~" .. BotCache.tokensSaved .. COLORS.RESET, 1, 1, 1)
        GameTooltip:AddLine(" ", 1, 1, 1)
        GameTooltip:AddLine("|cff00ccffLeft-Click:|r Toggle Master HUD", 0.7, 0.7, 0.7)
        GameTooltip:AddLine("|cff00ccffRight-Click:|r Toggle Tabbed / Floating mode", 0.7, 0.7, 0.7)
        GameTooltip:AddLine("|cff00ccffDrag:|r Move icon around minimap", 0.7, 0.7, 0.7)
        GameTooltip:Show()
    end)
    btn:SetScript("OnLeave", function(self)
        GameTooltip:Hide()
    end)

    UpdateMinimapPos()
    if not GetSettings().showMinimap then
        btn:Hide()
    end
end

-- Initialize Master HUD and wire buttons
local function InitializeMasterFrame()
    SetupFrame(AzerothFriendMasterFrame, AzerothFriendMasterFrameTitleBar, 620, 380)
    -- Layouts saved before the autonomy switch was added are narrower than the
    -- control bar needs; widen them instead of letting the switch clip.
    if AzerothFriendMasterFrame:GetWidth() < 620 then
        AzerothFriendMasterFrame:SetWidth(620)
    end

    -- Dedicated Interactive Actions Panel (Tab 4) with Checkbox Catalog
    local actionsPanel = CreateFrame("Frame", "AzerothFriendActionsPanel", AzerothFriendMasterFrame)
    actionsPanel:SetPoint("TOPLEFT", AzerothFriendMasterFrame, "TOPLEFT", 14, -62)
    actionsPanel:SetPoint("BOTTOMRIGHT", AzerothFriendMasterFrame, "BOTTOMRIGHT", -24, 38)
    actionsPanel:Hide()

    local pipelineHeader = actionsPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    pipelineHeader:SetPoint("TOPLEFT", 4, -2)
    pipelineHeader:SetText(COLORS.HEADER .. "Active Action Execution Pipeline" .. COLORS.RESET)

    local pipelineStatus = actionsPanel:CreateFontString("AzerothFriendActionsPipelineStatus", "OVERLAY", "GameFontHighlightSmall")
    pipelineStatus:SetPoint("TOPLEFT", pipelineHeader, "BOTTOMLEFT", 0, -4)
    pipelineStatus:SetJustifyH("LEFT")
    pipelineStatus:SetWidth(460)

    -- The old mindset toggles are gone. They only fired a one-shot command and
    -- tracked their own client-side state, so they never reflected what the server
    -- was doing. In their place: the companion's purpose and its current objective.
    local goalHeader = actionsPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    goalHeader:SetPoint("TOPLEFT", pipelineStatus, "BOTTOMLEFT", 0, -12)
    goalHeader:SetText(COLORS.SUBHEADER .. "Companion Goals (the agent's enduring purpose):" .. COLORS.RESET)

    local goalHelp = actionsPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    goalHelp:SetPoint("TOPLEFT", goalHeader, "BOTTOMLEFT", 0, -2)
    goalHelp:SetText("Long-term guides everything; the agent derives and adopts its own short-term objectives from it.")

    local longLabel = actionsPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    longLabel:SetPoint("TOPLEFT", goalHelp, "BOTTOMLEFT", 0, -8)
    longLabel:SetText(COLORS.LABEL .. "Long-term (overall purpose):" .. COLORS.RESET)

    local longEdit = CreateFrame("EditBox", "AzerothFriendLongGoalEdit", actionsPanel, "InputBoxTemplate")
    longEdit:SetHeight(22)
    longEdit:SetPoint("TOPLEFT", longLabel, "BOTTOMLEFT", 4, -4)
    longEdit:SetPoint("TOPRIGHT", actionsPanel, "TOPRIGHT", -120, -0)
    longEdit:SetAutoFocus(false)
    longEdit:SetMaxLetters(2000)
    longEdit:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

    local longSet = CreateFrame("Button", nil, actionsPanel, "UIPanelButtonTemplate")
    longSet:SetSize(96, 22)
    longSet:SetText("Set purpose")
    longSet:SetPoint("LEFT", longEdit, "RIGHT", 6, 0)
    longSet:SetScript("OnClick", function()
        local text = (longEdit:GetText() or ""):gsub("^%s+", ""):gsub("%s+$", "")
        if text == "" then return end
        SendChatMessage(".af goal longterm " .. text)
        longEdit:ClearFocus()
        print(COLORS.SUCCESS .. "AzerothFriend: " .. COLORS.RESET .. "Long-term goal sent: " .. text)
    end)

    local shortLabel = actionsPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlight")
    shortLabel:SetPoint("TOPLEFT", longEdit, "BOTTOMLEFT", -4, -10)
    shortLabel:SetText(COLORS.LABEL .. "Short-term (what it is doing now):" .. COLORS.RESET)

    local shortEdit = CreateFrame("EditBox", "AzerothFriendShortGoalEdit", actionsPanel, "InputBoxTemplate")
    shortEdit:SetHeight(22)
    shortEdit:SetPoint("TOPLEFT", shortLabel, "BOTTOMLEFT", 4, -4)
    shortEdit:SetPoint("TOPRIGHT", actionsPanel, "TOPRIGHT", -120, -0)
    shortEdit:SetAutoFocus(false)
    shortEdit:SetMaxLetters(255)
    shortEdit:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

    local shortSet = CreateFrame("Button", nil, actionsPanel, "UIPanelButtonTemplate")
    shortSet:SetSize(96, 22)
    shortSet:SetText("Set objective")
    shortSet:SetPoint("LEFT", shortEdit, "RIGHT", 6, 0)
    shortSet:SetScript("OnClick", function()
        local text = (shortEdit:GetText() or ""):gsub("^%s+", ""):gsub("%s+$", "")
        if text == "" then return end
        SendChatMessage(".af goal set " .. text)
        shortEdit:ClearFocus()
        print(COLORS.SUCCESS .. "AzerothFriend: " .. COLORS.RESET .. "Short-term goal sent: " .. text)
    end)

    local goalStatusText = actionsPanel:CreateFontString("AzerothFriendGoalStatusText", "OVERLAY", "GameFontHighlightSmall")
    goalStatusText:SetPoint("TOPLEFT", shortEdit, "BOTTOMLEFT", -4, -12)
    goalStatusText:SetJustifyH("LEFT")
    goalStatusText:SetWidth(520)
    goalStatusText:SetText(COLORS.MUTED .. "Awaiting goal telemetry..." .. COLORS.RESET)

    local footerNote = actionsPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    footerNote:SetPoint("BOTTOMLEFT", actionsPanel, "BOTTOMLEFT", 4, 4)
    footerNote:SetText(COLORS.MUTED .. "* Goals persist on the server. Mindset is chosen by the agent and shown read-only." .. COLORS.RESET)

    -- Dedicated Bot API v1 panel. Inputs only dispatch existing server commands;
    -- every contract, permission flag and result shown below comes from telemetry.
    local apiPanel = CreateFrame("Frame", "AzerothFriendApiPanel", AzerothFriendMasterFrame)
    apiPanel:SetPoint("TOPLEFT", AzerothFriendMasterFrame, "TOPLEFT", 14, -62)
    apiPanel:SetPoint("BOTTOMRIGHT", AzerothFriendMasterFrame, "BOTTOMRIGHT", -24, 38)
    apiPanel:Hide()

    local apiHeader = apiPanel:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    apiHeader:SetPoint("TOPLEFT", 4, -2)
    apiHeader:SetText(COLORS.HEADER .. "Playerbot Semantic Bot API v1" .. COLORS.RESET)

    local apiStatus = apiPanel:CreateFontString("AzerothFriendApiStatusText", "OVERLAY", "GameFontHighlightSmall")
    apiStatus:SetPoint("TOPLEFT", apiHeader, "BOTTOMLEFT", 0, -3)
    apiStatus:SetJustifyH("LEFT")
    apiStatus:SetWidth(570)
    apiStatus:SetText(COLORS.MUTED .. "Awaiting server Bot API telemetry..." .. COLORS.RESET)

    local capabilityLabel = apiPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    capabilityLabel:SetPoint("TOPLEFT", apiStatus, "BOTTOMLEFT", 0, -8)
    capabilityLabel:SetText(COLORS.LABEL .. "Capability" .. COLORS.RESET)

    local capabilityEdit = CreateFrame("EditBox", "AzerothFriendApiCapabilityEdit", apiPanel, "InputBoxTemplate")
    capabilityEdit:SetSize(190, 22)
    capabilityEdit:SetPoint("LEFT", capabilityLabel, "RIGHT", 10, 0)
    capabilityEdit:SetAutoFocus(false)
    capabilityEdit:SetMaxLetters(64)
    capabilityEdit:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

    local describeButton = CreateFrame("Button", nil, apiPanel, "UIPanelButtonTemplate")
    describeButton:SetSize(82, 22)
    describeButton:SetPoint("LEFT", capabilityEdit, "RIGHT", 6, 0)
    describeButton:SetText("Describe")
    describeButton:SetScript("OnClick", function()
        local capability = Trim(capabilityEdit:GetText()):lower()
        if capability == "" or not capability:match("^[%w_%-]+$") then
            print(COLORS.ALERT .. "AzerothFriend: Enter one exact capability name from the index." .. COLORS.RESET)
            return
        end
        BotCache.catalogSilenceUntil = GetTime() + 6
        SendChatMessage(".af catalog describe " .. capability .. " " .. tostring(BotCache.name or ""))
        capabilityEdit:ClearFocus()
    end)

    local refreshCatalog = CreateFrame("Button", nil, apiPanel, "UIPanelButtonTemplate")
    refreshCatalog:SetSize(105, 22)
    refreshCatalog:SetPoint("LEFT", describeButton, "RIGHT", 6, 0)
    refreshCatalog:SetText("Refresh Catalog")
    refreshCatalog:SetScript("OnClick", RequestBotApiCatalog)

    local paramsLabel = apiPanel:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
    paramsLabel:SetPoint("TOPLEFT", capabilityLabel, "BOTTOMLEFT", 0, -10)
    paramsLabel:SetText(COLORS.LABEL .. "JSON params" .. COLORS.RESET)

    local paramsEdit = CreateFrame("EditBox", "AzerothFriendApiParamsEdit", apiPanel, "InputBoxTemplate")
    paramsEdit:SetSize(355, 22)
    paramsEdit:SetPoint("LEFT", paramsLabel, "RIGHT", 10, 0)
    paramsEdit:SetAutoFocus(false)
    paramsEdit:SetMaxLetters(180)
    paramsEdit:SetText("{}")
    paramsEdit:SetScript("OnEscapePressed", function(self) self:ClearFocus() end)

    local runButton = CreateFrame("Button", "AzerothFriendApiRunButton", apiPanel, "UIPanelButtonTemplate")
    runButton:SetSize(105, 22)
    runButton:SetPoint("LEFT", paramsEdit, "RIGHT", 6, 0)
    runButton:SetText("Shift + Run")
    runButton:SetScript("OnClick", function()
        if not IsShiftKeyDown() then
            print(COLORS.WARNING .. "AzerothFriend: Hold Shift while clicking Run to confirm a manual capability call." .. COLORS.RESET)
            return
        end
        local capability = Trim(capabilityEdit:GetText()):lower()
        local params = Trim(paramsEdit:GetText())
        if capability == "" or not capability:match("^[%w_%-]+$") then
            print(COLORS.ALERT .. "AzerothFriend: Enter one exact capability name before running it." .. COLORS.RESET)
            return
        end
        if params == "" then params = "{}" end
        if not params:match("^%b{}$") then
            print(COLORS.ALERT .. "AzerothFriend: Parameters must be one JSON object, for example {\"seconds\":5}." .. COLORS.RESET)
            return
        end
        SendChatMessage(".af run " .. tostring(BotCache.name or "") .. " " .. capability .. " " .. params)
        capabilityEdit:ClearFocus()
        paramsEdit:ClearFocus()
        print(COLORS.SUCCESS .. "AzerothFriend: Manual Bot API request sent; awaiting server outcome telemetry." .. COLORS.RESET)
    end)
    runButton:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_TOP")
        GameTooltip:AddLine("Manual Bot API Call", 1, 0.82, 0)
        GameTooltip:AddLine("Hold Shift and click to dispatch through .af run.", 1, 1, 1)
        GameTooltip:AddLine("The server still enforces ownership, control revision, preconditions, and playerbot binding.", 0.7, 0.7, 0.7, true)
        GameTooltip:Show()
    end)
    runButton:SetScript("OnLeave", function() GameTooltip:Hide() end)

    local apiHint = apiPanel:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
    apiHint:SetPoint("TOPLEFT", paramsLabel, "BOTTOMLEFT", 0, -7)
    apiHint:SetText("Describe before Run. Dispatch acknowledgement is not proof of a verified world-state change.")

    local apiScroll = CreateFrame("ScrollFrame", "AzerothFriendApiContractScroll", apiPanel, "UIPanelScrollFrameTemplate")
    apiScroll:SetPoint("TOPLEFT", apiHint, "BOTTOMLEFT", 0, -7)
    apiScroll:SetPoint("BOTTOMRIGHT", apiPanel, "BOTTOMRIGHT", -24, 2)
    local apiBodyFrame = CreateFrame("Frame", "AzerothFriendApiContractBodyFrame", apiScroll)
    apiBodyFrame:SetSize(570, 180)
    local apiBody = apiBodyFrame:CreateFontString("AzerothFriendApiContractBodyFrameText", "OVERLAY", "GameFontHighlightSmall")
    apiBody:SetPoint("TOPLEFT", 0, 0)
    apiBody:SetWidth(570)
    apiBody:SetJustifyH("LEFT")
    apiBody:SetJustifyV("TOP")
    apiScroll:SetScrollChild(apiBodyFrame)

    -- Tab buttons (5 tabs)
    for i = 1, 5 do
        local tabIndex = i
        local btn = _G["AzerothFriendMasterTab" .. tabIndex]
        if btn then
            btn:SetScript("OnClick", function()
                GetSettings().activeTab = tabIndex
                if AzerothFriendMasterScrollFrame then
                    AzerothFriendMasterScrollFrame:SetVerticalScroll(0)
                end
                if tabIndex == 5 and AzerothFriendApiContractScroll then
                    AzerothFriendApiContractScroll:SetVerticalScroll(0)
                    if not BotCache.apiCatalogLoaded then RequestBotApiCatalog() end
                end
                RefreshMasterTab()
            end)
        end
    end

    -- Bottom Remote Control Buttons & Switches (Zero Tokens)
    if AzerothFriendBtnClaim then
        AzerothFriendBtnClaim:SetScript("OnClick", function(self)
            local botName = BotCache.name
            local targetName = UnitName("target")
            if targetName and UnitIsPlayer("target") and not UnitIsUnit("target", "player") then
                botName = targetName
                BotCache.name = targetName
            end

            local isCurrentlyClaimed = (tostring(BotCache.claimed):upper() == "CLAIMED")
            if isCurrentlyClaimed then
                BotCache.claimed = "RELEASED"
                if self.SetChecked then self:SetChecked(nil) end
                self:SetText("Claim: OFF")
                SendChatMessage(".af release " .. (botName or ""))
                print(COLORS.HEADER .. "AzerothFriend: " .. COLORS.RESET .. "Reconnected " .. (botName or "companion") .. " to the bridge with autonomy still off.")
            else
                -- Mutual Exclusivity: Turning Claim ON cancels Autonomy
                local goal = AFGetGoalState and AFGetGoalState() or nil
                if goal and goal.enabled then
                    goal.enabled = false
                    SendChatMessage(".af autonomy off")
                    if AzerothFriendBtnAutonomy then
                        if AzerothFriendBtnAutonomy.SetChecked then AzerothFriendBtnAutonomy:SetChecked(nil) end
                        AzerothFriendBtnAutonomy:SetText("Auto: OFF")
                    end
                end

                BotCache.claimed = "CLAIMED"
                if self.SetChecked then self:SetChecked(1) end
                self:SetText("|cff00ff00Claim: ON|r")
                SendChatMessage(".af claim " .. (botName or ""))
                print(COLORS.HEADER .. "AzerothFriend: " .. COLORS.RESET .. "Claimed " .. (botName or "companion") .. " (AzerothFriend bridge disconnected, 0 tokens).")
            end

            RefreshMasterTab()
        end)
        AzerothFriendBtnClaim:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine("Companion Bridge Disconnect (Claim - 0 Tokens)", 1, 0.82, 0)
            GameTooltip:AddLine("Click: Lock/release companion to direct manual control", 1, 1, 1)
            GameTooltip:AddLine("Status: " .. tostring(BotCache.claimed or "RELEASED"), 0.7, 0.9, 1)
            GameTooltip:AddLine("Disconnects AzerothFriend entirely. Native playerbot commands remain available.", 0.7, 0.7, 0.7)
            GameTooltip:Show()
        end)
        AzerothFriendBtnClaim:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    -- Thinking Cadence Switch (Zero Tokens)
    if AzerothFriendBtnThinking then
        AzerothFriendBtnThinking:SetScript("OnClick", function(self)
            local current = BotCache.thinkingCadence or "normal"
            local nextTier = "normal"
            if current == "low" then
                nextTier = "normal"
            elseif current == "normal" then
                nextTier = "high"
            elseif current == "high" then
                nextTier = "low"
            end
            BotCache.thinkingCadence = nextTier
            SendChatMessage(".af thinking " .. nextTier)
            if nextTier == "low" then
                self:SetText("Think: LOW")
            elseif nextTier == "high" then
                self:SetText("Think: HIGH")
            else
                self:SetText("Think: NORM")
            end
            print(COLORS.HEADER .. "AzerothFriend: " .. COLORS.RESET .. "Thinking cadence set to " .. COLORS.VALUE .. nextTier:upper() .. COLORS.RESET .. ".")
        end)
        AzerothFriendBtnThinking:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine("Thinking Cadence (Zero Tokens)", 1, 0.82, 0)
            GameTooltip:AddLine("Click: Cycle thinking cadence (Low -> Normal -> High)", 1, 1, 1)
            local current = (BotCache.thinkingCadence or "normal"):upper()
            local interval = current == "LOW" and "20s" or (current == "HIGH" and "4s" or "10s")
            GameTooltip:AddLine("Current Tier: " .. current .. " (" .. interval .. ")", 0.7, 0.9, 1)
            GameTooltip:AddLine("Controls idle background thought updates when no world events occur.", 0.7, 0.7, 0.7)
            GameTooltip:Show()
        end)
        AzerothFriendBtnThinking:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    -- Action Mode Toggles (0 Tokens)
    if AzerothFriendBtnModeCombat then
        AzerothFriendBtnModeCombat:SetScript("OnClick", function()
            SendChatMessage(".af mode " .. (BotCache.name or "") .. " combat")
            BotCache.actionMode = "combat"
            print(COLORS.ALERT .. "AzerothFriend: " .. COLORS.RESET .. "Action mode set to " .. COLORS.ALERT .. "COMBAT" .. COLORS.RESET .. " (+grind, +combat, +loot).")
            RefreshMasterTab()
        end)
        AzerothFriendBtnModeCombat:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine("Combat Action Mode (0 Tokens)", 1, 0.3, 0.3)
            GameTooltip:AddLine("Engages aggressive stance: +grind, +combat, +loot.", 1, 1, 1)
            GameTooltip:AddLine("Bot prioritizes combat, assists master, and clears nearby hostiles.", 0.7, 0.7, 0.7)
            GameTooltip:Show()
        end)
        AzerothFriendBtnModeCombat:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    if AzerothFriendBtnModeTravel then
        AzerothFriendBtnModeTravel:SetScript("OnClick", function()
            SendChatMessage(".af mode " .. (BotCache.name or "") .. " travel")
            BotCache.actionMode = "travel"
            print(COLORS.SUCCESS .. "AzerothFriend: " .. COLORS.RESET .. "Action mode set to " .. COLORS.CYAN .. "TRAVEL" .. COLORS.RESET .. " (+travel, +follow, -stay).")
            RefreshMasterTab()
        end)
        AzerothFriendBtnModeTravel:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine("Travel Action Mode (0 Tokens)", 0.2, 0.8, 1)
            GameTooltip:AddLine("Engages traveling stance: +travel, +follow, -stay.", 1, 1, 1)
            GameTooltip:AddLine("Bot maintains close formation with master and pathfinds along route.", 0.7, 0.7, 0.7)
            GameTooltip:Show()
        end)
        AzerothFriendBtnModeTravel:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    if AzerothFriendBtnModeIdle then
        AzerothFriendBtnModeIdle:SetScript("OnClick", function()
            SendChatMessage(".af mode " .. (BotCache.name or "") .. " idle")
            BotCache.actionMode = "idle"
            print(COLORS.WARNING .. "AzerothFriend: " .. COLORS.RESET .. "Action mode set to " .. COLORS.SUCCESS .. "IDLE" .. COLORS.RESET .. " (+stay, -follow).")
            RefreshMasterTab()
        end)
        AzerothFriendBtnModeIdle:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine("Idle Action Mode (0 Tokens)", 0.3, 1, 0.3)
            GameTooltip:AddLine("Engages resting stance: +stay, -follow.", 1, 1, 1)
            GameTooltip:AddLine("Bot holds position, eats/drinks to recover, and sits/relaxes.", 0.7, 0.7, 0.7)
            GameTooltip:Show()
        end)
        AzerothFriendBtnModeIdle:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    if AzerothFriendBtnModeSocial then
        AzerothFriendBtnModeSocial:SetScript("OnClick", function()
            SendChatMessage(".af mode " .. (BotCache.name or "") .. " social")
            BotCache.actionMode = "social"
            print(COLORS.HEADER .. "AzerothFriend: " .. COLORS.RESET .. "Action mode set to " .. "|cFFDA70D6SOCIAL|r" .. COLORS.RESET .. " (+rpg, -grind).")
            RefreshMasterTab()
        end)
        AzerothFriendBtnModeSocial:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine("Social Action Mode (0 Tokens)", 0.85, 0.44, 0.84)
            GameTooltip:AddLine("Engages social/RPG stance: +rpg, -grind.", 1, 1, 1)
            GameTooltip:AddLine("Bot interacts with nearby NPCs, vendors, and friendly adventurers.", 0.7, 0.7, 0.7)
            GameTooltip:Show()
        end)
        AzerothFriendBtnModeSocial:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    if AzerothFriendBtnFollow then
        AzerothFriendBtnFollow:SetScript("OnClick", function()
            SendChatMessage(".af action follow")
            SendChatMessage("follow", "WHISPER", nil, BotCache.name)
            print(COLORS.SUCCESS .. "AzerothFriend: Follow command sent to " .. BotCache.name .. "." .. COLORS.RESET)
        end)
    end

    if AzerothFriendBtnHold then
        AzerothFriendBtnHold:SetScript("OnClick", function()
            SendChatMessage(".af action stop")
            SendChatMessage("stay", "WHISPER", nil, BotCache.name)
            print(COLORS.WARNING .. "AzerothFriend: Hold position command sent to " .. BotCache.name .. "." .. COLORS.RESET)
        end)
    end

    if AzerothFriendBtnAttack then
        AzerothFriendBtnAttack:SetScript("OnClick", function()
            SendChatMessage(".af action attack")
            SendChatMessage("attack", "WHISPER", nil, BotCache.name)
            print(COLORS.ALERT .. "AzerothFriend: Attack target command sent to " .. BotCache.name .. "." .. COLORS.RESET)
        end)
    end

    if AzerothFriendBtnLoot then
        AzerothFriendBtnLoot:SetScript("OnClick", function()
            SendChatMessage(".af action loot all")
            SendChatMessage("loot all", "WHISPER", nil, BotCache.name)
            print(COLORS.ORANGE .. "AzerothFriend: Loot all command sent to " .. BotCache.name .. "." .. COLORS.RESET)
        end)
    end

    if AzerothFriendBtnRest then
        AzerothFriendBtnRest:SetScript("OnClick", function()
            SendChatMessage(".af action eat_drink")
            SendChatMessage("drink", "WHISPER", nil, BotCache.name)
            print(COLORS.CYAN .. "AzerothFriend: Rest/eat_drink command sent to " .. BotCache.name .. "." .. COLORS.RESET)
        end)
    end

    if AzerothFriendBtnSync then
        AzerothFriendBtnSync:SetScript("OnClick", function()
            local botName = BotCache.name
            if not botName or botName == "" then
                local targetName = UnitName("target")
                if targetName and UnitIsPlayer("target") and not UnitIsUnit("target", "player") then
                    botName = targetName
                    BotCache.name = targetName
                end
            end
            if botName and botName ~= "" then
                SendChatMessage(".af inspect " .. botName)
            else
                SendChatMessage(".af inspect")
            end
            RequestBotApiCatalog()
            print(COLORS.SUCCESS .. "AzerothFriend: Zero-token context & catalog sync requested for " .. (botName or "companion") .. "." .. COLORS.RESET)
        end)
    end

    if AzerothFriendBtnDebug then
        AzerothFriendBtnDebug:SetScript("OnClick", function()
            if IsShiftKeyDown() then
                SendChatMessage(".af debug")
            else
                local s = GetSettings()
                s.activeTab = 2
                if AzerothFriendMasterScrollFrame then
                    AzerothFriendMasterScrollFrame:SetVerticalScroll(0)
                end
                if AzerothFriendMasterFrame and not AzerothFriendMasterFrame:IsShown() then
                    AzerothFriendMasterFrame:Show()
                end
                RefreshMasterTab()
            end
        end)
        AzerothFriendBtnDebug:SetScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine("Mindset & Telemetry Debug", 1, 0.82, 0)
            GameTooltip:AddLine("Click: Switch to Mindset/Debug dashboard tab", 1, 1, 1)
            GameTooltip:AddLine("Shift-Click: Send .af debug to server chat", 0.7, 0.7, 0.7)
            GameTooltip:Show()
        end)
        AzerothFriendBtnDebug:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end

    if AzerothFriendBtnAutonomy then
        AzerothFriendBtnAutonomy:SetScript("OnClick", function(self)
            local goal = AFGetGoalState and AFGetGoalState() or nil
            local enabled = (goal and goal.enabled) and true or false
            local botName = BotCache.name or "companion"

            if enabled then
                SendChatMessage(".af autonomy off")
                if goal then goal.enabled = false end
                if self.SetChecked then self:SetChecked(nil) end
                self:SetText("Auto: OFF")
                print(COLORS.WARNING .. "AzerothFriend: " .. COLORS.RESET .. "Autonomy OFF requested for " ..
                      botName .. " (waiting for server confirmation)." .. COLORS.RESET)
            else
                -- Mutual Exclusivity: Turning Autonomy ON cancels Claim
                local isCurrentlyClaimed = (tostring(BotCache.claimed):upper() == "CLAIMED")
                if isCurrentlyClaimed then
                    BotCache.claimed = "RELEASED"
                    SendChatMessage(".af release " .. (BotCache.name or ""))
                    if AzerothFriendBtnClaim then
                        if AzerothFriendBtnClaim.SetChecked then AzerothFriendBtnClaim:SetChecked(nil) end
                        AzerothFriendBtnClaim:SetText("Claim: OFF")
                    end
                end

                SendChatMessage(".af autonomy on")
                if goal then goal.enabled = true end
                if self.SetChecked then self:SetChecked(1) end
                self:SetText("|cff00e5ffAuto: ON|r")
                print(COLORS.SUCCESS .. "AzerothFriend: " .. COLORS.RESET .. "Autonomy ON requested for " ..
                      botName .. " (requires a focused goal)." .. COLORS.RESET)
            end

            RefreshMasterTab()
        end)
        AzerothFriendBtnAutonomy:SetScript("OnEnter", function(self)
            local goal = AFGetGoalState and AFGetGoalState() or nil
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine("Companion Autonomy", 1, 0.82, 0)
            GameTooltip:AddLine("Click: Opt in/out of self-directed autonomous play", 1, 1, 1)
            GameTooltip:AddLine("Goal: " .. ((goal and goal.goal and goal.goal ~= "" and goal.goal) or "none set"), 0.7, 0.9, 1, true)
            GameTooltip:AddLine("Status: " .. ((goal and goal.status) or "Unknown"), 0.7, 0.7, 0.7)
            GameTooltip:AddLine("Cancels Claim. Set a goal first with /af goals", 0.5, 0.5, 0.5)
            GameTooltip:Show()
        end)
        AzerothFriendBtnAutonomy:SetScript("OnLeave", function() GameTooltip:Hide() end)
    end
    if AzerothFriendMasterFrameTitleBar then
        AzerothFriendMasterFrameTitleBar:EnableMouse(true)
        AzerothFriendMasterFrameTitleBar:HookScript("OnEnter", function(self)
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            GameTooltip:AddLine("AzerothFriend Token & Activity Status", 1, 0.82, 0)
            local tok = BotCache.lastTokens or { total = 0, prompt = 0, completion = 0, reasoning = 0 }
            local actLevel = BotCache.activityLevel or "NORMAL"
            local r, g, b = 0, 1, 0
            if actLevel == "HIGH" then r, g, b = 1, 0.2, 0.2
            elseif actLevel == "ELEVATED" then r, g, b = 1, 1, 0 end
            GameTooltip:AddLine("Activity Level: " .. actLevel, r, g, b)
            GameTooltip:AddLine(BotCache.activityReason or "Normal activity", 0.7, 0.7, 0.7)
            GameTooltip:AddLine(string.format("Latest Call: %d tok (Prompt: %d, Completion: %d, Reasoning: %d)", tok.total, tok.prompt, tok.completion, tok.reasoning), 0.8, 1, 0.8)
            GameTooltip:AddLine(string.format("Session Total: %d tokens across %d calls", BotCache.sessionTokens or 0, BotCache.sessionCalls or 0), 0.9, 0.9, 0.9)
            GameTooltip:Show()
        end)
        AzerothFriendMasterFrameTitleBar:HookScript("OnLeave", function() GameTooltip:Hide() end)
    end

    local s = GetSettings()
    if s.showMaster then
        AzerothFriendMasterFrame:Show()
        RefreshMasterTab()
    else
        AzerothFriendMasterFrame:Hide()
    end
end

-- Initialize all components
local function InitializeUI()
    local settings = GetSettings()

    -- Floating panels
    SetupFrame(AzerothFriendStateFrame, AzerothFriendStateFrameTitleBar, 260, 140)
    SetupFrame(AzerothFriendThoughtFrame, AzerothFriendThoughtFrameTitleBar, 260, 120)
    SetupFrame(AzerothFriendActionFrame, AzerothFriendActionFrameTitleBar, 260, 100)
    SetupFrame(AzerothFriendEnvFrame, AzerothFriendEnvFrameTitleBar, 260, 120)
    SetupFrame(AzerothFriendMemoryFrame, AzerothFriendMemoryFrameTitleBar, 260, 140)

    -- Master HUD & Minimap
    InitializeMasterFrame()
    SetupMinimapButton()
    HookAllChatFrames()

    if settings.layoutMode == "floating" then
        ApplyLayoutMode()
    end
end

-- ----------------------------------------------------------------------------
-- Event Listener Frame
-- ----------------------------------------------------------------------------

local listener = CreateFrame("Frame", "AzerothFriendEventListener")
listener:RegisterEvent("ADDON_LOADED")
listener:RegisterEvent("PLAYER_ENTERING_WORLD")
listener:RegisterEvent("CHAT_MSG_SYSTEM")
listener:RegisterEvent("CHAT_MSG_ADDON")
listener:RegisterEvent("CHAT_MSG_WHISPER")
listener:RegisterEvent("CHAT_MSG_WHISPER_INFORM")
listener:RegisterEvent("CHAT_MSG_SAY")
listener:RegisterEvent("CHAT_MSG_YELL")
listener:RegisterEvent("CHAT_MSG_PARTY")
listener:RegisterEvent("CHAT_MSG_PARTY_LEADER")
listener:RegisterEvent("CHAT_MSG_EMOTE")
listener:RegisterEvent("CHAT_MSG_TEXT_EMOTE")
listener:RegisterEvent("CHAT_MSG_MONSTER_SAY")
listener:RegisterEvent("CHAT_MSG_MONSTER_YELL")
listener:RegisterEvent("CHAT_MSG_MONSTER_EMOTE")

listener:SetScript("OnEvent", function(self, event, ...)
    if event == "ADDON_LOADED" then
        local loadedAddon = ...
        if loadedAddon == ADDON_NAME then
            InitializeUI()
            HookAllChatFrames()
        end
    elseif event == "PLAYER_ENTERING_WORLD" then
        HookAllChatFrames()
        ChatFrame_AddMessageEventFilter("CHAT_MSG_SYSTEM", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_WHISPER", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_WHISPER_INFORM", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_SAY", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_YELL", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_PARTY", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_PARTY_LEADER", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_MONSTER_SAY", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_MONSTER_YELL", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_MONSTER_EMOTE", ChatFilter)
        ChatFrame_AddMessageEventFilter("CHAT_MSG_EMOTE", ChatFilter)
        SendChatMessage(".af inspect")
    elseif event == "CHAT_MSG_SYSTEM" then
        local message = ...
        ProcessMessage(message)
    elseif event == "CHAT_MSG_ADDON" then
        -- Addon messages carry (prefix, payload, channel, sender). Other addons
        -- broadcast localisation tables and protocol frames through this channel;
        -- they are transport, never dialogue. Only our own legacy prefix is parsed.
        local prefix, payload = ...
        if prefix == ADDON_PREFIX and type(payload) == "string" then
            ProcessMessage(payload)
        end
        return
    elseif event:find("^CHAT_MSG_") and event ~= "CHAT_MSG_SYSTEM" then
        local msg, sender = ...
        local chan = "say"
        if event == "CHAT_MSG_PARTY" or event == "CHAT_MSG_PARTY_LEADER" then
            chan = "party"
        elseif event == "CHAT_MSG_WHISPER" or event == "CHAT_MSG_WHISPER_INFORM" then
            chan = "whisper"
        elseif event == "CHAT_MSG_YELL" then
            chan = "yell"
        elseif event == "CHAT_MSG_MONSTER_SAY" then
            chan = "npc-say"
        elseif event == "CHAT_MSG_MONSTER_YELL" then
            chan = "npc-yell"
        elseif event == "CHAT_MSG_MONSTER_EMOTE" or event == "CHAT_MSG_EMOTE" or event == "CHAT_MSG_TEXT_EMOTE" then
            chan = "emote"
        end
        RecordChat(sender, chan, msg)
    end
end)

-- Coalesced repaint loop: at most one HUD render per 0.2s no matter how many
-- telemetry packets land in between.
listener:SetScript("OnUpdate", function(_, elapsed)
    if not refreshQueued then return end
    refreshElapsed = refreshElapsed + elapsed
    if refreshElapsed < 0.2 then return end
    refreshElapsed = 0
    refreshQueued = false
    RefreshMasterTab()
end)

-- ----------------------------------------------------------------------------
-- Slash Commands
-- ----------------------------------------------------------------------------

SLASH_AZEROTHFRIEND1 = "/af"
SLASH_AZEROTHFRIEND2 = "/azerothfriend"
SLASH_AZEROTHFRIEND3 = "/friend"

SlashCmdList["AZEROTHFRIEND"] = function(arg)
    if AFHandleSlash and AFHandleSlash(arg or "") then return end
    local cmd = (arg or ""):lower():gsub("^%s+", ""):gsub("%s+$", "")
    local s = GetSettings()

    if cmd == "" or cmd == "hud" or cmd == "master" then
        if AzerothFriendMasterFrame:IsShown() then
            AzerothFriendMasterFrame:Hide()
        else
            AzerothFriendMasterFrame:Show()
            RefreshMasterTab()
        end
    elseif cmd == "context" or cmd == "env" or cmd == "radar" or cmd == "surroundings" then
        s.activeTab = 1
        if AzerothFriendMasterScrollFrame then AzerothFriendMasterScrollFrame:SetVerticalScroll(0) end
        if s.layoutMode == "floating" then
            if AzerothFriendStateFrame then AzerothFriendStateFrame:Show() end
            if AzerothFriendEnvFrame then AzerothFriendEnvFrame:Show() end
        else
            if AzerothFriendMasterFrame then AzerothFriendMasterFrame:Show() end
        end
        RefreshMasterTab()
    elseif cmd == "debug" or cmd == "mindset" then
        s.activeTab = 2
        if AzerothFriendMasterScrollFrame then AzerothFriendMasterScrollFrame:SetVerticalScroll(0) end
        if s.layoutMode == "floating" then
            if AzerothFriendMemoryFrame then AzerothFriendMemoryFrame:Show() end
        else
            if AzerothFriendMasterFrame then AzerothFriendMasterFrame:Show() end
        end
        RefreshMasterTab()
    elseif cmd == "actions" then
        s.activeTab = 3
        if AzerothFriendMasterScrollFrame then AzerothFriendMasterScrollFrame:SetVerticalScroll(0) end
        if s.layoutMode == "floating" then
            if AzerothFriendActionFrame then AzerothFriendActionFrame:Show() end
        else
            if AzerothFriendMasterFrame then AzerothFriendMasterFrame:Show() end
        end
        RefreshMasterTab()
    elseif cmd == "thoughts" or cmd == "thought" or cmd == "memory" then
        s.activeTab = 4
        if AzerothFriendMasterScrollFrame then AzerothFriendMasterScrollFrame:SetVerticalScroll(0) end
        if s.layoutMode == "floating" then
            if AzerothFriendThoughtFrame then AzerothFriendThoughtFrame:Show() end
        else
            if AzerothFriendMasterFrame then AzerothFriendMasterFrame:Show() end
        end
        RefreshMasterTab()
    elseif cmd == "api" or cmd == "botapi" or cmd == "catalog" then
        s.activeTab = 5
        if AzerothFriendApiContractScroll then AzerothFriendApiContractScroll:SetVerticalScroll(0) end
        if AzerothFriendMasterFrame then AzerothFriendMasterFrame:Show() end
        RequestBotApiCatalog()
        RefreshMasterTab()
    elseif cmd == "minimap" then
        s.showMinimap = not s.showMinimap
        if AzerothFriendMinimapButton then
            if s.showMinimap then AzerothFriendMinimapButton:Show() else AzerothFriendMinimapButton:Hide() end
        end
        print(COLORS.HEADER .. "AzerothFriend UI: " .. COLORS.RESET .. "Minimap button " .. (s.showMinimap and COLORS.SUCCESS .. "enabled" or COLORS.MUTED .. "disabled") .. COLORS.RESET)
    elseif cmd:find("^action%s*") or cmd:find("^act%s*") then
        local subAction = cmd:gsub("^actions?%s*", ""):gsub("^act%s*", "")
        if subAction == "" or subAction == "help" then
            s.activeTab = 3
            AzerothFriendMasterFrame:Show()
            RefreshMasterTab()
        else
            SendChatMessage(".af action " .. subAction)
            if BotCache.name and BotCache.name ~= "" then
                SendChatMessage(subAction, "WHISPER", nil, BotCache.name)
            end
            print(COLORS.SUCCESS .. "AzerothFriend: Sent action command '" .. subAction .. "' to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
        end
    elseif cmd == "attack" then
        SendChatMessage(".af action attack")
        SendChatMessage("attack", "WHISPER", nil, BotCache.name)
        print(COLORS.ALERT .. "AzerothFriend: Attack target command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "follow" then
        SendChatMessage(".af action follow")
        SendChatMessage("follow", "WHISPER", nil, BotCache.name)
        print(COLORS.SUCCESS .. "AzerothFriend: Follow command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "hold" or cmd == "stay" or cmd == "stop" then
        SendChatMessage(".af action stop")
        SendChatMessage("stay", "WHISPER", nil, BotCache.name)
        print(COLORS.WARNING .. "AzerothFriend: Hold position command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "flee" then
        SendChatMessage(".af action flee")
        SendChatMessage("flee", "WHISPER", nil, BotCache.name)
        print(COLORS.ALERT .. "AzerothFriend: Flee command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "loot" or cmd == "lootall" or cmd == "loot all" then
        SendChatMessage(".af action loot all")
        SendChatMessage("loot all", "WHISPER", nil, BotCache.name)
        print(COLORS.ORANGE .. "AzerothFriend: Loot all command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "rest" or cmd == "drink" or cmd == "eat" then
        SendChatMessage(".af action eat_drink")
        SendChatMessage("drink", "WHISPER", nil, BotCache.name)
        print(COLORS.CYAN .. "AzerothFriend: Rest command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd:find("^rpg%s*") then
        local rpgArg = cmd:gsub("^rpg%s*", "")
        local fullRpg = (rpgArg ~= "") and ("rpg " .. rpgArg) or "rpg ?"
        SendChatMessage(".af action " .. fullRpg)
        SendChatMessage(fullRpg, "WHISPER", nil, BotCache.name)
        print(COLORS.SUBHEADER .. "AzerothFriend: RPG quest command '" .. fullRpg .. "' sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "accept" then
        SendChatMessage(".af action accept")
        SendChatMessage("accept", "WHISPER", nil, BotCache.name)
        print(COLORS.SUCCESS .. "AzerothFriend: Accept quest command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd:find("^reward%s*") then
        local rewIdx = cmd:gsub("^reward%s*", "")
        SendChatMessage(".af action reward " .. rewIdx)
        SendChatMessage("reward " .. rewIdx, "WHISPER", nil, BotCache.name)
        print(COLORS.SUCCESS .. "AzerothFriend: Choose reward command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "co" or cmd == "cooldowns" then
        SendChatMessage(".af action co")
        SendChatMessage("co", "WHISPER", nil, BotCache.name)
        print(COLORS.WARNING .. "AzerothFriend: Cooldowns toggled for " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "open" or cmd == "open items" then
        SendChatMessage(".af action open items")
        SendChatMessage("open items", "WHISPER", nil, BotCache.name)
        print(COLORS.CYAN .. "AzerothFriend: Open items command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "trainer" or cmd == "learn" then
        SendChatMessage(".af action trainer learn")
        SendChatMessage("trainer learn", "WHISPER", nil, BotCache.name)
        print(COLORS.CYAN .. "AzerothFriend: Trainer learn command sent to " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd:find("^mode%s*") then
        local targetMode = cmd:gsub("^mode%s*", ""):lower():match("^%s*(.-)%s*$")
        if targetMode ~= "" then
            SendChatMessage(".af mode " .. (BotCache.name or "") .. " " .. targetMode)
            BotCache.actionMode = targetMode
            print(COLORS.SUCCESS .. "AzerothFriend: " .. COLORS.RESET .. "Action mode set to " .. targetMode:upper() .. " for " .. (BotCache.name or "companion") .. ".")
            RefreshMasterTab()
        else
            print(COLORS.HEADER .. "Usage: /af mode <combat|travel|idle|social>" .. COLORS.RESET)
        end
    elseif cmd == "combat" or cmd == "travel" or cmd == "idle" or cmd == "social" then
        SendChatMessage(".af mode " .. (BotCache.name or "") .. " " .. cmd)
        BotCache.actionMode = cmd
        print(COLORS.SUCCESS .. "AzerothFriend: " .. COLORS.RESET .. "Action mode set to " .. cmd:upper() .. " for " .. (BotCache.name or "companion") .. ".")
        RefreshMasterTab()
    elseif cmd == "claim" then
        SendChatMessage(".af claim " .. (BotCache.name or ""))
        print(COLORS.HEADER .. "AzerothFriend: " .. COLORS.RESET .. "Claimed " .. (BotCache.name or "companion") .. " until release (AzerothFriend bridge disconnected).")
        RefreshMasterTab()
    elseif cmd == "release" then
        SendChatMessage(".af release " .. (BotCache.name or ""))
        print(COLORS.HEADER .. "AzerothFriend: " .. COLORS.RESET .. "Reconnected " .. (BotCache.name or "companion") .. " with autonomy still off.")
        RefreshMasterTab()
    elseif cmd:find("^mindset%s*") then
        local targetMindset = cmd:gsub("^mindset%s*", ""):upper()
        if targetMindset ~= "" then
            SendChatMessage(".af action " .. targetMindset:lower())
            print(COLORS.SUCCESS .. "AzerothFriend: Switched mindset to " .. targetMindset .. " for " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
            RefreshMasterTab()
        end
    elseif cmd == "sync" or cmd == "inspect" then
        SendChatMessage(".af inspect " .. (BotCache.name or ""))
        print(COLORS.SUCCESS .. "AzerothFriend: Triggered zero-token context sync for " .. (BotCache.name or "companion") .. "." .. COLORS.RESET)
    elseif cmd == "layout" then
        s.layoutMode = (s.layoutMode == "tabbed") and "floating" or "tabbed"
        print(COLORS.HEADER .. "AzerothFriend UI: " .. COLORS.RESET .. "Layout mode set to " .. COLORS.WARNING .. s.layoutMode .. COLORS.RESET)
        ApplyLayoutMode()
    elseif cmd == "reset" then
        AzerothFriendMasterFrame:ClearAllPoints()
        AzerothFriendMasterFrame:SetPoint("CENTER", UIParent, "CENTER", 0, 50)
        AzerothFriendMasterFrame:SetSize(660, 440)
        s.frames = {}
        SaveFramePosition(AzerothFriendMasterFrame)
        print(COLORS.SUCCESS .. "AzerothFriend UI: Frame position reset." .. COLORS.RESET)
    else
        print(COLORS.HEADER .. "=== AzerothFriend Companion UI Commands ===" .. COLORS.RESET)
        print(COLORS.SUBHEADER .. "/af" .. COLORS.RESET .. " - Toggle unified Master HUD")
        print(COLORS.SUBHEADER .. "/af mode <combat|travel|idle|social>" .. COLORS.RESET .. " - Tactical action mode toggle (0 tokens)")
        print(COLORS.SUBHEADER .. "/af attack | /af follow | /af stay | /af flee" .. COLORS.RESET .. " - Instant tactical combat controls")
        print(COLORS.SUBHEADER .. "/af loot [all] | /af open" .. COLORS.RESET .. " - Loot corpses / open items")
        print(COLORS.SUBHEADER .. "/af rpg [id|?] | /af accept | /af reward [1-6]" .. COLORS.RESET .. " - Autonomous quest commands")
        print(COLORS.SUBHEADER .. "/af co | /af trainer | /af rest" .. COLORS.RESET .. " - Cooldowns, class trainer, downtime")
        print(COLORS.SUBHEADER .. "/af action [command]" .. COLORS.RESET .. " - Dispatch any indexed catalog action")
        print(COLORS.SUBHEADER .. "/af api | /af catalog" .. COLORS.RESET .. " - Open Bot API contracts, authority, and verified outcomes")
        print(COLORS.SUBHEADER .. "/af claim | /af release" .. COLORS.RESET .. " - Toggle companion agency lock")
        print(COLORS.SUBHEADER .. "/af goal set [text] | /af autonomy on|off" .. COLORS.RESET .. " - Focused goal & self-directed play")
        print(COLORS.SUBHEADER .. "/af context | /af debug | /af sync" .. COLORS.RESET .. " - Zero-token inspection & sync")
        print(COLORS.SUBHEADER .. "/af minimap | /af layout | /af reset" .. COLORS.RESET .. " - UI layout & minimap toggle")
    end
end
