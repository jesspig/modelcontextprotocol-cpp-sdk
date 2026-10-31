include(McpDiscoverTests)

add_executable(mcp-core-tests
    JsonRpcTests.cpp
    McpTypesTests.cpp
    JsonParserTests.cpp
    LogTests.cpp
    ResponseCacheTests.cpp)

target_link_libraries(mcp-core-tests PRIVATE mcp-core mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-core-tests)

add_executable(mcp-wire-codec-tests
    WireCodecTests.cpp
    SessionHandlerTests.cpp
    SpanHookTests.cpp)
target_link_libraries(mcp-wire-codec-tests PRIVATE mcp-protocol mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-wire-codec-tests)

add_executable(mcp-server-tests McpServerTests.cpp)
target_link_libraries(mcp-server-tests PRIVATE mcp-client mcp-server mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-server-tests)

add_executable(mcp-client-tests McpClientTests.cpp)
target_link_libraries(mcp-client-tests PRIVATE mcp-client mcp-server mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-client-tests)

add_executable(mcp-oauth-tests OAuthTests.cpp)
target_link_libraries(mcp-oauth-tests PRIVATE mcp-client mcp-http mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-oauth-tests)

add_executable(mcp-transport-tests TransportTests.cpp MessageChannelTests.cpp)
target_link_libraries(mcp-transport-tests PRIVATE mcp-transport mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-transport-tests)

add_executable(mcp-process-test-helper ProcessTestHelper.cpp)
target_link_libraries(mcp-process-test-helper PRIVATE mcp-transport)

add_executable(mcp-process-tests ProcessTests.cpp)
target_link_libraries(mcp-process-tests PRIVATE mcp-transport mcp-test-support GTest::gtest_main)
target_compile_definitions(mcp-process-tests PRIVATE
    MCP_PROCESS_TEST_HELPER_PATH="${CMAKE_CURRENT_BINARY_DIR}/mcp-process-test-helper${CMAKE_EXECUTABLE_SUFFIX}")
mcp_discover_tests(mcp-process-tests)

add_executable(mcp-net-tests
    NetStackTests.cpp
    WebSocketClientTests.cpp)
target_link_libraries(mcp-net-tests PRIVATE mcp-transport mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-net-tests)

add_executable(mcp-http-tests HttpServerTests.cpp)
target_link_libraries(mcp-http-tests PRIVATE mcp-http mcp-protocol mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-http-tests)

add_executable(mcp-message-filter-tests MessageFilterTests.cpp)
target_link_libraries(mcp-message-filter-tests PRIVATE mcp-protocol mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-message-filter-tests)

add_executable(mcp-token-cache-tests FileTokenCacheTests.cpp)
target_link_libraries(mcp-token-cache-tests PRIVATE mcp-client mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-token-cache-tests)

add_executable(mcp-task-store-tests FileTaskStoreTests.cpp)
target_link_libraries(mcp-task-store-tests PRIVATE mcp-server mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-task-store-tests)

add_executable(mcp-event-store-tests FileEventStoreTests.cpp)
target_link_libraries(mcp-event-store-tests PRIVATE mcp-http mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-event-store-tests)

add_executable(mcp-streamable-http-tests StreamableHttpTransportTests.cpp SessionStoreTests.cpp)
target_link_libraries(mcp-streamable-http-tests PRIVATE mcp-http mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-streamable-http-tests)

add_executable(mcp-websocket-tests WebSocketTransportTests.cpp)
target_link_libraries(mcp-websocket-tests PRIVATE mcp-transport mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-websocket-tests)

add_executable(mcp-uri-template-tests UriTemplateTests.cpp)
target_link_libraries(mcp-uri-template-tests PRIVATE mcp-core mcp-test-support GTest::gtest_main)
mcp_discover_tests(mcp-uri-template-tests)
