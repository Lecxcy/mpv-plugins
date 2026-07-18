local mp = require("mp")
local script_dir = (debug.getinfo(1, "S").source:match("^@(.*/)") or "./")
local config = dofile(script_dir .. "../../../shared/lua/plugin_config.lua").tail_frame_extension

local function escape_filter_value(value)
    return tostring(value):gsub("\\", "\\\\"):gsub(":", "\\:")
end

local function remove_filter(chain, label)
    local filters = mp.get_property_native(chain) or {}
    for _, filter in ipairs(filters) do
        if filter and filter.label == "@" .. label then
            mp.commandv(chain, "remove", "@" .. label)
            return
        end
    end
end

local function apply_filter(chain, spec)
    local ok, err = pcall(function()
        mp.commandv(chain, "add", spec)
    end)
    if not ok then
        mp.msg.error(err)
    end
end

local function has_video_track()
    local track_list = mp.get_property_native("track-list") or {}
    for _, track in ipairs(track_list) do
        if track and track.type == "video" and track.selected then
            return true
        end
    end
    return false
end

local function has_audio_track()
    local track_list = mp.get_property_native("track-list") or {}
    for _, track in ipairs(track_list) do
        if track and track.type == "audio" and track.selected then
            return true
        end
    end
    return false
end

local function clear_tail_extension()
    remove_filter("vf", config.video_filter_label)
    remove_filter("af", config.audio_filter_label)
end

local function apply_tail_extension()
    clear_tail_extension()

    if not config.enabled or (config.duration or 0) <= 0 then
        return
    end

    if not has_video_track() then
        return
    end

    local duration = escape_filter_value(string.format("%.6f", config.duration))
    apply_filter(
        "vf",
        string.format("@%s:lavfi=[tpad=stop_mode=clone:stop_duration=%s]", config.video_filter_label, duration)
    )

    if has_audio_track() then
        apply_filter(
            "af",
            string.format("@%s:lavfi=[apad=pad_dur=%s]", config.audio_filter_label, duration)
        )
    end
end

mp.register_event("file-loaded", apply_tail_extension)
mp.register_event("end-file", clear_tail_extension)
mp.register_event("shutdown", clear_tail_extension)
