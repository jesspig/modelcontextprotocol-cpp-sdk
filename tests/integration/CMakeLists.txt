include(McpDiscoverTests)

add_executable(mcp-integration-tests
    ClientServerRoundTrip.cpp
    Phase1InteropTests.cpp
    Phase2ClosureTests.cpp)

target_link_libraries(mcp-integration-tests PRIVATE
    mcp-client mcp-server mcp-http mcp-test-main)
target_include_directories(mcp-integration-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

mcp_discover_tests(mcp-integration-tests)
