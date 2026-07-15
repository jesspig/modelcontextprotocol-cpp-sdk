# ====================================================================
# Platform / Architecture / CI detection
# ====================================================================

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MCP_ARCH "x86_64")
else()
    set(MCP_ARCH "x86")
endif()

if(DEFINED ENV{CI})
    set(MCP_IS_CI ON)
    message(STATUS "[mcp] CI environment detected")
else()
    set(MCP_IS_CI OFF)
endif()

message(STATUS "[mcp] Platform: ${CMAKE_SYSTEM_NAME} ${MCP_ARCH}")
message(STATUS "[mcp] Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
