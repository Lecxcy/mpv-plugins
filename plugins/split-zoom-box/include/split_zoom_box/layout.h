#pragma once

#include <optional>
#include <string>
#include <vector>

#include "split_zoom_box/geometry.h"

// 分屏布局：tmux 式的递归二分树。任意网格（含 2x2）都由"把某个窗格再一分为
// 二"递归产生，而不是行列网格——这样既能表达 2x2，也能表达不对称布局，且与
// 嵌套 hstack/vstack 一一对应。

namespace split_zoom_box {

enum class SplitDir {
    kHorizontal, // 左右并排 -> hstack
    kVertical,   // 上下并排 -> vstack
};

// arena 索引存储，nodes[0] 恒为根。叶子 = 一个窗格，内部节点 = 一次分屏。
struct Node {
    bool leaf = true;
    Region region;                        // 仅叶子有意义
    SplitDir dir = SplitDir::kHorizontal; // 仅内部节点有意义
    int first = -1;
    int second = -1;
};

struct Layout {
    std::vector<Node> nodes;
    int focused = 0; // 聚焦的叶子索引
};

Layout make_layout(Region region = Region{});

int leaf_count(const Layout &layout);

// 按"左 -> 右、上 -> 下"的稳定顺序返回所有叶子的节点索引。窗格编号、
// compute_pane_rects 的返回顺序、滤镜图里的 pN 标签都以它为准。
std::vector<int> leaf_order(const Layout &layout);

// 把聚焦窗格一分为二。两个子窗格都继承原窗格的区域（tmux 语义：分屏后看到的
// 是同样的内容），焦点留在第一个子窗格。聚焦的不是叶子时返回 false。
bool split_focused(Layout &layout, SplitDir dir);

// 关闭聚焦窗格，兄弟节点顶替父节点。只剩一个窗格时不做任何事并返回 false，
// 由调用方决定这种情况是不是"整体退出分屏"。
bool close_focused(Layout &layout);

void focus_next(Layout &layout);

bool set_focused_region(Layout &layout, const Region &region);

struct PixelRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// 每个叶子在画布上的整数像素矩形，顺序与 leaf_order 一致。
//
// 硬性不变量：同一个内部节点下，两个子节点在堆叠轴上的尺寸必须精确求和等于
// 父节点。hstack 要求所有输入高度完全相同、vstack 要求宽度完全相同，差 1 个
// 像素都会让整张图配置失败（实测 360 vs 300 直接 Failed to configure output
// pad）。这里靠"第二个子节点取减法余数"而不是两边各自取整来保证。
std::vector<PixelRect> compute_pane_rects(const Layout &layout, int canvas_w, int canvas_h);

// 生成 1 进 1 出的 lavfi 图内容（不含外层的 "@label:lavfi=[...]" 包装）。
//
// 每个窗格固定发 crop -> scale -> setsar=1。setsar 不能省：源的 SAR 会透过
// scale 传下去，实测会把 dwidth 从 640 改成 720（画面被横向拉伸）。
//
// hardware_frames 表示解码器当前吐的是不是硬件帧。实测的真值表是：
//
//     帧类型      无前缀    hwdownload,format=...
//     硬件帧      失败      成功
//     软件帧      成功      失败
//
// 两种情况必须用不同的图，没有一份能同时兼容——所以调用方必须在生成时读一次
// hwdec-current 来决定。刻意**不**通过切 hwdec 属性来统一成软件帧：反复切
// hwdec 会把 VideoToolbox 解码器会话打坏（实测出现 -12909
// "output image buffer is null" 连环解码失败），详见 SPEC §6.7。
//
// 单窗格布局不该走这里（调用方应改用 video-zoom 路径），传进来会返回空串。
std::string build_filter_graph(const Layout &layout, int src_w, int src_h, int canvas_w, int canvas_h,
                                bool hardware_frames = false);

struct PaneHit {
    int leaf = -1;  // 命中的叶子节点索引
    double u = 0.0; // 窗格内归一化坐标
    double v = 0.0;
};

// cu/cv 是画布归一化坐标。分屏生效后屏幕上显示的是拼接画布而不是源画面，
// 所以"在窗格里框选"必须先经过这一步定位到具体窗格，再经 subregion 换算回
// 源坐标；少了这层反查，第二次调整某个窗格就会取到错的区域。
std::optional<PaneHit> hit_test(const Layout &layout, int canvas_w, int canvas_h, double cu, double cv);

// 窗格内的归一化子矩形 -> 源画面区域。
Region subregion(const Region &pane, double u1, double v1, double u2, double v2);

// ---- 时间段 ----

// 一段时间内生效的分屏布局。区间之间互不重叠（由 overlapping_segment 在插入
// 时把关）。
//
// 刻意**不**做 enabled 标志：enhanced-ab-loop 需要"临时禁用某段但不删掉"是
// 因为循环是全程高频使用的；分屏没有这个使用模式，加了只是多一份要维护、
// 要持久化、要在 UI 上体现的状态。要停用就直接删。
struct LayoutSegment {
    double a = 0.0;
    double b = 0.0;
    Layout layout;
};

// 按起点升序排序。这里刻意用精确比较、不做 epsilon tie-break：
// enhanced-ab-loop 早期版本在比较函数里加容差，破坏了严格弱序，是已确认的
// bug（见该插件 SPEC）。容差只允许用在"播放位置 vs 存下来的边界"这种比较上，
// 不能用在"存下来的值互相比较"上。
void sort_segments(std::vector<LayoutSegment> &segments);

// 落在闭区间 [a,b] 内的第一个区间。
std::optional<std::size_t> find_segment_at(const std::vector<LayoutSegment> &segments, double pos);

// 与已有区间重叠则返回那个区间的下标。冲突直接拒绝插入，不做隐式的边界借用
// 或吞并——同一时刻两个布局都想生效是没有意义的。
std::optional<std::size_t> overlapping_segment(const std::vector<LayoutSegment> &segments, double a, double b,
                                                std::optional<std::size_t> ignore_index = std::nullopt);

} // namespace split_zoom_box
