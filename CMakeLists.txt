cmake_minimum_required(VERSION 4.2)

# ── 编译器自动探测 (project 之前, 不影响预设显式指定) ──
if(WIN32 AND NOT DEFINED CACHE{CMAKE_CXX_COMPILER})
    find_program(MCP_CLANG_CL NAMES clang-cl)
    if(MCP_CLANG_CL)
        set(CMAKE_C_COMPILER "${MCP_CLANG_CL}")
        set(CMAKE_CXX_COMPILER "${MCP_CLANG_CL}")
        set(CMAKE_CXX_FLAGS "--target=x86_64-pc-windows-msvc" CACHE STRING "")
        message(STATUS "[mcp] Auto-detected: clang-cl (${MCP_CLANG_CL})")
    else()
        message(STATUS "[mcp] No explicit compiler set, using system default")
    endif()
endif()

project(mcp-cpp-sdk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 模块包含 ──
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Platform)              # 平台/架构/CI 检测
include(CompilerOptions)       # 编译器特定标志
include(BuildOptimization)     # Unity batch + Ninja pools (CPU+memory aware)
include(Cache)                 # sccache / ccache
include(FetchDependencies)     # FetchContent 依赖
include(Lto)                   # LTO (Release 仅)

# ── 链接器优化: lld-link — MSVC 下显著加速 ──
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_program(MCP_LLD_LINK NAMES lld-link)
    if(MCP_LLD_LINK)
        add_link_options("/lldlink")
        message(STATUS "[mcp] Linker: lld-link (${MCP_LLD_LINK})")
    else()
        message(STATUS "[mcp] Linker: default (lld-link not found, using link.exe)")
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "[mcp] Linker: lld (native, clang)")
endif()

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
    src/transport/SseClientTransport.cpp
    src/transport/InMemoryTransport.cpp)
target_link_libraries(mcp-transport PUBLIC mcp-core asio::asio)
set_property(TARGET mcp-transport PROPERTY UNITY_BUILD ${MCP_UNITY_ENABLED})
set_property(TARGET mcp-transport PROPERTY UNITY_BUILD_BATCH_SIZE ${MCP_UNITY_BATCH})

# ====================================================================
# mcp-protocol — STATIC
# ====================================================================
add_library(mcp-protocol STATIC)
target_sources(mcp-protocol PRIVATE
    src/protocol/Protocol.cpp
    src/protocol/WireCodec.cpp)
target_link_libraries(mcp-protocol PUBLIC mcp-transport)
set_property(TARGET mcp-protocol PROPERTY UNITY_BUILD ${MCP_UNITY_ENABLED})
set_property(TARGET mcp-protocol PROPERTY UNITY_BUILD_BATCH_SIZE ${MCP_UNITY_BATCH})

# ====================================================================
# mcp-server — STATIC
# ====================================================================
add_library(mcp-server STATIC)
target_sources(mcp-server PRIVATE
    src/server/McpServer.cpp)
target_link_libraries(mcp-server PUBLIC mcp-protocol)
set_property(TARGET mcp-server PROPERTY UNITY_BUILD ${MCP_UNITY_ENABLED})
set_property(TARGET mcp-server PROPERTY UNITY_BUILD_BATCH_SIZE ${MCP_UNITY_BATCH})

# ====================================================================
# mcp-client — STATIC
# ====================================================================
add_library(mcp-client STATIC)
target_sources(mcp-client PRIVATE
    src/client/McpClient.cpp)
target_link_libraries(mcp-client PUBLIC mcp-protocol)
set_property(TARGET mcp-client PROPERTY UNITY_BUILD ${MCP_UNITY_ENABLED})
set_property(TARGET mcp-client PROPERTY UNITY_BUILD_BATCH_SIZE ${MCP_UNITY_BATCH})
set_property(TARGET mcp-client PROPERTY UNITY_BUILD_UNIQUE_ID ON)

# ====================================================================
# mcp-http — STATIC
# ====================================================================
add_library(mcp-http STATIC)
target_sources(mcp-http PRIVATE
    src/http/HttpServer.cpp
    src/http/EventStore.cpp
    src/http/StreamableHttpServerTransport.cpp)
target_link_libraries(mcp-http PUBLIC mcp-transport)
set_property(TARGET mcp-http PROPERTY UNITY_BUILD ${MCP_UNITY_ENABLED})
set_property(TARGET mcp-http PROPERTY UNITY_BUILD_BATCH_SIZE ${MCP_UNITY_BATCH})

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

# ====================================================================
# 构建配置摘要
# ====================================================================
file(WRITE "${CMAKE_BINARY_DIR}/build_config.txt"
    "version=${PROJECT_VERSION}\n"
    "build_type=${CMAKE_BUILD_TYPE}\n"
    "generator=${CMAKE_GENERATOR}\n"
    "compiler_id=${CMAKE_CXX_COMPILER_ID}\n"
    "compiler_launcher=${CMAKE_CXX_COMPILER_LAUNCHER}\n"
    "lto=${MCP_LTO}\n"
    "unity_enabled=${MCP_UNITY_ENABLED}\n"
    "unity_batch=${MCP_UNITY_BATCH}\n"
)
