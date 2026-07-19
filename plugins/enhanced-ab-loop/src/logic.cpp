#include "enhanced_ab_loop/logic.h"

#include <algorithm>
#include <limits>

namespace enhanced_ab_loop {

namespace {

void insert_sorted(std::vector<ActiveEntry> &order, ActiveEntry entry) {
    auto it = std::upper_bound(order.begin(), order.end(), entry,
                                [](const ActiveEntry &lhs, const ActiveEntry &rhs) { return lhs.a < rhs.a; });
    order.insert(it, entry);
}

} // namespace

void sort_segments(std::vector<Segment> &segments) {
    std::sort(segments.begin(), segments.end(), [](const Segment &lhs, const Segment &rhs) { return lhs.a < rhs.a; });
}

bool is_strictly_inside(double pos, const Segment &seg) {
    return pos > seg.a + kCursorEpsilon && pos < seg.b - kCursorEpsilon;
}

bool is_within_closed_range(double pos, const Segment &seg) {
    return pos >= seg.a && pos <= seg.b;
}

std::optional<std::size_t> find_complete_index_at(const std::vector<Segment> &segments, double pos) {
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (is_strictly_inside(pos, segments[i])) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> find_prev_index(const std::vector<Segment> &segments, double point) {
    std::optional<std::size_t> idx;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].b <= point) {
            idx = i;
        } else {
            break;
        }
    }
    return idx;
}

std::optional<std::size_t> find_next_index(const std::vector<Segment> &segments, double point) {
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (segments[i].a >= point) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> overlaps_complete(const std::vector<Segment> &segments, double a, double b,
                                              std::optional<std::size_t> ignore_index) {
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (ignore_index && i == *ignore_index) {
            continue;
        }
        if (a < segments[i].b && b > segments[i].a) {
            return i;
        }
    }
    return std::nullopt;
}

namespace {

// set_a/set_b 里“两个端点都齐了，尝试配对成完整区间”这一步是共用的：校验
// a<b、校验不重叠，通过就插入 segments 并清空 pending。冲突时把 pending
// 清空（不留下半吊子状态）并说明原因。
OpOutcome try_pair_pending(std::vector<Segment> &segments, Pending &pending) {
    if (pending.a >= pending.b) {
        pending.clear();
        return {OpStatus::kDenied, "Invalid partial"};
    }
    if (overlaps_complete(segments, pending.a, pending.b)) {
        pending.clear();
        return {OpStatus::kDenied, "Overlap denied"};
    }
    segments.push_back(Segment{pending.a, pending.b, true});
    sort_segments(segments);
    pending.clear();
    return {OpStatus::kOk, "Add segment"};
}

} // namespace

OpOutcome set_a(std::vector<Segment> &segments, Pending &pending, double pos) {
    auto idx = find_complete_index_at(segments, pos);
    if (idx && pending.empty()) {
        // Move A：pos 严格落在 segments[idx] 内部（is_strictly_inside 保证
        // seg.a 原值 < pos < seg.b），而 seg.a 原值本来就 >= 前一段的 b（互不
        // 重叠不变式），所以新 a=pos 不可能撞到前一段，不需要额外校验。
        segments[*idx].a = pos;
        return {OpStatus::kOk, "Move A"};
    }
    if (idx) {
        return {OpStatus::kDenied, "A denied"};
    }

    if (pending.has_b && pos >= pending.b - kCursorEpsilon) {
        return {OpStatus::kDenied, "A >= B denied"};
    }

    pending.has_a = true;
    pending.a = pos;
    if (pending.has_a && pending.has_b) {
        return try_pair_pending(segments, pending);
    }
    return {OpStatus::kOk, "Set A"};
}

OpOutcome set_b(std::vector<Segment> &segments, Pending &pending, double pos) {
    auto idx = find_complete_index_at(segments, pos);
    if (idx && pending.empty()) {
        // Move B：同 Move A 的推理，pos < seg.b 原值 <= 下一段的 a，不会撞到
        // 下一段。
        segments[*idx].b = pos;
        return {OpStatus::kOk, "Move B"};
    }
    if (idx) {
        return {OpStatus::kDenied, "B denied"};
    }

    if (pending.has_a && pos <= pending.a + kCursorEpsilon) {
        return {OpStatus::kDenied, "B <= A denied"};
    }

    pending.has_b = true;
    pending.b = pos;
    if (pending.has_a && pending.has_b) {
        return try_pair_pending(segments, pending);
    }
    return {OpStatus::kOk, "Set B"};
}

OpOutcome extend_prev(std::vector<Segment> &segments, const Pending &pending, double pos) {
    if (!pending.empty()) {
        return {OpStatus::kDenied, "Extend denied: pending insert in progress"};
    }
    if (find_complete_index_at(segments, pos)) {
        return {OpStatus::kDenied, "Extend denied: inside a segment"};
    }
    auto prev_idx = find_prev_index(segments, pos);
    if (!prev_idx) {
        return {OpStatus::kDenied, "No previous segment to extend"};
    }
    // pos 已确认不落在任何 segment 内部，prev/next 是紧邻 pos 所在空隙两侧
    // 最近的段，中间不可能夹着第三个 segment，所以这里不需要额外的重叠校验
    // ——拉伸目标必然安全。
    segments[*prev_idx].b = pos;
    return {OpStatus::kOk, "Extend prev"};
}

OpOutcome extend_next(std::vector<Segment> &segments, const Pending &pending, double pos) {
    if (!pending.empty()) {
        return {OpStatus::kDenied, "Extend denied: pending insert in progress"};
    }
    if (find_complete_index_at(segments, pos)) {
        return {OpStatus::kDenied, "Extend denied: inside a segment"};
    }
    auto next_idx = find_next_index(segments, pos);
    if (!next_idx) {
        return {OpStatus::kDenied, "No next segment to extend"};
    }
    segments[*next_idx].a = pos;
    return {OpStatus::kOk, "Extend next"};
}

OpOutcome nudge_a(std::vector<Segment> &segments, double pos, double delta) {
    std::optional<std::size_t> idx;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (is_within_closed_range(pos, segments[i])) {
            idx = i;
            break;
        }
    }
    if (!idx) {
        return {OpStatus::kDenied, "Nudge A denied"};
    }

    Segment &seg = segments[*idx];
    double min_a = (*idx > 0) ? segments[*idx - 1].b : 0.0;
    double new_a = seg.a + delta;
    if (new_a < min_a || new_a >= seg.b) {
        return {OpStatus::kDenied, "Nudge A at boundary"};
    }
    seg.a = new_a;
    return {OpStatus::kOk, "Nudge A"};
}

OpOutcome nudge_b(std::vector<Segment> &segments, double pos, double delta) {
    std::optional<std::size_t> idx;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (is_within_closed_range(pos, segments[i])) {
            idx = i;
            break;
        }
    }
    if (!idx) {
        return {OpStatus::kDenied, "Nudge B denied"};
    }

    Segment &seg = segments[*idx];
    double max_b =
        (*idx + 1 < segments.size()) ? segments[*idx + 1].a : std::numeric_limits<double>::infinity();
    double new_b = seg.b + delta;
    if (new_b > max_b || new_b <= seg.a) {
        return {OpStatus::kDenied, "Nudge B at boundary"};
    }
    seg.b = new_b;
    return {OpStatus::kOk, "Nudge B"};
}

OpOutcome unset_a(std::vector<Segment> &segments, Pending &pending, double pos) {
    if (!pending.empty()) {
        if (!pending.has_a) {
            return {OpStatus::kDenied, "A not set"};
        }
        pending.has_a = false;
        return {OpStatus::kOk, "Unset A"};
    }

    auto idx = find_complete_index_at(segments, pos);
    if (!idx) {
        return {OpStatus::kDenied, "Unset A denied"};
    }

    double removed_b = segments[*idx].b;
    segments.erase(segments.begin() + static_cast<std::ptrdiff_t>(*idx));
    pending.has_a = false;
    pending.has_b = true;
    pending.b = removed_b;
    return {OpStatus::kOk, "Unset A"};
}

OpOutcome unset_b(std::vector<Segment> &segments, Pending &pending, double pos) {
    if (!pending.empty()) {
        if (!pending.has_b) {
            return {OpStatus::kDenied, "B not set"};
        }
        pending.has_b = false;
        return {OpStatus::kOk, "Unset B"};
    }

    auto idx = find_complete_index_at(segments, pos);
    if (!idx) {
        return {OpStatus::kDenied, "Unset B denied"};
    }

    double removed_a = segments[*idx].a;
    segments.erase(segments.begin() + static_cast<std::ptrdiff_t>(*idx));
    pending.has_b = false;
    pending.has_a = true;
    pending.a = removed_a;
    return {OpStatus::kOk, "Unset B"};
}

OpOutcome toggle_segment(std::vector<Segment> &segments, double pos) {
    auto idx = find_complete_index_at(segments, pos);
    if (!idx) {
        return {OpStatus::kDenied, "Toggle denied"};
    }
    segments[*idx].enabled = !segments[*idx].enabled;
    return {OpStatus::kOk, segments[*idx].enabled ? "Segment enabled" : "Segment disabled"};
}

std::vector<ActiveEntry> build_active_order(const std::vector<Segment> &segments, const Pending &pending) {
    std::vector<ActiveEntry> order;
    order.reserve(segments.size() + 1);
    for (const auto &seg : segments) {
        if (seg.enabled) {
            order.push_back(ActiveEntry{seg.a, seg.b});
        }
    }

    // SPEC §2.3：临时边界故意用“近端”（下一段的起点/上一段的终点），不是
    // “远端”，避免编辑中的开放区间一路播穿整个相邻的已有区间。这里查邻居
    // 时用的是完整 segments 列表（含被禁用的），不是只看 enabled 的
    // order——被禁用的区间依然真实占用那段时间，不应该被当成不存在。
    if (pending.has_a && !pending.has_b) {
        auto next_idx = find_next_index(segments, pending.a);
        std::optional<double> temp_b;
        if (next_idx) {
            temp_b = segments[*next_idx].a;
        }
        insert_sorted(order, ActiveEntry{pending.a, temp_b});
    } else if (pending.has_b && !pending.has_a) {
        auto prev_idx = find_prev_index(segments, pending.b);
        double temp_a = prev_idx ? segments[*prev_idx].b : 0.0;
        insert_sorted(order, ActiveEntry{temp_a, pending.b});
    }

    return order;
}

bool is_within_active_entry(const ActiveEntry &entry, double pos) {
    bool after_start = pos >= entry.a - kCursorEpsilon;
    bool before_end = !entry.b || pos < *entry.b - kCursorEpsilon;
    return after_start && before_end;
}

std::optional<std::size_t> locate_active_index(const std::vector<ActiveEntry> &order, double pos) {
    if (order.empty()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (is_within_active_entry(order[i], pos)) {
            return i;
        }
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i].a > pos + kCursorEpsilon) {
            return i;
        }
    }
    // 两个循环都没匹配上，说明 pos 落在“最后一项自己的 b 及之后”（包括恰好
    // 等于 b 的边界情况——比如刚用 set-b 定完最后一段，光标原地未动）。落回
    // 第一项会算出 ab_loop_a > ab_loop_b 的颠倒 pair；mpv 自己的
    // get_ab_loop_times() 会把这种颠倒 pair 交换过来变成“合法”区间，但
    // update_ab_loop_clip()（playloop.c:665）比较的是“交换前就已经确定”的
    // ab_loop_clip 标志——一旦当前 pos 已经超过交换后的有效 b，ab_loop_clip
    // 直接判 false，原生循环整个失效，表现为“循环不生效，一路播到文件末尾”。
    // 落回最后一项，current.b 就是 pos 自己，链路上不会再出现 a > b。
    return order.size() - 1;
}

std::optional<double> loop_reengage_target(const std::vector<ActiveEntry> &order, double pos) {
    auto idx = locate_active_index(order, pos);
    if (!idx) {
        return std::nullopt;
    }
    if (is_within_active_entry(order[*idx], pos)) {
        return std::nullopt;
    }
    if (pos < order[*idx].a) {
        // 空隙里，pos 还没到这一项自己的 a——包括“全部区间最前面”这个由
        // locate_active_index 第二轮判定出来的情况——朝它去就行。
        return order[*idx].a;
    }
    // pos 已经越过了这一项自己的 a（意味着整项都已经被甩在身后：locate_
    // active_index 在这种输入下只可能通过“全部都在 pos 之前”那条兜底路径
    // 走到这里，返回的是最后一项）。真正该去的是队列里的下一项，wrap 到
    // 第一项——不能待在原地重武装这一项自己，那只是让它自己的 a/b 再套一
    // 遍，压根不会往前推进到“下一个该播的东西”。
    std::size_t next = (*idx + 1) % order.size();
    return order[next].a;
}

std::optional<double> eof_fallback_target(const std::vector<ActiveEntry> &order) {
    if (order.empty()) {
        return std::nullopt;
    }
    return order.back().a;
}

JumpPair compute_jump_pair(const std::vector<ActiveEntry> &order, std::size_t landed_index, double real_duration,
                            double tail_freeze_seconds) {
    JumpPair pair;
    if (order.empty() || landed_index >= order.size()) {
        return pair;
    }

    const ActiveEntry &current = order[landed_index];
    if (!current.b) {
        // 开放到文件末尾：get_ab_loop_times 要求 a、b 都是有效值才生效
        // （misc.c:124-141），这里没法表达，交给 eof_fallback_target 兜底，
        // 不写入任何属性。
        return pair;
    }

    // 只写“当前落在的这一项自己”的 a/b，绝不掺入下一项的起点。原来的写法
    // 是 ab_loop_a=下一项的起点、ab_loop_b=本项的终点，指望原生机制“命中
    // b 就跳到当前的 a 值”——但 mpv 的 get_ab_loop_times()
    // （misc.c:124-141）在使用前会把 a/b 按大小 MPSWAP 排序，cutoff 恒定
    // 取较大值、跳转目标恒定取较小值，不管字面上写在 ab-loop-a 还是
    // ab-loop-b 里。正向多段轮播里下一项的起点几乎总是比本项的终点大，
    // 于是排序后 cutoff 变成了下一项的起点（本项自己的终点反而被当成了
    // 跳转目标），实际表现是“播过本项终点、一直播到下一项起点才被咔一下
    // 倒跳回本项终点”——完全不会跳到下一项。这里只用本项自己的 a<b，恒
    // 定有效，不会被排序坑；真正跨段的“跳到下一项”改由 plugin.cpp 里
    // 监测到原生自环命中自己的 a 之后，显式补一次 seek 来完成。
    pair.ab_loop_a = current.a;

    double b = *current.b;
    if (tail_freeze_seconds > 0.0 && b >= real_duration - kCursorEpsilon) {
        // SPEC §4：终点语义是“循环到文件末尾”时，实际写入的 b 要落在 tpad/
        // apad 补出来的延长段里，播入（不是 seek 进）之后原生机制才能在延长
        // 段内自然触发跳转，画面全程正常播放、不进入暂停状态。
        b += tail_freeze_seconds;
    }
    pair.ab_loop_b = b;
    return pair;
}

} // namespace enhanced_ab_loop
