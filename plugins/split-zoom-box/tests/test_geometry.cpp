#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "split_zoom_box/geometry.h"

using namespace split_zoom_box;

TEST_CASE("只认两个对角线方向", "[geometry][direction]") {
    constexpr double kMin = 4.0;
    REQUIRE(classify_direction(10.0, 10.0, kMin) == DragDirection::kZoomIn);
    REQUIRE(classify_direction(-10.0, -10.0, kMin) == DragDirection::kReset);
    // 两个反对角线
    REQUIRE(classify_direction(10.0, -10.0, kMin) == DragDirection::kNone);
    REQUIRE(classify_direction(-10.0, 10.0, kMin) == DragDirection::kNone);
    // 纯水平 / 纯垂直 / 原地
    REQUIRE(classify_direction(10.0, 0.0, kMin) == DragDirection::kNone);
    REQUIRE(classify_direction(0.0, 10.0, kMin) == DragDirection::kNone);
    REQUIRE(classify_direction(0.0, 0.0, kMin) == DragDirection::kNone);
}

TEST_CASE("最小拖拽距离对两个分支都生效", "[geometry][direction]") {
    constexpr double kMin = 4.0;
    // 恰好达到阈值算数，差一点不算——恢复分支也受保护，避免手抖清零缩放
    REQUIRE(classify_direction(4.0, 4.0, kMin) == DragDirection::kZoomIn);
    REQUIRE(classify_direction(3.9, 4.0, kMin) == DragDirection::kNone);
    REQUIRE(classify_direction(-4.0, -4.0, kMin) == DragDirection::kReset);
    REQUIRE(classify_direction(-3.9, -4.0, kMin) == DragDirection::kNone);
}

TEST_CASE("normalize_box 与拖拽方向无关", "[geometry]") {
    Box a = normalize_box(Point{10.0, 20.0}, Point{30.0, 40.0});
    Box b = normalize_box(Point{30.0, 40.0}, Point{10.0, 20.0});
    REQUIRE(a.x1 == Catch::Approx(b.x1));
    REQUIRE(a.y1 == Catch::Approx(b.y1));
    REQUIRE(a.x2 == Catch::Approx(b.x2));
    REQUIRE(a.y2 == Catch::Approx(b.y2));
    REQUIRE(a.x1 <= a.x2);
    REQUIRE(a.y1 <= a.y2);
}

TEST_CASE("compute_geometry 拒绝非法/退化输入", "[geometry]") {
    REQUIRE_FALSE(compute_geometry(0.0, 720.0, 0, 0, 0, 0, 1280, 720).has_value());
    REQUIRE_FALSE(compute_geometry(1280.0, 720.0, 0, 0, 0, 0, 0, 720).has_value());
    // margins 把渲染矩形吃成零宽
    REQUIRE_FALSE(compute_geometry(1280.0, 720.0, 640, 640, 0, 0, 1280, 720).has_value());
}

TEST_CASE("框选 1/4 区域换算成 2 倍缩放", "[geometry][zoom]") {
    // 1000x1000 视频完整贴合 1000x1000 窗口
    auto geometry = compute_geometry(1000.0, 1000.0, 0, 0, 0, 0, 1000, 1000);
    REQUIRE(geometry.has_value());

    auto region = box_to_region(*geometry, Box{0.0, 0.0, 500.0, 500.0});
    REQUIRE(region.has_value());
    REQUIRE(region->x1 == Catch::Approx(0.0));
    REQUIRE(region->x2 == Catch::Approx(0.5));

    auto zoom_pan = region_to_zoom_pan(*region, geometry->osd_w, geometry->osd_h, geometry->base_w,
                                        geometry->base_h);
    REQUIRE(zoom_pan.has_value());
    REQUIRE(zoom_pan->zoom == Catch::Approx(1.0)); // log2(2)
    REQUIRE(zoom_pan->pan_x == Catch::Approx(0.25));
    REQUIRE(zoom_pan->pan_y == Catch::Approx(0.25));
}

TEST_CASE("框选越出渲染矩形时裁剪到完整画面", "[geometry][zoom]") {
    auto geometry = compute_geometry(1000.0, 1000.0, 0, 0, 0, 0, 1000, 1000);
    REQUIRE(geometry.has_value());

    // 框比视频还大，裁剪后就是完整画面 -> 不缩放、不平移
    auto region = box_to_region(*geometry, Box{-500.0, -500.0, 1500.0, 1500.0});
    REQUIRE(region.has_value());
    REQUIRE(region_is_full(*region));

    auto zoom_pan = region_to_zoom_pan(*region, geometry->osd_w, geometry->osd_h, geometry->base_w,
                                        geometry->base_h);
    REQUIRE(zoom_pan.has_value());
    REQUIRE(zoom_pan->zoom == Catch::Approx(0.0));
    REQUIRE(zoom_pan->pan_x == Catch::Approx(0.0));
    REQUIRE(zoom_pan->pan_y == Catch::Approx(0.0));
}

TEST_CASE("整个框落在渲染矩形之外时拒绝", "[geometry][zoom]") {
    auto geometry = compute_geometry(1000.0, 1000.0, 0, 0, 0, 0, 1000, 1000);
    REQUIRE(geometry.has_value());
    // 完全在左侧之外，裁剪后退化成零宽
    REQUIRE_FALSE(box_to_region(*geometry, Box{-500.0, 100.0, -100.0, 400.0}).has_value());
}

TEST_CASE("已缩放状态下的框选直接得到源坐标", "[geometry][zoom]") {
    // 视频被放大 2 倍：渲染矩形是窗口的两倍大，左上角在窗口外
    // margins 已含当前缩放，所以换算出来的就是相对完整画面的坐标，
    // 不需要再和旧的缩放状态做一次复合
    auto geometry = compute_geometry(1000.0, 1000.0, -500.0, -500.0, -500.0, -500.0, 1000, 1000);
    REQUIRE(geometry.has_value());
    REQUIRE(geometry->scaled_w == Catch::Approx(2000.0));

    // 窗口正中间那一小块
    auto region = box_to_region(*geometry, Box{500.0, 500.0, 1000.0, 1000.0});
    REQUIRE(region.has_value());
    REQUIRE(region->x1 == Catch::Approx(0.5));
    REQUIRE(region->x2 == Catch::Approx(0.75));
}

TEST_CASE("region_valid 与 region_is_full 的边界", "[geometry]") {
    REQUIRE(region_valid(Region{0.0, 0.0, 1.0, 1.0}));
    REQUIRE_FALSE(region_valid(Region{0.5, 0.0, 0.5, 1.0}));
    REQUIRE_FALSE(region_valid(Region{0.0, 0.0, 1.5, 1.0}));
    REQUIRE(region_is_full(Region{0.0, 0.0, 1.0, 1.0}));
    REQUIRE_FALSE(region_is_full(Region{0.1, 0.0, 1.0, 1.0}));
}
