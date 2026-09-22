-- AzerothFriend protocol v1. Lua 5.1 / WoW 3.3.5 compatible.
local pending, completed = {}, {}
function AFDecodePacket(message)
    local id, index, count, hex = message:match("^%[AF1%] (%d+):(%d+):(%d+):(%x+)$")
    index, count = tonumber(index), tonumber(count)
    if not id or not index or not count or count > 512 or index < 1 or index > count or #hex > 180 or #hex % 2 ~= 0 then return end
    local now = GetTime()
    for key, value in pairs(pending) do if now - value.time > 10 then pending[key] = nil end end
    for key, value in pairs(completed) do if now - value > 10 then completed[key] = nil end end
    if completed[id] then return end
    local packet = pending[id] or {time = now, count = count, parts = {}, received = 0}
    if packet.count ~= count then pending[id] = nil; return end
    if not packet.parts[index] then packet.received = packet.received + 1 end
    packet.parts[index] = hex
    pending[id] = packet
    if packet.received ~= count then return end
    pending[id], completed[id] = nil, now
    return (table.concat(packet.parts):gsub("%x%x", function(pair) return string.char(tonumber(pair, 16)) end))
end

-- Decode the server's JSON string fields, including escaped quotes and delimiters.
function AFDecodeObject(text)
    local result, i = {}, 1
    local function skip() while text:sub(i,i):match("%s") do i = i + 1 end end
    local function quoted()
        if text:sub(i,i) ~= '"' then error('Expected string') end
        i = i + 1
        local out = {}
        while i <= #text do
            local c = text:sub(i,i); i = i + 1
            if c == '"' then return table.concat(out) end
            if c == '\\' then
                c = text:sub(i,i); i = i + 1
                local escapes = {n='\n', r='\r', t='\t', b='\b', f='\f', ['"']='"', ['\\']='\\', ['/']='/'}
                if not escapes[c] then error('Unsupported escape') end
                c = escapes[c]
            end
            out[#out+1] = c
        end
        error('Unterminated string')
    end
    local ok = pcall(function()
        skip(); if text:sub(i,i) ~= '{' then error('Expected object') end; i = i + 1
        while true do
            skip(); if text:sub(i,i) == '}' then return end
            local key = quoted(); skip()
            if text:sub(i,i) ~= ':' then error('Expected colon') end; i = i + 1; skip()
            if text:sub(i,i) == '"' then result[key] = quoted()
            elseif text:sub(i,i+3) == 'true' then result[key] = true; i = i + 4
            elseif text:sub(i,i+4) == 'false' then result[key] = false; i = i + 5
            else error('Unexpected value') end
            skip(); local c = text:sub(i,i); i = i + 1
            if c == '}' then return end
            if c ~= ',' then error('Expected comma') end
        end
    end)
    return ok and result or nil
end
