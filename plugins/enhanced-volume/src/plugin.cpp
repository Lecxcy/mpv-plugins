#include <mpv/client.h>

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "enhanced_volume/logic.h"
#include "shared/cpp/mpv_util.h"

namespace {

using enhanced_volume::HoldProfileStep;
using enhanced_volume::HoldState;

constexpr double kOsdDurationSeconds = 1.2;
constexpr double kMaxVolume = 300.0;
constexpr double kTapStep = 5.0;
constexpr double kHoldDelay = 0.28;
// 长按连发期间 mpv_wait_event 的轮询超时：跟原 Lua config.timer_resolution
// 保持一致，用来在没有其他事件时定期醒来检查是否到了 HoldState.next_at。
constexpr double kTimerResolution = 0.02;

const std::vector<HoldProfileStep> &hold_profile() {
    static const std::vector<HoldProfileStep> profile = {
        {0.8, 0.18},
        {1.8, 0.12},
        {1e18, 0.07}, // 原 Lua 用 math.huge 兜底最后一档，这里用一个不会被 elapsed 超过的大值代替。
    };
    return profile;
}

void show_volume(mpv_handle *handle) {
    double volume = mpv_util::get_double(handle, "volume", 0.0);
    bool muted = mpv_util::get_flag(handle, "mute", false);
    mpv_util::show_osd_message(handle, enhanced_volume::format_volume_label(volume, muted), kOsdDurationSeconds);
}

void adjust_volume(mpv_handle *handle, double delta) {
    double current = mpv_util::get_double(handle, "volume", 0.0);
    double next = enhanced_volume::next_volume(current, delta, kMaxVolume);
    mpv_util::set_double(handle, "volume", next);
    show_volume(handle);
}

double parse_delta(const char *arg) {
    if (!arg) {
        return 0.0;
    }
    char *end = nullptr;
    double value = std::strtod(arg, &end);
    if (end == arg) {
        return 0.0;
    }
    return value;
}

struct PluginState {
    mpv_handle *handle = nullptr;
    std::optional<HoldState> hold;
};

void begin_hold(PluginState &state, int direction) {
    if (state.hold && state.hold->direction == direction) {
        return;
    }
    adjust_volume(state.handle, direction * kTapStep);
    double now = static_cast<double>(mpv_get_time_us(state.handle)) / 1e6;
    state.hold = enhanced_volume::start_hold(direction, now, kHoldDelay);
}

void stop_hold(PluginState &state, int direction) {
    if (state.hold && state.hold->direction == direction) {
        state.hold.reset();
    }
}

// 只在 mpv_wait_event 因超时（没有别的事件）醒来时调用，对应原 Lua 里独立
// 于按键事件之外、由 add_periodic_timer 驱动的 tick_hold。
void tick_hold(PluginState &state) {
    if (!state.hold) {
        return;
    }
    double now = static_cast<double>(mpv_get_time_us(state.handle)) / 1e6;
    if (enhanced_volume::tick_hold(*state.hold, now, hold_profile())) {
        adjust_volume(state.handle, state.hold->direction * kTapStep);
    }
}

// volume-up/volume-down 通过 input.conf 里的 script-binding 转发过来，携带
// down/up/repeat/press 四种状态字符（同 enhanced-ab-loop 的 key-binding 处
// 理）。长按连发完全由本插件自己的 HoldState + 轮询驱动，所以这里忽略 mpv
// 自身可能发出的 "r"（repeat），避免和自己的调速节奏打架。
void handle_key_binding(PluginState &state, int direction, char phase) {
    if (phase == 'd' || phase == 'p') {
        begin_hold(state, direction);
    } else if (phase == 'u') {
        stop_hold(state, direction);
    }
}

void handle_client_message(PluginState &state, mpv_event_client_message *message) {
    if (message->num_args < 1) {
        return;
    }

    if (std::strcmp(message->args[0], "key-binding") == 0) {
        if (message->num_args < 3) {
            return;
        }
        const char *binding = message->args[1];
        char phase = message->args[2][0];
        if (std::strcmp(binding, "volume-up") == 0) {
            handle_key_binding(state, 1, phase);
        } else if (std::strcmp(binding, "volume-down") == 0) {
            handle_key_binding(state, -1, phase);
        }
        return;
    }

    if (std::strcmp(message->args[0], "adjust-volume") == 0) {
        double delta = message->num_args > 1 ? parse_delta(message->args[1]) : 0.0;
        adjust_volume(state.handle, delta);
    }
}

} // namespace

extern "C" int mpv_open_cplugin(mpv_handle *handle) {
    PluginState state;
    state.handle = handle;

    while (true) {
        double timeout = state.hold ? kTimerResolution : -1.0;
        mpv_event *event = mpv_wait_event(handle, timeout);

        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            return 0;
        }

        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
            handle_client_message(state, static_cast<mpv_event_client_message *>(event->data));
            continue;
        }

        if (event->event_id == MPV_EVENT_NONE) {
            tick_hold(state);
        }
    }
}
