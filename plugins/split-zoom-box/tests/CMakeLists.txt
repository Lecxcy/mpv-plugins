add_executable(split-zoom-box-tests
    test_geometry.cpp
    test_layout.cpp
    test_store.cpp
    ${CMAKE_SOURCE_DIR}/plugins/split-zoom-box/src/geometry.cpp
    ${CMAKE_SOURCE_DIR}/plugins/split-zoom-box/src/layout.cpp
    ${CMAKE_SOURCE_DIR}/plugins/split-zoom-box/src/store.cpp
)

target_link_libraries(split-zoom-box-tests PRIVATE
    Catch2::Catch2WithMain
    fmt::fmt-header-only
    nlohmann_json::nlohmann_json
)

target_include_directories(split-zoom-box-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/split-zoom-box/include
)

include(Catch)
catch_discover_tests(split-zoom-box-tests)
