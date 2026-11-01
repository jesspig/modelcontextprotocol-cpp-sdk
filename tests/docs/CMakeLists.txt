file(GLOB MCP_DOC_SNIPPET_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/docs/snippets/*.cpp")

add_custom_target(mcp-docs-snippets
    COMMAND "${CMAKE_COMMAND}" -E echo "[mcp-docs] snippet syntax check finished")

if(NOT MCP_DOC_SNIPPET_SOURCES)
    message(STATUS "[mcp-docs] docs/snippets 下无 .cpp，mcp-docs-snippets 为空目标")
    return()
endif()

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(MCP_DOC_SNIPPET_CHECK_FLAGS /Zs)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(MCP_DOC_SNIPPET_CHECK_FLAGS /Zs)
else()
    set(MCP_DOC_SNIPPET_CHECK_FLAGS -fsyntax-only)
endif()

# /Zs 会让 clang-cl 报 -c 为无用参数；开 MCP_WERROR 时这会变成硬错误
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(MCP_DOC_SNIPPET_EXTRA_FLAGS -Wno-unused-command-line-argument)
else()
    set(MCP_DOC_SNIPPET_EXTRA_FLAGS "")
endif()

foreach(MCP_DOC_SNIPPET_SOURCE IN LISTS MCP_DOC_SNIPPET_SOURCES)
    get_filename_component(MCP_DOC_SNIPPET_NAME "${MCP_DOC_SNIPPET_SOURCE}" NAME_WE)
    add_library(mcp-doc-snippet-${MCP_DOC_SNIPPET_NAME} OBJECT EXCLUDE_FROM_ALL "${MCP_DOC_SNIPPET_SOURCE}")
    target_link_libraries(mcp-doc-snippet-${MCP_DOC_SNIPPET_NAME} PRIVATE ${MCP_LIBRARY_TARGETS})
    target_compile_features(mcp-doc-snippet-${MCP_DOC_SNIPPET_NAME} PRIVATE cxx_std_17)
    target_compile_options(mcp-doc-snippet-${MCP_DOC_SNIPPET_NAME} PRIVATE ${MCP_DOC_SNIPPET_CHECK_FLAGS} ${MCP_DOC_SNIPPET_EXTRA_FLAGS})
    set_property(TARGET mcp-doc-snippet-${MCP_DOC_SNIPPET_NAME} PROPERTY UNITY_BUILD OFF)
    set_property(TARGET mcp-doc-snippet-${MCP_DOC_SNIPPET_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
    add_dependencies(mcp-docs-snippets mcp-doc-snippet-${MCP_DOC_SNIPPET_NAME})
endforeach()

list(LENGTH MCP_DOC_SNIPPET_SOURCES MCP_DOC_SNIPPET_COUNT)
message(STATUS "[mcp-docs] syntax-checking ${MCP_DOC_SNIPPET_COUNT} snippet(s) with '${MCP_DOC_SNIPPET_CHECK_FLAGS}'")
