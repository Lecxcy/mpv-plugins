add_executable(enhanced-rotation-tests
    test_enhanced_rotation.cpp
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-rotation/src/logic.cpp
)

target_link_libraries(enhanced-rotation-tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(enhanced-rotation-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-rotation/include
)

include(Catch)
catch_discover_tests(enhanced-rotation-tests)
