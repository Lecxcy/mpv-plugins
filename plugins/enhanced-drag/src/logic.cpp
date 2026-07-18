#include "enhanced_drag/logic.h"

namespace enhanced_drag {

PanResult compute_drag_pan(const Geometry &geometry, const DragOrigin &origin, double mouse_x, double mouse_y) {
    double video_w = geometry.window_w - geometry.margin_left - geometry.margin_right;
    double video_h = geometry.window_h - geometry.margin_top - geometry.margin_bottom;

    PanResult result{origin.pan_x, origin.pan_y};

    if (video_w > 0.0) {
        result.pan_x = origin.pan_x + (mouse_x - origin.mouse_x) / video_w;
    }

    if (video_h > 0.0) {
        result.pan_y = origin.pan_y + (mouse_y - origin.mouse_y) / video_h;
    }

    return result;
}

} // namespace enhanced_drag
