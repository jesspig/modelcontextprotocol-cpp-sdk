# 自研测试框架核心：静态库 mcp-test（断言/注册/运行）与 mcp-test-main（main 入口）
include(McpDiscoverTests)

add_library(mcp-test STATIC
    src/McpTestRunner.cpp)
target_include_directories(mcp-test PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(mcp-test-main STATIC
    src/McpTestMain.cpp)
target_link_libraries(mcp-test-main PUBLIC mcp-test)

add_executable(mcp-framework-self-tests SelfTests.cpp)
target_link_libraries(mcp-framework-self-tests PRIVATE mcp-test-main)
mcp_discover_tests(mcp-framework-self-tests)
