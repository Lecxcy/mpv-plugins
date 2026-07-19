#pragma once

#include <optional>
#include <string>

// 纯逻辑层：不依赖 mpv client API。覆盖两块内容——OSD 文本格式化，以及长按
// 连续快进/快退的节奏状态机（HoldState）。两者都不触碰 mpv 属性/命令，方便
// 直接用 Catch2 测试，真正的 mpv 交互（读 time-pos/duration、发 seek 命令、
// 显示 OSD）留在 src/plugin.cpp。
namespace enhanced_seek {

// 时长格式化：>=1 小时时 "H:MM:SS"，否则 "MM:SS"；负数按 0 处理。
std::string format_time(double seconds);

// 拼出与原 Lua 版本一致的 "position/duration (percent%)" 文本；percent 为
// nullopt（对应 mpv percent-pos 属性不可用，例如没有 duration 信息）时按
// "0%" 展示。
std::string format_status(double position, double duration, std::optional<double> percent);

// 长按连续快进/快退的节奏参数。用户明确要求不做 enhanced-volume 那种随按住
// 时长加速的 profile，进入连续模式后用恒定间隔重复触发，速度全程不变。
struct HoldTiming {
    double hold_delay;     // 按下到开始连续触发之间的延迟，短按不会触发连续 seek
    double repeat_interval; // 连续触发之间的恒定间隔
};

// 长按方向键的节奏状态机：只记录“当前按住的方向”和“下一次连续触发的时间
// 点”，不知道 seek 的具体步长，也不碰任何 mpv 状态。
class HoldState {
public:
    bool active() const { return direction_ != 0; }
    int direction() const { return direction_; }

    // 按下方向键（direction 为 +1/-1）：记录方向，把下一次连续触发安排在
    // now + hold_delay。两个方向键同时按住时，后按下的方向会覆盖前一个——
    // 和 enhanced-volume 的 Lua 实现一样，同一时刻只支持一个方向连续触发。
    void begin(int direction, double now, const HoldTiming &timing);

    // 松开方向键：只有 direction 与当前持有的方向一致才会真正停止，避免
    // “按住 A 时按了一下 B 又松开 B”误把 A 的连续触发打断。
    void stop(int direction);

    // 轮询是否到了该触发下一次连续 seek 的时间，返回应该触发的次数（正常
    // 情况下是 0 或 1；只有轮询被推迟——比如进程被挂起后恢复——才会 > 1，
    // 用于追平被拖慢的连续 seek 距离，调用方应该把这个次数乘以单次 seek
    // 步长，一次性发出，而不是真的循环调用 mpv 命令 N 次）。
    int poll(double now, const HoldTiming &timing);

private:
    int direction_ = 0;
    double next_tick_at_ = 0.0;
};

} // namespace enhanced_seek
