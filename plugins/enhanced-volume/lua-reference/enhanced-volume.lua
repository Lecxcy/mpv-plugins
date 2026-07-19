local mp = require("mp")
local script_dir = (debug.getinfo(1, "S").source:match("^@(.*/)") or "./")
local config = dofile(script_dir .. "../../../shared/lua/plugin_config.lua").enhanced_volume

local active_hold = nil
local hold_timer = nil

local function clamp(value, min_value, max_value)
    if value < min_value then
        return min_value
    end
    if value > max_value then
        return max_value
    end
    return value
end

local function show_volume()
    local volume = tonumber(mp.get_property("volume")) or 0
    local muted = mp.get_property_bool("mute", false)
    local label = muted and "Mute" or string.format("Volume: %d%%", math.floor(volume + 0.5))
    mp.osd_message(label, config.osd_duration)
end

local function adjust_volume(delta)
    delta = tonumber(delta) or 0
    local current = tonumber(mp.get_property("volume")) or 0
    local next_volume = clamp(current + delta, 0, config.max_volume)
    mp.set_property_number("volume", next_volume)
    show_volume()
end

local function stop_hold()
    active_hold = nil
    if hold_timer then
        hold_timer:kill()
    end
end

local function get_hold_profile(elapsed)
    for _, profile in ipairs(config.hold_profile) do
        if elapsed < profile.elapsed then
            return profile.interval
        end
    end
    return config.hold_profile[#config.hold_profile].interval
end

local function tick_hold()
    if not active_hold then
        stop_hold()
        return
    end

    local now = mp.get_time()
    if now < active_hold.next_at then
        return
    end

    local interval = get_hold_profile(now - active_hold.started_at)
    adjust_volume(active_hold.direction * config.tap_step)
    active_hold.next_at = now + interval
end

local function start_hold(direction)
    local now = mp.get_time()
    adjust_volume(direction * config.tap_step)
    active_hold = {
        direction = direction,
        started_at = now,
        next_at = now + config.hold_delay,
    }

    if not hold_timer then
        hold_timer = mp.add_periodic_timer(config.timer_resolution, tick_hold)
        if hold_timer then
            hold_timer:kill()
        end
    end
    if hold_timer then
        hold_timer:resume()
    end
end

local function handle_volume_key(direction, state)
    local event = state and state.event or "press"
    if event == "down" or event == "press" then
        if active_hold and active_hold.direction == direction then
            return
        end
        start_hold(direction)
        return
    end

    if event == "up" or event == "cancel" then
        if active_hold and active_hold.direction == direction then
            stop_hold()
        end
    end
end

mp.register_script_message("adjust-volume", adjust_volume)
mp.add_key_binding(nil, "volume-up", function(state)
    handle_volume_key(1, state)
end, { complex = true })
mp.add_key_binding(nil, "volume-down", function(state)
    handle_volume_key(-1, state)
end, { complex = true })
