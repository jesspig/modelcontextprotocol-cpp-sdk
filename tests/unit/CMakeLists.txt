include(McpDiscoverTests)

add_executable(mcp-core-tests
    JsonRpcTests.cpp
    McpTypesTests.cpp
    JsonParserTests.cpp)

target_link_libraries(mcp-core-tests PRIVATE mcp-core mcp-test-main)
target_include_directories(mcp-core-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
mcp_discover_tests(mcp-core-tests)

add_executable(mcp-wire-codec-tests
    WireCodecTests.cpp
    SessionHandlerTests.cpp)
target_link_libraries(mcp-wire-codec-tests PRIVATE mcp-protocol mcp-test-main)
mcp_discover_tests(mcp-wire-codec-tests)

add_executable(mcp-server-tests McpServerTests.cpp)
target_link_libraries(mcp-server-tests PRIVATE mcp-server mcp-test-main)
mcp_discover_tests(mcp-server-tests)

add_executable(mcp-client-tests McpClientTests.cpp)
target_link_libraries(mcp-client-tests PRIVATE mcp-client mcp-server mcp-test-main)
mcp_discover_tests(mcp-client-tests)

add_executable(mcp-oauth-tests OAuthTests.cpp)
target_link_libraries(mcp-oauth-tests PRIVATE mcp-client mcp-http mcp-test-main)
target_include_directories(mcp-oauth-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
mcp_discover_tests(mcp-oauth-tests)

add_executable(mcp-transport-tests TransportTests.cpp)
target_link_libraries(mcp-transport-tests PRIVATE mcp-transport mcp-test-main)
mcp_discover_tests(mcp-transport-tests)

add_executable(mcp-net-tests
    NetStackTests.cpp
    WebSocketClientTests.cpp)
target_link_libraries(mcp-net-tests PRIVATE mcp-transport mcp-test-main)
target_include_directories(mcp-net-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
mcp_discover_tests(mcp-net-tests)

add_executable(mcp-http-tests HttpServerTests.cpp)
target_link_libraries(mcp-http-tests PRIVATE mcp-http mcp-protocol mcp-test-main)
target_include_directories(mcp-http-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
mcp_discover_tests(mcp-http-tests)

add_executable(mcp-message-filter-tests MessageFilterTests.cpp)
target_link_libraries(mcp-message-filter-tests PRIVATE mcp-protocol mcp-test-main)
mcp_discover_tests(mcp-message-filter-tests)

add_executable(mcp-token-cache-tests FileTokenCacheTests.cpp)
target_link_libraries(mcp-token-cache-tests PRIVATE mcp-client mcp-test-main)
mcp_discover_tests(mcp-token-cache-tests)

add_executable(mcp-task-store-tests FileTaskStoreTests.cpp)
target_link_libraries(mcp-task-store-tests PRIVATE mcp-server mcp-test-main)
mcp_discover_tests(mcp-task-store-tests)

add_executable(mcp-streamable-http-tests StreamableHttpTransportTests.cpp)
target_link_libraries(mcp-streamable-http-tests PRIVATE mcp-http mcp-test-main)
mcp_discover_tests(mcp-streamable-http-tests)

add_executable(mcp-websocket-tests WebSocketTransportTests.cpp)
target_link_libraries(mcp-websocket-tests PRIVATE mcp-transport mcp-test-main)
mcp_discover_tests(mcp-websocket-tests)
