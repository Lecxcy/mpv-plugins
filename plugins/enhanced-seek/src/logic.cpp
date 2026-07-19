#include "enhanced_seek/logic.h"

#include <fmt/core.h>

namespace enhanced_seek {

std::string format_time(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }

    long total = static_cast<long>(seconds + 0.5);
    long hours = total / 3600;
    long minutes = (total % 3600) / 60;
    long secs = total % 60;

    if (hours > 0) {
        return fmt::format("{}:{:02}:{:02}", hours, minutes, secs);
    }
    return fmt::format("{:02}:{:02}", minutes, secs);
}

std::string format_status(double position, double duration, std::optional<double> percent) {
    std::string percent_text = percent ? fmt::format("{:.1f}%", *percent) : "0%";
    return fmt::format("{}/{} ({})", format_time(position), format_time(duration), percent_text);
}

void HoldState::begin(int direction, double now, const HoldTiming &timing) {
    direction_ = direction;
    next_tick_at_ = now + timing.hold_delay;
}

void HoldState::stop(int direction) {
    if (direction_ == direction) {
        direction_ = 0;
    }
}

int HoldState::poll(double now, const HoldTiming &timing) {
    if (!active()) {
        return 0;
    }

    // 轮询被推迟太多（例如宿主进程被挂起后恢复，now 一次性跳了一大截）时
    // 不做无界追平，避免一次性算出天文数字般的 tick 数、进而在 plugin.cpp
    // 里发出一次跨度离谱的 seek。
    constexpr int kMaxTicksPerPoll = 1000;

    int ticks = 0;
    while (now >= next_tick_at_ && ticks < kMaxTicksPerPoll) {
        next_tick_at_ += timing.repeat_interval;
        ++ticks;
    }
    return ticks;
}

} // namespace enhanced_seek
