# ====================================================================
# mcp-cpp-sdk 依赖管理 (FetchContent)
# 使用 CMake 默认缓存目录 (build/<preset>/_deps)，无需全局共享缓存
# ====================================================================
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ====================================================================
# nlohmann/json — JSON 解析/序列化
# ====================================================================
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    SYSTEM)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_CI OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ====================================================================
# asio — 异步 I/O (standalone, header-only)
# 注: asio 仓库没有 CMakeLists.txt，需要手动创建接口目标
# ====================================================================
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-30-2
    GIT_SHALLOW    TRUE
    SYSTEM)

cmake_policy(PUSH)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(asio)
if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
    add_library(asio INTERFACE)
    target_include_directories(asio INTERFACE
        "${asio_SOURCE_DIR}/asio/include")
    add_library(asio::asio ALIAS asio)
endif()
cmake_policy(POP)

# ====================================================================
# OpenSSL — 用于 TLS (OAuth, WebSocket, SSE POSIX)
# 可选依赖: find_package 找到即启用，找不到则使用纯 C++ SHA-256 降级
# PKCE 所需的 SHA-256 已有纯 C++ 实现 (mcp/detail/sha256.hpp)，不依赖 OpenSSL
# ====================================================================
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "[mcp] OpenSSL found: ${OPENSSL_VERSION}")
else()
    message(STATUS "[mcp] OpenSSL not found — TLS features disabled, PKCE uses built-in SHA-256")
endif()

# ====================================================================
# googletest — 单元测试 (仅在构建测试时获取)
# ====================================================================
if(MCP_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()
