add_executable(mcp-json-bench JsonParseBench.cpp)
target_link_libraries(mcp-json-bench PRIVATE mcp-core)

add_executable(mcp-http-bench HttpBench.cpp)
target_link_libraries(mcp-http-bench PRIVATE mcp-http)
target_include_directories(mcp-http-bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
