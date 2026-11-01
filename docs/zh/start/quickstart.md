# 快速上手

五分钟跑通一个服务端与一个客户端：注册一个工具、列出工具、调用工具并读回结果。全程在同一个进程里用内存传输，不需要外部宿主。

## 前置条件

- 已按[安装与集成](/zh/start/installation)配好 CMake 3.28+ 与 Ninja。
- 本仓库源码检出（示例程序默认不构建，需要显式打开开关）。

## 一、跑通现成的示例

```bash
cmake --preset debug -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug --target SimpleClient
./build/debug/examples/SimpleClient/SimpleClient        # Windows 下为 SimpleClient.exe
```

期望输出（实测）：

```text
Server: EchoServer v1.0.0
Protocol: 2026-07-28
Available tools:
  - echo: Echo input

Calling echo tool with 'Hello, MCP!'...
Response: Hello, MCP!

Done!
```

注意第二行：进程内的 `InMemoryTransport` 配对让客户端以默认的 `Auto` 档位先探测 `server/discover`，成功即协商到现代时代（`2026-07-28`）。若握手走的是 `initialize` 路径，协商结果只会落在旧时代（最高 `2025-11-25`），详见[协议版本](/zh/learn/protocol-versions)。

## 二、自己写一个最小服务端

```cpp
#include <mcp/Content.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/StdioServerTransport.hpp>

using namespace mcp;

int main() {
    ServerOptions options;
    options.server_info = Implementation{"my-server", "1.0.0"};

    auto server = McpServer::Create(std::make_shared<StdioServerTransport>(), options);

    server->RegisterTool(
        "greet",
        ToolOptions{}.Description("Say hello"),
        [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "hello"});
            return result;
        });

    server->Run();
    return 0;
}
```

要点：

- `McpServer::Create` 返回 `std::unique_ptr`，传输用 `std::shared_ptr<ITransport>` 传入。
- `ToolOptions{}.Description(...)` 是可链式 setter，其余字段直接赋值。
- `Run()` 阻塞在服务循环里，`Close()` 才退出；在 IO 线程的回调里调 `Close()` 会 join 自己，不要那么做。
- 完整注册形态（资源、模板、提示、技能）见[注册一个工具](/zh/howto/register-a-tool)与[服务端参考](/zh/reference/server)。

## 三、连上它

- stdio：把你的可执行文件配进任意 MCP 宿主（Claude Desktop、IDE 插件等），命令指向带 `.exe` 的产物路径。
- 端到端自测：`examples/SimpleClient/main.cpp` 用 `InMemoryTransport::CreatePair()` 在一个进程里同时起两端，是最快的调试形态。
- 进程外验证：`cmake --build --preset debug --target EchoServer` 后接官方 MCP Inspector 连 stdio。

## 四、验证你改动了什么

```bash
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

期望：构建无错误，全量用例通过（当前 18 个测试目标、Windows 本机注册 982 例）。改完示例只验证一个目标时用 `cmake --build --preset debug --target SimpleClient`。

## 常见错误

| 现象 | 成因 | 处置 |
|---|---|---|
| `No rule to make target 'SimpleClient'` | 没带 `-DMCP_BUILD_EXAMPLES=ON` | 重新 configure 后只构建该目标 |
| `cannot open file '.../SimpleClient'` | Windows 下忘了 `.exe`，或预设产物路径不是 `build/debug` | 按平台补后缀 |
| 宿主连不上、无输出 | 服务端没往 stdout 写日志的习惯，或 stdio 帧被自定义日志污染 | 内部日志默认全关；调试用 `MCP_LOG_LEVEL=4`，别把业务日志打到 stdout |
| 工具调用回 `is_error` + `invalid arguments: ...` | 入参不合 `input_schema`，SDK 在调用 handler 前就拒了 | 见[校验工具入参](/zh/howto/validate-tool-input) |

## 下一步

- 系统学习服务端能力：[注册一个工具](/zh/howto/register-a-tool)、[暴露资源](/zh/howto/serve-a-resource)、[向用户收集输入](/zh/howto/collect-user-input)
- 传输选型：[传输层概念](/zh/learn/transports)
- 签名与字段：[服务端参考](/zh/reference/server)、[客户端参考](/zh/reference/client)

## 本页对应源码

- `examples/SimpleClient/main.cpp`、`examples/EchoServer/main.cpp`、`include/mcp/server/McpServer.hpp`、`include/mcp/transport/InMemoryTransport.hpp`、`CMakePresets.json`
