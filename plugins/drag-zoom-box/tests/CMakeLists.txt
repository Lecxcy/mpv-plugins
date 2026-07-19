add_executable(drag-zoom-box-tests
    test_drag_zoom_box.cpp
    ${CMAKE_SOURCE_DIR}/plugins/drag-zoom-box/src/logic.cpp
)

target_link_libraries(drag-zoom-box-tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(drag-zoom-box-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/drag-zoom-box/include
)

include(Catch)
catch_discover_tests(drag-zoom-box-tests)
