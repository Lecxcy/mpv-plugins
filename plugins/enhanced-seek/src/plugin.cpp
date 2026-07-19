#include <mpv/client.h>

#include <cstring>
#include <optional>
#include <string>

#include <fmt/core.h>

#include "enhanced_seek/logic.h"
#include "shared/cpp/mpv_util.h"

namespace {

using enhanced_seek::HoldState;
using enhanced_seek::HoldTiming;

constexpr double kStepSeconds = 2.0;
constexpr double kOsdDurationSeconds = 1.2;
// 长按连续 seek 的节奏：短按（松开早于 hold_delay）只会触发上面的单次
// kStepSeconds，不会进入连续模式；进入连续模式后固定每 repeat_interval 秒
// 前进/后退一次 kStepSeconds，不做加速——这是用户明确要求的，区别于
// enhanced-volume 那种随按住时长加速的 hold profile。
constexpr HoldTiming kHoldTiming{/*hold_delay=*/0.28, /*repeat_interval=*/0.1};

double now_seconds(mpv_handle *handle) {
    return static_cast<double>(mpv_get_time_us(handle)) / 1'000'000.0;
}

void seek_by(mpv_handle *handle, double delta_seconds) {
    std::string arg = fmt::format("{:.6f}", delta_seconds);
    const char *args[] = {"seek", arg.c_str(), "relative", nullptr};
    mpv_command(handle, args);
}

void show_seek_status(mpv_handle *handle) {
    double position = mpv_util::get_double(handle, "time-pos", 0.0);
    double duration = mpv_util::get_double(handle, "duration", 0.0);

    std::optional<double> percent;
    double percent_value = 0.0;
    if (mpv_get_property(handle, "percent-pos", MPV_FORMAT_DOUBLE, &percent_value) >= 0) {
        percent = percent_value;
    }

    std::string text = enhanced_seek::format_status(position, duration, percent);
    mpv_util::show_osd_message(handle, text, kOsdDurationSeconds);
}

void on_key_down(mpv_handle *handle, HoldState &hold, int direction) {
    seek_by(handle, direction * kStepSeconds);
    show_seek_status(handle);
    hold.begin(direction, now_seconds(handle), kHoldTiming);
    MPV_UTIL_DEBUG("on_key_down: direction={} now={:.4f}\n", direction, now_seconds(handle));
}

void on_key_up(HoldState &hold, int direction) {
    MPV_UTIL_DEBUG("on_key_up: direction={} hold.active={} hold.direction={}\n", direction, hold.active(),
                    hold.direction());
    hold.stop(direction);
}

// C 插件没有 mp.add_key_binding 那样的封装，参照 enhanced-drag/
// enhanced-ab-loop 的写法，直接接住 mpv 转发过来的 "key-binding" client
// message（input.rst:1399-1439：script-binding 命令内部就是这样转发的，不
// 需要提前注册）。只处理 d（按下）/u（松开）两个 phase：这两个绑定没有加
// `repeatable` 前缀，不会收到 OS 按键自动重复产生的 r（repeat）事件，长按
// 时的连续 seek 完全由本插件自己的计时逻辑（HoldState）驱动，不依赖 mpv
// 的原生按键自动重复速率（那是全局设置，没法只给这一个绑定单独调速度）。
void handle_client_message(mpv_handle *handle, mpv_event_client_message *message, HoldState &hold) {
    if (message->num_args < 3 || std::strcmp(message->args[0], "key-binding") != 0) {
        return;
    }

    std::string binding = message->args[1];
    int direction = 0;
    if (binding == "seek-forward") {
        direction = 1;
    } else if (binding == "seek-backward") {
        direction = -1;
    } else {
        return;
    }

    char phase = message->args[2][0];
    if (phase == 'd' || phase == 'p') {
        // 'p'：按下/抬起分不清时的单次触发，按普通单次点按处理，不进入连续
        // 模式的等待也无妨——反正后续不会再有对应的 'u' 来推进它。
        on_key_down(handle, hold, direction);
    } else if (phase == 'u') {
        on_key_up(hold, direction);
    }
}

} // namespace

extern "C" int mpv_open_cplugin(mpv_handle *handle) {
    HoldState hold;

    while (true) {
        double timeout = hold.active() ? kHoldTiming.repeat_interval : -1.0;
        mpv_event *event = mpv_wait_event(handle, timeout);

        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            return 0;
        }

        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
            handle_client_message(handle, static_cast<mpv_event_client_message *>(event->data), hold);
            continue;
        }

        if (event->event_id == MPV_EVENT_NONE && hold.active()) {
            int ticks = hold.poll(now_seconds(handle), kHoldTiming);
            MPV_UTIL_DEBUG("poll: direction={} ticks={} now={:.4f}\n", hold.direction(), ticks,
                            now_seconds(handle));
            if (ticks > 0) {
                seek_by(handle, hold.direction() * kStepSeconds * ticks);
                show_seek_status(handle);
            }
        }
    }
}
