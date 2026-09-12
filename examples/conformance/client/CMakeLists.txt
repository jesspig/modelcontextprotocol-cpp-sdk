add_executable(conformance-client main.cpp)
target_link_libraries(conformance-client PRIVATE mcp-client mcp-http)
# clang-cl 静态链接时使用 LTCG 确保所有符号被包含
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32)
    target_link_options(conformance-client PRIVATE "LINKER:/LTCG")
endif()
