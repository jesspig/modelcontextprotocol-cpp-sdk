# ====================================================================
# mcp-cpp-sdk 编译器选项
# ====================================================================

# — MSVC —
set(MCP_MSVC_WARNINGS
    /W4
    /wd4324  # 结构被填充为对齐
    /utf-8)

# — GCC / Clang —
set(MCP_GCC_CLANG_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter)

# — 按编译器应用 —
if(MSVC)
    add_compile_options(${MCP_MSVC_WARNINGS})
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(${MCP_GCC_CLANG_WARNINGS})
endif()

# — ASIO 全局定义 —
add_compile_definitions(ASIO_STANDALONE)
add_compile_definitions(ASIO_NO_DEPRECATED)

# — Position Independent Code —
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
