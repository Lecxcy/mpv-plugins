#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "enhanced_drag/logic.h"

using enhanced_drag::compute_drag_pan;
using enhanced_drag::DragOrigin;
using enhanced_drag::Geometry;

TEST_CASE("compute_drag_pan follows the mouse when zoomed in", "[enhanced-drag]") {
    Geometry geometry;
    geometry.window_w = 1000.0;
    geometry.window_h = 1000.0;
    geometry.margin_left = -500.0;
    geometry.margin_right = -500.0;
    geometry.margin_top = 0.0;
    geometry.margin_bottom = 0.0;

    DragOrigin origin{100.0, 100.0, 0.0, 0.0};

    auto result = compute_drag_pan(geometry, origin, /*mouse_x=*/150.0, /*mouse_y=*/200.0);

    CHECK(result.pan_x == Catch::Approx(0.025));
    CHECK(result.pan_y == Catch::Approx(0.1));
}

TEST_CASE("compute_drag_pan also follows the mouse at zoom 0 (fully visible video)", "[enhanced-drag]") {
    // 用户明确要求：画面未缩放（video-zoom 为 0，视频完整可见）时也应该能拖拽，
    // 不应该被锁死。
    Geometry geometry;
    geometry.window_w = 1000.0;
    geometry.window_h = 1000.0;
    geometry.margin_left = 0.0;
    geometry.margin_right = 0.0;
    geometry.margin_top = 0.0;
    geometry.margin_bottom = 0.0;

    DragOrigin origin{100.0, 100.0, 0.0, 0.0};

    auto result = compute_drag_pan(geometry, origin, /*mouse_x=*/150.0, /*mouse_y=*/200.0);

    CHECK(result.pan_x == Catch::Approx(0.05));
    CHECK(result.pan_y == Catch::Approx(0.1));
}

TEST_CASE("compute_drag_pan is not bounded and allows dragging past the video edge", "[enhanced-drag]") {
    // 用户明确要求取消从 occivink/mpv-image-viewer 移植过来的 margin 边界
    // 限制：拖到边界后应该能继续往外拖，不应该被卡住。这里用一个远超过
    // 视频尺寸的位移，确认 pan 值会跟着线性增长、不会被夹在任何范围内。
    Geometry geometry;
    geometry.window_w = 1000.0;
    geometry.window_h = 1000.0;
    geometry.margin_left = -500.0;
    geometry.margin_right = -500.0;
    geometry.margin_top = -500.0;
    geometry.margin_bottom = -500.0;

    DragOrigin origin{100.0, 100.0, 0.0, 0.0};

    auto result = compute_drag_pan(geometry, origin, /*mouse_x=*/10100.0, /*mouse_y=*/100.0);

    // video_w = window_w - margin_left - margin_right = 2000
    CHECK(result.pan_x == Catch::Approx(5.0));
    CHECK(result.pan_y == 0.0);
}

TEST_CASE("compute_drag_pan keeps the origin pan when the video has no usable extent", "[enhanced-drag]") {
    Geometry geometry;
    geometry.window_w = 1000.0;
    geometry.window_h = 1000.0;
    geometry.margin_left = 600.0;
    geometry.margin_right = 600.0;
    geometry.margin_top = 600.0;
    geometry.margin_bottom = 600.0;

    DragOrigin origin{100.0, 100.0, 0.2, 0.3};

    auto result = compute_drag_pan(geometry, origin, /*mouse_x=*/900.0, /*mouse_y=*/900.0);

    CHECK(result.pan_x == 0.2);
    CHECK(result.pan_y == 0.3);
}
