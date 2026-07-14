# ====================================================================
# LTO (Link-Time Optimization) — 全自动，Release 构建自动启用。
# 优先级: Clang ThinLTO > MSVC LTCG > GCC IPO
# ====================================================================

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "[mcp] LTO: skipped (non-Release build)")
    return()
endif()

set(MCP_LTO "OFF" CACHE INTERNAL "")

# ── Clang (including clang-cl on Windows): ThinLTO ──
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options($<$<CONFIG:Release>:-flto=thin>)
    add_link_options($<$<CONFIG:Release>:-flto=thin>)
    set(MCP_LTO "ON (ThinLTO)" CACHE INTERNAL "")
    message(STATUS "[mcp] LTO: thin (Clang ThinLTO, Release only)")
    return()
endif()

# ── MSVC cl.exe: /GL + /LTCG ──
if(MSVC)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(MCP_LTO "ON (LTCG)" CACHE INTERNAL "")
    message(STATUS "[mcp] LTO: LTCG (MSVC, Release only)")
    return()
endif()

# ── GCC / fallback IPO ──
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_ok)
if(_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(MCP_LTO "ON (IPO)" CACHE INTERNAL "")
    message(STATUS "[mcp] LTO: full (via IPO, Release only)")
else()
    message(STATUS "[mcp] LTO: skipped (not supported by compiler)")
endif()
