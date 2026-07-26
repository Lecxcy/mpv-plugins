#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "split_zoom_box/layout.h"

using namespace split_zoom_box;

namespace {

// 沿着树把每个内部节点的两个子节点在堆叠轴上的尺寸加起来，逐一断言等于父
// 节点。hstack/vstack 对这个是零容忍的（差 1 像素整张图就配置失败），所以
// 这条不变量要独立于 build_filter_graph 单独验证。
void check_exact_sums(const Layout &layout, int index, PixelRect rect) {
    const Node &node = layout.nodes[index];
    if (node.leaf) {
        return;
    }
    std::vector<PixelRect> children;
    if (node.dir == SplitDir::kHorizontal) {
        int w1 = rect.w / 2;
        int w2 = rect.w - w1;
        REQUIRE(w1 + w2 == rect.w);
        children.push_back(PixelRect{rect.x, rect.y, w1, rect.h});
        children.push_back(PixelRect{rect.x + w1, rect.y, w2, rect.h});
        // hstack 要求两边高度完全相同
        REQUIRE(children[0].h == children[1].h);
    } else {
        int h1 = rect.h / 2;
        int h2 = rect.h - h1;
        REQUIRE(h1 + h2 == rect.h);
        children.push_back(PixelRect{rect.x, rect.y, rect.w, h1});
        children.push_back(PixelRect{rect.x, rect.y + h1, rect.w, h2});
        // vstack 要求两边宽度完全相同
        REQUIRE(children[0].w == children[1].w);
    }
    check_exact_sums(layout, node.first, children[0]);
    check_exact_sums(layout, node.second, children[1]);
}

} // namespace

TEST_CASE("make_layout 是单窗格且覆盖完整画面", "[layout]") {
    Layout layout = make_layout();
    REQUIRE(leaf_count(layout) == 1);
    REQUIRE(region_is_full(layout.nodes[0].region));
    // 单窗格走 video-zoom 路径，不应该产出滤镜图
    REQUIRE(build_filter_graph(layout, 640, 360, 640, 360).empty());
}

TEST_CASE("分屏时两个子窗格继承原区域", "[layout]") {
    Region zoomed{0.25, 0.25, 0.75, 0.75};
    Layout layout = make_layout(zoomed);
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    REQUIRE(leaf_count(layout) == 2);

    std::vector<int> leaves = leaf_order(layout);
    for (int leaf : leaves) {
        REQUIRE(layout.nodes[leaf].region.x1 == Catch::Approx(0.25));
        REQUIRE(layout.nodes[leaf].region.x2 == Catch::Approx(0.75));
    }
    // 焦点留在第一个子窗格
    REQUIRE(layout.focused == leaves.front());
}

TEST_CASE("窗格像素尺寸在各种画布下都精确求和", "[layout][invariant]") {
    // 奇数画布尺寸是最容易暴露取整问题的场景
    for (int canvas_w : {640, 641, 1919, 100}) {
        for (int canvas_h : {360, 361, 1081, 51}) {
            Layout layout = make_layout();
            REQUIRE(split_focused(layout, SplitDir::kHorizontal));
            focus_next(layout);
            REQUIRE(split_focused(layout, SplitDir::kVertical));
            focus_next(layout);
            REQUIRE(split_focused(layout, SplitDir::kHorizontal));

            check_exact_sums(layout, 0, PixelRect{0, 0, canvas_w, canvas_h});

            std::vector<PixelRect> rects = compute_pane_rects(layout, canvas_w, canvas_h);
            REQUIRE(rects.size() == static_cast<std::size_t>(leaf_count(layout)));
            for (const PixelRect &rect : rects) {
                REQUIRE(rect.w > 0);
                REQUIRE(rect.h > 0);
            }
        }
    }
}

TEST_CASE("2x2 网格的滤镜图结构正确", "[layout][graph]") {
    // 根上下分屏，两个子节点各自左右分屏 -> 2x2
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kVertical));
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    focus_next(layout);
    focus_next(layout);
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    REQUIRE(leaf_count(layout) == 4);

    std::string graph = build_filter_graph(layout, 640, 360, 640, 360);
    REQUIRE_FALSE(graph.empty());
    REQUIRE(graph.find("split=4") != std::string::npos);
    // 每个窗格都必须带 setsar=1，漏了会让 SAR 透过 scale 改掉显示尺寸
    std::size_t setsar_count = 0;
    for (std::size_t pos = graph.find("setsar=1"); pos != std::string::npos;
         pos = graph.find("setsar=1", pos + 1)) {
        ++setsar_count;
    }
    REQUIRE(setsar_count == 4);
    // 两个 hstack 各带输出标签，根上的 vstack 不带（作为整张图的输出）
    REQUIRE(graph.find("hstack=inputs=2[s0]") != std::string::npos);
    REQUIRE(graph.find("hstack=inputs=2[s1]") != std::string::npos);
    REQUIRE(graph.find("vstack=inputs=2") != std::string::npos);
    REQUIRE(graph.back() != ';');
}

TEST_CASE("硬件帧下图头部加 hwdownload，软件帧下不能加", "[layout][graph][hwdec]") {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));

    // 实测真值表：硬件帧不加前缀会失败，软件帧加了前缀也会失败，
    // 没有一份图能同时兼容两种情况（SPEC §6.7）。
    std::string sw = build_filter_graph(layout, 640, 360, 640, 360, false);
    std::string hw = build_filter_graph(layout, 640, 360, 640, 360, true);

    REQUIRE(sw.find("hwdownload") == std::string::npos);
    REQUIRE(hw.rfind("hwdownload,format=", 0) == 0); // 必须在最前面
    REQUIRE(hw.find("nv12") != std::string::npos);
    REQUIRE(hw.find("p010le") != std::string::npos); // 给 10bit 留出口
    // 前缀之外两者应当完全一致
    REQUIRE(hw.substr(hw.find("split=")) == sw.substr(sw.find("split=")));
    // 默认参数保持软件帧行为，避免调用方漏传时悄悄变成硬件帧图
    REQUIRE(build_filter_graph(layout, 640, 360, 640, 360) == sw);
}

TEST_CASE("滤镜图里被引用的标签总是先定义后使用", "[layout][graph]") {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kVertical));
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));

    std::string graph = build_filter_graph(layout, 640, 360, 640, 360);
    REQUIRE_FALSE(graph.empty());
    // s0 必须先作为某条语句的输出出现，之后才被当作输入引用
    std::size_t defined = graph.find("[s0];");
    std::size_t used = graph.find("[s0][");
    REQUIRE(defined != std::string::npos);
    REQUIRE(used != std::string::npos);
    REQUIRE(defined < used);
}

TEST_CASE("crop 参数落在源画面范围内且不为零", "[layout][graph]") {
    Layout layout = make_layout(Region{0.0, 0.0, 1.0, 1.0});
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    // 给一个极窄的区域，检查不会退化成 0 宽
    REQUIRE(set_focused_region(layout, Region{0.9999, 0.9999, 1.0, 1.0}));

    std::string graph = build_filter_graph(layout, 640, 360, 640, 360);
    REQUIRE_FALSE(graph.empty());
    REQUIRE(graph.find("crop=0:") == std::string::npos);
    REQUIRE(graph.find("pad=") != std::string::npos); // 摆放用 pad 补黑
    REQUIRE(graph.find(":0:") != std::string::npos); // 完整画面那格的 x 偏移是 0
}

TEST_CASE("默认视口外扩成窗格比例：完整画面 + 黑边", "[layout][view]") {
    // 1280x720 的画面进 640x720 的窄窗格：视口要在**垂直方向外扩**，
    // 才能在保持比例的前提下把整幅画面装下，多出来的就是上下黑边。
    PixelRect pane{0, 0, 640, 720};
    Region v = fit_view_aspect(Region{}, 1280, 720, pane, true);
    REQUIRE(v.width() == Catch::Approx(1.0));  // 水平不动，整幅都在
    REQUIRE(v.height() > 1.0);                  // 垂直外扩出黑边
    // 视口宽高比（按源像素算）等于窗格比例
    double ar = (v.width() * 1280) / (v.height() * 720);
    REQUIRE(ar == Catch::Approx(640.0 / 720.0));
    // 以中心为基准
    REQUIRE(v.y1 == Catch::Approx(1.0 - v.y2));
}

TEST_CASE("框选内缩成窗格比例：占满窗格、裁掉多余", "[layout][view]") {
    PixelRect pane{0, 0, 640, 720};
    // 一个比窗格更"宽"的选区，内缩时应当削掉宽度
    Region picked{0.2, 0.4, 0.8, 0.6};
    Region v = fit_view_aspect(picked, 1280, 720, pane, false);
    REQUIRE(v.height() == Catch::Approx(picked.height())); // 高度不动
    REQUIRE(v.width() < picked.width());                    // 宽度被削
    double ar = (v.width() * 1280) / (v.height() * 720);
    REQUIRE(ar == Catch::Approx(640.0 / 720.0));
}

TEST_CASE("视口坐标映射不夹取，黑边能一起放大（回归）", "[layout][view]") {
    // 默认视口在垂直方向超出了 [0,1]，窗格顶端对应的是画面之外的黑边
    PixelRect pane{0, 0, 640, 720};
    Region v = fit_view_aspect(Region{}, 1280, 720, pane, true);
    double top = view_to_source_v(v, 0.0);
    REQUIRE(top < 0.0); // 落在画面之外 —— 不被夹到 0
    double bottom = view_to_source_v(v, 1.0);
    REQUIRE(bottom > 1.0);
    // 中点仍是画面中心
    REQUIRE(view_to_source_v(v, 0.5) == Catch::Approx(0.5));

    // 框住上半个窗格（含黑边）时，得到的视口也应当含画面之外的部分
    Region picked;
    picked.x1 = view_to_source_u(v, 0.0);
    picked.x2 = view_to_source_u(v, 1.0);
    picked.y1 = view_to_source_v(v, 0.0);
    picked.y2 = view_to_source_v(v, 0.5);
    REQUIRE(picked.y1 < 0.0);
}

TEST_CASE("平移视口不受任何限制", "[layout][view][pan]") {
    Region v{0.0, 0.0, 1.0, 1.0};
    Region moved = translate_view(v, 2.5, -1.5);
    REQUIRE(moved.x1 == Catch::Approx(2.5));
    REQUIRE(moved.y1 == Catch::Approx(-1.5));
    REQUIRE(moved.width() == Catch::Approx(v.width())); // 大小不变
    // 完整画面也能挪（早期版本这里会被夹死、拖不动）
    REQUIRE(moved.x1 != Catch::Approx(0.0));
}

TEST_CASE("视口合法性允许越界但挡住离谱值", "[layout][view]") {
    REQUIRE(view_valid(Region{-0.5, -0.5, 1.5, 1.5}));  // 越界是正常的
    REQUIRE_FALSE(view_valid(Region{0.5, 0.0, 0.5, 1.0})); // 零宽
    REQUIRE_FALSE(view_valid(Region{0.0, 0.0, 100.0, 1.0})); // 大到没意义
}

TEST_CASE("滤镜参数全部偶数对齐（yuv420p 回归）", "[layout][graph][even]") {
    // yuv420p 色度 2x2 子采样：奇数会让 pad 的 "padded >= input" 校验在对齐后
    // 失败，高度 1 的 crop 会让色度平面高度变成 0（见 SPEC §6.9）。
    for (double w : {0.02, 0.005, 0.3, 0.0008}) {
        for (double h : {0.02, 0.002, 0.3}) {
            Layout layout = make_layout();
            REQUIRE(split_focused(layout, SplitDir::kHorizontal));
            REQUIRE(set_focused_region(layout, Region{0.3, 0.3, 0.3 + w, 0.3 + h}));
            std::string g = build_filter_graph(layout, 1280, 720, 1280, 720);
            REQUIRE_FALSE(g.empty());
            // 抓出所有 crop/pad/scale 的数字，逐个确认是偶数
            for (const std::string &kw : {std::string("crop="), std::string("pad="), std::string("scale=")}) {
                for (std::size_t pos = g.find(kw); pos != std::string::npos; pos = g.find(kw, pos + 1)) {
                    std::size_t start = pos + kw.size();
                    std::size_t stop = g.find_first_of(",;[", start);
                    std::string args = g.substr(start, stop - start);
                    std::size_t f = 0;
                    while (f < args.size()) {
                        std::size_t colon = args.find(':', f);
                        std::string num = args.substr(f, colon == std::string::npos ? colon : colon - f);
                        if (!num.empty() && num.find_first_not_of("-0123456789") == std::string::npos) {
                            REQUIRE(std::stoi(num) % 2 == 0);
                        }
                        if (colon == std::string::npos) {
                            break;
                        }
                        f = colon + 1;
                    }
                }
            }
        }
    }
}

TEST_CASE("窗格小于 2 像素时不下发滤镜图", "[layout][graph]") {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    REQUIRE(build_filter_graph(layout, 1280, 720, 2, 720).empty());
    REQUIRE(build_filter_graph(layout, 1280, 720, 640, 1).empty());
    REQUIRE_FALSE(build_filter_graph(layout, 1280, 720, 1280, 720).empty());
}

TEST_CASE("视口完全移出画面时窗格全黑", "[layout][view][graph]") {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    REQUIRE(set_focused_region(layout, Region{5.0, 0.0, 6.0, 1.0})); // 挪到画面右侧很远
    std::string g = build_filter_graph(layout, 1280, 720, 1280, 720);
    REQUIRE_FALSE(g.empty());
    REQUIRE(g.find("drawbox") != std::string::npos); // 整片涂黑
}

TEST_CASE("hit_test 命中正确窗格并给出窗格内坐标", "[layout][hit]") {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    std::vector<int> leaves = leaf_order(layout);

    auto left = hit_test(layout, 640, 360, 0.25, 0.5);
    REQUIRE(left.has_value());
    REQUIRE(left->leaf == leaves[0]);
    REQUIRE(left->u == Catch::Approx(0.5).margin(0.01));

    auto right = hit_test(layout, 640, 360, 0.75, 0.5);
    REQUIRE(right.has_value());
    REQUIRE(right->leaf == leaves[1]);
    REQUIRE(right->u == Catch::Approx(0.5).margin(0.01));

    // 画布最右下角必须命中最后一个窗格，不能因为半开区间落空
    auto corner = hit_test(layout, 640, 360, 1.0, 1.0);
    REQUIRE(corner.has_value());
    REQUIRE(corner->leaf == leaves[1]);

    REQUIRE_FALSE(hit_test(layout, 640, 360, 1.5, 0.5).has_value());
}

TEST_CASE("窗格内框选换算回源坐标（坐标反查往返）", "[layout][hit]") {
    // 右窗格显示源画面右半边，在它内部框选中间一半
    Region pane{0.5, 0.0, 1.0, 1.0};
    Region result = subregion(pane, 0.25, 0.0, 0.75, 1.0);
    REQUIRE(result.x1 == Catch::Approx(0.625));
    REQUIRE(result.x2 == Catch::Approx(0.875));

    // 反向拖拽（u1>u2）也要归一化
    Region flipped = subregion(pane, 0.75, 1.0, 0.25, 0.0);
    REQUIRE(flipped.x1 == Catch::Approx(result.x1));
    REQUIRE(flipped.x2 == Catch::Approx(result.x2));
}

TEST_CASE("关闭窗格后兄弟顶替父节点", "[layout]") {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    REQUIRE(set_focused_region(layout, Region{0.0, 0.0, 0.5, 1.0}));
    focus_next(layout);
    REQUIRE(set_focused_region(layout, Region{0.5, 0.0, 1.0, 1.0}));

    // 关掉第二个窗格，应只剩第一个窗格的区域
    REQUIRE(close_focused(layout));
    REQUIRE(leaf_count(layout) == 1);
    std::vector<int> leaves = leaf_order(layout);
    REQUIRE(layout.nodes[leaves[0]].region.x2 == Catch::Approx(0.5));
    REQUIRE(layout.focused == leaves[0]);

    // 只剩一个窗格时再关返回 false（由调用方决定是不是整体退出分屏）
    REQUIRE_FALSE(close_focused(layout));
}

TEST_CASE("分割比例可调且像素精确求和", "[layout][ratio]") {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    // 默认等分
    std::vector<PixelRect> even = compute_pane_rects(layout, 1000, 400);
    REQUIRE(even[0].w == 500);
    REQUIRE(even[1].w == 500);

    REQUIRE(set_node_ratio(layout, 0, 0.3));
    std::vector<PixelRect> tuned = compute_pane_rects(layout, 1000, 400);
    REQUIRE(tuned[0].w == 300);
    REQUIRE(tuned[1].w == 700);
    // 两侧之和仍精确等于画布（hstack 对此零容忍）
    REQUIRE(tuned[0].w + tuned[1].w == 1000);
    REQUIRE(tuned[0].h == tuned[1].h);

    // 越界比例被夹住，不会把某一侧拖没
    REQUIRE(set_node_ratio(layout, 0, -5.0));
    REQUIRE(compute_pane_rects(layout, 1000, 400)[0].w > 0);
    REQUIRE(set_node_ratio(layout, 0, 5.0));
    std::vector<PixelRect> maxed = compute_pane_rects(layout, 1000, 400);
    REQUIRE(maxed[1].w > 0);
    REQUIRE(maxed[0].w + maxed[1].w == 1000);
}

TEST_CASE("分隔条命中判定", "[layout][ratio]") {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    REQUIRE(set_node_ratio(layout, 0, 0.25)); // 分隔条在 x=250/1000

    auto on = hit_test_divider(layout, 1000, 400, 0.25, 0.5, 8);
    REQUIRE(on.has_value());
    REQUIRE(on->node == 0);
    REQUIRE(on->dir == SplitDir::kHorizontal);

    // 离得远就不该命中，否则普通左键点击会被误吞
    REQUIRE_FALSE(hit_test_divider(layout, 1000, 400, 0.60, 0.5, 8).has_value());
    // 单窗格没有分隔条
    Layout single = make_layout();
    REQUIRE_FALSE(hit_test_divider(single, 1000, 400, 0.5, 0.5, 8).has_value());
}

TEST_CASE("默认不选中任何窗格，编辑时才落到具体窗格", "[layout][focus]") {
    Layout layout = make_layout();
    clear_focus(layout);
    REQUIRE(layout.focused == kNoFocus);
    // 未选中时编辑动作默认作用于第一个窗格
    REQUIRE(focused_or_first(layout) == leaf_order(layout).front());
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    REQUIRE(layout.focused != kNoFocus);

    clear_focus(layout);
    focus_next(layout); // 未选中时第一次切换选中第一个
    REQUIRE(layout.focused == leaf_order(layout).front());
}

TEST_CASE("滚轮以锚点为中心缩放视口", "[layout][view]") {
    Region v{0.0, 0.0, 1.0, 1.0};
    // 以正中间为锚点放大：锚点位置不动，视口变小
    Region in = zoom_view_at(v, 0.5, 0.5, 0.5);
    REQUIRE(in.width() == Catch::Approx(0.5));
    REQUIRE(view_to_source_u(in, 0.5) == Catch::Approx(0.5));

    // 以左上角为锚点放大：该点仍映射到原来的源坐标
    Region corner = zoom_view_at(v, 0.0, 0.0, 0.5);
    REQUIRE(view_to_source_u(corner, 0.0) == Catch::Approx(0.0));
    REQUIRE(corner.width() == Catch::Approx(0.5));

    // 缩小
    Region out = zoom_view_at(v, 0.5, 0.5, 2.0);
    REQUIRE(out.width() == Catch::Approx(2.0));
}

TEST_CASE("分屏段列表的省略规则与 ab-loop 一致（首尾各 6 段）", "[layout][display]") {
    // 不超过上限就全展示
    auto few = plan_segment_display(5);
    REQUIRE(few.head_count == 5);
    REQUIRE(few.hidden_count == 0);
    REQUIRE(few.tail_count == 0);

    auto exact = plan_segment_display(12);
    REQUIRE(exact.head_count == 12);
    REQUIRE(exact.hidden_count == 0);

    // 超过就首尾各留 6、中间折叠
    auto many = plan_segment_display(20);
    REQUIRE(many.head_count == 6);
    REQUIRE(many.tail_count == 6);
    REQUIRE(many.hidden_count == 8);
    REQUIRE(many.head_count + many.hidden_count + many.tail_count == 20);
}

TEST_CASE("focus_next 在所有窗格间循环", "[layout]") {
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    focus_next(layout);
    REQUIRE(split_focused(layout, SplitDir::kVertical));
    REQUIRE(leaf_count(layout) == 3);

    std::vector<int> leaves = leaf_order(layout);
    layout.focused = leaves[0];
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        REQUIRE(layout.focused == leaves[i]);
        focus_next(layout);
    }
    REQUIRE(layout.focused == leaves[0]); // 绕回起点
}

TEST_CASE("set_focused_region 拒绝退化视口但允许越界", "[layout][view]") {
    Layout layout = make_layout();
    REQUIRE_FALSE(set_focused_region(layout, Region{0.5, 0.5, 0.5, 0.9})); // 零宽
    REQUIRE_FALSE(set_focused_region(layout, Region{0.5, 0.5, 0.9, 0.5})); // 零高
    // 越界在视口模型下是**合法**的：超出源画面的部分就是黑边。
    // （旧模型把它当非法，于是框选框不到黑边、也没法把画面拖出窗格。）
    REQUIRE(set_focused_region(layout, Region{-0.1, 0.0, 0.5, 0.5}));
    REQUIRE(set_focused_region(layout, Region{0.1, 0.1, 0.9, 0.9}));
    // 但离谱的量级仍然挡掉
    REQUIRE_FALSE(set_focused_region(layout, Region{0.0, 0.0, 100.0, 1.0}));
}

namespace {

// 造一个能一眼认出来的段：布局的窗格数当指纹，用来验证两半确实各自拷了一份。
LayoutSegment make_segment(double a, double b, int splits) {
    LayoutSegment seg;
    seg.a = a;
    seg.b = b;
    // LayoutSegment 默认构造出来的 Layout 是空 nodes（不是单窗格），实际用法里
    // 总是从 active_layout 拷一份真实布局进来，这里也得显式建一个。
    seg.layout = make_layout();
    for (int i = 0; i < splits; ++i) {
        REQUIRE(split_focused(seg.layout, SplitDir::kHorizontal));
    }
    return seg;
}

} // namespace

TEST_CASE("divide_segment_at 以播放位置把段切成前后两半", "[layout][segment][divide]") {
    std::vector<LayoutSegment> segments{make_segment(5.0, 15.0, 1), make_segment(20.0, 30.0, 2)};

    auto back = divide_segment_at(segments, 8.0);
    REQUIRE(back);
    REQUIRE(*back == 1);
    REQUIRE(segments.size() == 3);

    // 前半 [5,8]、后半 [8,15]，分割点是两半共有的边界，后面的段整个不受影响。
    REQUIRE(segments[0].a == Catch::Approx(5.0));
    REQUIRE(segments[0].b == Catch::Approx(8.0));
    REQUIRE(segments[1].a == Catch::Approx(8.0));
    REQUIRE(segments[1].b == Catch::Approx(15.0));
    REQUIRE(segments[2].a == Catch::Approx(20.0));
    REQUIRE(segments[2].b == Catch::Approx(30.0));

    // 排序不变量（起点升序）不用再调 sort_segments 就已经成立。
    std::vector<LayoutSegment> sorted = segments;
    sort_segments(sorted);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        REQUIRE(sorted[i].a == Catch::Approx(segments[i].a));
    }

    // 互不重叠：首尾相接是允许的形状，divide 不该造出真正的重叠。
    REQUIRE_FALSE(overlapping_segment(segments, segments[0].a, segments[0].b, 0));
    REQUIRE_FALSE(overlapping_segment(segments, segments[1].a, segments[1].b, 1));

    // 两半各拿到一份原布局的拷贝，改一边不影响另一边。
    REQUIRE(leaf_count(segments[0].layout) == 2);
    REQUIRE(leaf_count(segments[1].layout) == 2);
    REQUIRE(split_focused(segments[1].layout, SplitDir::kVertical));
    REQUIRE(leaf_count(segments[1].layout) == 3);
    REQUIRE(leaf_count(segments[0].layout) == 2);
}

TEST_CASE("divide_segment_at 拒绝段外和贴边的分割点", "[layout][segment][divide]") {
    std::vector<LayoutSegment> segments{make_segment(5.0, 15.0, 1)};
    const std::vector<LayoutSegment> before = segments;

    auto unchanged = [&]() {
        REQUIRE(segments.size() == before.size());
        REQUIRE(segments[0].a == Catch::Approx(before[0].a));
        REQUIRE(segments[0].b == Catch::Approx(before[0].b));
    };

    REQUIRE_FALSE(divide_segment_at(segments, 2.0)); // 段前
    unchanged();
    REQUIRE_FALSE(divide_segment_at(segments, 20.0)); // 段后
    unchanged();
    // 两个端点本身：闭区间意义上"在段内"，但会切出零长的一半。
    REQUIRE_FALSE(divide_segment_at(segments, 5.0));
    unchanged();
    REQUIRE_FALSE(divide_segment_at(segments, 15.0));
    unchanged();
    // 贴边但严格在内部：比一帧还短的段没有意义，同样拒绝。
    REQUIRE_FALSE(divide_segment_at(segments, 5.0 + kMinSegmentDuration / 2.0));
    unchanged();
    REQUIRE_FALSE(divide_segment_at(segments, 15.0 - kMinSegmentDuration / 2.0));
    unchanged();
    // 稍微离开下限就放行。刻意不断言"恰好等于 kMinSegmentDuration"这个点：
    // 5.0 + 0.05 在二进制里落到 5.04999999999999982，写成等号的用例只是在考
    // 浮点舍入方向，不是在考这条规则。
    REQUIRE(divide_segment_at(segments, 5.0 + kMinSegmentDuration * 2.0));
    REQUIRE(segments.size() == 2);
    REQUIRE(segments[0].b == Catch::Approx(5.0 + kMinSegmentDuration * 2.0));
}

TEST_CASE("divide_segment_at 切出来的两半都能继续再切", "[layout][segment][divide]") {
    std::vector<LayoutSegment> segments{make_segment(0.0, 12.0, 0)};
    REQUIRE(divide_segment_at(segments, 6.0));
    REQUIRE(divide_segment_at(segments, 3.0));  // 切前半
    REQUIRE(divide_segment_at(segments, 9.0));  // 切后半
    REQUIRE(segments.size() == 4);

    const double bounds[5] = {0.0, 3.0, 6.0, 9.0, 12.0};
    for (std::size_t i = 0; i < segments.size(); ++i) {
        REQUIRE(segments[i].a == Catch::Approx(bounds[i]));
        REQUIRE(segments[i].b == Catch::Approx(bounds[i + 1]));
    }
    // 分割点归前一半：find_segment_at 在边界上命中的是先出现的那个。
    REQUIRE(find_segment_at(segments, 6.0) == std::size_t{1});
    REQUIRE(find_segment_at(segments, 4.0) == std::size_t{1});
    REQUIRE(find_segment_at(segments, 7.0) == std::size_t{2});
}
