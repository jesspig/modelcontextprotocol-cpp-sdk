# 连接到一个服务端

`McpClient::Create()` 一次调用把「建立传输 → 版本协商 → 起消息线程」全做完，返回时会话已经可用；难点在于分清 `IClientTransport` 与 `ITransport` 两个类型，以及理解三档协商各自发出什么包。签名与字段表只在 [/zh/reference/client](/zh/reference/client) 维护，本篇只讲操作流程。

## 前置条件

- 已能编译并链接 `mcp-client`，见 [/zh/start/installation](/zh/start/installation)；想看一次完整的进程内往返，照 [/zh/start/quickstart](/zh/start/quickstart) 跑通 `examples/SimpleClient`。
- 头文件按子目录全路径写（构建只给 `-I include`）：`mcp/client/McpClient.hpp`、`mcp/client/ClientOptions.hpp`、`mcp/Transport.hpp`、`mcp/transport/StdioClientTransport.hpp`、`mcp/transport/StreamableHttpClientTransport.hpp`、`mcp/transport/InMemoryTransport.hpp`、`mcp/Capabilities.hpp`、`mcp/Implementation.hpp`。
- 传输选项字段表见 [/zh/reference/transports](/zh/reference/transports)，`ClientOptions` 逐字段见 [/zh/reference/client](/zh/reference/client)，本篇不重复。
- 概念背景：分层与线程模型见 [/zh/learn/architecture](/zh/learn/architecture)，传输选型见 [/zh/learn/transports](/zh/learn/transports)。

## 第一步：选传输，并自己调用 `Connect()`

| 传输 | 构造入口 | `Connect()` 之后才开始 | 适合场景 |
|---|---|---|---|
| stdio 子进程 | `StdioClientTransport` | 拉起子进程并接管其 stdin/stdout | 本机可执行服务端 |
| Streamable HTTP | `StreamableHttpClientTransport` | 建立 HTTP 会话并按需开监听流 | 远端服务端 |
| 进程内 | `InMemoryTransport::CreatePair()` | 无需 `Connect()`，直接取 `.client` | 单元测试与自问自答 |

`McpClient::Create()` 收的是 `std::shared_ptr<ITransport>`，不是 `IClientTransport`（`include/mcp/client/McpClient.hpp`、`include/mcp/Transport.hpp`）。`Connect()` 才真正动手：stdio 的实现里先起进程再把管道交给会话对象（`src/transport/StdioClientTransport.cpp`），HTTP 的实现里构造会话并 `Start()`（`src/http/StreamableHttpClientTransport.cpp`）。

<<< @/snippets/connect-to-a-server.cpp#stdio-transport
<<< @/snippets/connect-to-a-server.cpp#http-transport
<<< @/snippets/connect-to-a-server.cpp#in-memory-pair

`Pid()` 只在 `Connect()` 之后有值，且恒为正（`TransportTest.StdioPidAvailable`）；`stderr_path` 与 `on_stderr` 互斥，同时给出直接在 `Connect()` 里抛 `InvalidParams`（`src/transport/StdioClientTransport.cpp`）。

## 第二步：装配 `ClientOptions`

<<< @/snippets/connect-to-a-server.cpp#client-options

- 要接收服务端发起的 `elicitation/create`，handler 必须写进 `ClientOptions` 再 `Create()`：出站能力由 `DeriveClientCapabilities` 在构造函数里定稿（`src/client/McpClient.cpp`），事后调用 `SetElicitationHandler()` 不会改变已经上报给对端的能力表。
- `connect_mode` 默认 `Auto`，`initialization_timeout` 默认 60s 只管 `initialize` 握手，`discover_probe_timeout` 默认 5s 只管 `server/discover` 探测（`include/mcp/client/ClientOptions.hpp`）。
- `on_transport_close`／`on_transport_error` 只有在传输是 `TransportBase` 子类时才接得上，否则记一条 Warning 后静默忽略（`src/client/McpClient.cpp`）；仓库自带的 stdio／HTTP／进程内三种传输都继承 `TransportBase`。
- `enable_etag_cache` 客户端默认 **真**（`include/mcp/client/ClientOptions.hpp`），与服务端默认相反。

## 第三步：`Create()` 即协商，失败即抛出

<<< @/snippets/connect-to-a-server.cpp#open-client
<<< @/snippets/connect-to-a-server.cpp#negotiate-failure

`Create()` 先构造对象（内部已 `handler_->Start()` 起消息线程），再同步执行协商（`src/client/McpClient.cpp`）。协商抛异常时，函数内的 `unique_ptr` 在栈回卷时析构，顺带 `Close()` 会话并关闭传输——stdio 场景下刚起的子进程一并收掉，不需要你补清理。

| 档位 | 发出的包 | 谈成的标志 | 失败表现 |
|---|---|---|---|
| `Auto` | 先 `server/discover`，据其选版 | `IsModernProtocol()` 为真 | 探测失败按传输分类：stdio 类回退 `initialize`，网络类直接抛错 |
| `Legacy` | `initialize` + `notifications/initialized` | `IsModernProtocol()` 为假 | 握手超时抛错，受 `initialization_timeout` 约束 |
| `Pin` | **一个包都不发**，本地合成结果 | `is_modern` 恒置真 | 不会失败，也就拿不到任何对端事实 |

声明位置：`include/mcp/client/ClientOptions.hpp`、`src/client/McpClient.cpp`；分类回退的细节见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 第四步：读协商快照，别把 `Pin` 当成省事档

<<< @/snippets/connect-to-a-server.cpp#negotiated-snapshot

`GetServerCapabilities()` 与 `GetServerInfo()` 返回的是协商结果内部的引用，不得越出客户端生命周期；`GetInstructions()` 按值给 `std::optional<std::string>`，`GetNegotiatedProtocolVersion()` 给指向内部字符串的视图（`src/client/McpClient.cpp`）。三处必须知道：

- `Pin` 档位合成的 discover 结果里 `capabilities` 是空对象，`server_info` 也没有赋值：`GetServerCapabilities()` 各项皆空，`GetServerInfo().name` 是空串（`src/client/McpClient.cpp`、`McpClientTest.InitialState`）。想按能力表决定是否发起 `tools/call`，就别用 `Pin`。
- `IsModernProtocol()` 返回协商时写的标志，`Pin` 下恒为真——即使钉的是 `2025-11-25`。要判真实时代，用 `IsModernProtocolVersion(GetNegotiatedProtocolVersion())`，判据是字典序 `>= "2026-07-28"`（`src/client/McpClient.cpp`、`include/mcp/ProtocolVersion.hpp`）。
- 五个协议版本里只有 `2026-07-28` 算现代；`Auto` 谈成的通常是它，`Legacy` 拿到的取决于对端 `initialize` 回的版本（`include/mcp/ProtocolVersion.hpp`）。

## 第五步：接住服务端发来的通知

<<< @/snippets/connect-to-a-server.cpp#passive-handlers

`SetLoggingHandler()` 是 `notifications/message` 的专用入口，注册即接管该通知，handler 抛异常只进内部日志、不影响连接（`src/client/McpClient.cpp`）。`SetNotificationHandler(method, handler)` 按方法名登记任意通知，但它是**同一张表的赋值**（`src/protocol/McpSessionHandler.cpp`）：客户端在构造期已经为 `tools/list_changed`、`resources/list_changed`、`prompts/list_changed`、`resources/updated`、`notifications/progress` 装了内置处理器，你再用同名注册就会顶掉内置行为——缓存失效随之消失。反向请求（`elicitation/create` 等）与订阅通道见 [/zh/howto/handle-server-initiated-requests](/zh/howto/handle-server-initiated-requests)。

## 第六步：关闭时机与在途请求

<<< @/snippets/connect-to-a-server.cpp#shutdown

- `Close()` 幂等：第二次进入直接被 `closed_` 标志挡掉（`src/protocol/McpSessionHandler.cpp`）。
- `Close()` 会 join 消息线程、超时线程与响应线程，最后 `transport_->Close()`（`src/protocol/McpSessionHandler.cpp`）。stdio 会话关闭时按 `shutdown_timeout` 等子进程退出，`terminate_process_tree` 决定孙进程是否一起收（`TransportTest.StdioProcessTreeTerminated`），细节见 [/zh/howto/manage-subprocesses](/zh/howto/manage-subprocesses)。
- 关闭时仍在途的请求以 `ConnectionClosed`（`-32000`，消息 `connection closed`）结算，不会永久阻塞（`src/protocol/McpSessionHandler.cpp`）。
- `Close()` 只清空私有作用域的响应缓存并重置鉴权上下文，公有条目留下（`src/client/McpClient.cpp`、`McpClientTest.CloseDropsPrivateCacheEntries`）。
- 析构函数再 `Close()` 一次并摘除传输回调（`src/client/McpClient.cpp`），所以 `unique_ptr` 出作用域即断开。

::: warning
`on_transport_close`／`on_transport_error` 与所有通知 handler 都在传输的 IO 线程上执行，回调里调 `Close()` 会 join 当前线程而 self-join 卡死。要断开请把请求投递到自己的线程再关，卡死排查见 [/zh/howto/debug-a-stuck-server](/zh/howto/debug-a-stuck-server)。
:::

## 完整代码

可编译的完整文件是 `docs/snippets/connect-to-a-server.cpp`，区域名依次为 `stdio-transport`、`http-transport`、`in-memory-pair`、`client-options`、`open-client`、`negotiate-failure`、`negotiated-snapshot`、`passive-handlers`、`shutdown`，已在上面六个小节里各引用一次。

## 验证

片段只做语法校验（MSVC 前端 `/Zs`），在工作树根执行：

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-connect-to-a-server
```

实测输出：configure 末尾 `[mcp-docs] syntax-checking 12 snippet(s) with '/Zs'`，build 输出 `[1/2] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-connect-to-a-server.dir\__\__\docs\snippets\connect-to-a-server.cpp.obj`，返回码 0、无 `error:` 行。

行为级验证需要测试目标（`cmake --preset debug && cmake --build --preset debug`）：

```bash
ctest --preset debug -R 'McpClientTest\.(ClientOptionsDefaults|InitialState|PinNegotiationUsesPinnedVersion|LegacyNegotiationUsesInitialize|AutoNegotiationDiscoversModern|AutoNegotiationFallsBackToInitialize|CreateAndClose|SetNotificationHandlerFiresAfterCreate)$|TransportTest\.(StdioPidAvailable|StdioStderrCaptured)$'
```

实测：`McpClientTest` 那组 `[==========] 8 tests from 1 test suite ran` 后跟 `[  PASSED  ] 8 tests`，`TransportTest` 那组 `[  PASSED  ] 2 tests`（直接跑 `build/debug/tests/unit/mcp-client-tests.exe` 与 `mcp-transport-tests.exe`，每条 gtest 用例对应一个 ctest 注册项，合计 10 条）。四条关键断言：

| 用例 | 断言值 |
|---|---|
| `McpClientTest.ClientOptionsDefaults` | `connect_mode == Auto`、`client_info.name == "mcp-cpp-client"`、两个超时分别是 60 与 5 |
| `McpClientTest.AutoNegotiationDiscoversModern` | `IsModernProtocol()` 为真、版本 `2026-07-28`、`GetServerInfo().name == "test-server"` |
| `McpClientTest.InitialState` | `Pin` 档位下 `GetServerInfo().name` 为空串 |
| `TransportTest.StdioPidAvailable` | `Connect()` 前 `Pid()` 无值，之后 `> 0` |

## 时代差异

判据是协商版本字典序 `>= "2026-07-28"`，会话级生效。

| 维度 | legacy（早于 2026-07-28） | modern（2026-07-28 及以后） |
|---|---|---|
| 建会话的包 | `initialize` + `notifications/initialized` | `server/discover`，没有 `initialize` 握手 |
| 请求必带 `_meta` | 不要求 | 要求，缺失判 `InvalidRequest`；SDK 在出站时自动补齐（`src/protocol/WireCodec.cpp`） |
| 响应必带 `resultType` | 不要求 | 要求，列表类必须为 `complete`（`src/protocol/WireCodec.cpp`） |
| 初始化前置检查 | 未 `initialized` 的请求被拒（`src/server/McpServer.cpp`） | 服务端跳过该检查 |
| 订阅通道 | `resources/subscribe`（`src/protocol/WireCodec.cpp`） | `subscriptions/listen` + acknowledged 帧 |

## 常见错误

| 症状 | 成因 | 处置 |
|---|---|---|
| 把 `StdioClientTransport` 对象直接传给 `Create()` 编译不过 | `Create()` 只要 `std::shared_ptr<ITransport>` | 先 `auto session = transport.Connect();` 再传 `session` |
| 连远端 HTTP 服务端时直接抛 `-32001` 或 `-32000`，压根没试 `initialize` | `Auto` 档对网络类传输不做回退 | 对端确认只支持旧版时显式 `ConnectMode::Legacy`（`src/client/McpClient.cpp`） |
| `GetServerCapabilities()` 全是空，代码判成「对端没有工具」 | 用了 `Pin` 档，能力表是合成的空对象 | 需要能力表就用 `Auto` 或 `Legacy`；`Pin` 仅用于强制版本 |
| `IsModernProtocol()` 为真但 `tools/list` 仍按旧格式发 | `Pin` 恒置 `is_modern`，与实际钉的版本无关 | 用 `IsModernProtocolVersion(GetNegotiatedProtocolVersion())` 判时代 |
| 服务端说客户端不具备 elicitation | `SetElicitationHandler()` 在 `Create()` 之后调用 | 把 handler 写进 `ClientOptions`，能力在构造期上报 |
| `on_transport_close` 从不触发 | 自定义传输没继承 `TransportBase` | 换成仓库自带传输，或在传输自己的关闭路径上通知（`src/client/McpClient.cpp`） |
| 程序退出时卡在 `Close()` | 在 IO 线程回调里调 `Close()`，join 自身线程 | 把关闭动作挪到别的线程；见第六步的 warning |

## 下一步

- 连上之后取数据：[/zh/howto/call-tools-and-read-resources](/zh/howto/call-tools-and-read-resources)
- 回答服务端发起的请求：[/zh/howto/handle-server-initiated-requests](/zh/howto/handle-server-initiated-requests)
- 带鉴权的连接：[/zh/howto/add-oauth-to-a-client](/zh/howto/add-oauth-to-a-client)
- 子进程传输的运维：[/zh/howto/manage-subprocesses](/zh/howto/manage-subprocesses)
- 取消在途请求：[/zh/howto/cancel-a-request](/zh/howto/cancel-a-request)
- 门面签名与字段全表：[/zh/reference/client](/zh/reference/client)

## 本页对应源码

- `include/mcp/client/McpClient.hpp`
- `include/mcp/client/ClientOptions.hpp`
- `include/mcp/Transport.hpp`
- `include/mcp/Capabilities.hpp`
- `include/mcp/ProtocolVersion.hpp`
- `include/mcp/transport/StdioClientTransport.hpp`
- `include/mcp/transport/StreamableHttpClientTransport.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/client/McpClient.cpp`（构造、三档协商、快照读取、关闭）
- `src/protocol/McpSessionHandler.cpp`（`Start`/`Close`、通知处理器表、在途请求结算）
- `src/protocol/WireCodec.cpp`（按时代的包形状校验）
- `src/transport/StdioClientTransport.cpp`、`src/http/StreamableHttpClientTransport.cpp`
- `docs/snippets/connect-to-a-server.cpp`
