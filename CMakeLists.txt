cmake_minimum_required(VERSION 3.28) # FetchContent_Declare(... EXCLUDE_FROM_ALL) 需要 3.28+

project(mpv_plugins LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 未显式指定时默认 Release（NDEBUG 生效），调试输出（见
# shared/cpp/mpv_util.h 的 MPV_UTIL_DEBUG）默认关闭；需要时用
# -DCMAKE_BUILD_TYPE=Debug 重新配置。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(CTest)

# Catch2、fmt 是构建依赖，不是需要人工/AI 阅读参考的上游源码，不进
# external/，改用 FetchContent 在配置期拉取（需要联网；固定版本号，不跟随
# 最新 tag）。
include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.2
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)

FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 12.2.0
    EXCLUDE_FROM_ALL
)
# 只用 fmt::fmt-header-only 这个纯头文件 INTERFACE 目标：fmt 自带的预编译
# src/format.cc 在较新 Clang（AppleClang 21+）下因 consteval 处理差异编译
# 失败，EXCLUDE_FROM_ALL（CMake 3.28+ 起 FetchContent_Declare 原生支持，
# 直接透传给底层 add_subdirectory）让默认构建不去碰这个用不到的目标，绕开
# 这个上游兼容性问题；纯头文件对我们这种调用量不大的用法没有明显运行时
# 代价。
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

# 显式添加 C++ 插件；纯 Lua 插件不参与 CMake 构建。
add_subdirectory(plugins/enhanced-rotation)
add_subdirectory(plugins/enhanced-drag)
add_subdirectory(plugins/drag-zoom-box)
add_subdirectory(plugins/enhanced-ab-loop)
add_subdirectory(plugins/enhanced-seek)
add_subdirectory(plugins/enhanced-volume)
