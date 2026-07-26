#include "split_zoom_box/geometry.h"

#include <algorithm>
#include <cmath>

namespace split_zoom_box {

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

bool region_valid(const Region &region) {
    return region.x1 >= -kRegionEpsilon && region.y1 >= -kRegionEpsilon && region.x2 <= 1.0 + kRegionEpsilon &&
           region.y2 <= 1.0 + kRegionEpsilon && region.width() > kRegionEpsilon &&
           region.height() > kRegionEpsilon;
}

bool view_valid(const Region &view) {
    if (!(view.width() > kRegionEpsilon) || !(view.height() > kRegionEpsilon)) {
        return false;
    }
    if (view.width() > kMaxViewExtent || view.height() > kMaxViewExtent) {
        return false;
    }
    // 位置也要防呆：拖得再远也不该离画面几十倍远。
    return std::abs(view.x1) <= kMaxViewExtent && std::abs(view.y1) <= kMaxViewExtent &&
           std::abs(view.x2) <= kMaxViewExtent && std::abs(view.y2) <= kMaxViewExtent;
}

bool region_is_full(const Region &region) {
    return region.x1 <= kRegionEpsilon && region.y1 <= kRegionEpsilon && region.x2 >= 1.0 - kRegionEpsilon &&
           region.y2 >= 1.0 - kRegionEpsilon;
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

std::optional<Region> box_to_region(const Geometry &geometry, const Box &box) {
    if (geometry.scaled_w <= 0.0 || geometry.scaled_h <= 0.0) {
        return std::nullopt;
    }

    Region region;
    region.x1 = clamp01((box.x1 - geometry.rect_x) / geometry.scaled_w);
    region.y1 = clamp01((box.y1 - geometry.rect_y) / geometry.scaled_h);
    region.x2 = clamp01((box.x2 - geometry.rect_x) / geometry.scaled_w);
    region.y2 = clamp01((box.y2 - geometry.rect_y) / geometry.scaled_h);

    if (region.width() <= 0.0 || region.height() <= 0.0) {
        return std::nullopt;
    }
    return region;
}

std::optional<ZoomPan> region_to_zoom_pan(const Region &region, double osd_w, double osd_h, double base_w,
                                           double base_h) {
    if (osd_w <= 0.0 || osd_h <= 0.0 || base_w <= 0.0 || base_h <= 0.0) {
        return std::nullopt;
    }
    double du = region.width();
    double dv = region.height();
    if (du <= 0.0 || dv <= 0.0) {
        return std::nullopt;
    }

    double scale_x = osd_w / (du * base_w);
    double scale_y = osd_h / (dv * base_h);
    double scale = std::min(scale_x, scale_y);
    if (!(scale > 0.0)) {
        return std::nullopt;
    }

    ZoomPan result;
    result.zoom = log2_value(scale);
    result.pan_x = 0.5 - (region.x1 + region.x2) / 2.0;
    result.pan_y = 0.5 - (region.y1 + region.y2) / 2.0;
    return result;
}

} // namespace split_zoom_box
