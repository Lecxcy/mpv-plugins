return {
    drag_zoom_box = {
        min_selection_pixels = 4,
        border_width = 2,
        color_zoom = "00FF00",
        color_reset = "0000FF",
        refresh_rate = 1 / 60,
    },
    enhanced_ab_loop = {
        nudge_step = 0.05,
        check_interval = 0.05,
        epsilon = 0.005,
        state_osd_duration = 1.8,
        toggle_osd_duration = 1.8,
    },
    enhanced_drag = {
        refresh_rate = 1 / 120,
    },
    enhanced_rotation = {
        osd_duration = 1,
    },
    enhanced_seek = {
        step = 2,
        osd_duration = 1.2,
    },
    enhanced_volume = {
        osd_duration = 1.2,
        max_volume = 300,
        tap_step = 5,
        hold_delay = 0.28,
        timer_resolution = 0.02,
        hold_profile = {
            { elapsed = 0.8, interval = 0.18 },
            { elapsed = 1.8, interval = 0.12 },
            { elapsed = math.huge, interval = 0.07 },
        },
    },
    tail_frame_extension = {
        enabled = true,
        duration = 0.3,
        video_filter_label = "tail-frame-extension-video",
        audio_filter_label = "tail-frame-extension-audio",
    },
}
