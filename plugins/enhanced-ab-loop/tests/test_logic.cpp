#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "enhanced_ab_loop/logic.h"

using namespace enhanced_ab_loop;
using Catch::Approx;

namespace {

Segment make(double a, double b, bool enabled = true) {
    return Segment{a, b, enabled};
}

} // namespace

TEST_CASE("sort_segments orders strictly by a with no epsilon fuzzing", "[logic][sort]") {
    std::vector<Segment> segments{make(10, 20), make(0, 5), make(30, 40)};
    sort_segments(segments);
    REQUIRE(segments.size() == 3);
    CHECK(segments[0].a == Approx(0));
    CHECK(segments[1].a == Approx(10));
    CHECK(segments[2].a == Approx(30));
}

TEST_CASE("sort_segments stays correct for closely-spaced segments that used to break the old "
          "epsilon tie-break comparator",
          "[logic][sort]") {
    // 旧实现里 a 差值 <= epsilon 就走 tie-break，破坏传递性：a1=0.000 与
    // a2=0.004 判定“相等”走 tie-break，a2 与 a3=0.008 也判定“相等”走
    // tie-break，但 a1 与 a3 差 0.008 > epsilon 直接比较——三者两两比较用了
    //不同规则，table.sort 不保证正确排序。这里用同样的数值分布验证新实现
    // （精确比较，没有 tie-break）总是给出严格递增的顺序。
    std::vector<Segment> segments{make(0.008, 0.009, true), make(0.000, 0.001, true),
                                   make(0.004, 0.0045, true)};
    sort_segments(segments);
    REQUIRE(segments.size() == 3);
    CHECK(segments[0].a == Approx(0.000));
    CHECK(segments[1].a == Approx(0.004));
    CHECK(segments[2].a == Approx(0.008));
}

TEST_CASE("find_complete_index_at requires strictly inside, boundary itself is not inside",
          "[logic][find]") {
    std::vector<Segment> segments{make(10, 20)};
    CHECK(find_complete_index_at(segments, 15) == 0);
    CHECK_FALSE(find_complete_index_at(segments, 10).has_value());
    CHECK_FALSE(find_complete_index_at(segments, 20).has_value());
    CHECK_FALSE(find_complete_index_at(segments, 5).has_value());
}

TEST_CASE("find_prev_index / find_next_index locate the nearest neighbours around a gap",
          "[logic][find]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20), make(30, 40)};
    CHECK(find_prev_index(segments, 25) == 1);
    CHECK(find_next_index(segments, 25) == 2);
    CHECK_FALSE(find_prev_index(segments, -1).has_value());
    CHECK_FALSE(find_next_index(segments, 45).has_value());
}

TEST_CASE("overlaps_complete detects exact-boundary touching as non-overlap", "[logic][overlap]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20)};
    CHECK_FALSE(overlaps_complete(segments, 5, 10).has_value()); // 恰好卡在空隙里，贴边不算重叠
    CHECK(overlaps_complete(segments, 4, 11).has_value());       // 真正跨进两段
    CHECK(overlaps_complete(segments, 12, 18).has_value());      // 完全落在一段内部
}

TEST_CASE("set_a then set_b in a gap inserts an independent new segment", "[logic][insert]") {
    std::vector<Segment> segments;
    Pending pending;

    auto r1 = set_a(segments, pending, 10);
    CHECK(r1.ok());
    CHECK(segments.empty());

    auto r2 = set_b(segments, pending, 20);
    CHECK(r2.ok());
    REQUIRE(segments.size() == 1);
    CHECK(segments[0].a == Approx(10));
    CHECK(segments[0].b == Approx(20));
    CHECK(pending.empty());
}

TEST_CASE("set_b lands inside an already-complete segment is rejected outright, not silently "
          "resolved (SPEC §3.1: 冲突就拒绝，不做隐式处理)",
          "[logic][insert]") {
    std::vector<Segment> segments{make(30, 40)};
    Pending pending;

    REQUIRE(set_a(segments, pending, 10).ok());
    auto r = set_b(segments, pending, 35); // 35 落在 [30,40] 内部
    CHECK_FALSE(r.ok());
    CHECK(segments.size() == 1); // 没有新增，也没有动 [30,40]
    CHECK(segments[0].a == Approx(30));
    CHECK(segments[0].b == Approx(40));
}

TEST_CASE("pairing a pending point that would overlap an existing segment is denied and pending "
          "is cleared",
          "[logic][insert]") {
    // pos 必须落在任何已有区间外面，才会走到“两个端点都齐了、尝试配对”这条
    // 路径——如果 pos 本身就落在某个已有区间内部，会被 §3.1 的“B denied”
    // 提前拦截，根本不会走到重叠检测这一步（另见上面那个专门测“B denied”
    // 分支的用例）。这里选 25（在 [10,20] 外面），但 [5,25] 整体包住了
    // [10,20]，用来触发 overlaps_complete 那条路径。
    std::vector<Segment> segments{make(10, 20)};
    Pending pending;

    REQUIRE(set_a(segments, pending, 5).ok());
    auto r = set_b(segments, pending, 25); // [5,25] 与 [10,20] 重叠
    CHECK_FALSE(r.ok());
    CHECK(segments.size() == 1);
    CHECK(pending.empty());
}

TEST_CASE("set_b landing inside an existing segment while a pending A is active leaves pending "
          "untouched (denied, not cleared)",
          "[logic][insert]") {
    // 这是和上面那个用例刻意对照的场景：pos 落在已有区间内部时，走的是
    // §3.1 “B denied”这条早退分支，pending 完全不受影响，不会被清空——
    // 用户还可以换一个位置继续按 set-b。
    std::vector<Segment> segments{make(10, 20)};
    Pending pending;

    REQUIRE(set_a(segments, pending, 5).ok());
    auto r = set_b(segments, pending, 15); // 15 落在 [10,20] 内部
    CHECK_FALSE(r.ok());
    CHECK(segments.size() == 1);
    CHECK(pending.has_a);
    CHECK(pending.a == Approx(5));
    CHECK_FALSE(pending.has_b);
}

TEST_CASE("set_a / set_b inside an existing complete segment moves that segment's own boundary "
          "(Move A/B, aligned with PotPlayer)",
          "[logic][move]") {
    std::vector<Segment> segments{make(10, 20)};
    Pending pending;

    auto r = set_a(segments, pending, 15);
    CHECK(r.ok());
    CHECK(segments[0].a == Approx(15));
    CHECK(segments[0].b == Approx(20));

    auto r2 = set_b(segments, pending, 18);
    CHECK(r2.ok());
    CHECK(segments[0].a == Approx(15));
    CHECK(segments[0].b == Approx(18));
}

TEST_CASE("Move A/B never crosses into a neighbouring segment because the strictly-inside "
          "precondition already bounds it",
          "[logic][move]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20)};
    Pending pending;

    // pos=12 严格落在 [10,20] 内部，Move A 把 a 移到 12，不可能撞到 [0,5]。
    auto r = set_a(segments, pending, 12);
    CHECK(r.ok());
    CHECK(segments[0].a == Approx(0));
    CHECK(segments[1].a == Approx(12));
}

TEST_CASE("extend_prev stretches the preceding segment's end into the gap", "[logic][extend]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20)};
    Pending pending;

    auto r = extend_prev(segments, pending, 7);
    CHECK(r.ok());
    CHECK(segments[0].b == Approx(7));
    CHECK(segments[1].a == Approx(10)); // 没动到后一段
}

TEST_CASE("extend_next stretches the following segment's start into the gap", "[logic][extend]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20)};
    Pending pending;

    auto r = extend_next(segments, pending, 7);
    CHECK(r.ok());
    CHECK(segments[1].a == Approx(7));
    CHECK(segments[0].b == Approx(5)); // 没动到前一段
}

TEST_CASE("extend is denied while a pending insert is in progress", "[logic][extend]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20)};
    Pending pending;
    pending.has_a = true;
    pending.a = 6;

    CHECK_FALSE(extend_prev(segments, pending, 7).ok());
    CHECK_FALSE(extend_next(segments, pending, 7).ok());
    CHECK(segments[0].b == Approx(5));
    CHECK(segments[1].a == Approx(10));
}

TEST_CASE("extend is denied when the cursor is inside a segment", "[logic][extend]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20)};
    Pending pending;

    CHECK_FALSE(extend_prev(segments, pending, 15).ok());
    CHECK_FALSE(extend_next(segments, pending, 15).ok());
}

TEST_CASE("extend is denied when there is no neighbour in that direction", "[logic][extend]") {
    std::vector<Segment> segments{make(10, 20)};
    Pending pending;

    CHECK_FALSE(extend_prev(segments, pending, 5).ok());  // 前面没有区间
    CHECK_FALSE(extend_next(segments, pending, 25).ok()); // 后面没有区间
}

TEST_CASE("nudge_a triggers right at the boundary itself, not just strictly inside", "[logic][nudge]") {
    std::vector<Segment> segments{make(10, 20)};
    // 落在 a 本身（循环刚跳回来时最典型的场景），旧的严格 is_strictly_inside
    // 会拒绝，新的 is_within_closed_range 应该允许。
    auto r = nudge_a(segments, 10, 0.05);
    CHECK(r.ok());
    CHECK(segments[0].a == Approx(10.05));
}

TEST_CASE("nudge_b triggers right at the boundary itself", "[logic][nudge]") {
    std::vector<Segment> segments{make(10, 20)};
    auto r = nudge_b(segments, 20, -0.05);
    CHECK(r.ok());
    CHECK(segments[0].b == Approx(19.95));
}

TEST_CASE("nudge is denied (not silently clamped) when it would cross a boundary", "[logic][nudge]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20), make(30, 40)};

    auto r_a = nudge_a(segments, 10, -20); // 会顶到/越过前一段的 b=5
    CHECK_FALSE(r_a.ok());
    CHECK(segments[1].a == Approx(10)); // 没有被静默 clamp 成别的值

    auto r_b = nudge_b(segments, 20, 20); // 会顶到/越过下一段的 a=30
    CHECK_FALSE(r_b.ok());
    CHECK(segments[1].b == Approx(20));
}

TEST_CASE("nudge_a allowed to touch exactly the previous segment's end", "[logic][nudge]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20)};
    auto r = nudge_a(segments, 10, -5); // 10 - 5 = 5，正好等于前一段的 b
    CHECK(r.ok());
    CHECK(segments[1].a == Approx(5));
}

TEST_CASE("unset_a splits a complete segment into an independent pending point, without touching "
          "any neighbouring segment (regression test for the old '嫁接到邻居' bug)",
          "[logic][unset]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20)};
    Pending pending;

    auto r = unset_a(segments, pending, 15); // 落在 [10,20] 内部
    CHECK(r.ok());
    REQUIRE(segments.size() == 1);
    CHECK(segments[0].a == Approx(0)); // [0,5] 完全没被动过
    CHECK(segments[0].b == Approx(5));
    CHECK(pending.has_b);
    CHECK_FALSE(pending.has_a);
    CHECK(pending.b == Approx(20));
}

TEST_CASE("unset_b splits a complete segment into an independent pending point, without touching "
          "any neighbouring segment",
          "[logic][unset]") {
    std::vector<Segment> segments{make(0, 5), make(10, 20)};
    Pending pending;

    auto r = unset_b(segments, pending, 3); // 落在 [0,5] 内部
    CHECK(r.ok());
    REQUIRE(segments.size() == 1);
    CHECK(segments[0].a == Approx(10)); // [10,20] 完全没被动过
    CHECK(segments[0].b == Approx(20));
    CHECK(pending.has_a);
    CHECK_FALSE(pending.has_b);
    CHECK(pending.a == Approx(0));
}

TEST_CASE("unset_a/unset_b on an already-pending point just clears that half", "[logic][unset]") {
    std::vector<Segment> segments;
    Pending pending;
    pending.has_a = true;
    pending.a = 5;

    auto r = unset_a(segments, pending, 999); // pos 无所谓，pending 非空走这条分支
    CHECK(r.ok());
    CHECK(pending.empty());
}

TEST_CASE("toggle_segment flips enabled for the segment the cursor is inside", "[logic][toggle]") {
    std::vector<Segment> segments{make(10, 20)};
    CHECK(segments[0].enabled);

    auto r = toggle_segment(segments, 15);
    CHECK(r.ok());
    CHECK_FALSE(segments[0].enabled);

    toggle_segment(segments, 15);
    CHECK(segments[0].enabled);
}

TEST_CASE("build_active_order only includes enabled segments, sorted by a", "[logic][active]") {
    std::vector<Segment> segments{make(0, 5, true), make(10, 20, false), make(30, 40, true)};
    Pending pending;

    auto order = build_active_order(segments, pending);
    REQUIRE(order.size() == 2);
    CHECK(order[0].a == Approx(0));
    CHECK(order[1].a == Approx(30));
}

TEST_CASE("pending with only A set is bounded by the *start* of the next segment, not its end "
          "(SPEC §2.3: 用近端不用远端，避免播穿整个邻居)",
          "[logic][active]") {
    std::vector<Segment> segments{make(10, 20)};
    Pending pending;
    pending.has_a = true;
    pending.a = 3;

    auto order = build_active_order(segments, pending);
    REQUIRE(order.size() == 2);
    CHECK(order[0].a == Approx(3));
    REQUIRE(order[0].b.has_value());
    CHECK(*order[0].b == Approx(10)); // 邻居的起点，不是它的终点 20
    // 邻居自己作为独立的一项完整保留，没有被吞并/篡改。
    CHECK(order[1].a == Approx(10));
    REQUIRE(order[1].b.has_value());
    CHECK(*order[1].b == Approx(20));
}

TEST_CASE("pending with only B set is bounded by the *end* of the previous segment, not its start",
          "[logic][active]") {
    std::vector<Segment> segments{make(0, 5)};
    Pending pending;
    pending.has_b = true;
    pending.b = 20;

    auto order = build_active_order(segments, pending);
    REQUIRE(order.size() == 2);
    CHECK(order[0].a == Approx(0));
    REQUIRE(order[0].b.has_value());
    CHECK(*order[0].b == Approx(5)); // 前一段完整保留，没被“嫁接”到新的 b
    CHECK(order[1].a == Approx(5));  // 借来的近端边界，是前一段的终点
    REQUIRE(order[1].b.has_value());
    CHECK(*order[1].b == Approx(20));
}

TEST_CASE("pending with only A set and no next segment stays open-ended (nullopt b)",
          "[logic][active]") {
    std::vector<Segment> segments;
    Pending pending;
    pending.has_a = true;
    pending.a = 3;

    auto order = build_active_order(segments, pending);
    REQUIRE(order.size() == 1);
    CHECK_FALSE(order[0].b.has_value());
}

TEST_CASE("pending with only B set and no previous segment defaults its temp start to 0",
          "[logic][active]") {
    std::vector<Segment> segments;
    Pending pending;
    pending.has_b = true;
    pending.b = 20;

    auto order = build_active_order(segments, pending);
    REQUIRE(order.size() == 1);
    CHECK(order[0].a == Approx(0));
}

TEST_CASE("locate_active_index finds the entry containing pos, falls back to next, or to the last entry when past everything",
          "[logic][active]") {
    std::vector<ActiveEntry> order{{0, 5.0}, {10, 20.0}, {30, 40.0}};

    CHECK(locate_active_index(order, 12) == 1);   // 落在第二项内部
    CHECK(locate_active_index(order, 25) == 2);   // 落在空隙，下一项
    // pos 落在最后一项之后（含恰好等于最后一项的 b），落回最后一项本身，不
    // 是 wrap 回第一项——否则会算出 ab_loop_a > ab_loop_b 的颠倒 pair，被
    // mpv update_ab_loop_clip() 判定当前 pos 已经超过（交换后的）有效 b，
    // 导致 ab-loop 整个失效（见 logic.cpp locate_active_index 的注释）。
    CHECK(locate_active_index(order, 100) == 2);
    CHECK(locate_active_index(order, 40) == 2);  // 恰好等于最后一项自己的 b
    CHECK_FALSE(locate_active_index({}, 5).has_value());
}

TEST_CASE("loop_reengage_target returns nullopt when pos is already inside an active entry",
          "[logic][reengage]") {
    std::vector<ActiveEntry> order{{2, 5.0}, {10, 15.0}};
    CHECK_FALSE(loop_reengage_target(order, 3.5).has_value());
    CHECK_FALSE(loop_reengage_target(order, 12.0).has_value());
}

TEST_CASE("loop_reengage_target seeks to the upcoming entry's start when pos sits in a gap before it",
          "[logic][reengage]") {
    std::vector<ActiveEntry> order{{2, 5.0}, {10, 15.0}};

    // 全部区间最前面（0 号项之前）。
    auto before_all = loop_reengage_target(order, 0.5);
    REQUIRE(before_all.has_value());
    CHECK(*before_all == Approx(2));

    // 两段之间的空隙。
    auto between = loop_reengage_target(order, 7.0);
    REQUIRE(between.has_value());
    CHECK(*between == Approx(10));
}

TEST_CASE("loop_reengage_target wraps to the first entry's start when pos is already past everything "
          "(regression test: closing loop, playing past all segments, then reopening it must NOT just "
          "keep playing to real EOF)",
          "[logic][reengage]") {
    std::vector<ActiveEntry> order{{2, 5.0}, {10, 15.0}};
    auto target = loop_reengage_target(order, 16.0);
    REQUIRE(target.has_value());
    CHECK(*target == Approx(2)); // wrap 回第一项，不是待在第 1 项（[10,15]）原地
}

TEST_CASE("loop_reengage_target wraps a single active entry back to its own start", "[logic][reengage]") {
    std::vector<ActiveEntry> order{{2, 5.0}};
    auto target = loop_reengage_target(order, 8.0);
    REQUIRE(target.has_value());
    CHECK(*target == Approx(2));
}

TEST_CASE("loop_reengage_target on an empty order returns nullopt", "[logic][reengage]") {
    CHECK_FALSE(loop_reengage_target({}, 5.0).has_value());
}

TEST_CASE("eof_fallback_target returns the LAST entry's start, not the first one (regression test "
          "for the old active[1].a bug)",
          "[logic][eof]") {
    std::vector<ActiveEntry> order{{0, 5.0}, {10, 20.0}, {30, std::nullopt}};
    auto target = eof_fallback_target(order);
    REQUIRE(target.has_value());
    CHECK(*target == Approx(30)); // 不是 0
}

TEST_CASE("eof_fallback_target with a single entry matches both the old and new formula (no "
          "regression for the previously-working case)",
          "[logic][eof]") {
    std::vector<ActiveEntry> order{{7, std::nullopt}};
    auto target = eof_fallback_target(order);
    REQUIRE(target.has_value());
    CHECK(*target == Approx(7));
}

TEST_CASE("eof_fallback_target on an empty order returns nullopt", "[logic][eof]") {
    CHECK_FALSE(eof_fallback_target({}).has_value());
}

// 回归背景见 logic.cpp compute_jump_pair 的注释（mpv get_ab_loop_times()
// 的 MPSWAP 排序坑）；用真实 mpv 头文件 + headless IPC 驱动复现确认过。
TEST_CASE("compute_jump_pair predicts the CURRENT active entry's own bounds, not the next one's",
          "[logic][jump]") {
    std::vector<ActiveEntry> order{{0, 5.0}, {10, 20.0}, {30, 40.0}};

    auto pair = compute_jump_pair(order, 0, /*real_duration=*/100.0, /*tail_freeze_seconds=*/0.0);
    REQUIRE(pair.ab_loop_a.has_value());
    REQUIRE(pair.ab_loop_b.has_value());
    CHECK(*pair.ab_loop_a == Approx(0)); // 本段自己的起点，不是下一段的起点
    CHECK(*pair.ab_loop_b == Approx(5)); // 本段自己的终点
}

TEST_CASE("compute_jump_pair on the last entry still returns its own bounds (no more implicit wrap)",
          "[logic][jump]") {
    std::vector<ActiveEntry> order{{0, 5.0}, {10, 20.0}};

    auto pair = compute_jump_pair(order, 1, 100.0, 0.0);
    REQUIRE(pair.ab_loop_a.has_value());
    CHECK(*pair.ab_loop_a == Approx(10)); // 本段自己的起点，不是 wrap 回第一段的 0
    REQUIRE(pair.ab_loop_b.has_value());
    CHECK(*pair.ab_loop_b == Approx(20));
}

TEST_CASE("compute_jump_pair leaves both properties unset for an open-ended (nullopt b) entry",
          "[logic][jump]") {
    std::vector<ActiveEntry> order{{0, 5.0}, {10, std::nullopt}};

    auto pair = compute_jump_pair(order, 1, 100.0, 0.5);
    CHECK_FALSE(pair.ab_loop_a.has_value());
    CHECK_FALSE(pair.ab_loop_b.has_value());
}

TEST_CASE("compute_jump_pair extends b into the tail-freeze padded region when b sits at the real "
          "file end (SPEC §4: 播入延长段，不是暂停+计时器)",
          "[logic][jump]") {
    std::vector<ActiveEntry> order{{0, 5.0}, {10, 100.0}}; // 第二段的 b 就是真实 duration
    auto pair = compute_jump_pair(order, 1, /*real_duration=*/100.0, /*tail_freeze_seconds=*/0.3);
    REQUIRE(pair.ab_loop_b.has_value());
    CHECK(*pair.ab_loop_b == Approx(100.3)); // 真实末尾 + 冻结时长
}

TEST_CASE("compute_jump_pair does NOT add the tail-freeze bonus for an ordinary mid-file segment",
          "[logic][jump]") {
    std::vector<ActiveEntry> order{{0, 5.0}, {10, 20.0}};
    auto pair = compute_jump_pair(order, 1, /*real_duration=*/100.0, /*tail_freeze_seconds=*/0.3);
    REQUIRE(pair.ab_loop_b.has_value());
    CHECK(*pair.ab_loop_b == Approx(20)); // 不应该被加成 20.3
}

TEST_CASE("compute_jump_pair does not add the tail-freeze bonus when tail_freeze_seconds is zero "
          "or negative even if b sits at the real file end",
          "[logic][jump]") {
    std::vector<ActiveEntry> order{{0, 5.0}, {10, 100.0}};
    auto pair = compute_jump_pair(order, 1, 100.0, 0.0);
    REQUIRE(pair.ab_loop_b.has_value());
    CHECK(*pair.ab_loop_b == Approx(100.0));
}

TEST_CASE("compute_jump_pair on an out-of-range index or empty order returns an empty pair",
          "[logic][jump]") {
    std::vector<ActiveEntry> order{{0, 5.0}};
    auto pair = compute_jump_pair(order, 5, 100.0, 0.0);
    CHECK_FALSE(pair.ab_loop_a.has_value());
    CHECK_FALSE(pair.ab_loop_b.has_value());

    auto empty_pair = compute_jump_pair({}, 0, 100.0, 0.0);
    CHECK_FALSE(empty_pair.ab_loop_a.has_value());
    CHECK_FALSE(empty_pair.ab_loop_b.has_value());
}

TEST_CASE("plan_segment_display shows everything with no ellipsis when total is at or below the "
          "limit",
          "[logic][display]") {
    CHECK(plan_segment_display(0, 12).head_count == 0);
    CHECK(plan_segment_display(0, 12).hidden_count == 0);
    CHECK(plan_segment_display(0, 12).tail_count == 0);

    auto exact = plan_segment_display(12, 12);
    CHECK(exact.head_count == 12);
    CHECK(exact.hidden_count == 0);
    CHECK(exact.tail_count == 0);

    auto below = plan_segment_display(5, 12);
    CHECK(below.head_count == 5);
    CHECK(below.hidden_count == 0);
    CHECK(below.tail_count == 0);
}

TEST_CASE("plan_segment_display splits head/tail evenly and reports the correct hidden count once "
          "over the limit",
          "[logic][display]") {
    // 对应真实跑过的场景：25 个区间、上限 12，应该是首 6 + 隐藏 13 + 尾 6。
    auto plan = plan_segment_display(25, 12);
    CHECK(plan.head_count == 6);
    CHECK(plan.hidden_count == 13);
    CHECK(plan.tail_count == 6);
    CHECK(plan.head_count + plan.hidden_count + plan.tail_count == 25);
}

TEST_CASE("plan_segment_display just one over the limit still hides exactly one entry",
          "[logic][display]") {
    auto plan = plan_segment_display(13, 12);
    CHECK(plan.head_count == 6);
    CHECK(plan.hidden_count == 1);
    CHECK(plan.tail_count == 6);
}

TEST_CASE("plan_segment_display favours the tail by one when max_visible is odd (head = max/2, "
          "rounded down)",
          "[logic][display]") {
    auto plan = plan_segment_display(50, 5);
    CHECK(plan.head_count == 2);
    CHECK(plan.tail_count == 3);
    CHECK(plan.hidden_count == 45);
}

TEST_CASE("plan_segment_display uses kMaxVisibleSegments as the default limit", "[logic][display]") {
    auto plan = plan_segment_display(kMaxVisibleSegments + 1);
    CHECK(plan.head_count + plan.tail_count == kMaxVisibleSegments);
    CHECK(plan.hidden_count == 1);
}
