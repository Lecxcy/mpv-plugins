local mp = require("mp")
local script_dir = (debug.getinfo(1, "S").source:match("^@(.*/)") or "./")
local config = dofile(script_dir .. "../../../shared/lua/plugin_config.lua").enhanced_ab_loop

local A = nil
local B = nil
local segments = {}
local loop_enabled = true

local build_active_segments

local function format_status_time(seconds)
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

local function fmt(t)
    if t == nil then return "unset" end
    local h = math.floor(t / 3600)
    local m = math.floor((t % 3600) / 60)
    local s = t % 60
    if h > 0 then
        return string.format("%d:%02d:%06.3f", h, m, s)
    else
        return string.format("%02d:%06.3f", m, s)
    end
end

local function fmt_segment(seg)
    if seg.b == nil then
        return string.format("[%s,end]", fmt(seg.a))
    end
    return string.format("[%s,%s]", fmt(seg.a), fmt(seg.b))
end

local function show_state(prefix)
    prefix = prefix or "State"
    local position = mp.get_property_number("time-pos", 0)
    local duration = mp.get_property_number("duration", 0)
    local percent = mp.get_property_number("percent-pos")
    local percent_text = "0%"
    local lines = {
        string.format(
            "%s | %s/%s (%s)",
            prefix,
            format_status_time(position),
            format_status_time(duration),
            percent and string.format("%.1f%%", percent) or percent_text
        )
    }

    for _, seg in ipairs(build_active_segments()) do
        lines[#lines + 1] = fmt_segment(seg)
    end

    mp.osd_message(table.concat(lines, "\n"), config.state_osd_duration)
end

local function show_current_state()
    show_state("State")
end

local function seek(target)
    mp.commandv("seek", tostring(target), "absolute", "exact")
    mp.set_property_bool("pause", false)
end

local function clone_segments()
    local out = {}
    for i, seg in ipairs(segments) do
        out[i] = { a = seg.a, b = seg.b }
    end
    return out
end

local function sort_segments(list)
    table.sort(list, function(lhs, rhs)
        if math.abs(lhs.a - rhs.a) <= config.epsilon then
            local lb = lhs.b or math.huge
            local rb = rhs.b or math.huge
            return lb < rb
        end
        return lhs.a < rhs.a
    end)
end

local function is_inside_segment(pos, seg)
    return pos > seg.a + config.epsilon and pos < seg.b - config.epsilon
end

local function find_complete_index_at(pos)
    for i, seg in ipairs(segments) do
        if is_inside_segment(pos, seg) then
            return i
        end
    end
    return nil
end

local function find_prev_index(point)
    local idx = nil
    for i, seg in ipairs(segments) do
        if seg.b <= point + config.epsilon then
            idx = i
        else
            break
        end
    end
    return idx
end

local function find_next_index(point)
    for i, seg in ipairs(segments) do
        if seg.a >= point - config.epsilon then
            return i
        end
    end
    return nil
end

local function overlaps_complete(a, b, ignore_index)
    for i, seg in ipairs(segments) do
        if i ~= ignore_index and a < seg.b - config.epsilon and b > seg.a + config.epsilon then
            return i
        end
    end
    return nil
end

build_active_segments = function()
    local active = clone_segments()

    if A and (not B) then
        local idx = find_next_index(A)
        if idx then
            active[idx] = { a = A, b = segments[idx].b }
        else
            active[#active + 1] = { a = A, b = nil }
        end
    elseif B and (not A) then
        local idx = find_prev_index(B)
        if idx then
            active[idx] = { a = segments[idx].a, b = B }
        else
            table.insert(active, 1, { a = 0, b = B })
        end
    end

    sort_segments(active)
    return active
end

local function find_active_index_at(pos, active)
    for i, seg in ipairs(active) do
        if pos >= seg.a - config.epsilon and (seg.b == nil or pos < seg.b - config.epsilon) then
            return i
        end
    end
    return nil
end

local function find_next_active_index(pos, active)
    for i, seg in ipairs(active) do
        if seg.a > pos + config.epsilon then
            return i
        end
    end
    return nil
end

local function maybe_insert_partial_segment()
    if not A or not B then
        return false, nil
    end

    if A >= B - config.epsilon then
        return false, "invalid"
    end

    if overlaps_complete(A, B) then
        return false, "overlap"
    end

    segments[#segments + 1] = { a = A, b = B }
    sort_segments(segments)
    A = nil
    B = nil
    return true, nil
end

local function check()
    if not loop_enabled then return end

    local pos = mp.get_property_number("time-pos")
    if not pos then return end

    local active = build_active_segments()
    if #active == 0 then return end

    local idx = find_active_index_at(pos, active)
    if idx then
        local seg = active[idx]
        if seg.b and pos >= seg.b - config.epsilon then
            local next_idx = idx % #active + 1
            seek(active[next_idx].a)
        end
        return
    end

    local next_idx = find_next_active_index(pos, active)
    if next_idx then
        seek(active[next_idx].a)
    else
        seek(active[1].a)
    end
end

local function set_a()
    local pos = mp.get_property_number("time-pos")
    if not pos then return end
    pos = math.max(0, pos)

    local idx = find_complete_index_at(pos)
    if idx and (not A) and (not B) then
        segments[idx].a = pos
        sort_segments(segments)
        check()
        show_state("Move A")
        return
    end

    if idx then
        show_state("A denied")
        return
    end

    if B and pos >= B - config.epsilon then
        show_state("A >= B denied")
        return
    end

    A = pos
    local inserted, err = maybe_insert_partial_segment()
    if err == "overlap" then
        A = nil
        show_state("Overlap denied")
        return
    end
    if err == "invalid" then
        A = nil
        show_state("Invalid partial")
        return
    end
    check()
    show_state(inserted and "Add segment" or "Set A")
end

local function set_b()
    local pos = mp.get_property_number("time-pos")
    if not pos then return end
    pos = math.max(0, pos)

    local idx = find_complete_index_at(pos)
    if idx and (not A) and (not B) then
        segments[idx].b = pos
        sort_segments(segments)
        check()
        show_state("Move B")
        return
    end

    if idx then
        show_state("B denied")
        return
    end

    if A and pos <= A + config.epsilon then
        show_state("B <= A denied")
        return
    end

    B = pos
    local inserted, err = maybe_insert_partial_segment()
    if err == "overlap" then
        B = nil
        show_state("Overlap denied")
        return
    end
    if err == "invalid" then
        B = nil
        show_state("Invalid partial")
        return
    end
    check()
    show_state(inserted and "Add segment" or "Set B")
end

local function clear_partial(which)
    if which == "A" then
        if not A then
            show_state("A not set")
            return false
        end
        A = nil
        return true
    end

    if not B then
        show_state("B not set")
        return false
    end
    B = nil
    return true
end

local function unset_a()
    if A or B then
        if clear_partial("A") then
            check()
            show_state("Unset A")
        end
        return
    end

    local pos = mp.get_property_number("time-pos")
    if not pos then return end
    local idx = find_complete_index_at(pos)
    if not idx then
        show_state("Unset A denied")
        return
    end

    local seg = table.remove(segments, idx)
    A = nil
    B = seg.b
    check()
    show_state("Unset A")
end

local function unset_b()
    if A or B then
        if clear_partial("B") then
            check()
            show_state("Unset B")
        end
        return
    end

    local pos = mp.get_property_number("time-pos")
    if not pos then return end
    local idx = find_complete_index_at(pos)
    if not idx then
        show_state("Unset B denied")
        return
    end

    local seg = table.remove(segments, idx)
    A = seg.a
    B = nil
    check()
    show_state("Unset B")
end

local function nudge_a(delta)
    local pos = mp.get_property_number("time-pos")
    if not pos then return end

    local idx = find_complete_index_at(pos)
    if not idx then
        show_state("Nudge A denied")
        return
    end

    local seg = segments[idx]
    local prev = segments[idx - 1]
    local min_a = 0
    if prev then
        min_a = prev.b
    end

    local new_a = math.max(min_a, seg.a + delta)
    if new_a >= seg.b - config.epsilon then
        new_a = math.max(min_a, seg.b - 0.01)
    end

    seg.a = new_a
    sort_segments(segments)
    check()
    show_state("Nudge A")
end

local function nudge_b(delta)
    local pos = mp.get_property_number("time-pos")
    if not pos then return end

    local idx = find_complete_index_at(pos)
    if not idx then
        show_state("Nudge B denied")
        return
    end

    local seg = segments[idx]
    local nxt = segments[idx + 1]
    local max_b = math.huge
    if nxt then
        max_b = nxt.a
    end

    local new_b = seg.b + delta
    if new_b <= seg.a + config.epsilon then
        new_b = seg.a + 0.01
    end
    if new_b > max_b then
        new_b = max_b
    end
    if new_b <= seg.a + config.epsilon then
        new_b = seg.a + 0.01
    end

    seg.b = new_b
    sort_segments(segments)
    check()
    show_state("Nudge B")
end

local function toggle_enabled()
    loop_enabled = not loop_enabled
    check()
    mp.osd_message(loop_enabled and "loop on" or "loop off", config.toggle_osd_duration)
end

local timer = mp.add_periodic_timer(config.check_interval, check)
timer:resume()

mp.observe_property("eof-reached", "bool", function(_, v)
    if not v or not loop_enabled then return end

    local active = build_active_segments()
    if #active == 0 then return end
    seek(active[1].a)
end)

mp.add_key_binding(nil, "set-a", set_a)
mp.add_key_binding(nil, "set-b", set_b)
mp.add_key_binding(nil, "unset-a", unset_a)
mp.add_key_binding(nil, "unset-b", unset_b)
mp.add_key_binding(nil, "show-state", show_current_state)
mp.add_key_binding(nil, "toggle-enabled", toggle_enabled)

mp.add_key_binding(nil, "nudge-a-back", function() nudge_a(-config.nudge_step) end)
mp.add_key_binding(nil, "nudge-a-forward", function() nudge_a(config.nudge_step) end)
mp.add_key_binding(nil, "nudge-b-back", function() nudge_b(-config.nudge_step) end)
mp.add_key_binding(nil, "nudge-b-forward", function() nudge_b(config.nudge_step) end)
