include(McpDiscoverTests)

add_executable(mcp-conformance-tests ProtocolConformance.cpp)
target_link_libraries(mcp-conformance-tests PRIVATE mcp-protocol mcp-client mcp-server mcp-test-main)
mcp_discover_tests(mcp-conformance-tests)
