add_executable(conformance-server
    main.cpp
    ContentTools.cpp
    InteractionTools.cpp
    MrtrTools.cpp
    ResourcesAndPrompts.cpp
)
target_link_libraries(conformance-server PRIVATE mcp-server mcp-http)
# clang-cl 静态链接时使用 LTCG 确保所有符号被包含
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND WIN32)
    target_link_options(conformance-server PRIVATE "LINKER:/LTCG")
endif()
