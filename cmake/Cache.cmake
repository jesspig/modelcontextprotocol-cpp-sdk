# ====================================================================
# Compiler cache: sccache / ccache auto-detection.
#   sccache — supports MSVC + GCC/Clang
#   ccache — GCC/Clang only (rejected on MSVC)
# ====================================================================

set(MCP_CACHE "auto" CACHE STRING
    "Compiler cache: auto, sccache, ccache, off")
set_property(CACHE MCP_CACHE PROPERTY STRINGS auto sccache ccache off)

if(MCP_CACHE STREQUAL "off")
    message(STATUS "[mcp] Cache: disabled")
    return()
endif()

find_program(MCP_SCCACHE NAMES sccache)
find_program(MCP_CCACHE NAMES ccache)

set(_cache_program "")

if(MCP_CACHE STREQUAL "sccache")
    if(MCP_SCCACHE)
        set(_cache_program "${MCP_SCCACHE}")
    else()
        message(WARNING "[mcp] sccache requested but not found")
    endif()

elseif(MCP_CACHE STREQUAL "ccache")
    if(MSVC)
        message(WARNING "[mcp] ccache does not support MSVC; ignored")
    elseif(MCP_CCACHE)
        set(_cache_program "${MCP_CCACHE}")
    else()
        message(WARNING "[mcp] ccache requested but not found")
    endif()

else()
    # auto: prefer sccache (supports MSVC), fallback ccache (GCC/Clang only)
    if(MCP_SCCACHE)
        set(_cache_program "${MCP_SCCACHE}")
    elseif(NOT MSVC AND MCP_CCACHE)
        set(_cache_program "${MCP_CCACHE}")
    endif()
endif()

if(_cache_program)
    set(CMAKE_C_COMPILER_LAUNCHER "${_cache_program}" CACHE INTERNAL "")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_cache_program}" CACHE INTERNAL "")
    get_filename_component(_cache_name "${_cache_program}" NAME)
    message(STATUS "[mcp] Cache: ${_cache_name} (${_cache_program})")
else()
    message(STATUS "[mcp] Cache: none found (direct compilation)")
endif()
