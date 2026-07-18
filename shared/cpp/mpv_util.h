#pragma once

#include <mpv/client.h>

#include <cstdint>
#include <string>

#include <fmt/core.h>

// 供 C++ mpv 插件共用的最小 mpv client API 包装。仅覆盖当前多个插件都需要的
// 属性读写和 OSD 提示；插件专属的逻辑（如 NODE 结构解析）留在各插件自己的
// 源码中，不在此提前抽象。
namespace mpv_util {

inline double get_double(mpv_handle *handle, const char *name, double fallback) {
    double value = fallback;
    if (mpv_get_property(handle, name, MPV_FORMAT_DOUBLE, &value) < 0) {
        return fallback;
    }
    return value;
}

namespace detail {

inline bool fits_int64(double value) {
    return value >= static_cast<double>(INT64_MIN) && value <= static_cast<double>(INT64_MAX) &&
           value == static_cast<double>(static_cast<int64_t>(value));
}

} // namespace detail

// mpv 的属性系统在设置时通常只支持 INT64 -> DOUBLE 的隐式放宽转换，反过来
// 用 MPV_FORMAT_DOUBLE 去设置一个底层是整数/choice 类型的属性（例如
// video-rotate）会静默失败。这里照抄 mpv 自带 Lua 绑定
// （player/lua.c: script_set_property_number）的处理方式：值是整数时改用
// MPV_FORMAT_INT64。
inline void set_double(mpv_handle *handle, const char *name, double value) {
    if (detail::fits_int64(value)) {
        int64_t as_int = static_cast<int64_t>(value);
        mpv_set_property(handle, name, MPV_FORMAT_INT64, &as_int);
        return;
    }
    mpv_set_property(handle, name, MPV_FORMAT_DOUBLE, &value);
}

inline void show_osd_message(mpv_handle *handle, const std::string &text, double duration_seconds) {
    std::string duration_ms = fmt::format("{}", static_cast<int>(duration_seconds * 1000.0));
    const char *args[] = {"show-text", text.c_str(), duration_ms.c_str(), nullptr};
    mpv_command(handle, args);
}

} // namespace mpv_util

// 排查问题时加的调试输出用这个宏包裹，不要事后删除：Release 构建（顶层
// CMakeLists.txt 的默认构建类型）下 NDEBUG 生效，整段调用在编译期被去掉；
// 用 -DCMAKE_BUILD_TYPE=Debug 重新配置后才会输出到 stderr。格式串用 fmt 的
// `{}` 占位符（编译期做类型检查），不是 printf 的 `%` 占位符。
#ifndef NDEBUG
#define MPV_UTIL_DEBUG(...) ::fmt::print(stderr, "[mpv-plugins] " __VA_ARGS__)
#else
#define MPV_UTIL_DEBUG(...) ((void)0)
#endif
