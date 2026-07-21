#include "drag_zoom_box/logic.h"

#include <algorithm>
#include <cmath>

namespace drag_zoom_box {

namespace {

double clamp01(double value) {
    return std::clamp(value, 0.0, 1.0);
}

constexpr double kLog2Base = 0.6931471805599453; // ln(2)

double log2_value(double value) {
    return std::log(value) / kLog2Base;
}

} // namespace

Box normalize_box(Point start, Point current) {
    Box box;
    box.x1 = std::min(start.x, current.x);
    box.y1 = std::min(start.y, current.y);
    box.x2 = std::max(start.x, current.x);
    box.y2 = std::max(start.y, current.y);
    return box;
}

DragDirection classify_direction(double dx, double dy, double min_drag_pixels) {
    if (dx >= min_drag_pixels && dy >= min_drag_pixels) {
        return DragDirection::kZoomIn;
    }
    if (dx <= -min_drag_pixels && dy <= -min_drag_pixels) {
        return DragDirection::kReset;
    }
    return DragDirection::kNone;
}

std::optional<Geometry> compute_geometry(double osd_w, double osd_h, double margin_left, double margin_right,
                                          double margin_top, double margin_bottom, double dw, double dh) {
    if (osd_w <= 0.0 || osd_h <= 0.0 || dw <= 0.0 || dh <= 0.0) {
        return std::nullopt;
    }

    double scaled_w = osd_w - margin_left - margin_right;
    double scaled_h = osd_h - margin_top - margin_bottom;
    if (scaled_w <= 0.0 || scaled_h <= 0.0) {
        return std::nullopt;
    }

    double base_scale = std::min(osd_w / dw, osd_h / dh);

    Geometry geometry;
    geometry.osd_w = osd_w;
    geometry.osd_h = osd_h;
    geometry.base_w = dw * base_scale;
    geometry.base_h = dh * base_scale;
    geometry.rect_x = margin_left;
    geometry.rect_y = margin_top;
    geometry.scaled_w = scaled_w;
    geometry.scaled_h = scaled_h;
    return geometry;
}

std::optional<ZoomResult> compute_zoom(const Geometry &geometry, const Box &box) {
    double u1 = clamp01((box.x1 - geometry.rect_x) / geometry.scaled_w);
    double v1 = clamp01((box.y1 - geometry.rect_y) / geometry.scaled_h);
    double u2 = clamp01((box.x2 - geometry.rect_x) / geometry.scaled_w);
    double v2 = clamp01((box.y2 - geometry.rect_y) / geometry.scaled_h);

    double du = u2 - u1;
    double dv = v2 - v1;
    if (du <= 0.0 || dv <= 0.0) {
        return std::nullopt;
    }

    double scale_x = geometry.osd_w / (du * geometry.base_w);
    double scale_y = geometry.osd_h / (dv * geometry.base_h);
    double scale = std::min(scale_x, scale_y);
    if (!(scale > 0.0)) {
        return std::nullopt;
    }

    ZoomResult result;
    result.zoom = log2_value(scale);
    result.pan_x = 0.5 - (u1 + u2) / 2.0;
    result.pan_y = 0.5 - (v1 + v2) / 2.0;
    result.u1 = u1;
    result.v1 = v1;
    result.u2 = u2;
    result.v2 = v2;
    result.scale_x = scale_x;
    result.scale_y = scale_y;
    return result;
}

} // namespace drag_zoom_box
