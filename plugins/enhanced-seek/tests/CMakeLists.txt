add_executable(enhanced-seek-tests
    test_enhanced_seek.cpp
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-seek/src/logic.cpp
)

target_link_libraries(enhanced-seek-tests PRIVATE Catch2::Catch2WithMain fmt::fmt-header-only)
target_include_directories(enhanced-seek-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-seek/include
)

include(Catch)
catch_discover_tests(enhanced-seek-tests)
