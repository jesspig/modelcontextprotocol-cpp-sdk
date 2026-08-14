// McpTestMain.cpp — 测试可执行文件入口

#include <mcp/test/McpTest.hpp>

int main(int argc, char** argv) {
    return mcp::test::RunAll(argc, argv);
}
