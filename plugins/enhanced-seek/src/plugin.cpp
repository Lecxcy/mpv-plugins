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

double time_pos(mpv_handle *handle) {
    return mpv_util::get_double(handle, "time-pos", 0.0);
}

void seek_by(mpv_handle *handle, double delta_seconds) {
    // 加 exact：非 exact 的 relative seek 只吸附到最近关键帧，关键帧稀疏的
    // 文件上会导致单次 seek 的实际位移远超 delta_seconds（曾经把用户"点一下
    // 快进 2 秒"的操作吸附成了跳 6 秒开外）。exact 需要从关键帧解码到目标
    // 位置，长按连续触发（每 repeat_interval 一次）时如果解码耗时超过
    // repeat_interval，可能感觉卡顿——如果实测这种卡顿明显，可以考虑只在
    // 单次点按时用 exact，长按连续 ticks 退回非 exact。
    std::string arg = fmt::format("{:.6f}", delta_seconds);
    const char *args[] = {"seek", arg.c_str(), "relative", "exact", nullptr};
    mpv_command(handle, args);
}

void show_seek_status(mpv_handle *handle) {
    double position = time_pos(handle);
    double duration = mpv_util::get_double(handle, "duration", 0.0);

    std::optional<double> percent;
    double percent_value = 0.0;
    if (mpv_get_property(handle, "percent-pos", MPV_FORMAT_DOUBLE, &percent_value) >= 0) {
        percent = percent_value;
    }

    std::string text = enhanced_seek::format_status(position, duration, percent);
    mpv_util::show_osd_message(handle, text, kOsdDurationSeconds);
}

// 记录每个方向最近一次按下的时间点，仅用于 debug 打印"这次到底按住了多久"，
// 不参与任何 seek 逻辑（HoldState 自己内部已经有一份用于调度的时间点）。
struct KeyDownClock {
    double down_at[2] = {-1.0, -1.0}; // index 0: backward(-1), index 1: forward(+1)

    double &slot(int direction) { return down_at[direction > 0 ? 1 : 0]; }
};

// enable_hold=false 用于 'p'（按下/抬起分不清）：这种 phase 不保证后面一定
// 会收到匹配的 'u'，如果照常 hold.begin() 进入长按等待，遇到不发 'u' 的输入
// 源（比如上下不可追踪的遥控器/手柄）会导致连续 seek 永远停不下来，所以只
// 当作一次性单击处理，不进入连续模式。
void on_key_down(mpv_handle *handle, HoldState &hold, KeyDownClock &clock, int direction, bool enable_hold) {
    seek_by(handle, direction * kStepSeconds);
    show_seek_status(handle);
    double now = now_seconds(handle);
    clock.slot(direction) = now;
    if (enable_hold) {
        hold.begin(direction, now, kHoldTiming);
    }
    MPV_UTIL_DEBUG("on_key_down: direction={} now={:.4f} pos_after_tap={:.4f} enable_hold={}\n", direction, now,
                    time_pos(handle), enable_hold);
}

void on_key_up(mpv_handle *handle, HoldState &hold, KeyDownClock &clock, int direction) {
    double down_at = clock.slot(direction);
    double now = now_seconds(handle);
    double held_for = down_at >= 0.0 ? now - down_at : -1.0;
    MPV_UTIL_DEBUG("on_key_up: direction={} held_for={:.4f}s pos_at_release={:.4f} hold.active={} "
                    "hold.direction={}\n",
                    direction, held_for, time_pos(handle), hold.active(), hold.direction());
    hold.stop(direction);
}

// C 插件没有 mp.add_key_binding 那样的封装，参照 enhanced-drag/
// enhanced-ab-loop 的写法，直接接住 mpv 转发过来的 "key-binding" client
// message（input.rst:1399-1439：script-binding 命令内部就是这样转发的，不
// 需要提前注册）。
//
// 注意：`script-binding` 这个 mpv 内置命令本身默认就是 allow_auto_repeat=true
// （见 external/mpv/player/command.c 里 "script-binding" 的定义），也就是说
// 只要一直按住方向键，mpv 会按全局的 --input-ar-delay/--input-ar-rate
// （默认 200ms 后开始，之后每 25ms 一次）持续推 phase='r' 的 key-binding
// message 过来——这一点和本文件早前的注释假设（"没有 repeatable 前缀就不会
// 收到 r 事件"）不符，特此更正。连续 seek 的节奏仍然完全由下面事件循环里
// 自己的 HoldState 计时驱动，不使用这个 r 事件本身的时间戳/频率；但因为
// mpv_wait_event 只要收到任何事件（包括这里被忽略的 r）就会提前返回，
// r 事件到达的频率会影响我们自己的超时轮询多快能拿到一次 MPV_EVENT_NONE。
void handle_client_message(mpv_handle *handle, mpv_event_client_message *message, HoldState &hold,
                            KeyDownClock &clock) {
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
    MPV_UTIL_DEBUG("key-binding: binding={} phase={} now={:.4f}\n", binding, phase, now_seconds(handle));

    if (phase == 'd') {
        on_key_down(handle, hold, clock, direction, /*enable_hold=*/true);
    } else if (phase == 'p') {
        on_key_down(handle, hold, clock, direction, /*enable_hold=*/false);
    } else if (phase == 'u') {
        on_key_up(handle, hold, clock, direction);
    }
    // phase == 'r'：mpv 自己的按键自动重复通知，本插件的连续 seek 不依赖它，
    // 忽略即可（上面已经打印出来，方便 debug 时确认它出现的频率）。
}

} // namespace

extern "C" int mpv_open_cplugin(mpv_handle *handle) {
    HoldState hold;
    KeyDownClock clock;

    while (true) {
        double timeout = hold.active() ? kHoldTiming.repeat_interval : -1.0;
        mpv_event *event = mpv_wait_event(handle, timeout);

        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            return 0;
        }

        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
            handle_client_message(handle, static_cast<mpv_event_client_message *>(event->data), hold, clock);
        }

        // 不能只在超时返回的 MPV_EVENT_NONE 里才检查是否该 tick：
        // script-binding 命令默认 allow_auto_repeat=true，按住方向键时 mpv
        // 会按 --input-ar-rate（默认约每 25ms 一次）持续推 phase='r' 的
        // key-binding message，这个频率比我们自己的 repeat_interval（100ms）
        // 密得多，会一直把 mpv_wait_event 提前唤醒、抢在超时之前，导致
        // MPV_EVENT_NONE 分支几乎永远走不到、hold.poll() 被活活饿死（实测：
        // 真实长按时除了刚进入连续模式那一下，之后再也没有 poll 被调用过）。
        // 所以每次事件循环醒来（不管因为什么醒的）都检查一次，把 mpv 自己
        // 密集的自动重复通知当成心跳来驱动 tick，而不是依赖我们自己的
        // 超时——poll() 本身是按真实经过时间算该不该 tick，被更频繁地调用
        // 完全没问题，多余的调用只会返回 0 个 tick。
        if (hold.active()) {
            int ticks = hold.poll(now_seconds(handle), kHoldTiming);
            if (ticks > 0) {
                seek_by(handle, hold.direction() * kStepSeconds * ticks);
                show_seek_status(handle);
                MPV_UTIL_DEBUG("poll: direction={} ticks={} now={:.4f} pos_after={:.4f}\n", hold.direction(),
                                ticks, now_seconds(handle), time_pos(handle));
            }
        }
    }
}
