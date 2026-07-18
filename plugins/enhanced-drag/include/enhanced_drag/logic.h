#pragma once

namespace enhanced_drag {

struct Geometry {
    double window_w = 0.0;
    double window_h = 0.0;
    double margin_left = 0.0;
    double margin_right = 0.0;
    double margin_top = 0.0;
    double margin_bottom = 0.0;
};

struct DragOrigin {
    double mouse_x = 0.0;
    double mouse_y = 0.0;
    double pan_x = 0.0;
    double pan_y = 0.0;
};

struct PanResult {
    double pan_x = 0.0;
    double pan_y = 0.0;
};

PanResult compute_drag_pan(const Geometry &geometry, const DragOrigin &origin, double mouse_x, double mouse_y);

} // namespace enhanced_drag
