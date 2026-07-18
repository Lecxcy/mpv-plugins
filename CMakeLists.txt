cmake_minimum_required(VERSION 3.20)

project(mpv_plugins LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

# 显式添加 C++ 插件；纯 Lua 插件不参与 CMake 构建。
# 当前没有 C++ 插件；新增插件时在此处显式 add_subdirectory(plugins/<插件名>)
