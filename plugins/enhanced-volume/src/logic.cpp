#include "enhanced_volume/logic.h"

#include <cmath>

#include <fmt/core.h>

namespace enhanced_volume {

double clamp(double value, double min_value, double max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

double next_volume(double current, double delta, double max_volume) {
    return clamp(current + delta, 0.0, max_volume);
}

std::string format_volume_label(double volume, bool muted) {
    if (muted) {
        return "Mute";
    }
    return fmt::format("Volume: {}%", static_cast<long>(std::floor(volume + 0.5)));
}

double resolve_hold_interval(const std::vector<HoldProfileStep> &profile, double elapsed_since_start) {
    for (const auto &step : profile) {
        if (elapsed_since_start < step.elapsed) {
            return step.interval;
        }
    }
    return profile.empty() ? 0.0 : profile.back().interval;
}

HoldState start_hold(int direction, double now, double hold_delay) {
    return HoldState{direction, now, now + hold_delay};
}

bool tick_hold(HoldState &state, double now, const std::vector<HoldProfileStep> &profile) {
    if (now < state.next_at) {
        return false;
    }
    double elapsed = now - state.started_at;
    state.next_at = now + resolve_hold_interval(profile, elapsed);
    return true;
}

} // namespace enhanced_volume
