#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "drag_zoom_box/logic.h"

using namespace drag_zoom_box;

TEST_CASE("classify_direction only accepts the two matching diagonals", "[drag-zoom-box]") {
    CHECK(classify_direction(10.0, 10.0, 4.0) == DragDirection::kZoomIn);
    CHECK(classify_direction(-10.0, -10.0, 4.0) == DragDirection::kReset);

    // 上游 Lua 版本只看水平方向，这两个"反对角线"会被误判为 zoom/reset；
    // 重写后要求两个轴同号才算合法对角线，这两种情况都应判为 kNone。
    CHECK(classify_direction(10.0, -10.0, 4.0) == DragDirection::kNone);
    CHECK(classify_direction(-10.0, 10.0, 4.0) == DragDirection::kNone);

    // 纯水平/纯垂直拖拽不算对角线。
    CHECK(classify_direction(10.0, 0.0, 4.0) == DragDirection::kNone);
    CHECK(classify_direction(0.0, 10.0, 4.0) == DragDirection::kNone);
    CHECK(classify_direction(0.0, 0.0, 4.0) == DragDirection::kNone);
}

TEST_CASE("classify_direction gates on the minimum drag distance", "[drag-zoom-box]") {
    CHECK(classify_direction(2.0, 2.0, 4.0) == DragDirection::kNone);
    CHECK(classify_direction(4.0, 4.0, 4.0) == DragDirection::kZoomIn);
    CHECK(classify_direction(-4.0, -4.0, 4.0) == DragDirection::kReset);
    CHECK(classify_direction(-2.0, -2.0, 4.0) == DragDirection::kNone);
}

TEST_CASE("normalize_box orders coordinates regardless of drag direction", "[drag-zoom-box]") {
    Box box = normalize_box(Point{10.0, 20.0}, Point{5.0, 5.0});
    CHECK(box.x1 == 5.0);
    CHECK(box.y1 == 5.0);
    CHECK(box.x2 == 10.0);
    CHECK(box.y2 == 20.0);
}

TEST_CASE("compute_geometry rejects unusable inputs", "[drag-zoom-box]") {
    CHECK_FALSE(compute_geometry(0.0, 800.0, 0.0, 0.0, 0.0, 0.0, 1600.0, 900.0).has_value());
    CHECK_FALSE(compute_geometry(1000.0, 800.0, 0.0, 0.0, 0.0, 0.0, 0.0, 900.0).has_value());
    // 左右 margin 之和超过窗口宽度：视频渲染矩形退化，无法拖拽。
    CHECK_FALSE(compute_geometry(1000.0, 800.0, 600.0, 600.0, 0.0, 0.0, 1600.0, 900.0).has_value());
}

TEST_CASE("compute_geometry derives rect and base size from osd-dimensions margins", "[drag-zoom-box]") {
    // 窗口 1000x800，视频左右各留白 100（当前渲染矩形 800x800），
    // 原始显示尺寸 1600x900（16:9）。
    auto geometry = compute_geometry(1000.0, 800.0, 100.0, 100.0, 0.0, 0.0, 1600.0, 900.0);
    REQUIRE(geometry.has_value());

    CHECK(geometry->osd_w == 1000.0);
    CHECK(geometry->osd_h == 800.0);
    CHECK(geometry->rect_x == 100.0);
    CHECK(geometry->rect_y == 0.0);
    CHECK(geometry->scaled_w == 800.0);
    CHECK(geometry->scaled_h == 800.0);
    // base_scale = min(1000/1600, 800/900) = 0.625
    CHECK(geometry->base_w == Catch::Approx(1000.0));
    CHECK(geometry->base_h == Catch::Approx(562.5));
}

TEST_CASE("compute_zoom matches hand-computed values for a quarter selection", "[drag-zoom-box]") {
    // 视频贴合整个窗口（zoom=0 起点），框选左上四分之一。
    Geometry geometry = *compute_geometry(1000.0, 1000.0, 0.0, 0.0, 0.0, 0.0, 1000.0, 1000.0);
    Box box{0.0, 0.0, 500.0, 500.0};

    auto result = compute_zoom(geometry, box);
    REQUIRE(result.has_value());
    CHECK(result->zoom == Catch::Approx(1.0));   // log2(2)
    CHECK(result->pan_x == Catch::Approx(0.25));
    CHECK(result->pan_y == Catch::Approx(0.25));
}

TEST_CASE("compute_zoom clamps a box that spills outside the visible video rect", "[drag-zoom-box]") {
    Geometry geometry = *compute_geometry(1000.0, 1000.0, 100.0, 100.0, 0.0, 0.0, 800.0, 1000.0);
    // box 左边界越过视频渲染矩形左边，右边界正好落在矩形右边，等价于
    // "选中整个可见视频"。
    Box box{-50.0, 0.0, 900.0, 1000.0};

    auto result = compute_zoom(geometry, box);
    REQUIRE(result.has_value());
    CHECK(result->zoom == Catch::Approx(0.0));
    CHECK(result->pan_x == Catch::Approx(0.0));
    CHECK(result->pan_y == Catch::Approx(0.0));
}

TEST_CASE("compute_zoom rejects a box collapsed to zero width or height", "[drag-zoom-box]") {
    Geometry geometry = *compute_geometry(1000.0, 1000.0, 0.0, 0.0, 0.0, 0.0, 1000.0, 1000.0);
    Box collapsed{100.0, 100.0, 100.0, 500.0};
    CHECK_FALSE(compute_zoom(geometry, collapsed).has_value());
}

TEST_CASE("compute_zoom rejects a box entirely outside the visible video rect", "[drag-zoom-box]") {
    // 视频只占窗口左半边（scaled_w=500），框选完全落在右半边空白区域。
    Geometry geometry = *compute_geometry(1000.0, 1000.0, 0.0, 500.0, 0.0, 0.0, 500.0, 1000.0);
    Box box{600.0, 0.0, 700.0, 1000.0};
    CHECK_FALSE(compute_zoom(geometry, box).has_value());
}
