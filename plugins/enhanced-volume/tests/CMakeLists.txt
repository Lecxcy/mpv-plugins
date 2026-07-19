add_executable(enhanced-volume-tests
    test_enhanced_volume.cpp
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-volume/src/logic.cpp
)

target_link_libraries(enhanced-volume-tests PRIVATE Catch2::Catch2WithMain fmt::fmt-header-only)
target_include_directories(enhanced-volume-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-volume/include
)

include(Catch)
catch_discover_tests(enhanced-volume-tests)
