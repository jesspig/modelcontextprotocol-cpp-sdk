find_program(MCP_SCCACHE NAMES sccache)
find_program(MCP_CCACHE NAMES ccache)

set(_cache_program "")

if(MCP_SCCACHE)
    set(_cache_program "${MCP_SCCACHE}")
elseif(NOT MSVC AND MCP_CCACHE)
    set(_cache_program "${MCP_CCACHE}")
endif()

if(_cache_program)
    set(CMAKE_C_COMPILER_LAUNCHER "${_cache_program}" CACHE INTERNAL "")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${_cache_program}" CACHE INTERNAL "")
    get_filename_component(_cache_name "${_cache_program}" NAME)
    message(STATUS "[mcp] Cache: ${_cache_name} (${_cache_program})")
else()
    message(STATUS "[mcp] Cache: none found (direct compilation)")
endif()
