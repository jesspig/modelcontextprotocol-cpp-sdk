add_executable(EchoServer main.cpp)
target_link_libraries(EchoServer PRIVATE mcp-server)
# clang-cl 静态链接时使用 LTCG 确保所有符号被包含
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32)
    target_link_options(EchoServer PRIVATE "LINKER:/LTCG")
endif()
