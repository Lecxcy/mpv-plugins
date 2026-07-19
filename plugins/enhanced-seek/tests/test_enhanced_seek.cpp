#include <catch2/catch_test_macros.hpp>

#include "enhanced_seek/logic.h"

using enhanced_seek::format_status;
using enhanced_seek::format_time;
using enhanced_seek::HoldState;
using enhanced_seek::HoldTiming;

TEST_CASE("format_time uses MM:SS below one hour and rounds to the nearest second", "[format]") {
    CHECK(format_time(0.0) == "00:00");
    CHECK(format_time(65.0) == "01:05");
    CHECK(format_time(59.49) == "00:59");
    CHECK(format_time(59.5) == "01:00");
}

TEST_CASE("format_time switches to H:MM:SS at one hour and clamps negatives to zero", "[format]") {
    CHECK(format_time(3661.0) == "1:01:01");
    CHECK(format_time(-5.0) == "00:00");
}

TEST_CASE("format_status renders position/duration and falls back to 0% without a percent", "[format]") {
    CHECK(format_status(10.0, 100.0, 25.0) == "00:10/01:40 (25.0%)");
    CHECK(format_status(10.0, 0.0, std::nullopt) == "00:10/00:00 (0%)");
}

TEST_CASE("HoldState stays inactive and produces no ticks before hold_delay elapses", "[hold]") {
    HoldState hold;
    HoldTiming timing{/*hold_delay=*/0.28, /*repeat_interval=*/0.1};

    hold.begin(1, /*now=*/0.0, timing);
    REQUIRE(hold.active());
    CHECK(hold.direction() == 1);

    // 短按：还没到 hold_delay 就查询，不应该产生任何连续触发。
    CHECK(hold.poll(0.1, timing) == 0);
}

TEST_CASE("HoldState fires one tick per repeat_interval at a constant cadence (no acceleration)", "[hold]") {
    HoldState hold;
    HoldTiming timing{/*hold_delay=*/0.28, /*repeat_interval=*/0.1};

    hold.begin(1, /*now=*/0.0, timing);

    CHECK(hold.poll(0.28, timing) == 1);
    // 恰好一个 repeat_interval 之后，应该正好再触发一次——速度全程不变，
    // 不会像 enhanced-volume 那样随时间加速。
    CHECK(hold.poll(0.38, timing) == 1);
    CHECK(hold.poll(0.42, timing) == 0);
    CHECK(hold.poll(0.48, timing) == 1);
}

TEST_CASE("HoldState catches up with multiple ticks when polling is delayed", "[hold]") {
    HoldState hold;
    HoldTiming timing{/*hold_delay=*/0.28, /*repeat_interval=*/0.1};

    hold.begin(1, /*now=*/0.0, timing);

    // next_tick_at 依次落在 0.28/0.38/0.48/0.58，一次性 poll(0.58) 应该把
    // 这 4 次都追平，而不是只触发一次、把剩下的连续 seek 距离漏掉。
    CHECK(hold.poll(0.58, timing) == 4);
}

TEST_CASE("HoldState only stops when the released direction matches the held direction", "[hold]") {
    HoldState hold;
    HoldTiming timing{/*hold_delay=*/0.28, /*repeat_interval=*/0.1};

    hold.begin(1, /*now=*/0.0, timing);
    hold.stop(-1);
    CHECK(hold.active());
    CHECK(hold.direction() == 1);

    hold.stop(1);
    CHECK_FALSE(hold.active());
}

TEST_CASE("HoldState begin() while active switches direction and resets the schedule", "[hold]") {
    HoldState hold;
    HoldTiming timing{/*hold_delay=*/0.28, /*repeat_interval=*/0.1};

    hold.begin(1, /*now=*/0.0, timing);
    hold.begin(-1, /*now=*/0.1, timing);

    CHECK(hold.direction() == -1);
    // 重新安排的下一次触发应该以新的 begin 时刻为基准，而不是延用旧方向
    // 累积下来的 next_tick_at。
    CHECK(hold.poll(0.37, timing) == 0);
    CHECK(hold.poll(0.38, timing) == 1);
}
