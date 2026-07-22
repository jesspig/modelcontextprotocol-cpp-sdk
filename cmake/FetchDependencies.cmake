include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# 代理设置
if(DEFINED ENV{HTTP_PROXY})
    message(STATUS "[mcp] Using proxy from environment: $ENV{HTTP_PROXY}")
elseif(EXISTS "$ENV{LOCALAPPDATA}/Clash")
    set(ENV{HTTP_PROXY} "http://127.0.0.1:10808")
    set(ENV{HTTPS_PROXY} "http://127.0.0.1:10808")
    message(STATUS "[mcp] Auto-detected proxy: 127.0.0.1:10808")
endif()

# ====================================================================
# simdjson — JSON 解析 (内部使用，不暴露于公共 API)
# ====================================================================
FetchContent_Declare(simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        v3.12.3
    GIT_SHALLOW    TRUE)
set(SIMDJSON_JUST_LIBRARY ON CACHE BOOL "" FORCE)
set(SIMDJSON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson)

# ====================================================================
# OpenSSL — 提前探测，影响 libhv 的 TLS 选项
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
# libhv — HTTP 客户端 + 事件循环 (编译型库)
# ====================================================================
set(HV_OPENSSL ${MCP_OPENSSL_FOUND} CACHE BOOL "Enable OpenSSL in libhv")
FetchContent_Declare(libhv
    GIT_REPOSITORY https://github.com/ithewei/libhv.git
    GIT_TAG        v1.3.4
    GIT_SHALLOW    TRUE)
set(WITH_OPENSSL ${MCP_OPENSSL_FOUND} CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
# libhv 的 file(INSTALL ...) 在 configure 时会打印 ~90 行 "Up-to-date"
# 在 MakeAvailable 前打补丁移除这两行
FetchContent_GetProperties(libhv)
if(NOT libhv_POPULATED)
    cmake_policy(PUSH)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(libhv)
    cmake_policy(POP)
    file(READ "${libhv_SOURCE_DIR}/CMakeLists.txt" _hv_cmake)
    string(REGEX REPLACE "\nfile\\(INSTALL \\$\\{LIBHV_HEADERS\\} DESTINATION [^\n]+\\)" "" _hv_cmake "${_hv_cmake}")
    file(WRITE "${libhv_SOURCE_DIR}/CMakeLists.txt" "${_hv_cmake}")
endif()
FetchContent_MakeAvailable(libhv)

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
