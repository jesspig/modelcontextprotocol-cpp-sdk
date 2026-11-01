# Simple 客户端逐段走读

`examples/SimpleClient` 在**一个进程里同时跑服务端与客户端**，用 `InMemoryTransport::CreatePair()` 把两端连起来，走一遍「协商 → 列工具 → 调用 → 收尾」的最短闭环。它是排查问题时最省事的复现骨架。源码：[`examples/SimpleClient/main.cpp`](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/examples/SimpleClient/main.cpp)。

## 这个示例演示什么

- 不依赖进程与网络：内存传输把 `McpServer` 与 `McpClient` 直连。
- 客户端侧四个读取动作：`GetServerInfo()`、`GetNegotiatedProtocolVersion()`、`ListTools()`、`CallTool()`。
- 服务端必须有自己的线程（`Run()` 阻塞），以及配套的收尾顺序。
- 异常路径也要把服务端线程收干净，不留 joinable 线程。

## 一进程两端的装配顺序

```cpp
auto transport_pair = InMemoryTransport::CreatePair();

ServerOptions srv_opts;
srv_opts.server_info = Implementation{"EchoServer", "1.0.0"};
server = McpServer::Create(
    std::move(transport_pair.server), srv_opts);
```

对应 `examples/SimpleClient/main.cpp:16-21`。`CreatePair()` 返回带 `.client` 与 `.server` 两个 `shared_ptr<ITransport>` 的 `Pair`（`include/mcp/transport/InMemoryTransport.hpp:10-18`），`McpServer::Create` 收的是移动进去的传输。

服务端注册了一个 `echo` 工具（`:23-35`），handler 里用仓库惯用写法读参：

```cpp
if (auto* v = ctx.Params().arguments->Find("text"); v && v->IsString())
    text = v->GetString();
```

注意它先判 `ctx.Params().arguments`（`std::optional<JsonValue>`）再 `Find`——`arguments` 为空时直接解引用就是未定义行为（`:28-31`）。

## 服务端跑在独立线程

```cpp
server_thread = std::thread([&server]() { server->Run(); });
```

对应 `examples/SimpleClient/main.cpp:37`。`Run()` 是阻塞式消息循环，客户端在同一线程上创建会死锁，因此示例把服务端丢进线程、主线程继续建客户端：

```cpp
ClientOptions cl_opts;
cl_opts.client_info = Implementation{"SimpleClient", "1.0.0"};
cl_opts.connect_mode = ConnectMode::Auto;

auto client = McpClient::Create(
    std::move(transport_pair.client), cl_opts);
```

对应 `:39-44`。`ConnectMode::Auto` 会先探测再协商（内存传输属于 stdio 类，探测失败回退 `initialize`，见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)）；`McpClient::Create` 内部完成 `initialize` 与 `notifications/initialized`，所以拿到的客户端已经是可用状态。

## 客户端读取协商结果

```cpp
std::cout << "Server: " << client->GetServerInfo().name
          << " v" << client->GetServerInfo().version << std::endl;
std::cout << "Protocol: " << client->GetNegotiatedProtocolVersion() << std::endl;
```

对应 `examples/SimpleClient/main.cpp:46-48`。这两个读取是**免费的**：值来自 `initialize` 结果的缓存，不会发请求。排查「谈成了哪个时代」时，先看 `GetNegotiatedProtocolVersion()`，再决定后面该按哪套语义读行为。

## 列工具与调用工具

```cpp
auto tools = client->ListTools();
std::cout << "Available tools:" << std::endl;
for (const auto& t : tools.tools) {
    std::cout << "  - " << t.name
              << (t.description ? ": " + *t.description : "")
              << std::endl;
}
```

对应 `examples/SimpleClient/main.cpp:50-56`。`ListTools()` 只取**一页**（`next_cursor` 有值时还有后续），要一次拿全用 `ListToolsAll()`；`t.description` 是 `std::optional<std::string>`，示例用三目拼接避免打印空串。

```cpp
JsonValue args((JsonValue::Object{{"text", JsonValue("Hello, MCP!")}}));
auto result = client->CallTool("echo", args);
for (const auto& content : result.content) {
    if (auto* text = std::get_if<TextContent>(&content)) {
        std::cout << "Response: " << text->text << std::endl;
    }
}
```

对应 `:59-65`。三处惯用法：

- `CallTool(name, arguments)` 的入参是 `std::optional<JsonValue>`，这里用 `JsonValue::Object{...}` 现场构造。
- `result.content` 是 `std::vector<ContentVariant>`，即多类型内容的 variant 数组，**必须先 `std::get_if<TextContent>` 判型**再取 `.text`；直接假设全是文本会在图片/资源链接上炸掉。
- `CallTool` 属于便捷方法，协议层错误以 `McpError` 抛出（工具层失败则是 `result.is_error`），所以外层要有 `catch`——见下一节。

## 收尾顺序与异常兜底

```cpp
client->Close();
server->Close();
server_thread.join();
```

对应 `examples/SimpleClient/main.cpp:67-69`：先关客户端，再关服务端，最后 join 线程。异常路径同样要把线程收掉，否则进程带着 joinable 线程退出：

```cpp
} catch (const std::exception& e) {
    if (server_thread.joinable()) {
        if (server) server->Close();
        server_thread.join();
    }
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
}
```

对应 `:73-80`。`Close()` 幂等，因此「成功路径关一次、异常路径再关一次」是安全的写法（`Close()` 的在途请求结算语义见 [/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)）。

## 运行与实测输出

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_EXAMPLES=ON
cmake --build build/debug --target SimpleClient
build/debug/examples/SimpleClient/SimpleClient
```

实测输出（Windows，clang-cl，返回码 0）：

```
Server: EchoServer v1.0.0
Protocol: 2026-07-28
Available tools:
  - echo: Echo input

Calling echo tool with 'Hello, MCP!'...
Response: Hello, MCP!

Done!
```

`Protocol: 2026-07-28` 说明内存对在 `Auto` 模式下谈成了 modern 时代；换 `ConnectMode::Legacy` 会看到更早的版本号，两时代行为差异见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 与 how-to 的对应关系

| 想做的事 | 去看 |
|---|---|
| 换成 stdio 或 HTTP 连真实服务端 | [/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server) |
| 分页、搜索与缓存提示 | [/zh/howto/call-tools-and-read-resources](/zh/howto/call-tools-and-read-resources) |
| 取消与超时 | [/zh/howto/cancel-a-request](/zh/howto/cancel-a-request) |
| 读懂错误码 | [/zh/howto/handle-errors](/zh/howto/handle-errors) |
| `McpClient` 方法全表 | [/zh/reference/client](/zh/reference/client) |

## 本页对应源码

- `examples/SimpleClient/main.cpp`
- `examples/SimpleClient/CMakeLists.txt`
- `include/mcp/transport/InMemoryTransport.hpp`
- `include/mcp/client/McpClient.hpp`、`include/mcp/client/ClientOptions.hpp`
- `include/mcp/server/McpServer.hpp`、`include/mcp/server/RequestContext.hpp`
- `include/mcp/Content.hpp`（`ContentVariant`、`TextContent`）
