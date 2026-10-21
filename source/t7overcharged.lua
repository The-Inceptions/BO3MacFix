-- BO3MacFix's stand-in for T7Overcharged, a Windows-only DLL that workshop mods
-- (e.g. All-around Enhancement) load for extra Lua libraries.
-- BO3MacFix runs this when a mod loads the DLL, passing in a few native helpers.
local nativeCreateDirectory, nativeDirectoryExists, nativeClipboardGet, nativeClipboardSet, nativePrint,
    nativeHttpGet, nativeHttpResult, logMissing = ...

if EnableGlobals then
    EnableGlobals()
end

local function noop()
end

local function log(message)
    nativePrint("[T7Overcharged] " .. tostring(message))
end

-- fills in a library table, leaving anything that already exists alone
local function provide(library, members)
    for name, implementation in pairs(members) do
        if library[name] == nil then
            library[name] = implementation
        end
    end
    -- functions we don't know about do nothing rather than erroring
    if getmetatable(library) == nil then
        setmetatable(library, { __index = function() return noop end })
    end
    return library
end

local function isSet(value)
    return value ~= nil and value ~= false and value ~= 0 and value ~= "0" and value ~= "false"
end

local function setup()
    local io = require("io")

    local function fileExists(path)
        local file = io.open(tostring(path), "rb")
        if file then
            file:close()
            return true
        end
        return false
    end

    local function readFile(path)
        local file = io.open(tostring(path), "rb")
        if not file then
            return nil
        end
        local contents = file:read("*a")
        file:close()
        return contents
    end

    local function writeFile(path, contents, append)
        local file = io.open(tostring(path), append and "ab" or "wb")
        if not file then
            return false
        end
        file:write(tostring(contents or ""))
        file:close()
        return true
    end

    FileIO = provide(FileIO or {}, {
        FileExists = fileExists,
        ReadFile = readFile,
        WriteFile = function(path, contents, append)
            return writeFile(path, contents, isSet(append))
        end,
        -- like Windows' CopyFile, the third argument means "fail if the destination exists"
        Copy = function(source, destination, failIfExists)
            if isSet(failIfExists) and fileExists(destination) then
                return false
            end
            local contents = readFile(source)
            if contents == nil then
                return false
            end
            return writeFile(destination, contents, false)
        end,
        FileSize = function(path)
            local file = io.open(tostring(path), "rb")
            if not file then
                return 0
            end
            local size = file:seek("end")
            file:close()
            return size or 0
        end,
        DirectoryExists = function(path)
            return nativeDirectoryExists(tostring(path))
        end,
        CreateDirectory = function(path)
            return nativeCreateDirectory(tostring(path))
        end,
        ClipboardGet = function()
            return nativeClipboardGet()
        end,
        ClipboardSet = function(text)
            nativeClipboardSet(tostring(text or ""))
        end,
        Wine = function()
            return false
        end,
    })

    Console = provide(Console or {}, {
        Print = function(...)
            local parts = {}
            for i = 1, select("#", ...) do
                parts[i] = tostring((select(i, ...)))
            end
            log(table.concat(parts, " "))
        end,
        PrintFile = function(name, ...)
            Console.Print(...)
        end,
        ShowExternalConsole = noop,
    })

    -- Axios: GET requests for AAE's version notes and weather when AAEOnline = 1 (BO3MacFix only fetches from a few
    -- public hosts). Posting to Discord webhooks and anything needing T7Overcharged's built-in keys does nothing.
    Axios = provide(Axios or {}, {
        GetUpdate = function(url, key)
            if type(url) == "string" and type(key) == "string" then
                return nativeHttpGet(url, key)
            end
            return false
        end,
        GetRetrieve = function(key)
            if type(key) == "string" then
                return nativeHttpResult(key)
            end
            return nil
        end,
    })
    DiscordRPC = provide(DiscordRPC or {}, {
        IsAvailable = function()
            return false
        end,
    })
    MainLUA = provide(MainLUA or {}, {})
    winOs = provide(winOs or {}, {})
    T7Overcharged = provide(T7Overcharged or {}, {})
    UIErrorHash = provide(UIErrorHash or {}, {})
    HotReload = provide(HotReload or {}, {})

    -- All-around Enhancement's GSC gets its saved careers, bank balance and weapon locker from Lua as menu responses
    -- ("ELMGDataGet..."), and its chat commands (/save, /dep, /bal) as "ChatNotify..." ones. Log that they were sent,
    -- without what's in them, so a bank that doesn't load shows whether the data got that far.
    if Engine and type(Engine.SendMenuResponse) == "function" and Engine.BO3MacFixSendMenuResponse == nil then
        local send = Engine.SendMenuResponse
        Engine.BO3MacFixSendMenuResponse = send
        Engine.SendMenuResponse = function(controller, menu, response, ...)
            if type(response) == "string" then
                local kind = string.match(response, "^ELMGData%a*") or string.match(response, "^ChatNotify")
                if kind ~= nil then
                    log("AAE sends " .. kind .. " (" .. #response .. " characters) to " .. tostring(menu))
                end
            end
            return send(controller, menu, response, ...)
        end
    end
    -- and the GSC's side: the "ELMGData" script notifies its Lua handler checks for with IsParamModelEqualToString
    local is_param_equal = rawget(_G, "IsParamModelEqualToString")
    if type(is_param_equal) == "function" and rawget(_G, "BO3MacFixIsParamModelEqualToString") == nil then
        rawset(_G, "BO3MacFixIsParamModelEqualToString", is_param_equal)
        rawset(_G, "IsParamModelEqualToString", function(model, name, ...)
            local equal = is_param_equal(model, name, ...)
            if equal and name == "ELMGData" then
                log("AAE's GSC sent ELMGData")
            end
            return equal
        end)
    end

    -- functions mods call from the Windows game's newer UI scripts that the Mac build never got
    if Engine then
        local function returnsEmptyTable()
            return {}
        end
        for name, implementation in pairs({
            PartyHostClearUIState = noop,
            PartyHostReassignTeams = noop,
            PartyHostSetUIState = noop,
            PartyHostToggleStart = noop,
            GetRegulations = returnsEmptyTable,
            GetUserRegulationPreference = returnsEmptyTable,
            SetModel = Engine.CreateModel,
        }) do
            if Engine[name] == nil then
                Engine[name] = implementation
            end
        end
    end

    -- BO3MACFIX_LUAMISSING=1 (a debugging aid): the first time a script looks up an Engine or CoD function the Mac game
    -- doesn't have, it's logged by name, so a mod calling one of the Windows game's newer functions shows even where
    -- the game doesn't report the error. A check like `if Engine.X then`, or CoD.X read before the mod sets it, is
    -- logged the same way.
    if logMissing then
        for libraryName, library in pairs({ Engine = Engine, CoD = CoD }) do
            local meta = type(library) == "table" and getmetatable(library)
            if meta == nil then
                meta = {}
                setmetatable(library, meta)
            end
            if type(meta) ~= "table" then
                if meta ~= false then
                    log("can't watch " .. libraryName .. " for missing functions")
                end
            elseif not rawget(meta, "BO3MacFixWatcher") then
                -- the game's Engine has a metatable of its own: whatever its __index answers is passed on unchanged,
                -- and only a name it has no answer for is logged, once per Lua state
                local logged, previous = {}, rawget(meta, "__index")
                rawset(meta, "BO3MacFixWatcher", true)
                rawset(meta, "__index", function(table, name)
                    local value
                    if type(previous) == "function" then
                        value = previous(table, name)
                    elseif type(previous) == "table" then
                        value = previous[name]
                    end
                    if value == nil and not logged[name] then
                        logged[name] = true
                        log("no " .. libraryName .. "." .. tostring(name) .. " here")
                    end
                    return value
                end)
            end
        end
    end
end

local ok, err = pcall(setup)
if ok then
    log("loaded")
else
    log("failed to load: " .. tostring(err))
end
