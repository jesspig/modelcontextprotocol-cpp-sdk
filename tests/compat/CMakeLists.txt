add_executable(mcp-compat-runtime EXCLUDE_FROM_ALL previous_public_api.cpp)
target_compile_definitions(mcp-compat-runtime PRIVATE MCP_COMPAT_RUNTIME=1)
target_link_libraries(mcp-compat-runtime PRIVATE mcp-core)

add_custom_target(mcp-compat-smoke
    COMMAND ${CMAKE_COMMAND}
        -DCOMPAT_COMPILER=${CMAKE_CXX_COMPILER}
        -DCOMPAT_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}
        -DCOMPAT_SIMULATE_ID=${CMAKE_CXX_SIMULATE_ID}
        -DCOMPAT_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/previous_public_api.cpp
        -DCOMPAT_INCLUDE_DIR=${PROJECT_SOURCE_DIR}/include
        -DCOMPAT_ALLOWLIST=${CMAKE_CURRENT_SOURCE_DIR}/expected_breaks.txt
        -DCOMPAT_RUNTIME=$<TARGET_FILE:mcp-compat-runtime>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_compat_smoke.cmake
    DEPENDS mcp-compat-runtime
    VERBATIM)
