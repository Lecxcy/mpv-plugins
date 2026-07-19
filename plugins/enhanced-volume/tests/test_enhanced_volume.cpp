#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "enhanced_volume/logic.h"

using namespace enhanced_volume;

TEST_CASE("next_volume clamps to [0, max_volume]", "[enhanced-volume]") {
    CHECK(next_volume(50.0, 5.0, 300.0) == 55.0);
    CHECK(next_volume(2.0, -5.0, 300.0) == 0.0);
    CHECK(next_volume(298.0, 5.0, 300.0) == 300.0);
}

TEST_CASE("format_volume_label reports Mute when muted regardless of volume", "[enhanced-volume]") {
    CHECK(format_volume_label(80.0, true) == "Mute");
    CHECK(format_volume_label(0.0, true) == "Mute");
}

TEST_CASE("format_volume_label rounds like the original Lua math.floor(volume + 0.5)", "[enhanced-volume]") {
    CHECK(format_volume_label(49.4, false) == "Volume: 49%");
    CHECK(format_volume_label(49.5, false) == "Volume: 50%");
    CHECK(format_volume_label(100.0, false) == "Volume: 100%");
}

TEST_CASE("resolve_hold_interval picks the first bucket the elapsed time is still under", "[enhanced-volume]") {
    std::vector<HoldProfileStep> profile = {
        {0.8, 0.18},
        {1.8, 0.12},
        {1e18, 0.07},
    };

    CHECK(resolve_hold_interval(profile, 0.0) == 0.18);
    CHECK(resolve_hold_interval(profile, 0.79) == 0.18);
    CHECK(resolve_hold_interval(profile, 0.8) == 0.12);
    CHECK(resolve_hold_interval(profile, 1.79) == 0.12);
    CHECK(resolve_hold_interval(profile, 1.8) == 0.07);
    CHECK(resolve_hold_interval(profile, 100.0) == 0.07);
}

TEST_CASE("tick_hold only fires once next_at has passed and reschedules using the profile", "[enhanced-volume]") {
    std::vector<HoldProfileStep> profile = {
        {0.8, 0.18},
        {1.8, 0.12},
        {1e18, 0.07},
    };

    HoldState state = start_hold(1, /*now=*/0.0, /*hold_delay=*/0.28);
    CHECK(state.direction == 1);
    CHECK(state.next_at == 0.28);

    // 还没到 next_at，不触发，状态不变。
    CHECK_FALSE(tick_hold(state, 0.1, profile));
    CHECK(state.next_at == 0.28);

    // 每次都用上一次算出来的 next_at 本身去触发下一次，避免手写十进制字面量
    // 在累加过程中和状态里存的浮点值产生誤差、导致 `now < next_at` 判断
    // 出人意料地为真（0.1 这类十进制小数不能被 double 精确表示）。

    // 到点触发一次；elapsed = 0.28 < 0.8，取 0.18 档。
    CHECK(tick_hold(state, state.next_at, profile));
    CHECK(state.next_at == Catch::Approx(0.46));

    CHECK(tick_hold(state, state.next_at, profile));
    CHECK(state.next_at == Catch::Approx(0.64));

    CHECK(tick_hold(state, state.next_at, profile));
    CHECK(state.next_at == Catch::Approx(0.82));

    // elapsed = 0.82 >= 0.8，跨进 0.12 档。
    CHECK(tick_hold(state, state.next_at, profile));
    CHECK(state.next_at == Catch::Approx(0.94));
}

TEST_CASE("start_hold records direction and the initial hold_delay deadline", "[enhanced-volume]") {
    HoldState state = start_hold(-1, /*now=*/10.0, /*hold_delay=*/0.28);
    CHECK(state.direction == -1);
    CHECK(state.started_at == 10.0);
    CHECK(state.next_at == Catch::Approx(10.28));
}
