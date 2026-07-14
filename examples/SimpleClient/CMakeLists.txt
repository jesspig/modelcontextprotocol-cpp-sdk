add_executable(SimpleClient main.cpp)
target_link_libraries(SimpleClient PRIVATE mcp-client mcp-server)
