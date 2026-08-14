include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# 代理设置 — 仅从环境变量读取，不做自动探测
if(DEFINED ENV{HTTP_PROXY})
    message(STATUS "[mcp] Using proxy from environment: $ENV{HTTP_PROXY}")
endif()

# ====================================================================
# OpenSSL — TLS 与 PKCE 随机数（find_package 探测）
# ====================================================================
if(WIN32 AND NOT DEFINED OPENSSL_ROOT_DIR)
    file(GLOB _mcp_openssl_dirs "C:/Program Files/OpenSSL*")
    if(_mcp_openssl_dirs)
        list(SORT _mcp_openssl_dirs ORDER DESCENDING)
        list(GET _mcp_openssl_dirs 0 _mcp_openssl_root)
        set(OPENSSL_ROOT_DIR "${_mcp_openssl_root}")
        message(STATUS "[mcp] Auto-detected OpenSSL root: ${_mcp_openssl_root}")
    endif()
endif()
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "[mcp] OpenSSL found: ${OPENSSL_VERSION}")
    set(MCP_OPENSSL_FOUND ON)
else()
    message(STATUS "[mcp] OpenSSL not found — TLS disabled, PKCE uses built-in SHA-256")
    set(MCP_OPENSSL_FOUND OFF)
endif()
