# Echo 服务端逐段走读

`examples/EchoServer` 是本仓库最小的「四元齐活」服务端：一个工具、一个静态资源、一个资源模板、一个提示词，全走 stdio。逐段看清它每个调用的实际签名，比读字段表更快建立心智模型。源码：[`examples/EchoServer/main.cpp`](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/examples/EchoServer/main.cpp)。

## 这个示例演示什么

- 用 `StdioServerTransport` 起服务端，而不是 HTTP。
- `McpServer::Create` 收 `std::unique_ptr<ITransport>`，因此传输对象的所有权交给服务端。
- 三类注册 API 的最小形态：`RegisterTool`、`RegisterResource`、`RegisterResourceTemplate`、`RegisterPrompt`。
- 工具入参要从 `ctx.Params().arguments`（`std::optional<JsonValue>`）里取，判空是必须的。

## 装配骨架

```cpp
auto transport = std::make_unique<StdioServerTransport>();

ServerOptions opts;
opts.server_info = Implementation{"EchoServer", "1.0.0"};
opts.server_instructions = "An echo server — sends back what you send.";

auto server = McpServer::Create(std::move(transport), opts);
```

对应 `examples/EchoServer/main.cpp:13-19`。三件事值得注意：

- `server_instructions` 会进 `initialize` 响应的 `instructions` 字段，客户端能读（`McpClient::GetInstructions()`）。
- `Implementation{name, version}` 是服务端标识，会出现在 `tools/list` 的 `_meta` 与 server card 里。
- 示例没有设置 `ServerOptions::protocol_version`，因此走协商默认；要固定版本就显式给（见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)）。

## 注册工具：echo

```cpp
server->RegisterTool("echo",
    ToolOptions{}.Description("Echo the input text back"),
    std::function<CallToolResult(const Ctx&)>(
        [](const Ctx& ctx) -> CallToolResult {
            auto& params = ctx.Params();
            std::string text;
            if (params.arguments && params.arguments->Contains("text")) {
                text = (*params.arguments)["text"].GetString();
            }
            CallToolResult result;
            result.content.push_back(TextContent{"text", text});
            return result;
        }));
```

对应 `examples/EchoServer/main.cpp:21-33`，其中 `using Ctx = RequestContext<CallToolRequestParams>;` 在文件头 `:10`。

- 真实工具名取 `RegisterTool` 的**第一个实参** `"echo"`，`ToolOptions` 只带描述（`examples` 里没设 `name`，那个字段没有消费点）。
- 这里**没有声明 `InputSchema`**，所以 SDK 不会做入参预校验；示例改用 `Contains("text")` 自保。要理解声明 schema 后 SDK 会替你拦一路，见 [/zh/howto/validate-tool-input](/zh/howto/validate-tool-input)。
- `std::function<CallToolResult(const Ctx&)>` 的显式包装是必须的：裸 lambda 无法推导到重载的注册接口。
- 返回的 `TextContent{"text", text}` 是聚合初始化，顺序为 `{type, text, mime_type, annotations, meta}`（`include/mcp/Content.hpp:42-47`）。

## 注册静态资源与模板资源

```cpp
server->RegisterResource("echo-static", "echo://static",
    ResourceOptions{}.Description("Static echo resource"),
    [](const std::string& uri) -> ReadResourceResult {
        TextResourceContents trc;
        trc.uri = uri;
        trc.text = "Echo!";
        ReadResourceResult rr;
        rr.contents = {ResourceContents{trc}};
        return rr;
    });
```

对应 `examples/EchoServer/main.cpp:35-44`。资源 handler 只收 `uri`，内容装进 `contents`，`uri` 字段要回填成请求的那一个。

```cpp
server->RegisterResourceTemplate("echo-template",
    "echo://{text}",
    ResourceOptions{}.Description("Echo the URI parameter"),
    [](const std::string& uri,
       const std::map<std::string, std::string>& vars) -> ReadResourceResult {
        TextResourceContents trc;
        trc.uri = uri;
        trc.text = "Echo: " + vars.at("text");
        ReadResourceResult rr;
        rr.contents = {ResourceContents{trc}};
        return rr;
    });
```

对应 `examples/EchoServer/main.cpp:46-57`。模板 handler 多收一个已解析的变量表：`vars.at("text")` 直接取模板里的 `{text}`。注册时 SDK 会先校验模板合法性，非法模板在注册点就抛 `InvalidParams`（`src/server/McpServer.cpp:329`），不会拖到第一次读取。

## 注册提示词：capitalize

```cpp
server->RegisterPrompt("capitalize",
    PromptOptions{}.Description("Capitalize the input text"),
    [](const std::string& name,
       const std::optional<JsonValue>& args) -> GetPromptResult {
        (void)name;
        std::string text;
        if (args && args->Contains("text")) {
            text = (*args)["text"].GetString();
        }
        for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        GetPromptResult r;
        PromptMessage pm;
        pm.role = "user";
        pm.content = TextContent{"text", text};
        r.messages = {std::move(pm)};
        return r;
    });
```

对应 `examples/EchoServer/main.cpp:59-75`。

- 提示词 handler 收 `name` 与 `args`，回 `GetPromptResult.messages`，每条消息带 `role` 与 `content`。
- 这里用 `std::toupper` 而不是 `std::toupper(c)` 裸调，是因为 `char` 可能为负数，传非 `unsigned char` 值是未定义行为——示例的写法可以直接照抄。
- 示例没有声明 `PromptOptions::Arguments()`，因此 `prompts/list` 不会列出参数规格；要让客户端知道该传什么，见 [/zh/howto/serve-a-prompt](/zh/howto/serve-a-prompt)。

## 运行与观察

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_EXAMPLES=ON
cmake --build build/debug --target EchoServer
build/debug/examples/EchoServer/EchoServer
```

实测输出（Windows，clang-cl）：

```
EchoServer starting on stdio...
```

横幅写在 **stderr**（`examples/EchoServer/main.cpp:77`），stdout 留给协议帧——这也是为什么 `StdioClientTransportOptions::stderr_path` 值得配置。之后进程进入 `server->Run()` 阻塞循环（`:78`）。本次实测还确认：把 stdin 重定向到 `/dev/null` 后进程**不会自行退出**，说明 stdio 服务端的生命周期由宿主关闭进程决定，而不是靠 stdin EOF 自终止。

要看到它真的应答，用 `SimpleClient`（本页不重复，见 [/zh/examples/simple-client](/zh/examples/simple-client)），或用 Inspector 之类的客户端连上来跑 `tools/list` 与 `prompts/list`。

## 与 how-to 的对应关系

| 想做的事 | 去看 |
|---|---|
| 给工具加描述、标题、输入 schema、注解 | [/zh/howto/register-a-tool](/zh/howto/register-a-tool) |
| 让 SDK 替你校验入参 | [/zh/howto/validate-tool-input](/zh/howto/validate-tool-input) |
| 资源订阅与 `list_changed` | [/zh/howto/serve-a-resource](/zh/howto/serve-a-resource) |
| 提示词参数与补全 | [/zh/howto/serve-a-prompt](/zh/howto/serve-a-prompt) |
| 换成 Streamable HTTP 对外提供 | [/zh/howto/run-a-streamable-http-server](/zh/howto/run-a-streamable-http-server) |
| 注册 API 的签名与字段表 | [/zh/reference/server](/zh/reference/server) |

## 本页对应源码

- `examples/EchoServer/main.cpp`
- `examples/EchoServer/CMakeLists.txt`
- `include/mcp/server/McpServer.hpp`、`include/mcp/server/RequestContext.hpp`、`include/mcp/server/ServerOptions.hpp`
- `include/mcp/transport/StdioServerTransport.hpp`
- `include/mcp/Content.hpp`、`include/mcp/McpTypes.hpp`、`include/mcp/Implementation.hpp`
