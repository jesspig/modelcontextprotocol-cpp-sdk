include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# 代理设置 — 仅从环境变量读取，不做自动探测
if(DEFINED ENV{HTTP_PROXY})
    message(STATUS "[mcp] Using proxy from environment: $ENV{HTTP_PROXY}")
endif()

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
# libhv's install(FILES ... DESTINATION include/hv) is replaced with file(COPY ...)
# to generate the include/hv/ directory at configure time instead of install time.
# This ensures hv_static's BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include resolves.
FetchContent_GetProperties(libhv)
if(NOT libhv_POPULATED)
    cmake_policy(PUSH)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(libhv)
    cmake_policy(POP)
    file(READ "${libhv_SOURCE_DIR}/CMakeLists.txt" _hv_cmake)
    string(REGEX REPLACE
        "\n *install\\(FILES \\$\\{LIBHV_HEADERS\\} DESTINATION include/hv\\)"
        "\nfile(COPY \${LIBHV_HEADERS} DESTINATION \${CMAKE_CURRENT_SOURCE_DIR}/include/hv)"
        _hv_cmake "${_hv_cmake}")
    file(WRITE "${libhv_SOURCE_DIR}/CMakeLists.txt" "${_hv_cmake}")
endif()
FetchContent_MakeAvailable(libhv)
if(NOT TARGET hv_static)
    add_subdirectory("${libhv_SOURCE_DIR}" "${libhv_BINARY_DIR}")
endif()
set(_hv_include "${libhv_SOURCE_DIR}/include/hv")
if(EXISTS "${libhv_SOURCE_DIR}" AND NOT EXISTS "${_hv_include}")
    file(MAKE_DIRECTORY "${_hv_include}")
    foreach(_dir "" base ssl event util cpputil http http/server http/client)
        file(GLOB _files "${libhv_SOURCE_DIR}/${_dir}/*.h" "${libhv_SOURCE_DIR}/${_dir}/*.hpp")
        list(APPEND _hv_all ${_files})
    endforeach()
    file(COPY ${_hv_all} DESTINATION "${_hv_include}")
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
