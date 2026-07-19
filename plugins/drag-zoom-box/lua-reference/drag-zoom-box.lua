local mp = require("mp")
local script_dir = (debug.getinfo(1, "S").source:match("^@(.*/)") or "./")
local config = dofile(script_dir .. "../../../shared/lua/plugin_config.lua").drag_zoom_box

local overlay = mp.create_osd_overlay("ass-events")

local dragging = false
local start_pos = nil
local current_pos = nil
local drag_timer = nil

local log2_base = math.log(2)

local function log2(value)
    return math.log(value) / log2_base
end

local function pow2(value)
    return 2 ^ value
end

local function clamp(value, minimum, maximum)
    if value < minimum then
        return minimum
    end
    if value > maximum then
        return maximum
    end
    return value
end

local function reset_zoom()
    mp.set_property_number("video-zoom", 0)
    mp.set_property_number("video-pan-x", 0)
    mp.set_property_number("video-pan-y", 0)
    mp.osd_message("Zoom reset", 1)
end

local function get_mouse_pos()
    local pos = mp.get_property_native("mouse-pos")
    if not pos or not pos.hover or pos.x == nil or pos.y == nil then
        return nil
    end
    return { x = pos.x, y = pos.y }
end

local function get_video_geometry()
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
    local zoom = mp.get_property_number("video-zoom", 0)
    local scale = pow2(zoom)
    local scaled_w = params.dw * base_scale * scale
    local scaled_h = params.dh * base_scale * scale
    local pan_x = mp.get_property_number("video-pan-x", 0)
    local pan_y = mp.get_property_number("video-pan-y", 0)

    local rect_x = (osd_w - scaled_w) / 2 + pan_x * scaled_w
    local rect_y = (osd_h - scaled_h) / 2 + pan_y * scaled_h

    return {
        osd_w = osd_w,
        osd_h = osd_h,
        base_w = params.dw * base_scale,
        base_h = params.dh * base_scale,
        scaled_w = scaled_w,
        scaled_h = scaled_h,
        rect_x = rect_x,
        rect_y = rect_y,
    }
end

local function clear_overlay()
    overlay.data = ""
    overlay:update()
end

local function is_zoom_selection()
    if not start_pos or not current_pos then
        return false
    end
    return current_pos.x >= start_pos.x
end

local function draw_overlay()
    if not dragging or not start_pos or not current_pos then
        clear_overlay()
        return
    end

    local osd_w, osd_h = mp.get_osd_size()
    if not osd_w or not osd_h or osd_w <= 0 or osd_h <= 0 then
        clear_overlay()
        return
    end

    local x1 = math.min(start_pos.x, current_pos.x)
    local y1 = math.min(start_pos.y, current_pos.y)
    local x2 = math.max(start_pos.x, current_pos.x)
    local y2 = math.max(start_pos.y, current_pos.y)
    local color = is_zoom_selection() and config.color_zoom or config.color_reset

    overlay.res_x = osd_w
    overlay.res_y = osd_h
    overlay.z = 1000
    overlay.data = string.format(
        "{\\an7\\pos(0,0)\\bord%.3f\\shad0\\1a&HFF&\\3a&H00&\\3c&H%s&\\p1}" ..
        "m %.3f %.3f l %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f{\\p0}",
        config.border_width,
        color,
        x1, y1, x2, y1, x2, y2, x1, y2, x1, y1
    )
    overlay:update()
end

local function apply_box_zoom()
    if not start_pos or not current_pos then
        return
    end

    if not is_zoom_selection() then
        reset_zoom()
        return
    end

    local geometry = get_video_geometry()
    if not geometry then
        mp.osd_message("Video geometry unavailable", 1)
        return
    end

    local x1 = math.min(start_pos.x, current_pos.x)
    local y1 = math.min(start_pos.y, current_pos.y)
    local x2 = math.max(start_pos.x, current_pos.x)
    local y2 = math.max(start_pos.y, current_pos.y)

    if (x2 - x1) < config.min_selection_pixels or (y2 - y1) < config.min_selection_pixels then
        return
    end

    local u1 = clamp((x1 - geometry.rect_x) / geometry.scaled_w, 0, 1)
    local v1 = clamp((y1 - geometry.rect_y) / geometry.scaled_h, 0, 1)
    local u2 = clamp((x2 - geometry.rect_x) / geometry.scaled_w, 0, 1)
    local v2 = clamp((y2 - geometry.rect_y) / geometry.scaled_h, 0, 1)

    local du = u2 - u1
    local dv = v2 - v1
    if du <= 0 or dv <= 0 then
        return
    end

    local scale_x = geometry.osd_w / (du * geometry.base_w)
    local scale_y = geometry.osd_h / (dv * geometry.base_h)
    local scale = math.min(scale_x, scale_y)

    local center_u = (u1 + u2) / 2
    local center_v = (v1 + v2) / 2

    mp.set_property_number("video-zoom", log2(scale))
    mp.set_property_number("video-pan-x", 0.5 - center_u)
    mp.set_property_number("video-pan-y", 0.5 - center_v)
end

local function finish_drag()
    if not dragging then
        return
    end

    dragging = false
    if drag_timer then
        drag_timer:stop()
    end
    clear_overlay()
    apply_box_zoom()
    start_pos = nil
    current_pos = nil
end

drag_timer = mp.add_periodic_timer(config.refresh_rate, function()
    if not dragging then
        return
    end

    local pos = get_mouse_pos()
    if pos then
        current_pos = pos
        draw_overlay()
    end
end)
if drag_timer then
    drag_timer:stop()
end

local function handle_drag_binding(event)
    if event.event == "down" then
        local pos = get_mouse_pos()
        if not pos then
            return
        end
        dragging = true
        start_pos = pos
        current_pos = pos
        if drag_timer then
            drag_timer:resume()
        end
        draw_overlay()
        return
    end

    if not dragging then
        return
    end

    if event.event == "up" or event.canceled then
        local pos = get_mouse_pos()
        if pos then
            current_pos = pos
        end
        if drag_timer then
            drag_timer:stop()
        end
        finish_drag()
    end
end

mp.add_key_binding(nil, "drag-zoom-box", handle_drag_binding, { complex = true })

mp.register_event("shutdown", function()
    clear_overlay()
end)
