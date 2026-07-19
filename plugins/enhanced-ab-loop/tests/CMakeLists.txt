add_executable(enhanced-ab-loop-tests
    test_logic.cpp
    test_store.cpp
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-ab-loop/src/logic.cpp
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-ab-loop/src/store.cpp
)

target_link_libraries(enhanced-ab-loop-tests PRIVATE Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
target_include_directories(enhanced-ab-loop-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/enhanced-ab-loop/include
)

include(Catch)
catch_discover_tests(enhanced-ab-loop-tests)
