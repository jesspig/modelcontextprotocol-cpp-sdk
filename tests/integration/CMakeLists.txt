include(GoogleTest)

add_executable(mcp-integration-tests
    ClientServerRoundTrip.cpp)

target_link_libraries(mcp-integration-tests PRIVATE
    mcp-client mcp-server GTest::gtest_main)

gtest_discover_tests(mcp-integration-tests)
