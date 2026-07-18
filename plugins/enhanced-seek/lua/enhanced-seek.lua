local mp = require("mp")
local script_dir = (debug.getinfo(1, "S").source:match("^@(.*/)") or "./")
local config = dofile(script_dir .. "../../../shared/lua/plugin_config.lua").enhanced_seek

local overlay = mp.create_osd_overlay("ass-events")
local clear_timer = nil

local function format_time(seconds)
    seconds = tonumber(seconds) or 0
    if seconds < 0 then
        seconds = 0
    end

    local total = math.floor(seconds + 0.5)
    local hours = math.floor(total / 3600)
    local minutes = math.floor((total % 3600) / 60)
    local secs = total % 60

    if hours > 0 then
        return string.format("%d:%02d:%02d", hours, minutes, secs)
    end
    return string.format("%02d:%02d", minutes, secs)
end

local function show_seek_status()
    local position = mp.get_property_number("time-pos", 0)
    local duration = mp.get_property_number("duration", 0)
    local percent = mp.get_property_number("percent-pos")
    local osd_w, osd_h = mp.get_osd_size()

    local percent_text = "0%"
    if percent then
        percent_text = string.format("%.1f%%", percent)
    end

    if not osd_w or not osd_h or osd_w <= 0 or osd_h <= 0 then
        return
    end

    overlay.res_x = osd_w
    overlay.res_y = osd_h
    overlay.data = string.format("%s/%s (%s)", format_time(position), format_time(duration), percent_text)
    overlay:update()

    if clear_timer then
        clear_timer:kill()
    end
    clear_timer = mp.add_timeout(config.osd_duration, function()
        overlay.data = ""
        overlay:update()
    end)
end

local function seek_by(delta)
    mp.commandv("seek", tostring(delta), "relative")
    show_seek_status()
end

mp.add_key_binding(nil, "seek-forward", function()
    seek_by(config.step)
end)

mp.add_key_binding(nil, "seek-backward", function()
    seek_by(-config.step)
end)
