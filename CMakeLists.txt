cmake_minimum_required(VERSION 4.2)
project(mcp-cpp-sdk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 模块包含 ──
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(CompilerOptions)
include(FetchDependencies)

# ── Unity Build 全局默认 ──
set(CMAKE_UNITY_BUILD_DEFAULT ON)
set(CMAKE_UNITY_BUILD_BATCH_SIZE 8)

# ====================================================================
# mcp-core — INTERFACE, header-only
# ====================================================================
add_library(mcp-core INTERFACE)
target_include_directories(mcp-core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(mcp-core INTERFACE nlohmann_json::nlohmann_json)

# ====================================================================
# mcp-transport — STATIC
# ====================================================================
add_library(mcp-transport STATIC)
target_sources(mcp-transport PRIVATE
    src/transport/StdioServerTransport.cpp
    src/transport/StdioClientTransport.cpp
    src/transport/StreamableHttpServerTransport.cpp
    src/transport/StreamableHttpClientTransport.cpp
    src/transport/SseClientTransport.cpp)
target_link_libraries(mcp-transport PUBLIC mcp-core asio::asio)
set_property(TARGET mcp-transport PROPERTY UNITY_BUILD ON)

# ====================================================================
# mcp-protocol — STATIC
# ====================================================================
add_library(mcp-protocol STATIC)
target_sources(mcp-protocol PRIVATE
    src/protocol/Protocol.cpp
    src/protocol/WireCodec.cpp)
target_link_libraries(mcp-protocol PUBLIC mcp-transport)
set_property(TARGET mcp-protocol PROPERTY UNITY_BUILD ON)

# ====================================================================
# mcp-server — STATIC
# ====================================================================
add_library(mcp-server STATIC)
target_sources(mcp-server PRIVATE
    src/server/McpServer.cpp
    src/server/McpServerTool.cpp)
target_link_libraries(mcp-server PUBLIC mcp-protocol)
set_property(TARGET mcp-server PROPERTY UNITY_BUILD ON)

# ====================================================================
# mcp-client — STATIC
# ====================================================================
add_library(mcp-client STATIC)
target_sources(mcp-client PRIVATE
    src/client/McpClient.cpp)
target_link_libraries(mcp-client PUBLIC mcp-protocol)
set_property(TARGET mcp-client PROPERTY UNITY_BUILD ON)
set_property(TARGET mcp-client PROPERTY UNITY_BUILD_UNIQUE_ID ON)

# ====================================================================
# mcp-http — STATIC (optional HTTP server integration)
# ====================================================================
add_library(mcp-http STATIC)
target_sources(mcp-http PRIVATE
    src/http/HttpServer.cpp
    src/http/EventStore.cpp)
target_link_libraries(mcp-http PUBLIC mcp-server)
set_property(TARGET mcp-http PROPERTY UNITY_BUILD ON)

# ====================================================================
# 子目录
# ====================================================================
if(MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── 安装规则 (开发阶段暂不启用) ──
# include(InstallRules)
