#pragma once

#include <optional>

// 不依赖 mpv 的纯几何/状态判定逻辑，供 src/plugin.cpp 调用，也是
// tests/test_drag_zoom_box.cpp 的测试对象。

namespace drag_zoom_box {

struct Point {
    double x = 0.0;
    double y = 0.0;
};

// 框选矩形，始终满足 x1<=x2、y1<=y2（normalize_box 保证）。
struct Box {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 0.0;
    double y2 = 0.0;
};

Box normalize_box(Point start, Point current);

enum class DragDirection {
    kNone,   // 不满足任何一个合法对角线方向，不触发任何动作
    kZoomIn, // 左上 -> 右下
    kReset,  // 右下 -> 左上
};

// 与上游 Lua 版本的行为差异：上游只看水平方向（current.x >= start.x 就算
// 放大，否则恢复），四个对角线里有两个会被归到放大、另两个归到恢复，纯
// 水平/垂直拖拽也会被归类。这里改成必须同时在两个轴上分别达到
// min_drag_pixels 阈值、且符号一致，才判定为合法对角线；纯水平/垂直拖拽，
// 以及右上到左下、左下到右上这两个"反对角线"方向，一律判为 kNone，不触发
// 任何缩放/恢复动作。
DragDirection classify_direction(double dx, double dy, double min_drag_pixels);

// 视频当前实际渲染到窗口里的矩形区域 + 缩放为 0（完整贴合窗口）时的基准
// 尺寸，用来把鼠标像素坐标换算成视频内部的归一化 u/v 坐标，以及把框选范围
// 换算回新的 video-zoom 值。
struct Geometry {
    double osd_w = 0.0;
    double osd_h = 0.0;
    double base_w = 0.0;    // video-zoom=0 时的贴合窗口尺寸
    double base_h = 0.0;
    double rect_x = 0.0;    // 当前渲染矩形左上角（osd-dimensions 的 ml/mt）
    double rect_y = 0.0;
    double scaled_w = 0.0;  // 当前渲染矩形尺寸（osd-dimensions 推出的 w-ml-mr 等）
    double scaled_h = 0.0;
};

// osd_w/osd_h：osd-dimensions 的 w/h（窗口尺寸）。
// margin_left/right/top/bottom：osd-dimensions 的 ml/mr/mt/mb，mpv 已经把
// 当前 zoom/pan/rotate 都算在内的"视频渲染进窗口的矩形"，不需要插件自己用
// video-zoom/video-pan-x/y 重新推一遍（这一点与 enhanced-drag 改用
// osd-dimensions 而不是手动重算的思路一致）。
// dw/dh：video-target-params 或 video-out-params 的 dw/dh，视频贴合显示后
// 的原始宽高，只用来算 base_w/base_h 这个"缩放为 0"的基准。
std::optional<Geometry> compute_geometry(double osd_w, double osd_h, double margin_left, double margin_right,
                                          double margin_top, double margin_bottom, double dw, double dh);

struct ZoomResult {
    double zoom = 0.0;
    double pan_x = 0.0;
    double pan_y = 0.0;
    // 排查偏移问题用的中间量（框选换算到视频内部的归一化坐标、两个轴各自
    // 要求的缩放倍率），核心结果只看上面三个字段，这些不参与判断，只给
    // src/plugin.cpp 的调试日志用。
    double u1 = 0.0, v1 = 0.0, u2 = 0.0, v2 = 0.0;
    double scale_x = 0.0, scale_y = 0.0;
};

// box 是屏幕像素坐标系下的框选矩形；返回 nullopt 表示框选范围换算到视频
// 内部后退化成了零宽或零高（例如整个框都落在视频渲染矩形之外）。
std::optional<ZoomResult> compute_zoom(const Geometry &geometry, const Box &box);

} // namespace drag_zoom_box
