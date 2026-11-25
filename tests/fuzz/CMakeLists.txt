if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "[mcp] fuzz harnesses require Clang or GNU with libFuzzer")
endif()

include_directories(${CMAKE_SOURCE_DIR}/src)

function(mcp_add_fuzz_target target_name)
    add_executable(${target_name} ${ARGN})
    target_compile_options(${target_name} PRIVATE -fsanitize=fuzzer -fno-omit-frame-pointer)
    target_link_options(${target_name} PRIVATE -fsanitize=fuzzer)
    target_link_libraries(${target_name} PRIVATE mcp-core mcp-transport)
endfunction()

mcp_add_fuzz_target(mcp-fuzz-parser ParserFuzz.cpp)
mcp_add_fuzz_target(mcp-fuzz-http HttpFuzz.cpp)
mcp_add_fuzz_target(mcp-fuzz-sse SseFuzz.cpp)
mcp_add_fuzz_target(mcp-fuzz-websocket WebSocketFuzz.cpp)
mcp_add_fuzz_target(mcp-fuzz-schema SchemaFuzz.cpp)
