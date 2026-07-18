local mp = require("mp")
local script_dir = (debug.getinfo(1, "S").source:match("^@(.*/)") or "./")
local config = dofile(script_dir .. "../../../shared/lua/plugin_config.lua").enhanced_rotation

local function normalize_rotation(value)
    value = tonumber(value) or 0
    value = value % 360
    if value < 0 then
        value = value + 360
    end
    return value
end

local function rotate(delta)
    delta = tonumber(delta) or 0
    local current = tonumber(mp.get_property("video-rotate")) or 0
    local next_rotation = normalize_rotation(current + delta)
    mp.set_property_number("video-rotate", next_rotation)
    mp.osd_message("Rotation: " .. next_rotation .. "°", config.osd_duration)
end

local function reset_rotation()
    mp.set_property_number("video-rotate", 0)
    mp.osd_message("Rotation: 0°", config.osd_duration)
end

mp.register_script_message("rotate-video", rotate)
mp.register_script_message("rotate-video-reset", reset_rotation)
