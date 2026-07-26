#pragma once

#include <optional>

// 不依赖 mpv 的纯几何逻辑。这一层整体承接自已删除的 drag-zoom-box 插件
// （方向判定、渲染矩形换算、缩放/平移计算），并新增了 Region 这个"归一化
// 源画面区域"表示——单窗格和多窗格两条渲染路径都以它为唯一状态，避免两套
// 机制各自维护一份坐标。

namespace split_zoom_box {

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

// 承接自 drag-zoom-box：必须同时在两个轴上分别达到 min_drag_pixels 阈值、
// 且符号一致，才判定为合法对角线。纯水平/垂直拖拽，以及右上到左下、左下到
// 右上这两个"反对角线"方向，一律判为 kNone。阈值同时保护两个分支，避免手抖
// 把缩放误清零。
DragDirection classify_direction(double dx, double dy, double min_drag_pixels);

// 归一化的源画面区域，坐标系是完整源画面的 [0,1]x[0,1]。
// 单窗格时换算成 video-zoom/video-pan-*，多窗格时换算成 lavfi crop 参数。
struct Region {
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 1.0;
    double y2 = 1.0;

    double width() const { return x2 - x1; }
    double height() const { return y2 - y1; }
};

inline constexpr double kRegionEpsilon = 1e-9;

bool region_valid(const Region &region);
bool region_is_full(const Region &region);

// 视口的合法性：只要求有正的宽高、且量级不离谱。**不要求落在 [0,1] 内**
// ——视口看的是"源画面 + 无限黑色背景"，超出画面的部分就是黑色，这是正常
// 状态而不是错误。上限是防呆：视口大到几十倍画面时滤镜要处理的中间帧会
// 大到没有意义。
bool view_valid(const Region &view);
inline constexpr double kMaxViewExtent = 32.0;

// 视频当前实际渲染到窗口里的矩形区域 + 缩放为 0（完整贴合窗口）时的基准
// 尺寸。多窗格生效时，这个"视频"指的是拼接后的画布。
struct Geometry {
    double osd_w = 0.0;
    double osd_h = 0.0;
    double base_w = 0.0;   // video-zoom=0 时的贴合窗口尺寸
    double base_h = 0.0;
    double rect_x = 0.0;   // 当前渲染矩形左上角（osd-dimensions 的 ml/mt）
    double rect_y = 0.0;
    double scaled_w = 0.0; // 当前渲染矩形尺寸（w-ml-mr / h-mt-mb）
    double scaled_h = 0.0;
};

// margins 是 mpv 已经把当前 zoom/pan/rotate 都算进去的结果，不需要插件自己
// 用 video-zoom/video-pan-* 重新推一遍。
std::optional<Geometry> compute_geometry(double osd_w, double osd_h, double margin_left, double margin_right,
                                          double margin_top, double margin_bottom, double dw, double dh);

// 屏幕像素框选 -> 归一化坐标。因为 margins 已含当前缩放，得到的就是相对
// "完整画面"（多窗格时是完整画布）的坐标，不需要再和旧状态做一次复合。
// 返回 nullopt 表示换算后退化成零宽/零高（例如整个框都落在渲染矩形之外）。
std::optional<Region> box_to_region(const Geometry &geometry, const Box &box);

struct ZoomPan {
    double zoom = 0.0;
    double pan_x = 0.0;
    double pan_y = 0.0;
};

// Region -> video-zoom/video-pan-*，单窗格路径专用。
// 公式沿用 drag-zoom-box 已验证的实现：video-zoom 是 log2 刻度，两个轴各自
// 要求的倍率取较小值以保证整个区域都在窗口内可见。
std::optional<ZoomPan> region_to_zoom_pan(const Region &region, double osd_w, double osd_h, double base_w,
                                           double base_h);

} // namespace split_zoom_box
