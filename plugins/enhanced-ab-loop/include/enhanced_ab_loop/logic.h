#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// 纯逻辑层：不依赖 mpv client API，只处理 segment/pending 状态机。SPEC.md
// 记录了这里每一条规则的讨论过程，改动前先对照 SPEC 对应章节。
namespace enhanced_ab_loop {

// SPEC §1.1：完整区间。互不重叠、按 a 升序排列由调用方（build_active_order
// 以及每个操作函数）维持，这里只是纯数据。
struct Segment {
    double a = 0.0;
    double b = 0.0;
    bool enabled = true;
};

// SPEC §1.2：待定端点，同一时刻最多一个字段有值，不是独立的“编辑模式”标志位。
struct Pending {
    bool has_a = false;
    bool has_b = false;
    double a = 0.0;
    double b = 0.0;

    bool empty() const { return !has_a && !has_b; }
    void clear() {
        has_a = false;
        has_b = false;
    }
};

// SPEC §1.1：只用于“实时播放位置 vs 已存储边界”的 UX 容差（Move A/B、
// toggle-segment 判定光标是否落在区间内、以及尾帧冻结里 b 与 duration 的
// 比较）。存储值之间（segment 与 segment）的比较一律用精确相等/大小比较，
// 不经过这个常量。
inline constexpr double kCursorEpsilon = 0.005;

// 一份完整、独立的可持久化状态拷贝：segments + pending。“当前正在编辑的
// 状态”和每一个模板槽位在数据形状上完全一样——模板不是对当前 segments 的
// 引用或一份“启用掩码”，而是各自独立的一份拷贝。这样最简单的用法（同一批
// segments、只是 enabled 不同的几个模板）和更进阶的用法（每个模板存完全
// 不同的 segments）用的是同一套机制，不需要额外设计。
struct Snapshot {
    std::vector<Segment> segments;
    Pending pending;
};

// SPEC §2.1：active 队列里的一项。b 为空表示“开放到文件真实末尾”，原生
// ab-loop 机制表达不了（要求 a、b 都是有效值），需要靠 eof_fallback_target
// 兜底，不会写入 ab-loop-b。
struct ActiveEntry {
    double a = 0.0;
    std::optional<double> b;
};

enum class OpStatus { kOk, kDenied };

struct OpOutcome {
    OpStatus status = OpStatus::kDenied;
    std::string message;

    bool ok() const { return status == OpStatus::kOk; }
};

// 按 a 升序排序。比较函数只用 `<`，不做 epsilon tie-break——旧实现在这里
// 因为 epsilon tie-break 不满足严格弱序，table.sort 不保证正确排序，是已
// 确认的 bug。
void sort_segments(std::vector<Segment> &segments);

// 光标严格落在区间内部（Move A/B、extend、toggle-segment 用来判断“这是在
// 调整已有区间”还是“这是在空隙里操作”）。
bool is_strictly_inside(double pos, const Segment &seg);

// 光标落在闭区间 [a, b] 内（nudge 专用，不要求距边界超过 epsilon——SPEC
// §3.3：循环刚跳回边界附近就是最想 nudge 的时机，不能被这条判定拒绝）。
bool is_within_closed_range(double pos, const Segment &seg);

std::optional<std::size_t> find_complete_index_at(const std::vector<Segment> &segments, double pos);

// 假设 segments 已按 a 升序、互不重叠：最后一个 b <= point 的下标（“point
// 前面最近的一段”）。
std::optional<std::size_t> find_prev_index(const std::vector<Segment> &segments, double point);

// 第一个 a >= point 的下标（“point 后面最近的一段”）。
std::optional<std::size_t> find_next_index(const std::vector<Segment> &segments, double point);

// [a, b) 是否与 segments 中任意一段（ignore_index 除外）重叠。
std::optional<std::size_t> overlaps_complete(const std::vector<Segment> &segments, double a, double b,
                                              std::optional<std::size_t> ignore_index = std::nullopt);

// SPEC §3.1：光标落在已有完整区间内部且 pending 为空 -> Move；否则 -> 插入
// 新区间（永远不做隐式的边界借用/吞并，冲突直接拒绝）。
OpOutcome set_a(std::vector<Segment> &segments, Pending &pending, double pos);
OpOutcome set_b(std::vector<Segment> &segments, Pending &pending, double pos);

// SPEC §3.2：extend，独立键位，不复用 set_a/set_b。
OpOutcome extend_prev(std::vector<Segment> &segments, const Pending &pending, double pos);
OpOutcome extend_next(std::vector<Segment> &segments, const Pending &pending, double pos);

// SPEC §3.3：nudge，触发条件比 Move 宽松，顶到边界拒绝而非静默 clamp。
OpOutcome nudge_a(std::vector<Segment> &segments, double pos, double delta);
OpOutcome nudge_b(std::vector<Segment> &segments, double pos, double delta);

// SPEC §3.4：撤销端点。
OpOutcome unset_a(std::vector<Segment> &segments, Pending &pending, double pos);
OpOutcome unset_b(std::vector<Segment> &segments, Pending &pending, double pos);

// SPEC §5：选段循环。
OpOutcome toggle_segment(std::vector<Segment> &segments, double pos);

// SPEC §2.1 / §2.3：active 队列 = enabled 的完整区间 + pending 借来的临时项
// （近端边界，见 SPEC §2.3）。
std::vector<ActiveEntry> build_active_order(const std::vector<Segment> &segments, const Pending &pending);

// active_order 结构发生变化后，重新定位“当前应该在哪一项”：pos 落在某一项
// 范围内就返回那一项；不在任何一项内就返回下一项（空隙里，含全部之前）；
// 全部都在 pos 之前（含恰好落在最后一项自己的 b 上）就返回最后一项自己
// （不是 wrap 回第一项——原因见 logic.cpp 里这个函数的实现注释）。order 为
// 空返回 nullopt。
std::optional<std::size_t> locate_active_index(const std::vector<ActiveEntry> &order, double pos);

// pos 是否严格落在这一项自己的 [a, b) 内（b 为空——开放到文件末尾——时只要
// pos >= a 就算落在里面）。locate_active_index 的第一轮判定用的就是这个，
// 单独拆出来给 loop_reengage_target 复用。
bool is_within_active_entry(const ActiveEntry &entry, double pos);

// 重新打开循环开关（loop_enabled: false -> true）时用：如果当前光标已经
// 落在某一项内部，返回 nullopt（不需要额外动作，接着播就会正常触发原生
// 循环）。如果没有落在任何一项内部——不管是空隙里还是已经越过了全部
// 区间——返回“应该立刻 seek 过去”的目标点，让循环立刻生效，而不是指望
// “接着往下播、碰巧自然落回循环范围”这种运气：mpv 的
// update_ab_loop_clip()（playloop.c:665）只要发现当前 pos 已经超过目标
// 终点就会直接禁用 clip，继续播放只会一路播到文件真正结束（如果还配了
// loop-file=inf，效果就是从头重播，而不是回到我们定义的循环里）。
//   - pos 在某一项自己的 a 之前（空隙里，含全部区间最前面）：目标就是那
//     一项自己的 a。
//   - pos 已经越过了那一项自己的 a（意味着这一项整个已经被甩在身后，包括
//     “越过了全部区间”这种情况）：目标改成 active_order 里下一项
//     （wrap）自己的 a。
// order 为空返回 nullopt（没什么可 seek 的）。
std::optional<double> loop_reengage_target(const std::vector<ActiveEntry> &order, double pos);

// SPEC §2.2：eof-reached 兜底目标——active_order 里排在最后的那一项自己的
// a（不是硬编码第一项）。真正触发 eof-reached 的必然是这一项，因为更早的
// 项都有自己的 b 会提前用原生 pts 截断拦下。
std::optional<double> eof_fallback_target(const std::vector<ActiveEntry> &order);

// SPEC §2.1（已修正，见 logic.cpp compute_jump_pair 内的注释）+ SPEC §4
// 尾帧冻结：给定刚落到的 active_order 下标，算出应该写入 ab-loop-a /
// ab-loop-b 的值——恒为“这一项自己的” a/b（a<b 恒成立，不会被 mpv
// get_ab_loop_times() 的排序坑到）。跨段跳到下一项不是这个函数的职责，由
// plugin.cpp 监测到原生自环回到本项自己的 a 后显式补一次 seek 完成。
// current.b 为空（开放到文件末尾）时两者都不写，交给 eof_fallback_target
// 兜底。real_duration 是 mpv 报告的真实文件时长，tail_freeze_seconds <= 0
// 表示不启用尾帧冻结；current.b 落在真实时长附近时会被换成 “真实末尾 +
// tail_freeze_seconds”，让原生机制播入 tpad/apad 补出来的延长段后再触发
// 跳转。
struct JumpPair {
    std::optional<double> ab_loop_a;
    std::optional<double> ab_loop_b;
};

JumpPair compute_jump_pair(const std::vector<ActiveEntry> &order, std::size_t landed_index,
                            double real_duration, double tail_freeze_seconds);

// show-state 一次最多展示这么多行区间，超出的话首尾各留一半、中间用省略号
// 代替——几十个区间挨个列出来会直接把 OSD 撑到超出屏幕范围。
inline constexpr std::size_t kMaxVisibleSegments = 12;

// 给定区间总数，算出应该展示的头部数量、尾部数量、以及中间被折叠掉的数量。
// total <= kMaxVisibleSegments 时 hidden_count 恒为 0（不省略，全展示，
// head_count == total）；否则首尾各留一半（kMaxVisibleSegments 为奇数时
// 头比尾少一个），保证同时能看到最靠前和最靠后的区间，不是只留头或只留
// 尾。纯算术，不关心 Segment 具体内容，调用方按 [0, head_count) 和
// [total - tail_count, total) 两段下标去取实际的 Segment。
struct SegmentDisplayPlan {
    std::size_t head_count = 0;
    std::size_t hidden_count = 0;
    std::size_t tail_count = 0;
};

SegmentDisplayPlan plan_segment_display(std::size_t total, std::size_t max_visible = kMaxVisibleSegments);

} // namespace enhanced_ab_loop
