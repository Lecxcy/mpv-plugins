#pragma once

#include <string>
#include <vector>

// 纯逻辑层：不依赖 mpv client API，只处理音量数值计算和长按连发的调速状态机。
namespace enhanced_volume {

double clamp(double value, double min_value, double max_value);

// 音量加/减 delta 后夹到 [0, max_volume]；对应原 Lua adjust_volume 里的
// clamp(current + delta, 0, config.max_volume)。
double next_volume(double current, double delta, double max_volume);

// OSD 展示文案：静音时固定 "Mute"，否则按四舍五入（对应 Lua
// math.floor(volume + 0.5)）展示整数百分比。
std::string format_volume_label(double volume, bool muted);

// 长按连发的调速阶梯：按 elapsed 升序排列，取第一个 elapsed_since_start <
// step.elapsed 的 interval；全部都不满足时用最后一项（原 Lua
// get_hold_profile 用 math.huge 兜底最后一档，这里改为约定"落到末尾"）。
struct HoldProfileStep {
    double elapsed = 0.0;
    double interval = 0.0;
};

double resolve_hold_interval(const std::vector<HoldProfileStep> &profile, double elapsed_since_start);

// 一次长按会话的状态。direction 为 +1/-1，started_at 是按下时刻，next_at 是
// 下一次应该触发连发的时刻（原 Lua active_hold 表的字段照搬）。
struct HoldState {
    int direction = 0;
    double started_at = 0.0;
    double next_at = 0.0;
};

HoldState start_hold(int direction, double now, double hold_delay);

// 到了 next_at 才触发一次连发：返回 true 表示应该再调一次音量，并把 next_at
// 按 resolve_hold_interval(profile, now - started_at) 推到下一次；没到时间
// 原样返回 false，不改状态。对应原 Lua tick_hold 的判断+副作用逻辑。
bool tick_hold(HoldState &state, double now, const std::vector<HoldProfileStep> &profile);

} // namespace enhanced_volume
