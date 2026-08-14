include(McpDiscoverTests)

add_executable(mcp-integration-tests
    ClientServerRoundTrip.cpp)

target_link_libraries(mcp-integration-tests PRIVATE
    mcp-client mcp-server mcp-test-main)

mcp_discover_tests(mcp-integration-tests)
