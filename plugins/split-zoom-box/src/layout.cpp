#include "split_zoom_box/layout.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include <fmt/format.h>

namespace split_zoom_box {

namespace {

bool valid_index(const Layout &layout, int index) {
    return index >= 0 && static_cast<std::size_t>(index) < layout.nodes.size();
}

void collect_leaves(const Layout &layout, int index, std::vector<int> &out) {
    if (!valid_index(layout, index)) {
        return;
    }
    const Node &node = layout.nodes[index];
    if (node.leaf) {
        out.push_back(index);
        return;
    }
    collect_leaves(layout, node.first, out);
    collect_leaves(layout, node.second, out);
}

// 找到某个节点的父节点索引，没有父节点（根）返回 -1。树很小（窗格数个位
// 数），线性扫描比额外维护 parent 字段简单，也不会在关闭窗格时出现父指针
// 失效的问题。
int find_parent(const Layout &layout, int child) {
    for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
        const Node &node = layout.nodes[i];
        if (!node.leaf && (node.first == child || node.second == child)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int clamp_positive(int value) {
    return value < 1 ? 1 : value;
}

// yuv420p 的色度是 2x2 子采样，pad 会把尺寸和偏移按 2 对齐之后再做
// "padded >= input" 的校验；只要有一个是奇数，对齐后就可能反过来变小，
// 整张图直接配置失败（实测 scale=1337:720,pad=1337:720 就会报
// "Padded dimensions cannot be smaller than input dimensions"）。
// 所以摆放相关的尺寸和偏移一律取偶。
int even_floor(int value) {
    return static_cast<int>(std::floor(value / 2.0)) * 2;
}

int even_ceil(int value) {
    return static_cast<int>(std::ceil(value / 2.0)) * 2;
}

// 把区域换算成 crop 的整数参数，并夹回源画面范围内。
//
// 宽高和起点都取偶、且至少为 2：yuv420p 的色度平面是 2x2 子采样，高度 1 的
// 裁剪会让色度平面高度变成 0，crop 直接报 "Invalid too big or non positive
// size"。
struct CropParams {
    int x = 0;
    int y = 0;
    int w = 1;
    int h = 1;
};

CropParams region_to_crop(const Region &region, int src_w, int src_h) {
    CropParams crop;
    if (src_w < 2 || src_h < 2) {
        return crop;
    }
    int x1 = static_cast<int>(std::lround(region.x1 * src_w));
    int y1 = static_cast<int>(std::lround(region.y1 * src_h));
    int x2 = static_cast<int>(std::lround(region.x2 * src_w));
    int y2 = static_cast<int>(std::lround(region.y2 * src_h));

    x1 = even_floor(std::clamp(x1, 0, src_w - 2));
    y1 = even_floor(std::clamp(y1, 0, src_h - 2));
    x2 = std::clamp(x2, x1 + 2, src_w);
    y2 = std::clamp(y2, y1 + 2, src_h);

    int w = even_floor(x2 - x1);
    int h = even_floor(y2 - y1);
    w = std::clamp(w, 2, even_floor(src_w - x1));
    h = std::clamp(h, 2, even_floor(src_h - y1));

    crop.x = x1;
    crop.y = y1;
    crop.w = std::max(2, w);
    crop.h = std::max(2, h);
    return crop;
}

} // namespace

Layout make_layout(Region region) {
    Layout layout;
    Node root;
    root.leaf = true;
    root.region = region;
    layout.nodes.push_back(root);
    layout.focused = 0;
    return layout;
}

int leaf_count(const Layout &layout) {
    return static_cast<int>(leaf_order(layout).size());
}

std::vector<int> leaf_order(const Layout &layout) {
    std::vector<int> out;
    collect_leaves(layout, 0, out);
    return out;
}

bool split_focused(Layout &layout, SplitDir dir) {
    if (!valid_index(layout, layout.focused) || !layout.nodes[layout.focused].leaf) {
        return false;
    }

    Region inherited = layout.nodes[layout.focused].region;

    Node child_a;
    child_a.leaf = true;
    child_a.region = inherited;
    Node child_b;
    child_b.leaf = true;
    child_b.region = inherited;

    layout.nodes.push_back(child_a);
    int index_a = static_cast<int>(layout.nodes.size()) - 1;
    layout.nodes.push_back(child_b);
    int index_b = static_cast<int>(layout.nodes.size()) - 1;

    Node &parent = layout.nodes[layout.focused];
    parent.leaf = false;
    parent.dir = dir;
    parent.first = index_a;
    parent.second = index_b;

    layout.focused = index_a;
    return true;
}

bool close_focused(Layout &layout) {
    if (!valid_index(layout, layout.focused) || !layout.nodes[layout.focused].leaf) {
        return false;
    }
    int parent_index = find_parent(layout, layout.focused);
    if (parent_index < 0) {
        return false; // 只剩根这一个窗格
    }

    const Node &parent = layout.nodes[parent_index];
    int sibling = (parent.first == layout.focused) ? parent.second : parent.first;
    if (!valid_index(layout, sibling)) {
        return false;
    }

    // 兄弟顶替父节点。这里直接拷贝兄弟的内容覆盖父节点，而不是修改父指针，
    // 免得再去处理"根被替换"的特殊情况；被弃用的节点留在 arena 里不回收，
    // 窗格数量是个位数，不值得为此做压缩。
    Node promoted = layout.nodes[sibling];
    layout.nodes[parent_index] = promoted;

    // 焦点落到顶替后的子树里第一个叶子上。
    std::vector<int> leaves;
    collect_leaves(layout, parent_index, leaves);
    layout.focused = leaves.empty() ? 0 : leaves.front();
    return true;
}

void focus_next(Layout &layout) {
    std::vector<int> leaves = leaf_order(layout);
    if (leaves.empty()) {
        layout.focused = 0;
        return;
    }
    auto it = std::find(leaves.begin(), leaves.end(), layout.focused);
    if (it == leaves.end()) {
        layout.focused = leaves.front();
        return;
    }
    std::size_t next = (static_cast<std::size_t>(it - leaves.begin()) + 1) % leaves.size();
    layout.focused = leaves[next];
}

bool set_focused_region(Layout &layout, const Region &region) {
    if (!valid_index(layout, layout.focused) || !layout.nodes[layout.focused].leaf) {
        return false;
    }
    if (!region_valid(region)) {
        return false;
    }
    layout.nodes[layout.focused].region = region;
    // 换了显示区域就重新居中：上一次拖拽的位移是针对旧区域的，留着没有意义。
    layout.nodes[layout.focused].offset_x = 0.0;
    layout.nodes[layout.focused].offset_y = 0.0;
    return true;
}

std::vector<PixelRect> compute_pane_rects(const Layout &layout, int canvas_w, int canvas_h) {
    std::vector<PixelRect> out;
    if (layout.nodes.empty() || canvas_w <= 0 || canvas_h <= 0) {
        return out;
    }

    std::function<void(int, PixelRect)> walk = [&](int index, PixelRect rect) {
        if (!valid_index(layout, index)) {
            return;
        }
        const Node &node = layout.nodes[index];
        if (node.leaf) {
            out.push_back(rect);
            return;
        }
        if (node.dir == SplitDir::kHorizontal) {
            int w1 = rect.w / 2;
            int w2 = rect.w - w1; // 减法取余数，保证 w1+w2 精确等于 rect.w
            walk(node.first, PixelRect{rect.x, rect.y, w1, rect.h});
            walk(node.second, PixelRect{rect.x + w1, rect.y, w2, rect.h});
        } else {
            int h1 = rect.h / 2;
            int h2 = rect.h - h1;
            walk(node.first, PixelRect{rect.x, rect.y, rect.w, h1});
            walk(node.second, PixelRect{rect.x, rect.y + h1, rect.w, h2});
        }
    };

    walk(0, PixelRect{0, 0, canvas_w, canvas_h});
    return out;
}

std::string build_filter_graph(const Layout &layout, int src_w, int src_h, int canvas_w, int canvas_h,
                                bool hardware_frames) {
    std::vector<int> leaves = leaf_order(layout);
    if (leaves.size() < 2 || src_w <= 0 || src_h <= 0 || canvas_w <= 0 || canvas_h <= 0) {
        return {};
    }
    std::vector<PixelRect> rects = compute_pane_rects(layout, canvas_w, canvas_h);
    if (rects.size() != leaves.size()) {
        return {};
    }
    // 窗格窄到只剩 1 像素时，yuv420p 的色度平面宽/高会变成 0，最后那个 crop
    // 直接报 "Invalid too big or non positive size"。plugin.cpp 里的
    // kMinPanePixels 已经挡住了这种分屏，这里再兜一道：宁可不下发也不要下发
    // 一张必然失败的图。
    for (const PixelRect &rect : rects) {
        if (rect.w < 2 || rect.h < 2) {
            return {};
        }
    }

    std::string graph;

    if (hardware_frames) {
        // crop/scale 只吃软件帧。列出 nv12|p010le 而不是单写 nv12，是给 10bit
        // 素材留出口——format 会从列表里挑一个输入支持的。
        graph += "hwdownload,format=nv12|p010le,";
    }

    graph += fmt::format("split={}", leaves.size());
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        graph += fmt::format("[i{}]", i);
    }
    graph += ";";

    for (std::size_t i = 0; i < leaves.size(); ++i) {
        const Node &node = layout.nodes[leaves[i]];
        CropParams crop = region_to_crop(node.region, src_w, src_h);
        int pw = clamp_positive(rects[i].w);
        int ph = clamp_positive(rects[i].h);
        PanePlacement place =
            compute_placement(node.region, node.offset_x, node.offset_y, src_w, src_h, rects[i]);

        // 把缩放后的内容摆到窗格里的任意位置（可以部分/完全在窗格外），空出
        // 来的地方补黑。用 pad 先给内容加上需要的边距，再 crop 出窗格大小的
        // 窗口——pad 的偏移不能为负，所以负位移由 crop 的起点吸收。
        int pad_l = std::max(0, place.content_x);
        int pad_t = std::max(0, place.content_y);
        int win_x = pad_l - place.content_x; // 恒 >= 0
        int win_y = pad_t - place.content_y;
        // 向上取偶不会破坏 pad 的两个约束（>= 内容尺寸、>= 窗口右下边界）。
        int pad_w = even_ceil(std::max(place.content_w + pad_l, win_x + pw));
        int pad_h = even_ceil(std::max(place.content_h + pad_t, win_y + ph));

        graph += fmt::format("[i{}]crop={}:{}:{}:{},scale={}:{},pad={}:{}:{}:{}:black,"
                             "crop={}:{}:{}:{},setsar=1[p{}];",
                             i, crop.w, crop.h, crop.x, crop.y, place.content_w, place.content_h, pad_w,
                             pad_h, pad_l, pad_t, pw, ph, win_x, win_y, i);
    }

    // 叶子节点索引 -> 它在 leaves 里的序号，用来查 pN 标签。
    std::vector<int> leaf_slot(layout.nodes.size(), -1);
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        leaf_slot[leaves[i]] = static_cast<int>(i);
    }

    int stack_counter = 0;
    // 后序遍历：先把子树的 stack 语句拼进 graph，再拼父节点自己的，保证被
    // 引用的标签总是先于引用它的语句出现。
    std::function<std::string(int, bool)> combine = [&](int index, bool is_root) -> std::string {
        const Node &node = layout.nodes[index];
        if (node.leaf) {
            return fmt::format("p{}", leaf_slot[index]);
        }
        std::string a = combine(node.first, false);
        std::string b = combine(node.second, false);
        const char *op = (node.dir == SplitDir::kHorizontal) ? "hstack" : "vstack";
        if (is_root) {
            // 根节点的输出不带标签，作为整张图的输出 pad。
            graph += fmt::format("[{}][{}]{}=inputs=2", a, b, op);
            return {};
        }
        std::string label = fmt::format("s{}", stack_counter++);
        graph += fmt::format("[{}][{}]{}=inputs=2[{}];", a, b, op, label);
        return label;
    };

    combine(0, true);
    return graph;
}

PanePlacement compute_placement(const Region &region, double offset_x, double offset_y, int src_w,
                                 int src_h, const PixelRect &pane) {
    PanePlacement out;
    if (pane.w <= 0 || pane.h <= 0 || src_w <= 0 || src_h <= 0) {
        return out;
    }
    CropParams crop = region_to_crop(region, src_w, src_h);
    double fit = std::min(static_cast<double>(pane.w) / crop.w, static_cast<double>(pane.h) / crop.h);
    double fill = std::max(static_cast<double>(pane.w) / crop.w, static_cast<double>(pane.h) / crop.h);
    // 未放大时完整画面按原比例整个显示（多余处补黑），放大后才占满窗格。
    double scale = region_is_full(region) ? fit : fill;

    // 缩放比封顶：极端细长的选区（比如只有两三像素高）会让"填满"所需的倍率
    // 爆炸，算出几十万像素宽的中间帧——光一个平面就是几百 MB，滤镜要么失败
    // 要么把内存打爆。超过上限时按比例回退，这种选区本来也没有观察价值，
    // 退化成一个方向填不满、补黑边即可。
    constexpr double kMaxContentDimension = 8192.0;
    double cap = std::min(kMaxContentDimension / (crop.w * scale), kMaxContentDimension / (crop.h * scale));
    if (cap < 1.0) {
        scale *= cap;
    }

    // 尺寸向上取偶：向下取会让"填满"这一侧差 1 像素露出黑边。
    out.content_w = std::max(2, even_ceil(static_cast<int>(std::lround(crop.w * scale))));
    out.content_h = std::max(2, even_ceil(static_cast<int>(std::lround(crop.h * scale))));
    // 先居中，再叠加拖拽位移（按窗格尺寸归一化，不限制范围）。偏移取偶，
    // 差的那 1 像素肉眼不可见，但能让后面 pad 的参数全部保持偶数对齐。
    out.content_x =
        even_floor((pane.w - out.content_w) / 2 + static_cast<int>(std::lround(offset_x * pane.w)));
    out.content_y =
        even_floor((pane.h - out.content_h) / 2 + static_cast<int>(std::lround(offset_y * pane.h)));
    return out;
}

double placement_to_region_u(const PanePlacement &placement, double pane_px) {
    if (placement.content_w <= 0) {
        return 0.0;
    }
    return std::clamp((pane_px - placement.content_x) / placement.content_w, 0.0, 1.0);
}

double placement_to_region_v(const PanePlacement &placement, double pane_py) {
    if (placement.content_h <= 0) {
        return 0.0;
    }
    return std::clamp((pane_py - placement.content_y) / placement.content_h, 0.0, 1.0);
}

std::optional<PaneHit> hit_test(const Layout &layout, int canvas_w, int canvas_h, double cu, double cv) {
    if (cu < 0.0 || cu > 1.0 || cv < 0.0 || cv > 1.0) {
        return std::nullopt;
    }
    std::vector<int> leaves = leaf_order(layout);
    std::vector<PixelRect> rects = compute_pane_rects(layout, canvas_w, canvas_h);
    if (leaves.size() != rects.size() || canvas_w <= 0 || canvas_h <= 0) {
        return std::nullopt;
    }

    double px = cu * canvas_w;
    double py = cv * canvas_h;

    for (std::size_t i = 0; i < rects.size(); ++i) {
        const PixelRect &rect = rects[i];
        if (rect.w <= 0 || rect.h <= 0) {
            continue;
        }
        // 右/下边界用闭区间兜底，避免坐标正好落在画布最右/最下时命中不到。
        bool inside_x = px >= rect.x && (px < rect.x + rect.w || rect.x + rect.w >= canvas_w);
        bool inside_y = py >= rect.y && (py < rect.y + rect.h || rect.y + rect.h >= canvas_h);
        if (inside_x && inside_y) {
            PaneHit hit;
            hit.leaf = leaves[i];
            hit.u = std::clamp((px - rect.x) / rect.w, 0.0, 1.0);
            hit.v = std::clamp((py - rect.y) / rect.h, 0.0, 1.0);
            return hit;
        }
    }
    return std::nullopt;
}

void sort_segments(std::vector<LayoutSegment> &segments) {
    std::sort(segments.begin(), segments.end(),
              [](const LayoutSegment &lhs, const LayoutSegment &rhs) { return lhs.a < rhs.a; });
}

std::optional<std::size_t> find_segment_at(const std::vector<LayoutSegment> &segments, double pos) {
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const LayoutSegment &seg = segments[i];
        if (pos >= seg.a && pos <= seg.b) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> overlapping_segment(const std::vector<LayoutSegment> &segments, double a, double b,
                                                std::optional<std::size_t> ignore_index) {
    double lo = std::min(a, b);
    double hi = std::max(a, b);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (ignore_index && *ignore_index == i) {
            continue;
        }
        const LayoutSegment &seg = segments[i];
        // 严格重叠才算冲突：首尾相接（前一段的 b 等于后一段的 a）是允许的。
        if (lo < seg.b && seg.a < hi) {
            return i;
        }
    }
    return std::nullopt;
}

Region subregion(const Region &pane, double u1, double v1, double u2, double v2) {
    double lo_u = std::min(u1, u2);
    double hi_u = std::max(u1, u2);
    double lo_v = std::min(v1, v2);
    double hi_v = std::max(v1, v2);

    Region out;
    out.x1 = pane.x1 + lo_u * pane.width();
    out.x2 = pane.x1 + hi_u * pane.width();
    out.y1 = pane.y1 + lo_v * pane.height();
    out.y2 = pane.y1 + hi_v * pane.height();
    return out;
}

} // namespace split_zoom_box
