#include <catch2/catch_test_macros.hpp>

#include "shared/cpp/confirm_lockout.h"

using mpv_util::command_chain_contains;

namespace {
constexpr const char *kYes = "script-binding enhanced_ab_loop/confirm-yes";
} // namespace

TEST_CASE("单条命令精确命中", "[confirm_lockout]") {
    REQUIRE(command_chain_contains("script-binding enhanced_ab_loop/confirm-yes", kYes));
    REQUIRE_FALSE(command_chain_contains("script-binding enhanced_ab_loop/confirm-no", kYes));
    REQUIRE_FALSE(command_chain_contains("", kYes));
}

// 这条是把全等匹配换成拆分匹配的直接原因：同一个物理键要同时喂给两个插件
// 时，input.conf 里只能写成 `;` 串接的一整行，全等比较会整条落空。
TEST_CASE("`;` 串接的两条命令都能命中", "[confirm_lockout]") {
    const char *chain = "script-binding enhanced_ab_loop/confirm-yes ; script-binding split_zoom_box/confirm-yes";
    REQUIRE(command_chain_contains(chain, kYes));
    REQUIRE(command_chain_contains(chain, "script-binding split_zoom_box/confirm-yes"));
    REQUIRE_FALSE(command_chain_contains(chain, "script-binding split_zoom_box/confirm-no"));
}

TEST_CASE("串接顺序与数量都不影响命中", "[confirm_lockout]") {
    REQUIRE(command_chain_contains("script-binding split_zoom_box/confirm-yes;script-binding "
                                   "enhanced_ab_loop/confirm-yes",
                                   kYes));
    REQUIRE(command_chain_contains("no-osd set pause yes ; script-binding other/thing ; "
                                   "script-binding enhanced_ab_loop/confirm-yes",
                                   kYes));
}

TEST_CASE("分隔符两侧的空白被忽略", "[confirm_lockout]") {
    REQUIRE(command_chain_contains("  script-binding enhanced_ab_loop/confirm-yes  ", kYes));
    REQUIRE(command_chain_contains("foo ;\tscript-binding enhanced_ab_loop/confirm-yes\t; bar", kYes));
    // 尾随分号切出一段空片段，不能因此崩掉或误判
    REQUIRE(command_chain_contains("script-binding enhanced_ab_loop/confirm-yes ;", kYes));
    REQUIRE_FALSE(command_chain_contains(" ; ; ", kYes));
}

// 子串匹配会在这两条上误判，所以拆分之后仍要求整段相等，不能退化成 find()。
TEST_CASE("前缀重名和被引号包住的同名字样都不算命中", "[confirm_lockout]") {
    REQUIRE_FALSE(command_chain_contains("script-binding enhanced_ab_loop/confirm-yes-extra", kYes));
    REQUIRE_FALSE(command_chain_contains("script-binding xx_enhanced_ab_loop/confirm-yes", kYes));
    REQUIRE_FALSE(command_chain_contains("show-text \"script-binding enhanced_ab_loop/confirm-yes\"", kYes));
}
