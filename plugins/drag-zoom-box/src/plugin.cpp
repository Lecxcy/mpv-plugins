#include <mpv/client.h>

#include <cstring>
#include <optional>
#include <string>

#include <fmt/core.h>

#include "drag_zoom_box/logic.h"
#include "shared/cpp/mpv_util.h"

namespace {

using namespace drag_zoom_box;

// 对应旧 shared/lua/plugin_config.lua 里 drag_zoom_box 的配置，C 插件不解析
// Lua 配置文件，直接照搬同样的数值（做法与 enhanced-drag 一致）。
constexpr double kRefreshIntervalSeconds = 1.0 / 60.0;
constexpr double kMinDragPixels = 4.0;
constexpr double kBorderWidth = 2.0;
constexpr const char *kColorZoom = "00FF00";
constexpr const char *kColorReset = "0000FF";
// 拖拽方向还没落在合法对角线上时（不满足两个轴都过阈值、同号）展示的中性
// 颜色，让用户能立刻看出"这个方向不会触发任何动作"，这是上游 Lua 版本没有
// 的反馈——上游只有 zoom/reset 两种颜色，任何拖拽方向都会被归到其中一种。
constexpr const char *kColorNeutral = "808080";
constexpr const char *kBindingName = "drag-zoom-box";
constexpr int kOverlayZ = 1000;
constexpr double kResetOsdDurationSeconds = 1.0;
constexpr double kGeometryErrorOsdDurationSeconds = 1.0;

// mouse-pos/osd-dimensions/video-target-params 统一整个属性读成
// MPV_FORMAT_NODE 再从 node map 里取字段，不用 "mouse-pos/x" 这种子属性
// 路径 + 显式请求 MPV_FORMAT_DOUBLE 的写法——enhanced-drag 已经踩过这个坑
// （见其 README「已修复的问题」）：子属性路径在实测环境下会静默返回 0。
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

template <typename Fn>
void with_node_map(mpv_handle *handle, const char *name, Fn &&use) {
    mpv_node node;
    if (mpv_get_property(handle, name, MPV_FORMAT_NODE, &node) < 0) {
        return;
    }
    if (node.format == MPV_FORMAT_NODE_MAP) {
        use(*node.u.list);
    }
    mpv_free_node_contents(&node);
}

// 读取整个窗口尺寸，独立于视频是否已加载、几何是否可算——拖拽框选可以在
// 视频几何暂不可用时也照常显示（只是最终应用缩放时会失败并提示）。
struct WindowSize {
    double w = 0.0;
    double h = 0.0;
};

std::optional<WindowSize> read_window_size(mpv_handle *handle) {
    WindowSize size;
    with_node_map(handle, "osd-dimensions", [&](const mpv_node_list &list) {
        size.w = node_map_get_number(list, "w", 0.0);
        size.h = node_map_get_number(list, "h", 0.0);
    });
    if (size.w <= 0.0 || size.h <= 0.0) {
        return std::nullopt;
    }
    return size;
}

std::optional<Point> read_mouse_pos(mpv_handle *handle) {
    // hover 字段不检查：三个读取时机（down/更新/up）必然都有一个按键正按
    // 着，"hover 为 false 时忽略坐标"这条规则的前提本就不成立，enhanced-drag
    // 已经验证过这个检查是多余的（见其 README「已修复的问题」）。
    bool found = false;
    Point pos;
    with_node_map(handle, "mouse-pos", [&](const mpv_node_list &list) {
        pos.x = node_map_get_number(list, "x", 0.0);
        pos.y = node_map_get_number(list, "y", 0.0);
        found = true;
    });
    if (!found) {
        return std::nullopt;
    }
    return pos;
}

std::optional<Geometry> read_geometry(mpv_handle *handle) {
    auto window = read_window_size(handle);
    if (!window) {
        MPV_UTIL_DEBUG("read_geometry: osd-dimensions w/h unavailable\n");
        return std::nullopt;
    }

    double margin_left = 0.0, margin_right = 0.0, margin_top = 0.0, margin_bottom = 0.0;
    with_node_map(handle, "osd-dimensions", [&](const mpv_node_list &list) {
        margin_left = node_map_get_number(list, "ml", 0.0);
        margin_right = node_map_get_number(list, "mr", 0.0);
        margin_top = node_map_get_number(list, "mt", 0.0);
        margin_bottom = node_map_get_number(list, "mb", 0.0);
    });

    double dw = 0.0, dh = 0.0;
    bool have_display_size = false;
    const char *display_size_source = "none";
    auto read_display_size = [&](const char *name) {
        with_node_map(handle, name, [&](const mpv_node_list &list) {
            double w = node_map_get_number(list, "dw", 0.0);
            double h = node_map_get_number(list, "dh", 0.0);
            if (w > 0.0 && h > 0.0) {
                dw = w;
                dh = h;
                have_display_size = true;
                display_size_source = name;
            }
        });
    };
    read_display_size("video-target-params");
    if (!have_display_size) {
        read_display_size("video-out-params");
    }

    MPV_UTIL_DEBUG("read_geometry: osd=({:.1f},{:.1f}) margins=(l{:.1f},r{:.1f},t{:.1f},b{:.1f}) "
                   "dw/dh=({:.1f},{:.1f}) from={} zoom={:.4f} pan=({:.4f},{:.4f})\n",
                   window->w, window->h, margin_left, margin_right, margin_top, margin_bottom, dw, dh,
                   display_size_source, mpv_util::get_double(handle, "video-zoom", 0.0),
                   mpv_util::get_double(handle, "video-pan-x", 0.0), mpv_util::get_double(handle, "video-pan-y", 0.0));

    if (!have_display_size) {
        MPV_UTIL_DEBUG("read_geometry: dw/dh unavailable from either video-target-params or video-out-params\n");
        return std::nullopt;
    }

    auto geometry = compute_geometry(window->w, window->h, margin_left, margin_right, margin_top, margin_bottom, dw, dh);
    if (!geometry) {
        MPV_UTIL_DEBUG("read_geometry: compute_geometry rejected the inputs above (degenerate rect)\n");
        return std::nullopt;
    }

    MPV_UTIL_DEBUG("read_geometry: rect=({:.1f},{:.1f}) scaled=({:.1f},{:.1f}) base=({:.1f},{:.1f})\n",
                   geometry->rect_x, geometry->rect_y, geometry->scaled_w, geometry->scaled_h, geometry->base_w,
                   geometry->base_h);
    return geometry;
}

void set_overlay(mpv_handle *handle, const std::string &format, const std::string &data, int res_x, int res_y,
                  int z) {
    std::string res_x_str = fmt::format("{}", res_x);
    std::string res_y_str = fmt::format("{}", res_y);
    std::string z_str = fmt::format("{}", z);
    const char *args[] = {"osd-overlay", "0", format.c_str(), data.c_str(), res_x_str.c_str(),
                           res_y_str.c_str(), z_str.c_str(), nullptr};
    mpv_command(handle, args);
}

void clear_overlay(mpv_handle *handle) {
    set_overlay(handle, "none", "", 0, 720, 0);
}

const char *color_for_direction(DragDirection direction) {
    switch (direction) {
    case DragDirection::kZoomIn:
        return kColorZoom;
    case DragDirection::kReset:
        return kColorReset;
    case DragDirection::kNone:
    default:
        return kColorNeutral;
    }
}

void draw_overlay(mpv_handle *handle, const WindowSize &window, const Box &box, DragDirection direction) {
    std::string data = fmt::format("{{\\an7\\pos(0,0)\\bord{:.3f}\\shad0\\1a&HFF&\\3a&H00&\\3c&H{}&\\p1}}"
                                    "m {:.3f} {:.3f} l {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}{{\\p0}}",
                                    kBorderWidth, color_for_direction(direction), box.x1, box.y1, box.x2, box.y1,
                                    box.x2, box.y2, box.x1, box.y2, box.x1, box.y1);
    set_overlay(handle, "ass-events", data, static_cast<int>(window.w), static_cast<int>(window.h), kOverlayZ);
}

void reset_zoom(mpv_handle *handle) {
    mpv_util::set_double(handle, "video-zoom", 0.0);
    mpv_util::set_double(handle, "video-pan-x", 0.0);
    mpv_util::set_double(handle, "video-pan-y", 0.0);
    mpv_util::show_osd_message(handle, "Zoom reset", kResetOsdDurationSeconds);
}

void apply_zoom_in(mpv_handle *handle, const Box &box) {
    MPV_UTIL_DEBUG("apply_zoom_in: box=({:.1f},{:.1f})-({:.1f},{:.1f}) size=({:.1f},{:.1f})\n", box.x1, box.y1,
                   box.x2, box.y2, box.x2 - box.x1, box.y2 - box.y1);

    auto geometry = read_geometry(handle);
    if (!geometry) {
        mpv_util::show_osd_message(handle, "Video geometry unavailable", kGeometryErrorOsdDurationSeconds);
        return;
    }

    auto result = compute_zoom(*geometry, box);
    if (!result) {
        MPV_UTIL_DEBUG("apply_zoom_in: compute_zoom rejected the box (collapsed or outside visible rect)\n");
        return;
    }

    MPV_UTIL_DEBUG("apply_zoom_in: u/v=({:.4f},{:.4f})-({:.4f},{:.4f}) scale_x={:.4f} scale_y={:.4f} "
                   "-> zoom={:.4f} pan=({:.4f},{:.4f})\n",
                   result->u1, result->v1, result->u2, result->v2, result->scale_x, result->scale_y, result->zoom,
                   result->pan_x, result->pan_y);

    mpv_util::set_double(handle, "video-zoom", result->zoom);
    mpv_util::set_double(handle, "video-pan-x", result->pan_x);
    mpv_util::set_double(handle, "video-pan-y", result->pan_y);
}

class DragSession {
public:
    bool active() const { return dragging_; }

    void begin(mpv_handle *handle) {
        auto pos = read_mouse_pos(handle);
        if (!pos) {
            MPV_UTIL_DEBUG("begin: mouse-pos unavailable, drag not started\n");
            return;
        }
        MPV_UTIL_DEBUG("begin: mouse=({:.1f},{:.1f})\n", pos->x, pos->y);
        dragging_ = true;
        start_ = *pos;
        current_ = *pos;
        redraw(handle);
    }

    void update(mpv_handle *handle) {
        if (!dragging_) {
            return;
        }
        if (auto pos = read_mouse_pos(handle)) {
            current_ = *pos;
        }
        redraw(handle);
    }

    void finish(mpv_handle *handle) {
        if (!dragging_) {
            return;
        }
        if (auto pos = read_mouse_pos(handle)) {
            current_ = *pos;
        }
        dragging_ = false;
        clear_overlay(handle);

        Box box = normalize_box(start_, current_);
        double dx = current_.x - start_.x;
        double dy = current_.y - start_.y;
        DragDirection direction = classify_direction(dx, dy, kMinDragPixels);
        MPV_UTIL_DEBUG("finish: start=({:.1f},{:.1f}) current=({:.1f},{:.1f}) dx={:.1f} dy={:.1f} direction={}\n",
                       start_.x, start_.y, current_.x, current_.y, dx, dy, static_cast<int>(direction));
        switch (direction) {
        case DragDirection::kZoomIn:
            apply_zoom_in(handle, box);
            break;
        case DragDirection::kReset:
            reset_zoom(handle);
            break;
        case DragDirection::kNone:
            break;
        }
    }

private:
    void redraw(mpv_handle *handle) {
        auto window = read_window_size(handle);
        if (!window) {
            clear_overlay(handle);
            return;
        }
        Box box = normalize_box(start_, current_);
        DragDirection direction = classify_direction(current_.x - start_.x, current_.y - start_.y, kMinDragPixels);
        draw_overlay(handle, *window, box, direction);
    }

    bool dragging_ = false;
    Point start_;
    Point current_;
};

void handle_client_message(mpv_handle *handle, mpv_event_client_message *message, DragSession &session) {
    if (message->num_args < 3 || std::strcmp(message->args[0], "key-binding") != 0 ||
        std::strcmp(message->args[1], kBindingName) != 0) {
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
            clear_overlay(handle);
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
