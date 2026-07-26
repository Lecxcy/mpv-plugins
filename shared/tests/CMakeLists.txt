find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED mpv)

add_executable(shared-tests
    test_confirm_lockout.cpp
)

target_link_libraries(shared-tests PRIVATE Catch2::Catch2WithMain)

# confirm_lockout.h 里调 mpv client API 的那几个函数都是 inline 且这里没被
# 调到，编译期不会实例化，所以只要头文件路径、不需要链接 libmpv。
target_include_directories(shared-tests PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${MPV_INCLUDE_DIRS}
)

include(Catch)
catch_discover_tests(shared-tests)
