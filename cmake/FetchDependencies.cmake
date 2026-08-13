include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# 代理设置 — 仅从环境变量读取，不做自动探测
if(DEFINED ENV{HTTP_PROXY})
    message(STATUS "[mcp] Using proxy from environment: $ENV{HTTP_PROXY}")
endif()

# ====================================================================
# OpenSSL — TLS 与 PKCE 随机数（find_package 探测）
# ====================================================================
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "[mcp] OpenSSL found: ${OPENSSL_VERSION}")
    set(MCP_OPENSSL_FOUND ON)
else()
    message(STATUS "[mcp] OpenSSL not found — TLS disabled, PKCE uses built-in SHA-256")
    set(MCP_OPENSSL_FOUND OFF)
endif()

# ====================================================================
# googletest — 单元测试
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
