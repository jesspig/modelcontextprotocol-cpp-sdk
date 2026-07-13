# ====================================================================
# LTO (Link-Time Optimization) — Release builds only.
# 优先级: Clang ThinLTO > MSVC LTCG > GCC IPO
# ====================================================================

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(STATUS "[mcp] LTO: skipped (non-Release build)")
    return()
endif()

set(MCP_LTO_MODE "auto" CACHE STRING
    "LTO mode: auto (detect best), thin (ThinLTO), full (Full LTO)")
set_property(CACHE MCP_LTO_MODE PROPERTY STRINGS auto thin full)

set(MCP_LTO "OFF" CACHE INTERNAL "")

# ── Clang (including clang-cl on Windows): ThinLTO ──
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
    AND (MCP_LTO_MODE STREQUAL "auto" OR MCP_LTO_MODE STREQUAL "thin"))
    add_compile_options($<$<CONFIG:Release>:-flto=thin>)
    add_link_options($<$<CONFIG:Release>:-flto=thin>)
    set(MCP_LTO "ON (ThinLTO)" CACHE INTERNAL "")
    message(STATUS "[mcp] LTO: thin (Clang ThinLTO, Release only)")
    return()
endif()

# ── MSVC: /GL + /LTCG (cl.exe only, clang-cl already handled above) ──
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
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
