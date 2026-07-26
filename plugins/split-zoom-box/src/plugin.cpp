#include <mpv/client.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/core.h>

#include "shared/cpp/mpv_util.h"
#include "split_zoom_box/layout.h"
#include "split_zoom_box/store.h"

namespace {

using namespace split_zoom_box;

constexpr double kRefreshIntervalSeconds = 1.0 / 60.0;
constexpr double kMinDragPixels = 4.0;
constexpr double kBorderWidth = 2.0;
// 焦点框比框选框粗，否则在窗格边缘很难一眼看出选中的是哪个窗格。
constexpr double kFocusBorderWidth = 5.0;

// 注意：ASS 的颜色是 &HBBGGRR&（蓝绿红），不是常见的 RRGGBB。
// 下面这几个常量都按 ASS 的字节序写。kColorZoom/kColorNeutral 前后对称，
// 两种解读一样；kColorReset 承接自 drag-zoom-box，实际渲染出来是红色
// （旧文档里写成"蓝色"是按 RRGGBB 误读的）。
constexpr const char *kColorZoom = "00FF00";    // 绿
constexpr const char *kColorReset = "0000FF";   // 红
// 拖拽方向还没落在合法对角线上时展示的中性色，让用户在拖拽过程中就能看出
// "这个方向不会触发任何动作"。承接自 drag-zoom-box。
constexpr const char *kColorNeutral = "808080"; // 灰
constexpr const char *kColorFocus = "00A5FF";   // 橙

constexpr int kSelectionOverlayId = 0;
constexpr int kFocusOverlayId = 1;
constexpr int kOverlayZ = 1000;

constexpr const char *kFilterLabel = "split-zoom-box";
constexpr double kOsdDuration = 1.6;
constexpr double kConfirmOsdDuration = 24.0 * 3600.0;
constexpr std::size_t kContentSampleBytes = 65536;
// 窗格再小就没有观察价值了，而且过小的 crop/scale 容易让滤镜图配置失败。
constexpr int kMinPanePixels = 16;

// ---- 基础封装 ----

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

// 整个属性读成 MPV_FORMAT_NODE 再取字段，不用 "mouse-pos/x" 这种子属性路径
// ——enhanced-drag 踩过这个坑：子属性路径在实测环境下会静默返回 0。
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

int run_command(mpv_handle *h, std::initializer_list<const char *> args) {
    std::vector<const char *> argv(args);
    argv.push_back(nullptr);
    return mpv_command(h, argv.data());
}

std::string get_string_property(mpv_handle *h, const char *name) {
    char *value = nullptr;
    if (mpv_get_property(h, name, MPV_FORMAT_STRING, &value) < 0 || value == nullptr) {
        return "";
    }
    std::string out = value;
    mpv_free(value);
    return out;
}

std::string expand_path(mpv_handle *h, const std::string &path) {
    const char *args[] = {"expand-path", path.c_str(), nullptr};
    mpv_node result;
    if (mpv_command_ret(h, args, &result) < 0) {
        return "";
    }
    std::string out;
    if (result.format == MPV_FORMAT_STRING && result.u.string) {
        out = result.u.string;
    }
    mpv_free_node_contents(&result);
    return out;
}

std::optional<std::string> read_file_text(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool write_file_text(const std::string &path, const std::string &text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << text;
    return static_cast<bool>(file);
}

// ---- 几何 ----

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
    // 不检查 hover：三个读取时机（按下/拖拽中/松开）必然都有一个鼠标键正
    // 按着，"hover 为 false 时忽略坐标"这条规则的前提本就不成立。
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

// 当前渲染进窗口的矩形。多窗格生效时这里的"视频"已经是拼接后的画布，
// 所以同一套换算在两条路径下都成立。
std::optional<Geometry> read_geometry(mpv_handle *handle) {
    auto window = read_window_size(handle);
    if (!window) {
        return std::nullopt;
    }

    double ml = 0.0, mr = 0.0, mt = 0.0, mb = 0.0;
    with_node_map(handle, "osd-dimensions", [&](const mpv_node_list &list) {
        ml = node_map_get_number(list, "ml", 0.0);
        mr = node_map_get_number(list, "mr", 0.0);
        mt = node_map_get_number(list, "mt", 0.0);
        mb = node_map_get_number(list, "mb", 0.0);
    });

    double dw = 0.0, dh = 0.0;
    bool have = false;
    auto read_display_size = [&](const char *name) {
        with_node_map(handle, name, [&](const mpv_node_list &list) {
            double w = node_map_get_number(list, "dw", 0.0);
            double h = node_map_get_number(list, "dh", 0.0);
            if (w > 0.0 && h > 0.0) {
                dw = w;
                dh = h;
                have = true;
            }
        });
    };
    read_display_size("video-target-params");
    if (!have) {
        read_display_size("video-out-params");
    }
    if (!have) {
        return std::nullopt;
    }
    return compute_geometry(window->w, window->h, ml, mr, mt, mb, dw, dh);
}

// 源画面的存储尺寸（用于 crop 坐标）和显示尺寸（用作画布尺寸，保证拼接结果
// 的显示宽高比与原片一致）。
struct SourceSize {
    int src_w = 0;
    int src_h = 0;
    int canvas_w = 0;
    int canvas_h = 0;
};

std::optional<SourceSize> read_source_size(mpv_handle *handle) {
    SourceSize size;
    with_node_map(handle, "video-params", [&](const mpv_node_list &list) {
        size.src_w = static_cast<int>(node_map_get_number(list, "w", 0.0));
        size.src_h = static_cast<int>(node_map_get_number(list, "h", 0.0));
        size.canvas_w = static_cast<int>(node_map_get_number(list, "dw", 0.0));
        size.canvas_h = static_cast<int>(node_map_get_number(list, "dh", 0.0));
    });
    if (size.src_w <= 0 || size.src_h <= 0) {
        return std::nullopt;
    }
    if (size.canvas_w <= 0 || size.canvas_h <= 0) {
        size.canvas_w = size.src_w;
        size.canvas_h = size.src_h;
    }
    return size;
}

// ---- OSD ----

void set_overlay(mpv_handle *handle, int id, const std::string &format, const std::string &data, int res_x,
                  int res_y, int z) {
    std::string id_str = fmt::format("{}", id);
    std::string res_x_str = fmt::format("{}", res_x);
    std::string res_y_str = fmt::format("{}", res_y);
    std::string z_str = fmt::format("{}", z);
    // osd-overlay 的参数按 mpv 内部定义的位置顺序解析（id/format/data/
    // res_x/res_y/z/hidden/compute_bounds），不依赖具名参数。
    const char *args[] = {"osd-overlay",     id_str.c_str(),    format.c_str(), data.c_str(),
                           res_x_str.c_str(), res_y_str.c_str(), z_str.c_str(),  nullptr};
    mpv_command(handle, args);
}

void clear_overlay(mpv_handle *handle, int id) {
    set_overlay(handle, id, "none", "", 0, 720, 0);
}

std::string ass_rect(double x1, double y1, double x2, double y2, const char *color,
                      double border = kBorderWidth) {
    return fmt::format("{{\\an7\\pos(0,0)\\bord{:.3f}\\shad0\\1a&HFF&\\3a&H00&\\3c&H{}&\\p1}}"
                       "m {:.3f} {:.3f} l {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}{{\\p0}}",
                       border, color, x1, y1, x2, y1, x2, y2, x1, y2, x1, y1);
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

// ---- 插件状态 ----

struct PluginState {
    mpv_handle *handle = nullptr;

    Layout base = make_layout();
    std::vector<LayoutSegment> segments;

    // 已经实际下发给 mpv 的状态标识。只有它变化时才重建滤镜/改 zoom，
    // 否则 time-pos 每帧回调都会触发一次重建。
    std::string applied_key;

    std::string current_path;
    std::string current_path_key;
    std::string current_content_hash;
    std::optional<std::string> rename_from;
    bool awaiting_confirm = false;
    store::FileEntry pending_entry;
    bool paused_before_confirm = false;

    std::optional<double> pending_segment_start;

    bool dragging = false;
    Point drag_start;
    Point drag_current;
    // 多窗格时限制拖拽范围的窗格屏幕矩形（屏幕像素）。
    std::optional<Box> drag_bounds;

    // 平移（承接自已合并的 enhanced-drag）
    bool panning = false;
    bool pan_single = false;   // 单窗格走 video-pan-*，多窗格移动窗格的 Region
    Point pan_last;            // 多窗格增量用
    int pan_leaf = -1;
    Point pan_origin_mouse;    // 单窗格：起点鼠标位置
    double pan_origin_x = 0.0; // 单窗格：起点 video-pan-x/y
    double pan_origin_y = 0.0;
    std::chrono::steady_clock::time_point pan_last_apply{};

    // 拖分隔条调整窗格比例
    bool sizing = false;
    int sizing_node = -1;
    PixelRect sizing_area;
    std::chrono::steady_clock::time_point sizing_last_apply{};
};

// 把分屏段发布成原生 node，供 fork 的 uosc 在进度条上画出来。
// 必须用 MPV_FORMAT_NODE 而不是 JSON 字符串：enhanced-ab-loop 踩过这个坑
// （commit 50029a1）——Lua 侧 observe_property 回调里的 parse_json 会静默
// 返回原字符串而不是 table，属性看着有值、UI 却永远是空的。
void publish_segments_property(mpv_handle *h, const std::vector<LayoutSegment> &segments) {
    static char key_a[] = "a";
    static char key_b[] = "b";

    std::vector<std::array<mpv_node, 2>> map_values(segments.size());
    std::vector<std::array<char *, 2>> map_keys(segments.size());
    std::vector<mpv_node_list> maps(segments.size());
    std::vector<mpv_node> array_values(segments.size());

    for (std::size_t i = 0; i < segments.size(); ++i) {
        map_values[i][0] = {.u = {.double_ = segments[i].a}, .format = MPV_FORMAT_DOUBLE};
        map_values[i][1] = {.u = {.double_ = segments[i].b}, .format = MPV_FORMAT_DOUBLE};
        map_keys[i] = {key_a, key_b};

        maps[i].num = static_cast<int>(map_values[i].size());
        maps[i].values = map_values[i].data();
        maps[i].keys = map_keys[i].data();

        array_values[i].format = MPV_FORMAT_NODE_MAP;
        array_values[i].u.list = &maps[i];
    }

    mpv_node_list array_list{.num = static_cast<int>(segments.size()), .values = array_values.data(),
                              .keys = nullptr};
    mpv_node root{.u = {.list = &array_list}, .format = MPV_FORMAT_NODE_ARRAY};
    // mpv_set_property 在返回前就把整棵 node 拷走了，上面这些局部量只需要活过
    // 这一次调用。
    mpv_set_property(h, "user-data/split-zoom-box/segments", MPV_FORMAT_NODE, &root);
}

double time_pos(mpv_handle *h) {
    return mpv_util::get_double(h, "time-pos", 0.0);
}

// 当前时间点生效的布局：落在某个区间内就用它的，否则用 base。
// 编辑操作也作用在这同一个对象上——"在哪个时间点编辑，就改哪份布局"。
Layout &active_layout(PluginState &state) {
    if (auto index = find_segment_at(state.segments, time_pos(state.handle))) {
        return state.segments[*index].layout;
    }
    return state.base;
}

// ---- 滤镜与缩放的下发 ----

bool has_filter_label(mpv_handle *h, const char *chain, const char *label) {
    mpv_node node;
    if (mpv_get_property(h, chain, MPV_FORMAT_NODE, &node) < 0) {
        return false;
    }
    bool found = false;
    if (node.format == MPV_FORMAT_NODE_ARRAY) {
        for (int i = 0; i < node.u.list->num && !found; ++i) {
            const mpv_node &item = node.u.list->values[i];
            if (item.format != MPV_FORMAT_NODE_MAP) {
                continue;
            }
            const mpv_node_list &map = *item.u.list;
            for (int j = 0; j < map.num; ++j) {
                // vf 属性里的 label 不带 "@" 前缀，但命令参数需要带。
                if (std::strcmp(map.keys[j], "label") == 0 && map.values[j].format == MPV_FORMAT_STRING &&
                    map.values[j].u.string && std::strcmp(map.values[j].u.string, label) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }
    mpv_free_node_contents(&node);
    return found;
}

void remove_split_filter(PluginState &state) {
    if (has_filter_label(state.handle, "vf", kFilterLabel)) {
        run_command(state.handle, {"vf", "remove", (std::string("@") + kFilterLabel).c_str()});
    }
}

void reset_zoom_pan(PluginState &state) {
    mpv_util::set_double(state.handle, "video-zoom", 0.0);
    mpv_util::set_double(state.handle, "video-pan-x", 0.0);
    mpv_util::set_double(state.handle, "video-pan-y", 0.0);
}

// 把画面彻底恢复原状：视口回到完整画面，并强制清掉 video-zoom/pan。
// 单窗格路径下 video-pan-* 是被拖拽直接改的、不进 applied_key，所以只把视口
// 设回完整画面不够——applied_key 没变就会被去重守卫挡掉，偏移会残留。
void reset_view_and_transform(PluginState &state, Layout &layout) {
    set_focused_region(layout, Region{});
    reset_zoom_pan(state);
    state.applied_key.clear();
}

// 解码器当前吐的是不是真硬件帧（hwdec-current 既不是 no、也不以 -copy 结尾）。
//
// 早期实现是在进入多窗格时把 hwdec 切成 auto-copy、退出时还原，让滤镜永远只
// 面对软件帧。那个做法有个致命问题：**改 hwdec 属性会重建解码器**，播放中
// 反复重建会把 VideoToolbox 会话打坏，出现 -12909 "output image buffer is
// null" 的连环解码失败，并且还原的时机和滤镜移除之间存在竞态（详见
// SPEC §6.7）。现在改成完全不碰 hwdec，只是根据当前帧类型选择要不要在图头部
// 加 hwdownload——解码器全程不受打扰。
bool frames_are_hardware(mpv_handle *h) {
    std::string current = get_string_property(h, "hwdec-current");
    if (current.empty() || current == "no") {
        return false;
    }
    return !(current.size() >= 5 && current.compare(current.size() - 5, 5, "-copy") == 0);
}

// 把当前生效的布局下发出去。单窗格走 video-zoom（零拷贝、不碰滤镜链），
// 多窗格才挂滤镜。
//
// 返回值表示这次是否真的改变了输出。调用方据此决定要不要重画焦点框：
// time-pos 每帧都回调，不能每帧都重画叠加层。
bool apply_layout(PluginState &state, bool force = false) {
    Layout &layout = active_layout(state);
    auto size = read_source_size(state.handle);
    if (!size) {
        return false;
    }

    int panes = leaf_count(layout);
    std::string key;
    std::string graph;

    if (panes <= 1) {
        const Region &region = layout.nodes.empty() ? Region{} : layout.nodes[leaf_order(layout).front()].region;
        key = fmt::format("zoom:{:.6f},{:.6f},{:.6f},{:.6f}", region.x1, region.y1, region.x2, region.y2);
    } else {
        // 帧类型决定要不要 hwdownload 前缀，所以它天然进了 key：hwdec 在
        // 外部被改动（用户按键切换、mpv 自己回退）时，key 会变，滤镜随之重建。
        graph = build_filter_graph(layout, size->src_w, size->src_h, size->canvas_w, size->canvas_h,
                                    frames_are_hardware(state.handle));
        if (graph.empty()) {
            return false;
        }
        key = "graph:" + graph;
    }

    if (!force && key == state.applied_key) {
        return false;
    }

    if (panes <= 1) {
        remove_split_filter(state);

        std::vector<int> leaves = leaf_order(layout);
        Region region = leaves.empty() ? Region{} : layout.nodes[leaves.front()].region;
        // 多窗格的视口可能超出源画面（黑边），关到只剩一格后要夹回画面内：
        // 单窗格走的是 video-zoom，那条路径没有"画布上的黑色背景"这个概念。
        region.x1 = std::clamp(region.x1, 0.0, 1.0);
        region.y1 = std::clamp(region.y1, 0.0, 1.0);
        region.x2 = std::clamp(region.x2, region.x1 + kRegionEpsilon, 1.0);
        region.y2 = std::clamp(region.y2, region.y1 + kRegionEpsilon, 1.0);
        if (region_is_full(region)) {
            reset_zoom_pan(state);
        } else {
            auto geometry = read_geometry(state.handle);
            if (!geometry) {
                return false;
            }
            // 换算依赖的是"缩放为 0 时的基准尺寸"，与当前 zoom 无关，
            // 所以可以从任意缩放状态直接算出目标值，不需要先归零。
            auto zoom_pan = region_to_zoom_pan(region, geometry->osd_w, geometry->osd_h, geometry->base_w,
                                                geometry->base_h);
            if (!zoom_pan) {
                return false;
            }
            mpv_util::set_double(state.handle, "video-zoom", zoom_pan->zoom);
            mpv_util::set_double(state.handle, "video-pan-x", zoom_pan->pan_x);
            mpv_util::set_double(state.handle, "video-pan-y", zoom_pan->pan_y);
        }
    } else {
        // 多窗格时整块拼接画面不再额外缩放，否则屏幕坐标反查还要多一层复合。
        reset_zoom_pan(state);

        std::string spec = fmt::format("@{}:lavfi=[{}]", kFilterLabel, graph);
        // vf add 用同一个 label 会替换已有滤镜，这是唯一可行的改区域方式：
        // vf-command 对 lavfi 图里的 crop 实测无效（返回 success 但画面不变）。
        int rc = run_command(state.handle, {"vf", "add", spec.c_str()});
        if (rc < 0) {
            // mpv 在滤镜图非法时会回滚并保留上一版，命令返回值是唯一可靠的
            // 失败信号；不检查的话内存布局会和实际画面静默不一致。
            mpv_util::show_osd_message(state.handle, "Split filter failed", kOsdDuration);
            MPV_UTIL_DEBUG("vf add 失败: {}\n", spec);
            return false;
        }
    }

    state.applied_key = key;
    return true;
}

void clear_all_output(PluginState &state) {
    remove_split_filter(state);
    reset_zoom_pan(state);
    state.applied_key.clear();
}

// ---- 焦点提示 ----

// 某个窗格在画布像素里的矩形，以及它内部**实际画面**所占的矩形（去掉保比
// 缩放产生的黑边）。坐标反查必须用后者。
struct PaneRects {
    PixelRect pane;
    Region view; // 该窗格的有效视口（宽高比已对齐窗格）
};

std::optional<PaneRects> pane_canvas_rects(PluginState &state, const Layout &layout, int leaf) {
    auto size = read_source_size(state.handle);
    if (!size) {
        return std::nullopt;
    }
    std::vector<int> leaves = leaf_order(layout);
    std::vector<PixelRect> rects = compute_pane_rects(layout, size->canvas_w, size->canvas_h);
    if (leaves.size() != rects.size()) {
        return std::nullopt;
    }
    auto it = std::find(leaves.begin(), leaves.end(), leaf);
    if (it == leaves.end()) {
        return std::nullopt;
    }
    PaneRects out;
    out.pane = rects[static_cast<std::size_t>(it - leaves.begin())];
    out.view = fit_view_aspect(layout.nodes[leaf].region, size->src_w, size->src_h, out.pane, true);
    return out;
}

// 画布像素矩形 -> 屏幕像素矩形。
std::optional<Box> canvas_rect_to_screen(PluginState &state, const PixelRect &rect) {
    auto geometry = read_geometry(state.handle);
    auto size = read_source_size(state.handle);
    if (!geometry || !size || size->canvas_w <= 0 || size->canvas_h <= 0) {
        return std::nullopt;
    }
    Box box;
    box.x1 = geometry->rect_x + (static_cast<double>(rect.x) / size->canvas_w) * geometry->scaled_w;
    box.y1 = geometry->rect_y + (static_cast<double>(rect.y) / size->canvas_h) * geometry->scaled_h;
    box.x2 = geometry->rect_x + (static_cast<double>(rect.x + rect.w) / size->canvas_w) * geometry->scaled_w;
    box.y2 = geometry->rect_y + (static_cast<double>(rect.y + rect.h) / size->canvas_h) * geometry->scaled_h;
    return box;
}

// 焦点框画在整个窗格上（表示"这一格是选中的"），不是只画在画面上。
std::optional<Box> pane_screen_rect(PluginState &state, const Layout &layout, int leaf) {
    auto rects = pane_canvas_rects(state, layout, leaf);
    if (!rects) {
        return std::nullopt;
    }
    return canvas_rect_to_screen(state, rects->pane);
}

void draw_focus_overlay(PluginState &state) {
    Layout &layout = active_layout(state);
    auto window = read_window_size(state.handle);
    if (leaf_count(layout) <= 1 || !window) {
        clear_overlay(state.handle, kFocusOverlayId);
        return;
    }
    auto rect = pane_screen_rect(state, layout, layout.focused);
    if (!rect) {
        return;
    }
    // 边框画在窗格内侧：贴着边画的话，相邻两个窗格的框会在中缝重叠成一条线，
    // 分不出高亮的是哪一边。
    double inset = kFocusBorderWidth / 2.0;
    set_overlay(state.handle, kFocusOverlayId, "ass-events",
                ass_rect(rect->x1 + inset, rect->y1 + inset, rect->x2 - inset, rect->y2 - inset, kColorFocus,
                          kFocusBorderWidth),
                static_cast<int>(window->w), static_cast<int>(window->h), kOverlayZ);
}

void refresh(PluginState &state, bool force = false) {
    apply_layout(state, force);
    draw_focus_overlay(state);
}

// time-pos 每帧都回调，只有布局真的换了才重画/清掉焦点框——否则要么 60Hz
// 刷叠加层，要么（像之前那样）进出分屏段时焦点框根本不更新：进段不出现、
// 出段不消失。
void apply_and_sync_overlay(PluginState &state) {
    if (apply_layout(state)) {
        draw_focus_overlay(state);
    }
}

// ---- 拖拽框选 ----

void draw_selection(PluginState &state) {
    auto window = read_window_size(state.handle);
    if (!window) {
        clear_overlay(state.handle, kSelectionOverlayId);
        return;
    }
    Box box = normalize_box(state.drag_start, state.drag_current);
    DragDirection direction = classify_direction(state.drag_current.x - state.drag_start.x,
                                                  state.drag_current.y - state.drag_start.y, kMinDragPixels);
    set_overlay(state.handle, kSelectionOverlayId, "ass-events",
                ass_rect(box.x1, box.y1, box.x2, box.y2, color_for_direction(direction)),
                static_cast<int>(window->w), static_cast<int>(window->h), kOverlayZ);
}

// 把屏幕框选换算成"某个窗格的新源区域"。
// 单窗格时显示的就是源画面，换算结果直接就是源坐标；多窗格时显示的是拼接
// 画布，必须先命中窗格、再经窗格自身的区域映射回源坐标——少了这一层，第二次
// 调整某个窗格就会取到错的区域。
bool apply_drag_selection(PluginState &state, const Box &box, bool reset) {
    Layout &layout = active_layout(state);
    auto geometry = read_geometry(state.handle);
    if (!geometry) {
        mpv_util::show_osd_message(state.handle, "Video geometry unavailable", kOsdDuration);
        return false;
    }

    if (leaf_count(layout) <= 1) {
        if (reset) {
            // 右下->左上手势同样要连偏移一起复位，只清缩放会留下拖拽的位移。
            reset_view_and_transform(state, layout);
            return true;
        }
        auto region = box_to_region(*geometry, box);
        if (!region) {
            return false;
        }
        return set_focused_region(layout, *region);
    }

    auto size = read_source_size(state.handle);
    if (!size) {
        return false;
    }
    auto canvas_box = box_to_region(*geometry, box);
    if (!canvas_box) {
        return false;
    }

    // 用起点定位窗格：拖拽可能跨出窗格边界，一律按起点所在的窗格处理。
    Box start_box = normalize_box(state.drag_start, state.drag_start);
    auto start_norm = box_to_region(*geometry, Box{start_box.x1, start_box.y1, start_box.x1 + 1.0,
                                                    start_box.y1 + 1.0});
    if (!start_norm) {
        return false;
    }
    auto hit = hit_test(layout, size->canvas_w, size->canvas_h, start_norm->x1, start_norm->y1);
    if (!hit) {
        return false;
    }

    layout.focused = hit->leaf;
    if (reset) {
        return set_focused_region(layout, Region{});
    }

    auto rects = pane_canvas_rects(state, layout, hit->leaf);
    if (!rects) {
        return false;
    }
    const PixelRect &rect = rects->pane;
    if (rect.w <= 0 || rect.h <= 0) {
        return false;
    }

    // 视口到窗格是精确的线性映射，**不夹取**：框到黑边上就该算出画面之外的
    // 坐标，这样黑边才会跟着一起放大。
    const Region &view = rects->view;
    auto to_pane_u = [&](double canvas_u) {
        return view_to_source_u(view, (canvas_u * size->canvas_w - rect.x) / rect.w);
    };
    auto to_pane_v = [&](double canvas_v) {
        return view_to_source_v(view, (canvas_v * size->canvas_h - rect.y) / rect.h);
    };

    Region picked;
    picked.x1 = to_pane_u(canvas_box->x1);
    picked.x2 = to_pane_u(canvas_box->x2);
    picked.y1 = to_pane_v(canvas_box->y1);
    picked.y2 = to_pane_v(canvas_box->y2);
    if (picked.width() <= 0.0 || picked.height() <= 0.0) {
        return false;
    }
    // 内缩到窗格比例：占满窗格、裁掉多余（"用 max、裁掉溢出"这个已确认的取舍）。
    return set_focused_region(layout,
                              fit_view_aspect(picked, size->src_w, size->src_h, rects->pane, false));
}

// ---- 拖分隔条调整窗格比例 ----

constexpr int kDividerGrabPixels = 8;

// 分隔条拖拽刻意绑在 Alt+MBTN_LEFT 而不是裸 MBTN_LEFT：mpv 的窗口拖拽
// （--window-dragging，默认开）在 macOS 上由 Cocoa 层处理，早于 input 绑定，
// 按住裸左键后鼠标位置根本不会上报给插件——实测 ratio 恒为按下时的值、
// 分隔条纹丝不动。加修饰键就能绕开（平移用 meta+ 同理）。顺带也不会吞掉
// uosc 进度条上的左键点击。

// 返回鼠标当前所在的画布归一化坐标。
std::optional<Point> mouse_canvas_pos(PluginState &state) {
    auto pos = read_mouse_pos(state.handle);
    auto geometry = read_geometry(state.handle);
    if (!pos || !geometry || geometry->scaled_w <= 0.0 || geometry->scaled_h <= 0.0) {
        return std::nullopt;
    }
    return Point{(pos->x - geometry->rect_x) / geometry->scaled_w,
                 (pos->y - geometry->rect_y) / geometry->scaled_h};
}

void sizing_begin(PluginState &state) {
    state.sizing = false;
    Layout &layout = active_layout(state);
    if (leaf_count(layout) <= 1) {
        return;
    }
    auto size = read_source_size(state.handle);
    auto cpos = mouse_canvas_pos(state);
    auto geometry = read_geometry(state.handle);
    if (!size || !cpos || !geometry || geometry->scaled_w <= 0.0) {
        return;
    }
    // 容差按屏幕像素给，换算到画布像素，免得窗口缩放后手感变化。
    double canvas_per_screen = static_cast<double>(size->canvas_w) / geometry->scaled_w;
    int tol = std::max(2, static_cast<int>(std::lround(kDividerGrabPixels * canvas_per_screen)));
    auto hit = hit_test_divider(layout, size->canvas_w, size->canvas_h, cpos->x, cpos->y, tol);
    if (!hit) {
        return;
    }
    state.sizing = true;
    state.sizing_node = hit->node;
    state.sizing_area = hit->area;
    state.sizing_last_apply = {};
}

void sizing_update(PluginState &state) {
    if (!state.sizing) {
        return;
    }
    auto size = read_source_size(state.handle);
    auto cpos = mouse_canvas_pos(state);
    if (!size || !cpos) {
        return;
    }
    Layout &layout = active_layout(state);
    const PixelRect &area = state.sizing_area;
    double ratio = 0.5;
    if (layout.nodes[state.sizing_node].dir == SplitDir::kHorizontal) {
        if (area.w <= 0) {
            return;
        }
        ratio = (cpos->x * size->canvas_w - area.x) / area.w;
    } else {
        if (area.h <= 0) {
            return;
        }
        ratio = (cpos->y * size->canvas_h - area.y) / area.h;
    }
    if (!set_node_ratio(layout, state.sizing_node, ratio)) {
        return;
    }
    // 与平移同理：改比例要重建滤镜链，限流到约 16fps。
    auto now = std::chrono::steady_clock::now();
    if (now - state.sizing_last_apply >= std::chrono::milliseconds(60)) {
        state.sizing_last_apply = now;
        refresh(state);
    }
}

void sizing_finish(PluginState &state) {
    if (!state.sizing) {
        return;
    }
    sizing_update(state);
    state.sizing = false;
    refresh(state);
}

// ---- 滚轮缩放 ----

constexpr double kWheelZoomFactor = 1.1;

// 缩放目标：鼠标所在的窗格；鼠标不在任何窗格内就用选中的；都没有就用第一个。
void on_wheel_zoom(PluginState &state, bool zoom_in) {
    Layout &layout = active_layout(state);
    auto size = read_source_size(state.handle);
    if (!size) {
        return;
    }
    double factor = zoom_in ? 1.0 / kWheelZoomFactor : kWheelZoomFactor;

    int leaf = kNoFocus;
    double anchor_u = 0.5;
    double anchor_v = 0.5;
    if (auto cpos = mouse_canvas_pos(state)) {
        if (auto hit = hit_test(layout, size->canvas_w, size->canvas_h, std::clamp(cpos->x, 0.0, 1.0),
                                 std::clamp(cpos->y, 0.0, 1.0))) {
            leaf = hit->leaf;
            anchor_u = hit->u;
            anchor_v = hit->v;
        }
    }
    if (leaf == kNoFocus) {
        leaf = focused_or_first(layout);
    }
    if (leaf < 0 || static_cast<std::size_t>(leaf) >= layout.nodes.size()) {
        return;
    }

    std::vector<int> leaves = leaf_order(layout);
    std::vector<PixelRect> rects = compute_pane_rects(layout, size->canvas_w, size->canvas_h);
    auto it = std::find(leaves.begin(), leaves.end(), leaf);
    if (it == leaves.end() || leaves.size() != rects.size()) {
        return;
    }
    const PixelRect &pane = rects[static_cast<std::size_t>(it - leaves.begin())];
    Region view = fit_view_aspect(layout.nodes[leaf].region, size->src_w, size->src_h, pane, true);
    Region zoomed = zoom_view_at(view, anchor_u, anchor_v, factor);
    if (view_valid(zoomed)) {
        layout.nodes[leaf].region = zoomed;
        apply_layout(state);
    }
}

// ---- 平移（合并自 enhanced-drag）----
//
// 单窗格：完全沿用 enhanced-drag 的做法，直接写 video-pan-x/y，从按下时的
// 位置绝对计算，**不做任何边界约束**（用户明确要求过"拖到边界后不能再往外
// 拖"体验不好），缩放为 0 时也能拖。
//
// 多窗格：video-pan-* 作用在整块拼接画面上，拖起来是整个画面一起动，不是
// 用户要的。改成移动**鼠标所在窗格**的 Region——窗格显示的就是源画面上的一
// 个窗口，平移它等于在源画面上挪这个窗口。这条路径必然被源画面边界夹住：
// 窗口移出画面就没有内容可显示了。
void pan_begin(PluginState &state) {
    auto pos = read_mouse_pos(state.handle);
    if (!pos) {
        return;
    }
    state.panning = true;
    state.pan_last = *pos;
    state.pan_last_apply = {};

    Layout &layout = active_layout(state);
    if (leaf_count(layout) <= 1) {
        state.pan_single = true;
        state.pan_origin_mouse = *pos;
        state.pan_origin_x = mpv_util::get_double(state.handle, "video-pan-x", 0.0);
        state.pan_origin_y = mpv_util::get_double(state.handle, "video-pan-y", 0.0);
        return;
    }

    state.pan_single = false;
    state.pan_leaf = -1;
    auto size = read_source_size(state.handle);
    auto geometry = read_geometry(state.handle);
    if (!size || !geometry || geometry->scaled_w <= 0.0 || geometry->scaled_h <= 0.0) {
        return;
    }
    double cu = (pos->x - geometry->rect_x) / geometry->scaled_w;
    double cv = (pos->y - geometry->rect_y) / geometry->scaled_h;
    if (auto hit = hit_test(layout, size->canvas_w, size->canvas_h, std::clamp(cu, 0.0, 1.0),
                             std::clamp(cv, 0.0, 1.0))) {
        state.pan_leaf = hit->leaf;
        layout.focused = hit->leaf;

        draw_focus_overlay(state);
    }
}

void pan_update(PluginState &state) {
    if (!state.panning) {
        return;
    }
    auto pos = read_mouse_pos(state.handle);
    if (!pos) {
        return;
    }
    auto geometry = read_geometry(state.handle);
    if (!geometry) {
        return;
    }

    if (state.pan_single) {
        if (geometry->scaled_w > 0.0) {
            mpv_util::set_double(state.handle, "video-pan-x",
                                 state.pan_origin_x + (pos->x - state.pan_origin_mouse.x) / geometry->scaled_w);
        }
        if (geometry->scaled_h > 0.0) {
            mpv_util::set_double(state.handle, "video-pan-y",
                                 state.pan_origin_y + (pos->y - state.pan_origin_mouse.y) / geometry->scaled_h);
        }
        return;
    }

    Layout &layout = active_layout(state);
    if (state.pan_leaf < 0 || static_cast<std::size_t>(state.pan_leaf) >= layout.nodes.size()) {
        return;
    }
    double dx = pos->x - state.pan_last.x;
    double dy = pos->y - state.pan_last.y;
    if (dx == 0.0 && dy == 0.0) {
        return;
    }
    state.pan_last = *pos;

    auto rects = pane_canvas_rects(state, layout, state.pan_leaf);
    if (!rects) {
        return;
    }
    auto screen = canvas_rect_to_screen(state, rects->pane);
    if (!screen) {
        return;
    }
    double pw = screen->x2 - screen->x1;
    double ph = screen->y2 - screen->y1;
    if (pw <= 0.0 || ph <= 0.0) {
        return;
    }

    // 平移视口：画面跟着鼠标走（视口反向移动），**不做任何范围限制**，
    // 可以把画面整个拖出窗格，空出来的地方是黑的。
    Node &node = layout.nodes[state.pan_leaf];
    const Region &view = rects->view;
    Region moved = translate_view(view, -(dx / pw) * view.width(), -(dy / ph) * view.height());
    if (view_valid(moved)) {
        node.region = moved;
    }

    // 多窗格平移要重建滤镜链，60Hz 重建会明显卡顿，这里限流到 ~16fps；
    // 松开鼠标时无条件补一次，保证最终位置准确。
    auto now = std::chrono::steady_clock::now();
    if (now - state.pan_last_apply >= std::chrono::milliseconds(60)) {
        state.pan_last_apply = now;
        apply_layout(state);
    }
}

void pan_finish(PluginState &state) {
    if (!state.panning) {
        return;
    }
    pan_update(state);
    state.panning = false;
    if (!state.pan_single) {
        apply_layout(state);
        draw_focus_overlay(state);
    }
}

void drag_begin(PluginState &state) {
    auto pos = read_mouse_pos(state.handle);
    if (!pos) {
        return;
    }
    state.dragging = true;
    state.drag_start = *pos;
    state.drag_current = *pos;

    // 多窗格时把整个拖拽限制在起始窗格内：跨过中缝的部分本来也会被丢掉，
    // 与其让用户画出一个大半无效的框，不如让框自己停在边界上。
    state.drag_bounds.reset();
    Layout &layout = active_layout(state);
    if (leaf_count(layout) > 1) {
        auto size = read_source_size(state.handle);
        auto geometry = read_geometry(state.handle);
        if (size && geometry && geometry->scaled_w > 0.0 && geometry->scaled_h > 0.0) {
            double cu = (pos->x - geometry->rect_x) / geometry->scaled_w;
            double cv = (pos->y - geometry->rect_y) / geometry->scaled_h;
            if (auto hit = hit_test(layout, size->canvas_w, size->canvas_h, std::clamp(cu, 0.0, 1.0),
                                     std::clamp(cv, 0.0, 1.0))) {
                if (auto rects = pane_canvas_rects(state, layout, hit->leaf)) {
                    state.drag_bounds = canvas_rect_to_screen(state, rects->pane);
                }
                state.drag_start.x = std::clamp(state.drag_start.x, state.drag_bounds->x1,
                                                 state.drag_bounds->x2);
                state.drag_start.y = std::clamp(state.drag_start.y, state.drag_bounds->y1,
                                                 state.drag_bounds->y2);
                state.drag_current = state.drag_start;
            }
        }
    }
    draw_selection(state);
}

void drag_update(PluginState &state) {
    if (!state.dragging) {
        return;
    }
    if (auto pos = read_mouse_pos(state.handle)) {
        state.drag_current = *pos;
        if (state.drag_bounds) {
            state.drag_current.x = std::clamp(state.drag_current.x, state.drag_bounds->x1,
                                               state.drag_bounds->x2);
            state.drag_current.y = std::clamp(state.drag_current.y, state.drag_bounds->y1,
                                               state.drag_bounds->y2);
        }
    }
    draw_selection(state);
}

void drag_finish(PluginState &state) {
    if (!state.dragging) {
        return;
    }
    if (auto pos = read_mouse_pos(state.handle)) {
        state.drag_current = *pos;
        if (state.drag_bounds) {
            state.drag_current.x = std::clamp(state.drag_current.x, state.drag_bounds->x1,
                                               state.drag_bounds->x2);
            state.drag_current.y = std::clamp(state.drag_current.y, state.drag_bounds->y1,
                                               state.drag_bounds->y2);
        }
    }
    state.dragging = false;
    state.drag_bounds.reset();
    clear_overlay(state.handle, kSelectionOverlayId);

    Box box = normalize_box(state.drag_start, state.drag_current);
    DragDirection direction = classify_direction(state.drag_current.x - state.drag_start.x,
                                                  state.drag_current.y - state.drag_start.y, kMinDragPixels);
    if (direction == DragDirection::kNone) {
        return;
    }
    if (apply_drag_selection(state, box, direction == DragDirection::kReset)) {
        refresh(state);
    }
}

// ---- 按键动作 ----

// 两个时间格式都与 enhanced-ab-loop 保持一致，方便两个插件的 OSD 并排看。
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

std::string format_precise(double seconds) {
    double h = std::floor(seconds / 3600.0);
    double remainder = std::fmod(seconds, 3600.0);
    double m = std::floor(remainder / 60.0);
    double s = std::fmod(remainder, 60.0);
    if (h > 0) {
        return fmt::format("{}:{:02}:{:06.3f}", static_cast<long>(h), static_cast<long>(m), s);
    }
    return fmt::format("{:02}:{:06.3f}", static_cast<long>(m), s);
}

// 编辑落到全局布局上时提醒一句。只在**已经存在分屏段**时提示：没有段的时候
// base 就是唯一的布局，不存在"改错地方"的可能，提示纯属噪音。
void warn_if_editing_base(PluginState &state) {
    if (state.segments.empty()) {
        return;
    }
    if (!find_segment_at(state.segments, time_pos(state.handle))) {
        mpv_util::show_osd_message(state.handle, "Editing global layout (not in a split segment)", kOsdDuration);
    }
}

// 列出所有分屏段和当前所处位置。格式与 enhanced-ab-loop 的 show-state 对齐
// （首行状态 + 每段一行竖排 + 超过 12 段时首尾各留 6 段），只是没有 A/B
// 待定端点那一段——分屏没有"半个区间"的概念。
void on_show_state(PluginState &state) {
    double pos = time_pos(state.handle);
    auto current = find_segment_at(state.segments, pos);

    std::ostringstream text;
    text << "Split | " << format_time(pos) << "/"
         << format_time(mpv_util::get_double(state.handle, "duration", 0.0)) << " | ";
    if (current) {
        text << "segment " << (*current + 1);
    } else {
        text << "global (" << leaf_count(state.base) << " panes)";
    }
    if (state.pending_segment_start) {
        text << " | pending " << format_precise(*state.pending_segment_start);
    }

    auto append = [&](std::size_t i) {
        const LayoutSegment &seg = state.segments[i];
        text << "\n" << ((current && *current == i) ? "> " : "") << "[" << format_precise(seg.a) << ","
             << format_precise(seg.b) << "] " << leaf_count(seg.layout) << " panes";
    };

    std::size_t total = state.segments.size();
    SegmentDisplayPlan plan = plan_segment_display(total);
    for (std::size_t i = 0; i < plan.head_count; ++i) {
        append(i);
    }
    if (plan.hidden_count > 0) {
        text << "\n... (" << plan.hidden_count << " more)";
        for (std::size_t i = total - plan.tail_count; i < total; ++i) {
            append(i);
        }
    }

    mpv_util::show_osd_message(state.handle, text.str(), 4.0);
}

void on_split(PluginState &state, SplitDir dir) {
    auto size = read_source_size(state.handle);
    if (!size) {
        return;
    }
    Layout &layout = active_layout(state);
    Layout backup = layout;

    if (!split_focused(layout, dir)) {
        return;
    }
    // 分屏后窗格太小就撤销：过小的 crop/scale 容易让整张滤镜图配置失败。
    std::vector<PixelRect> rects = compute_pane_rects(layout, size->canvas_w, size->canvas_h);
    for (const PixelRect &rect : rects) {
        if (rect.w < kMinPanePixels || rect.h < kMinPanePixels) {
            layout = backup;
            mpv_util::show_osd_message(state.handle, "Pane too small to split", kOsdDuration);
            return;
        }
    }
    refresh(state);
    warn_if_editing_base(state);
    mpv_util::show_osd_message(state.handle, fmt::format("Split: {} panes", leaf_count(layout)), kOsdDuration);
}

void on_close_pane(PluginState &state) {
    Layout &layout = active_layout(state);
    if (!close_focused(layout)) {
        // 只剩一个窗格：把画面彻底恢复原状——缩放和偏移一起清零。
        reset_view_and_transform(state, layout);
        refresh(state, true);
        mpv_util::show_osd_message(state.handle, "Restored full view", kOsdDuration);
        return;
    }
    refresh(state);
    mpv_util::show_osd_message(state.handle, fmt::format("Pane closed, {} left", leaf_count(layout)), kOsdDuration);
}

void on_focus_next(PluginState &state) {
    Layout &layout = active_layout(state);
    if (leaf_count(layout) <= 1) {
        return;
    }
    focus_next(layout);
    draw_focus_overlay(state);

    std::vector<int> leaves = leaf_order(layout);
    auto it = std::find(leaves.begin(), leaves.end(), layout.focused);
    std::size_t index = (it == leaves.end()) ? 0 : static_cast<std::size_t>(it - leaves.begin());
    mpv_util::show_osd_message(state.handle, fmt::format("Pane {}/{}", index + 1, leaves.size()), kOsdDuration);
}

void on_segment_start(PluginState &state) {
    double pos = time_pos(state.handle);
    // 起点就落在已有区间里的话，无论终点设在哪都必然重叠。在这里就拒掉，
    // 不要等用户跑到终点再说——那时候他已经白操作一轮了。
    if (auto index = find_segment_at(state.segments, pos)) {
        mpv_util::show_osd_message(
            state.handle,
            fmt::format("Already inside split segment {} - {}", format_time(state.segments[*index].a),
                        format_time(state.segments[*index].b)),
            kOsdDuration);
        return;
    }
    state.pending_segment_start = pos;
    mpv_util::show_osd_message(state.handle, fmt::format("Split start {}", format_time(pos)), kOsdDuration);
}

void on_segment_end(PluginState &state) {
    if (!state.pending_segment_start) {
        mpv_util::show_osd_message(state.handle, "Set split start first", kOsdDuration);
        return;
    }
    double a = *state.pending_segment_start;
    double b = time_pos(state.handle);
    // 终点必须严格晚于起点。之前是 b<a 就把两者对调——那是"猜用户想干什么"
    // 的隐式行为：用户明确把终点设在起点之前，多半是记错了位置，直接拒绝
    // 让他重设，比悄悄换个意思更好。
    if (b <= a) {
        mpv_util::show_osd_message(
            state.handle, fmt::format("End {} not after start {}: denied", format_time(b), format_time(a)),
            kOsdDuration);
        return;
    }
    if (overlapping_segment(state.segments, a, b)) {
        mpv_util::show_osd_message(state.handle, "Overlaps an existing split segment: denied", kOsdDuration);
        return;
    }

    // 把"当前正在编辑的这份布局"快照进新区间。
    // 注意这里**不**重置 base：早期版本会把全局布局清成完整画面，等于用户
    // 建一个分屏段就把已有的全局缩放悄悄抹掉，是个没确认过的副作用。
    LayoutSegment segment;
    segment.a = a;
    segment.b = b;
    segment.layout = active_layout(state);
    state.segments.push_back(segment);
    sort_segments(state.segments);
    state.pending_segment_start.reset();

    publish_segments_property(state.handle, state.segments);
    refresh(state, true);
    mpv_util::show_osd_message(state.handle,
                               fmt::format("Split segment {} - {} ({} total)", format_time(a),
                                            format_time(b), state.segments.size()),
                               kOsdDuration);
}

// 读取 enhanced-ab-loop 发布的区间列表。它以原生 node 数组发布在
// user-data 下（该插件的 SPEC 记过：用 JSON 字符串会让 Lua 侧 parse_json
// 静默失败，所以是原生 node）。这里只读，不依赖对方插件是否加载——没加载
// 时属性不存在，返回空。
struct AbLoopSegment {
    double a = 0.0;
    double b = 0.0;
};

std::vector<AbLoopSegment> read_abloop_segments(mpv_handle *h) {
    std::vector<AbLoopSegment> out;
    mpv_node node;
    if (mpv_get_property(h, "user-data/enhanced-ab-loop/segments", MPV_FORMAT_NODE, &node) < 0) {
        return out;
    }
    if (node.format == MPV_FORMAT_NODE_ARRAY && node.u.list) {
        for (int i = 0; i < node.u.list->num; ++i) {
            const mpv_node &item = node.u.list->values[i];
            if (item.format != MPV_FORMAT_NODE_MAP || !item.u.list) {
                continue;
            }
            AbLoopSegment seg;
            seg.a = node_map_get_number(*item.u.list, "a", 0.0);
            seg.b = node_map_get_number(*item.u.list, "b", 0.0);
            if (seg.b > seg.a) {
                out.push_back(seg);
            }
        }
    }
    mpv_free_node_contents(&node);
    return out;
}

// 把播放位置所在的 ab-loop 区间原样建成一个分屏段，方便"每个 loop 段配一套
// 分屏布局"这种用法：不用再手动对齐两个插件的起止点。
void on_segment_from_abloop(PluginState &state) {
    double pos = time_pos(state.handle);
    std::vector<AbLoopSegment> loops = read_abloop_segments(state.handle);
    if (loops.empty()) {
        mpv_util::show_osd_message(state.handle, "No ab-loop segments", kOsdDuration);
        return;
    }
    const AbLoopSegment *found = nullptr;
    for (const AbLoopSegment &seg : loops) {
        if (pos >= seg.a && pos <= seg.b) {
            found = &seg;
            break;
        }
    }
    if (!found) {
        mpv_util::show_osd_message(state.handle, "Not inside an ab-loop segment", kOsdDuration);
        return;
    }
    if (auto index = find_segment_at(state.segments, pos)) {
        mpv_util::show_osd_message(state.handle,
                                   fmt::format("Already inside split segment {} - {}",
                                                format_time(state.segments[*index].a),
                                                format_time(state.segments[*index].b)),
                                   kOsdDuration);
        return;
    }
    if (overlapping_segment(state.segments, found->a, found->b)) {
        mpv_util::show_osd_message(state.handle, "Overlaps an existing split segment: denied", kOsdDuration);
        return;
    }

    LayoutSegment segment;
    segment.a = found->a;
    segment.b = found->b;
    segment.layout = active_layout(state);
    state.segments.push_back(segment);
    sort_segments(state.segments);
    state.pending_segment_start.reset();

    publish_segments_property(state.handle, state.segments);
    refresh(state, true);
    mpv_util::show_osd_message(state.handle,
                               fmt::format("Split segment from ab-loop {} - {} ({} total)",
                                            format_time(found->a), format_time(found->b),
                                            state.segments.size()),
                               kOsdDuration);
}

void on_segment_clear(PluginState &state) {
    double pos = time_pos(state.handle);
    if (auto index = find_segment_at(state.segments, pos)) {
        state.segments.erase(state.segments.begin() + static_cast<std::ptrdiff_t>(*index));
        publish_segments_property(state.handle, state.segments);
        refresh(state, true);
        mpv_util::show_osd_message(state.handle, fmt::format("Split segment deleted, {} left", state.segments.size()),
                                   kOsdDuration);
        return;
    }
    mpv_util::show_osd_message(state.handle, "No split segment here", kOsdDuration);
}

// ---- 存档 ----

struct ContentSample {
    std::uint64_t size = 0;
    std::string head;
    std::string tail;
};

std::optional<ContentSample> read_content_sample(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0) {
        return std::nullopt;
    }

    ContentSample sample;
    sample.size = static_cast<std::uint64_t>(size);
    store::SampleRanges ranges = store::compute_sample_ranges(sample.size, kContentSampleBytes);

    sample.head.resize(ranges.head_len);
    file.seekg(0, std::ios::beg);
    file.read(sample.head.data(), static_cast<std::streamsize>(ranges.head_len));

    sample.tail.resize(ranges.tail_len);
    file.seekg(static_cast<std::streamoff>(ranges.tail_offset), std::ios::beg);
    file.read(sample.tail.data(), static_cast<std::streamsize>(ranges.tail_len));
    return sample;
}

std::string archive_path_for_hash(mpv_handle *h, const std::string &hash) {
    // 用 ~~home/ 而不是 ~~/：后者语义是"子路径已存在时返回已存在的那个
    // 目录"，是给读取用的；这里要写入，需要明确指向 mpv 配置目录。
    std::string dir = expand_path(h, "~~home/split-layouts");
    // mpv 没有配置目录时（例如 --no-config）"~~home/" 会展开成空串，只剩下
    // 相对路径 "split-layouts"，直接用会把存档写进 mpv 当前的工作目录这种
    // 完全意料之外的位置。这里要求必须是绝对路径，否则宁可不存。
    if (dir.empty() || !std::filesystem::path(dir).is_absolute()) {
        MPV_UTIL_DEBUG("archive_path_for_hash: 展开结果不是绝对路径: '{}'\n", dir);
        return "";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return "";
    }
    return dir + "/" + hash + ".json";
}

void apply_entry(PluginState &state, store::FileEntry entry) {
    sort_segments(entry.segments);
    state.base = entry.base;
    state.segments = std::move(entry.segments);
    state.pending_segment_start.reset();
    publish_segments_property(state.handle, state.segments);
    refresh(state, true);
}

void on_save(PluginState &state) {
    if (state.current_content_hash.empty() || state.current_path_key.empty()) {
        mpv_util::show_osd_message(state.handle, "Save denied: no file", kOsdDuration);
        return;
    }
    std::string path = archive_path_for_hash(state.handle, state.current_content_hash);
    if (path.empty()) {
        mpv_util::show_osd_message(state.handle, "Save failed: bad path", kOsdDuration);
        return;
    }

    store::Archive archive;
    if (auto text = read_file_text(path)) {
        archive = store::deserialize_archive(*text);
    }

    store::FileEntry entry;
    entry.base = state.base;
    entry.segments = state.segments;
    store::upsert(archive, state.current_path_key, entry, state.rename_from);
    state.rename_from.reset();

    if (write_file_text(path, store::serialize_archive(archive))) {
        mpv_util::show_osd_message(state.handle, fmt::format("Split layouts saved ({} segments)", state.segments.size()),
                                   kOsdDuration);
    } else {
        mpv_util::show_osd_message(state.handle, "Save failed: write error", kOsdDuration);
    }
}

void on_load(PluginState &state) {
    if (state.current_content_hash.empty()) {
        return;
    }
    std::string path = archive_path_for_hash(state.handle, state.current_content_hash);
    auto text = path.empty() ? std::nullopt : read_file_text(path);
    if (!text) {
        mpv_util::show_osd_message(state.handle, "No saved layouts", kOsdDuration);
        return;
    }

    store::Archive archive = store::deserialize_archive(*text);
    store::LookupResult result = store::lookup(archive, state.current_path_key);
    switch (result.kind) {
    case store::LookupResult::Kind::kExactMatch:
        apply_entry(state, result.entry);
        mpv_util::show_osd_message(state.handle, fmt::format("Split layouts loaded ({} segments)", state.segments.size()),
                                   kOsdDuration);
        break;
    case store::LookupResult::Kind::kSingleCandidate:
        // 提示文案不带明文路径——插件这边也只有它的哈希。
        state.awaiting_confirm = true;
        state.pending_entry = result.entry;
        state.rename_from = result.matched_key;
        state.paused_before_confirm = mpv_util::get_flag(state.handle, "pause", false);
        mpv_util::set_flag(state.handle, "pause", true);
        // 换行用真实的 '\n'，不是 ASS 的 "\N"：mpv 的 show-text 不解析 ASS
        // 转义，写 "\N" 会原样显示成反斜杠加 N（实测真实换行渲染成两行、
        // 字面 \N 只有一行）。enhanced-ab-loop 的 show-state 用的也是 '\n'。
        mpv_util::show_osd_message(state.handle,
                                   "Archive has one entry with a different filename (renamed?).\n"
                                   "Alt+y to use it, Alt+n to cancel",
                                   kConfirmOsdDuration);
        break;
    case store::LookupResult::Kind::kNoArchive:
        mpv_util::show_osd_message(state.handle, "No matching layouts", kOsdDuration);
        break;
    }
}

void on_confirm(PluginState &state, bool yes) {
    if (!state.awaiting_confirm) {
        return;
    }
    state.awaiting_confirm = false;
    mpv_util::set_flag(state.handle, "pause", state.paused_before_confirm);
    mpv_util::show_osd_message(state.handle, "", 1);

    if (yes) {
        apply_entry(state, state.pending_entry);
        mpv_util::show_osd_message(state.handle, fmt::format("Split layouts loaded ({} segments)", state.segments.size()),
                                   kOsdDuration);
    } else {
        state.rename_from.reset();
        mpv_util::show_osd_message(state.handle, "Load cancelled", kOsdDuration);
    }
    state.pending_entry = store::FileEntry{};
}

// ---- 事件 ----

void on_file_loaded(PluginState &state) {
    state.current_path = get_string_property(state.handle, "path");
    state.current_path_key =
        state.current_path.empty() ? "" : store::compute_path_hash(store::extract_filename(state.current_path));
    state.current_content_hash.clear();
    if (!state.current_path.empty()) {
        if (auto sample = read_content_sample(state.current_path)) {
            state.current_content_hash = store::compute_content_hash(sample->size, sample->head, sample->tail);
        }
    }

    state.base = make_layout();
    state.segments.clear();
    state.pending_segment_start.reset();
    state.rename_from.reset();
    state.awaiting_confirm = false;
    state.applied_key.clear();
    publish_segments_property(state.handle, state.segments);
}

void handle_client_message(PluginState &state, mpv_event_client_message *message) {
    if (message->num_args < 3 || std::strcmp(message->args[0], "key-binding") != 0) {
        return;
    }
    std::string binding = message->args[1];
    char phase = message->args[2][0];

    // 拖拽需要按下/松开两个边沿，其余按键只在按下（或无法区分时的单次触发）
    // 时响应。
    if (binding == "drag-divider") {
        if (phase == 'd') {
            sizing_begin(state);
        } else if (phase == 'u') {
            sizing_finish(state);
        }
        return;
    }

    if (binding == "drag-pan") {
        if (phase == 'd') {
            pan_begin(state);
        } else if (phase == 'u') {
            pan_finish(state);
        }
        return;
    }

    if (binding == "drag-select") {
        if (phase == 'd') {
            drag_begin(state);
        } else if (phase == 'u') {
            drag_finish(state);
        }
        return;
    }

    if (phase != 'd' && phase != 'p') {
        return;
    }

    if (binding == "split-h") {
        on_split(state, SplitDir::kHorizontal);
    } else if (binding == "split-v") {
        on_split(state, SplitDir::kVertical);
    } else if (binding == "close-pane") {
        on_close_pane(state);
    } else if (binding == "focus-next") {
        on_focus_next(state);
    } else if (binding == "focus-clear") {
        clear_focus(active_layout(state));
        clear_overlay(state.handle, kFocusOverlayId);
    } else if (binding == "zoom-in") {
        on_wheel_zoom(state, true);
    } else if (binding == "zoom-out") {
        on_wheel_zoom(state, false);
    } else if (binding == "segment-from-abloop") {
        on_segment_from_abloop(state);
    } else if (binding == "segment-start") {
        on_segment_start(state);
    } else if (binding == "segment-end") {
        on_segment_end(state);
    } else if (binding == "show-state") {
        on_show_state(state);
    } else if (binding == "segment-clear") {
        on_segment_clear(state);
    } else if (binding == "save-layouts") {
        on_save(state);
    } else if (binding == "load-layouts") {
        on_load(state);
    } else if (binding == "confirm-yes") {
        on_confirm(state, true);
    } else if (binding == "confirm-no") {
        on_confirm(state, false);
    }
}

} // namespace

extern "C" int mpv_open_cplugin(mpv_handle *handle) {
    PluginState state;
    state.handle = handle;

    // time-pos 用来在时间段之间切换布局；apply_layout 里的 applied_key 去重
    // 保证每帧回调不会真的触发重建。
    mpv_observe_property(handle, 0, "time-pos", MPV_FORMAT_DOUBLE);
    // 窗口尺寸变化时焦点框要跟着重画。
    mpv_observe_property(handle, 0, "osd-dimensions", MPV_FORMAT_NODE);
    // 帧类型决定滤镜图要不要 hwdownload 前缀。用户自己切 hwdec、或 mpv 因为
    // 解码失败自动回退，都会让当前这张图不再适用，必须按新帧类型重建。
    mpv_observe_property(handle, 0, "hwdec-current", MPV_FORMAT_STRING);

    while (true) {
        double timeout = (state.dragging || state.panning || state.sizing) ? kRefreshIntervalSeconds : -1.0;
        mpv_event *event = mpv_wait_event(handle, timeout);

        switch (event->event_id) {
        case MPV_EVENT_SHUTDOWN:
            clear_overlay(handle, kSelectionOverlayId);
            clear_overlay(handle, kFocusOverlayId);
            return 0;
        case MPV_EVENT_CLIENT_MESSAGE:
            handle_client_message(state, static_cast<mpv_event_client_message *>(event->data));
            break;
        case MPV_EVENT_FILE_LOADED:
            on_file_loaded(state);
            break;
        case MPV_EVENT_END_FILE:
            clear_all_output(state);
            clear_overlay(handle, kSelectionOverlayId);
            clear_overlay(handle, kFocusOverlayId);
            break;
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(event->data);
            if (prop && (std::strcmp(prop->name, "time-pos") == 0 ||
                         std::strcmp(prop->name, "hwdec-current") == 0)) {
                apply_and_sync_overlay(state);
            } else if (prop && std::strcmp(prop->name, "osd-dimensions") == 0) {
                draw_focus_overlay(state);
            }
            break;
        }
        case MPV_EVENT_NONE:
            drag_update(state);
            pan_update(state);
            sizing_update(state);
            break;
        default:
            break;
        }
    }
}
