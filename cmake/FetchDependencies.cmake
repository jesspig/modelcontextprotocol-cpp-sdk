# ====================================================================
# mcp-cpp-sdk 依赖管理 (FetchContent)
# ====================================================================
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# 共享依赖缓存目录 — 所有 preset 共用，删 build/ 不丢
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.cache/deps" CACHE PATH
    "FetchContent base directory (shared across all presets)")

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
cmake_policy(SET CMP0169 OLD)
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
# OpenSSL — 用于 PKCE SHA-256 + 加密随机数 (OAuth, 可选)
# ====================================================================
find_package(OpenSSL QUIET)

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
