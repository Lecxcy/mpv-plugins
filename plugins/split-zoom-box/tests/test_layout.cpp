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
    REQUIRE(graph.find(":0:") != std::string::npos); // 完整画面那格的 x 偏移是 0
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

TEST_CASE("窗格内容矩形扣掉了保比缩放的黑边", "[layout][aspect]") {
    // 16:9 的完整画面塞进一个 640x720 的窄窗格：宽度撑满，上下留黑边
    PixelRect pane{0, 0, 640, 720};
    PixelRect content = pane_content_rect(Region{}, 1280, 720, pane);
    REQUIRE(content.w == 640);
    REQUIRE(content.h == 360);
    REQUIRE(content.x == 0);
    REQUIRE(content.y == 180); // 居中
    // 宽高比保持 16:9
    REQUIRE(static_cast<double>(content.w) / content.h == Catch::Approx(16.0 / 9.0));

    // 区域宽高比正好等于窗格时应当占满，没有黑边。
    // 720x720 的源里取 0..0.5 x 0..0.5 就是 360x360 的正方形区域。
    PixelRect square_pane{0, 0, 360, 360};
    PixelRect filled = pane_content_rect(Region{0.0, 0.0, 0.5, 0.5}, 720, 720, square_pane);
    REQUIRE(filled.w == 360);
    REQUIRE(filled.h == 360);
    REQUIRE(filled.x == 0);
    REQUIRE(filled.y == 0);

    // 区域比窗格更"高"时反过来：高度撑满，左右留黑边
    PixelRect tall = pane_content_rect(Region{0.0, 0.0, 0.5, 1.0}, 720, 720, square_pane);
    REQUIRE(tall.h == 360);
    REQUIRE(tall.w == 180);
    REQUIRE(tall.x == 90); // 水平居中
}

TEST_CASE("框选换算走内容矩形，黑边不参与（回归）", "[layout][aspect]") {
    // 左右分屏，左格显示完整 16:9 画面 -> 内容只占窗格中间一条，上下是黑边。
    Layout layout = make_layout();
    REQUIRE(split_focused(layout, SplitDir::kHorizontal));
    std::vector<PixelRect> panes = compute_pane_rects(layout, 1280, 720);
    PixelRect content = pane_content_rect(Region{}, 1280, 720, panes[0]);

    // 在内容正中间框一个居中的小框，换算回源坐标应当也居中。
    double u1 = 0.25, u2 = 0.75, v1 = 0.25, v2 = 0.75;
    Region got = subregion(layout.nodes[leaf_order(layout)[0]].region, u1, v1, u2, v2);
    REQUIRE(got.x1 == Catch::Approx(0.25));
    REQUIRE(got.y1 == Catch::Approx(0.25));

    // 关键回归：同一个屏幕位置，用"内容矩形"和用"整个窗格（含黑边）"当分母
    // 换算出来的归一化坐标必须不同——用错分母就是之前"放大后位置不对"的原因。
    //
    // 注意不能拿垂直中点来验：内容是居中放置的，中点在两种算法下恰好都等于
    // 0.5，是唯一验不出差别的位置。这里取内容的上边缘。
    double py = content.y; // 画面顶端
    double v_content = (py - content.y) / content.h;
    double v_pane = (py - panes[0].y) / panes[0].h;
    REQUIRE(v_content == Catch::Approx(0.0));
    REQUIRE(v_pane == Catch::Approx(0.25)); // 180/720：黑边占掉了上面 1/4
    REQUIRE(v_content != Catch::Approx(v_pane));
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

TEST_CASE("set_focused_region 拒绝退化区域", "[layout]") {
    Layout layout = make_layout();
    REQUIRE_FALSE(set_focused_region(layout, Region{0.5, 0.5, 0.5, 0.9}));  // 零宽
    REQUIRE_FALSE(set_focused_region(layout, Region{0.5, 0.5, 0.9, 0.5}));  // 零高
    REQUIRE_FALSE(set_focused_region(layout, Region{-0.1, 0.0, 0.5, 0.5})); // 越界
    REQUIRE(set_focused_region(layout, Region{0.1, 0.1, 0.9, 0.9}));
}
