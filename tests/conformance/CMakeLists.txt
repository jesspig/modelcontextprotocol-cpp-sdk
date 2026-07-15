include(GoogleTest)

add_executable(mcp-conformance-tests ProtocolConformance.cpp)
target_link_libraries(mcp-conformance-tests PRIVATE mcp-protocol mcp-client mcp-server GTest::gtest_main)
gtest_discover_tests(mcp-conformance-tests)
