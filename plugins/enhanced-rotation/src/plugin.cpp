#include <mpv/client.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "enhanced_rotation/logic.h"
#include "shared/cpp/mpv_util.h"

namespace {

constexpr double kOsdDurationSeconds = 1.0;

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

void rotate(mpv_handle *handle, double delta) {
    double current = mpv_util::get_double(handle, "video-rotate", 0.0);
    double next = enhanced_rotation::normalize_rotation(current + delta);
    mpv_util::set_double(handle, "video-rotate", next);
    mpv_util::show_osd_message(
        handle,
        "Rotation: " + std::to_string(static_cast<long>(std::llround(next))) + "°",
        kOsdDurationSeconds);
}

void reset_rotation(mpv_handle *handle) {
    mpv_util::set_double(handle, "video-rotate", 0.0);
    mpv_util::show_osd_message(handle, "Rotation: 0°", kOsdDurationSeconds);
}

void handle_client_message(mpv_handle *handle, mpv_event_client_message *message) {
    if (message->num_args < 1) {
        return;
    }
    if (std::strcmp(message->args[0], "rotate-video") == 0) {
        double delta = message->num_args > 1 ? parse_delta(message->args[1]) : 0.0;
        rotate(handle, delta);
    } else if (std::strcmp(message->args[0], "rotate-video-reset") == 0) {
        reset_rotation(handle);
    }
}

} // namespace

extern "C" int mpv_open_cplugin(mpv_handle *handle) {
    while (true) {
        mpv_event *event = mpv_wait_event(handle, -1);
        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            return 0;
        }
        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
            handle_client_message(handle, static_cast<mpv_event_client_message *>(event->data));
        }
    }
}
