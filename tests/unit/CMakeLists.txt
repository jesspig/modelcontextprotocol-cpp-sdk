include(GoogleTest)

add_executable(mcp-core-tests
    JsonRpcTests.cpp
    McpTypesTests.cpp)

target_link_libraries(mcp-core-tests PRIVATE mcp-core GTest::gtest_main)
gtest_discover_tests(mcp-core-tests)

add_executable(mcp-wire-codec-tests WireCodecTests.cpp)
target_link_libraries(mcp-wire-codec-tests PRIVATE mcp-protocol GTest::gtest_main)
gtest_discover_tests(mcp-wire-codec-tests)

add_executable(mcp-server-tests McpServerTests.cpp)
target_link_libraries(mcp-server-tests PRIVATE mcp-server GTest::gtest_main)
gtest_discover_tests(mcp-server-tests)

add_executable(mcp-client-tests McpClientTests.cpp)
target_link_libraries(mcp-client-tests PRIVATE mcp-client mcp-server GTest::gtest_main)
gtest_discover_tests(mcp-client-tests)

add_executable(mcp-transport-tests TransportTests.cpp)
target_link_libraries(mcp-transport-tests PRIVATE mcp-transport GTest::gtest_main)
gtest_discover_tests(mcp-transport-tests)
