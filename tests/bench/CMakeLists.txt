add_executable(mcp-transport-bench TransportBench.cpp)
target_link_libraries(mcp-transport-bench PRIVATE mcp-transport mcp-core)
