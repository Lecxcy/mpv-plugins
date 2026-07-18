local mp = require("mp")
local script_dir = (debug.getinfo(1, "S").source:match("^@(.*/)") or "./")
local config = dofile(script_dir .. "../../../shared/lua/plugin_config.lua").enhanced_drag

local dragging = false
local last_pos = nil
local drag_timer = nil

local function get_mouse_pos()
    local pos = mp.get_property_native("mouse-pos")
    if not pos or not pos.hover or pos.x == nil or pos.y == nil then
        return nil
    end
    return { x = pos.x, y = pos.y }
end

local function get_video_size()
    local osd_w, osd_h = mp.get_osd_size()
    if not osd_w or not osd_h or osd_w <= 0 or osd_h <= 0 then
        return nil
    end

    local params = mp.get_property_native("video-target-params")
    if not params then
        params = mp.get_property_native("video-out-params")
    end
    if not params or not params.dw or not params.dh or params.dw <= 0 or params.dh <= 0 then
        return nil
    end

    local base_scale = math.min(osd_w / params.dw, osd_h / params.dh)
    local scale = 2 ^ mp.get_property_number("video-zoom", 0)

    return {
        scaled_w = params.dw * base_scale * scale,
        scaled_h = params.dh * base_scale * scale,
    }
end

local function step_drag()
    if not dragging then
        return
    end

    local pos = get_mouse_pos()
    if not pos then
        return
    end

    if not last_pos then
        last_pos = pos
        return
    end

    local size = get_video_size()
    if not size then
        last_pos = pos
        return
    end

    local dx = pos.x - last_pos.x
    local dy = pos.y - last_pos.y
    last_pos = pos

    if dx == 0 and dy == 0 then
        return
    end

    local pan_x = mp.get_property_number("video-pan-x", 0)
    local pan_y = mp.get_property_number("video-pan-y", 0)

    if size.scaled_w > 0 then
        pan_x = pan_x + (dx / size.scaled_w)
    end
    if size.scaled_h > 0 then
        pan_y = pan_y + (dy / size.scaled_h)
    end

    mp.set_property_number("video-pan-x", pan_x)
    mp.set_property_number("video-pan-y", pan_y)
end

local function finish_drag()
    dragging = false
    last_pos = nil
    if drag_timer then
        drag_timer:stop()
    end
end

drag_timer = mp.add_periodic_timer(config.refresh_rate, step_drag)
if drag_timer then
    drag_timer:stop()
end

local function handle_drag(event)
    if event.event == "down" then
        local pos = get_mouse_pos()
        if not pos then
            return
        end
        dragging = true
        last_pos = pos
        if drag_timer then
            drag_timer:resume()
        end
        return
    end

    if not dragging then
        return
    end

    if event.event == "up" or event.canceled then
        step_drag()
        finish_drag()
    end
end

mp.add_key_binding(nil, "drag-pan-free", handle_drag, { complex = true })

mp.register_event("shutdown", finish_drag)
