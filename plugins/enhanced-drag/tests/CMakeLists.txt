if(NOT TARGET Catch2::Catch2WithMain)
    add_subdirectory(${CMAKE_SOURCE_DIR}/external/catch2
                      ${CMAKE_BINARY_DIR}/external/catch2 EXCLUDE_FROM_ALL)
endif()
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/external/catch2/extras)

add_executable(enhanced-drag-tests
    test_enhanced_drag.cpp
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-drag/src/logic.cpp
)

target_link_libraries(enhanced-drag-tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(enhanced-drag-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-drag/include
)

include(Catch)
catch_discover_tests(enhanced-drag-tests)
