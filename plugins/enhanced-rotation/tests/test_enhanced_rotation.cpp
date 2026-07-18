#include <catch2/catch_test_macros.hpp>

#include "enhanced_rotation/logic.h"

TEST_CASE("normalize_rotation wraps into [0, 360)", "[enhanced-rotation]") {
    CHECK(enhanced_rotation::normalize_rotation(0) == 0.0);
    CHECK(enhanced_rotation::normalize_rotation(90) == 90.0);
    CHECK(enhanced_rotation::normalize_rotation(359) == 359.0);
    CHECK(enhanced_rotation::normalize_rotation(360) == 0.0);
    CHECK(enhanced_rotation::normalize_rotation(450) == 90.0);
    CHECK(enhanced_rotation::normalize_rotation(720) == 0.0);
}

TEST_CASE("normalize_rotation wraps negative deltas backwards", "[enhanced-rotation]") {
    CHECK(enhanced_rotation::normalize_rotation(-90) == 270.0);
    CHECK(enhanced_rotation::normalize_rotation(-360) == 0.0);
    CHECK(enhanced_rotation::normalize_rotation(-450) == 270.0);
}
