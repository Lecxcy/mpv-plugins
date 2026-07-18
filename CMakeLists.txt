cmake_minimum_required(VERSION 3.20)

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

# 显式添加 C++ 插件；纯 Lua 插件不参与 CMake 构建。
add_subdirectory(plugins/enhanced-rotation)
add_subdirectory(plugins/enhanced-drag)
