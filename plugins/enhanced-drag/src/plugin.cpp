#include <mpv/client.h>

#include <cstring>

#include "enhanced_drag/logic.h"
#include "shared/cpp/mpv_util.h"

namespace {

constexpr double kRefreshIntervalSeconds = 1.0 / 120.0;
constexpr const char *kBindingName = "drag-pan-free";

// mouse-pos、osd-dimensions 都是 NODE_MAP 类型的属性，各字段本身是
// INT64/FLAG。实测发现：用形如 "mouse-pos/x" 的子属性路径 + 显式请求
// MPV_FORMAT_DOUBLE/FLAG 去读，会稳定返回 0/false（不报错，值就是错的），
// 但直接读整个父属性（MPV_FORMAT_NODE）再从 node map 里取字段，值是正确
// 且实时更新的——和 mpv 自带 Lua 绑定 mp.get_property_native("mouse-pos")
// 的做法一致。因此这里统一走"读整个 NODE 再取字段"这条路径，不再使用子
// 属性路径。
double node_map_get_number(const mpv_node_list &list, const char *key, double fallback) {
    for (int i = 0; i < list.num; ++i) {
        if (std::strcmp(list.keys[i], key) != 0) {
            continue;
        }
        const mpv_node &value = list.values[i];
        if (value.format == MPV_FORMAT_INT64) {
            return static_cast<double>(value.u.int64);
        }
        if (value.format == MPV_FORMAT_DOUBLE) {
            return value.u.double_;
        }
        break;
    }
    return fallback;
}

void with_node_map(mpv_handle *handle, const char *name, void (*use)(const mpv_node_list &, void *),
                   void *ctx) {
    mpv_node node;
    if (mpv_get_property(handle, name, MPV_FORMAT_NODE, &node) < 0) {
        return;
    }
    if (node.format == MPV_FORMAT_NODE_MAP) {
        use(*node.u.list, ctx);
    }
    mpv_free_node_contents(&node);
}

struct MousePos {
    double x = 0.0;
    double y = 0.0;
};

MousePos read_mouse_pos(mpv_handle *handle) {
    MousePos pos;
    with_node_map(
        handle, "mouse-pos",
        [](const mpv_node_list &list, void *ctx) {
            auto *out = static_cast<MousePos *>(ctx);
            out->x = node_map_get_number(list, "x", 0.0);
            out->y = node_map_get_number(list, "y", 0.0);
        },
        &pos);
    return pos;
}

enhanced_drag::Geometry read_geometry(mpv_handle *handle) {
    enhanced_drag::Geometry geometry;
    with_node_map(
        handle, "osd-dimensions",
        [](const mpv_node_list &list, void *ctx) {
            auto *out = static_cast<enhanced_drag::Geometry *>(ctx);
            out->window_w = node_map_get_number(list, "w", 0.0);
            out->window_h = node_map_get_number(list, "h", 0.0);
            out->margin_left = node_map_get_number(list, "ml", 0.0);
            out->margin_right = node_map_get_number(list, "mr", 0.0);
            out->margin_top = node_map_get_number(list, "mt", 0.0);
            out->margin_bottom = node_map_get_number(list, "mb", 0.0);
        },
        &geometry);
    return geometry;
}

void apply_pan(mpv_handle *handle, const enhanced_drag::PanResult &pan) {
    mpv_util::set_double(handle, "video-pan-x", pan.pan_x);
    mpv_util::set_double(handle, "video-pan-y", pan.pan_y);
}

class DragSession {
public:
    bool active() const { return active_; }

    void begin(mpv_handle *handle) {
        MousePos pos = read_mouse_pos(handle);

        geometry_ = read_geometry(handle);
        MPV_UTIL_DEBUG("begin mouse=({:.2f},{:.2f}) margin=({:.2f},{:.2f},{:.2f},{:.2f})\n", pos.x,
                       pos.y, geometry_.margin_left, geometry_.margin_right, geometry_.margin_top,
                       geometry_.margin_bottom);

        origin_.mouse_x = pos.x;
        origin_.mouse_y = pos.y;
        origin_.pan_x = mpv_util::get_double(handle, "video-pan-x", 0.0);
        origin_.pan_y = mpv_util::get_double(handle, "video-pan-y", 0.0);
        active_ = true;
    }

    void update(mpv_handle *handle) {
        if (!active_) {
            return;
        }
        MousePos pos = read_mouse_pos(handle);
        auto pan = enhanced_drag::compute_drag_pan(geometry_, origin_, pos.x, pos.y);
        MPV_UTIL_DEBUG("update mouse=({:.2f},{:.2f}) pan=({:.4f},{:.4f})\n", pos.x, pos.y, pan.pan_x,
                       pan.pan_y);
        apply_pan(handle, pan);
    }

    void finish(mpv_handle *handle) {
        update(handle);
        active_ = false;
    }

private:
    bool active_ = false;
    enhanced_drag::Geometry geometry_;
    enhanced_drag::DragOrigin origin_;
};

void handle_client_message(mpv_handle *handle, mpv_event_client_message *message, DragSession &session) {
    if (message->num_args < 3) {
        return;
    }
    if (std::strcmp(message->args[0], "key-binding") != 0) {
        return;
    }
    if (std::strcmp(message->args[1], kBindingName) != 0) {
        return;
    }

    char phase = message->args[2][0];
    if (phase == 'd') {
        session.begin(handle);
    } else if (phase == 'u') {
        session.finish(handle);
    }
}

} // namespace

extern "C" int mpv_open_cplugin(mpv_handle *handle) {
    DragSession session;

    while (true) {
        double timeout = session.active() ? kRefreshIntervalSeconds : -1.0;
        mpv_event *event = mpv_wait_event(handle, timeout);

        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            return 0;
        }

        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
            handle_client_message(handle, static_cast<mpv_event_client_message *>(event->data), session);
            continue;
        }

        if (event->event_id == MPV_EVENT_NONE) {
            session.update(handle);
        }
    }
}
