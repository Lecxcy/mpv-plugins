find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)

add_library(enhanced-seek MODULE
    src/plugin.cpp
    src/logic.cpp
)

target_include_directories(enhanced-seek PRIVATE
    include
    ${MPV_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(enhanced-seek PRIVATE fmt::fmt-header-only)

# mpv C 插件使用宿主 mpv 进程导出的客户端 API 符号，不能链接 libmpv。
# 在 macOS 上保留这些未解析符号，等 mpv 加载插件时再解析。
if(APPLE)
    target_link_options(enhanced-seek PRIVATE
        "-undefined"
        "dynamic_lookup"
    )
endif()

# Windows 加载器不会直接暴露宿主进程符号；定义此宏后，mpv 客户端头文件会
# 为 C 插件提供函数指针表。
if(WIN32)
    target_compile_definitions(enhanced-seek PRIVATE MPV_CPLUGIN_DYNAMIC_SYM)
endif()

# mpv 按文件名加载 C 插件，因此去掉常规的 "lib" 前缀。
set_target_properties(enhanced-seek PROPERTIES PREFIX "")

add_subdirectory(tests)
