# 仓库指南

## 构建与测试

```bash
cmake --preset debug                          # 配置（Ninja，Debug）
cmake --build --preset debug                  # 构建
ctest --preset debug --output-on-failure      # 全部测试
ctest -R WireCodec                            # 单个测试套件
cmake --build --preset debug --target mcp-server-tests   # 单个目标
```

预设：`debug`、`release`。仅 Ninja 生成器。CI：push/PR 到 `develop`（ci.yml，3 个 OS × 2 种构建类型）。

示例默认关闭：配置时加 `-DMCP_BUILD_EXAMPLES=ON` 即可构建示例。

无格式化/静态检查配置 —— Clang/GCC 用 `-Wextra -Wpedantic`，MSVC/clang-cl 用 `/W4`。

### 不易察觉的构建事实

- **Unity（jumbo）构建默认开启**。可用 `-DMCP_UNITY_BUILD=OFF` 覆盖。
- `mcp-transport` 与 `mcp-protocol` 显式关闭 Unity（匿名命名空间符号冲突）。`mcp-client` 使用 `UNITY_BUILD_UNIQUE_ID ON`（OAuth 符号）。
- **编译器自动探测**在 `project()` 之前执行：clang-cl（Win）> clang++-N + 匹配版本 clang-N（Linux）> 系统默认。若 `CMAKE_CXX_COMPILER` 已设置则跳过。
- **LTO**：Release 自动启用（clang-cl：LTCG，Clang：ThinLTO，GCC：IPO）。
- **编译器缓存**：sccache > ccache > 无。sccache 支持 MSVC/clang-cl；ccache 跳过 MSVC。
- **依赖缓存于 `build/<preset>/_deps/`**。删除 `build/` 代价高昂。
- **Werror**：仅在 `-DMCP_WERROR=ON` 时启用（CI 自动添加此选项）。
- **Ninja job 池自动调优**（编译 ≈ `mem/1500MB`，链接上限 2）。内存受限机器可用 `-DMCP_COMPILE_JOBS` / `-DMCP_LINK_JOBS` 覆盖。
- **`-march=native` 仅本地添加，CI 中从不启用**（`MCP_IS_CI` 门控）—— debug 二进制不可移植出构建机。
- **`mcp-core` 是 STATIC**（JsonValue.cpp、JsonRpc.cpp 等）—— 修改序列化会重编译大量依赖方。
- **`src/` 是私有 include 路径**：`mcp-core`/`mcp-server`/`mcp-client`/`mcp-protocol` 各有 PRIVATE `src/`——`src/detail/` 下的内部头（`JsonFields.hpp`、`JsonSerializer.hpp`）经 `#include <detail/...>` 包含。**注意两个 "detail" 目录**：`<mcp/detail/...>`（公共头，如 `ThreadUtils.hpp`）与 `<detail/...>`（src/detail 私有头）不是同一个。

### 跨平台陷阱

- macOS：匿名命名空间中的 `environ` 会产生 mangled 的 `mcp::detail::environ`。改用 `_NSGetEnviron()`。
- macOS：`pthread_setname_np` 是单参数——用 `#ifdef __APPLE__` 保护。
- GCC：`(void)` 强转**不能**抑制 `warn_unused_result`（仅 Clang 可以）。影响 `chdir()`、`close()`、`dup2()`、`pipe()`。
- Apple Clang 默认启用 `-Wunused-private-field`——配合 `-Werror` 时任何未使用的私有成员都是硬错误。
- clang-cl 静默接受 MSVC（`/W4`）和 GCC（`-Wall`）两种标志——拼写错误会直接通过。
- `CMake CMP0169`：用 `if(POLICY CMP0169)` 保护——CMake 3.30 之前不可用。

### Unity 构建陷阱

- **头文件自包含是强制要求**：Unity 会合并 `.cpp` 文件；依赖先前 `#include` 顺序的头文件会编译失败。
- **调试**：错误行号指向生成的 Unity 批处理文件而非原始源码。可用 `-DMCP_UNITY_BUILD=OFF` 关闭。
- GCC 在 Unity 文件中可能触发 `-Wunused-function`——用 `[[maybe_unused]]` 保护匿名命名空间函数。

### libhv FetchContent 补丁

`cmake/FetchDependencies.cmake` 会修补 libhv 的 CMakeLists.txt：将 `install(FILES ... DESTINATION include/hv)` 替换为 `file(COPY ...)`，使 `include/hv/` 目录在配置期即存在（匹配 `hv_static` 的 `BUILD_INTERFACE`）。若代理新增预设或修改依赖拉取，此补丁必须保留。

## 架构

```
include/mcp/       — 公共头文件
src/client/        — McpClient、OAuth、FileTokenCache
src/server/        — McpServer、FileTaskStore
src/protocol/      — McpSessionHandler（JSON-RPC 引擎）、WireCodec（双时代）
src/transport/     — Stdio、SSE、InMemory、WebSocket、StreamableHttp 实现
src/http/          — HttpServer、EventStore、StreamableHttp*
tests/             — unit/（gtest）、integration/、conformance/
examples/          — EchoServer、WeatherServer、SimpleClient
```

库依赖链：`mcp-core` → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`。`mcp-http` 依赖 `mcp-transport`。全部 STATIC。无 INTERFACE / header-only 库。

### 传输层层级

```
ITransport — TransportBase（三态：Initial→Connected→Disconnected）
  ├── StdioServerTransport
  ├── InMemoryTransportImpl
  ├── StreamableHttpServerTransport
  └── *SessionTransport（内部，匿名命名空间或 enable_shared_from_this）

IClientTransport（连接工厂）
  ├── StdioClientTransport（PlatformIO，合并 Win32+POSIX）
  ├── SseClientTransport（libhv HttpClient）
  ├── StreamableHttpClientTransport（libhv requests / WinHTTP）
  └── WebSocketClientTransport（libhv WebSocketClient）
```

无 asio 依赖——裸线程 + 标准库原语（mutex、condition_variable）或 libhv 事件循环。

`MessageChannel`（`include/mcp/protocol/MessageChannel.hpp`）是有界异步队列（`std::queue + mutex + condition_variable`），替代 `asio::experimental::channel`。默认容量 64；`Send` 满则**阻塞**，`TrySend` 满则返回 false 不阻塞。

`InMemoryTransport::CreatePair()` 返回 `Pair{client, server}`（各为 `shared_ptr<ITransport>`）。用 `dynamic_cast<TransportBase*>` 访问状态机。

### HttpServer（`mcp-http`）

基于 libhv `HttpService` 的 PIMPL 模式。关键细节：
- `HttpServerOptions::on_connect` 与 `on_disconnect` **已接线**——分别在 SSE 客户端添加/移除时触发；`on_disconnect` 是拷贝语义（每个连接触发一次，勿用 move 取走）。
- worker 线程 8；stateless 模式并发上限 8（超出返回 503 `"server busy"`），同步等待超时返回 **504**（非 500）。
- SSE 广播事件带 `id:` 行；SSE GET 支持 `Last-Event-ID` 头断线回放（`EventStore::GetEventsSince`）。
- `BroadcastSse` 对已断开连接写失败会**自动移除**该客户端（写失败抛异常 → 按 entry 自清理）；注册前有 `isClosed()` 预检。

### 版本协商（关键）

- **`HandleInitialize` 必须回显客户端的旧版版本号**：返回客户端发送的版本（如 `"2025-11-25"`）。切勿返回 `kLatestProtocolVersion`（`"2026-07-28"`）——TS SDK v2 会校验 `result.protocolVersion` 是否在其旧版列表中。
- **现代版本（2026-07-28+）绝不通过 `initialize` 协商**：只能通过 `server/discover`。
- `McpClient` 连接模式：`Auto`（默认，探测 `server/discover`，失败回退 `initialize`）、`Legacy`（仅 initialize）、`Pin`（固定版本）。**注意：Auto 模式任何失败（含超时）都回退**——`VersionNegotiation.hpp` 头文件注释写的是 -32022/-32601，实际实现更宽松，勿按注释假设。
- `McpClient::Create` **创建即阻塞**：构造后立即同步 `NegotiateProtocol()`，返回前协商完成。
- `SetNegotiatedProtocolVersion(version)` **线程安全**：存储版本并重建 `WireCodec`（`codec_` 为 `shared_ptr` + `codec_mutex_`），消息循环运行中可调用——`McpClient` 就是在 `Start()` 之后协商的。
- `WireCodec::EncodeErrorCode`（2026 时代）三映射：`RequestTimeout→HeaderMismatch`、`ConnectionRefused→MissingRequiredClientCapability`、`TlsHandshakeFailed→UnsupportedProtocolVersion`——依赖方按此解码错误。

## 关键协议模式

- **2026-07-28+（现代）**：无状态。`server/discover` 取代 `initialize`/`initialized`。每请求 `_meta` 携带 `protocolVersion`、`clientInfo`、`clientCapabilities`、`logLevel`。
- **MRTR**：服务端发起的 elicitation 以 `InputRequiredResult` 内嵌。客户端 `SendRequestWithMrtr` 默认预算：`max_rounds=8`、`round_timeout=600s`、`max_total_timeout=0`（0 = 不设总预算）。
- **订阅**：`subscriptions/listen` 取代 `resources/subscribe`。
- **缓存**：带 `ttlMs`/`cacheScope` 的 `CacheHint`。
- **Mcp-Method 头**：动态生成，取自 JSON-RPC 消息体的 method 字段（用于 Streamable HTTP + SSE）。

## 通知处理器

全部 17 种通知类型注册于 `WireCodec.cpp` 编解码器集合。服务端处理器在 `McpServer::WireHandlers()` 接线；客户端**不注册任何通知处理器**（`WireClientHandlers()` 仅注册 elicit 请求处理器）——客户端收到通知后静默丢弃，须自行 `SetNotificationHandler`。通知经 `McpSessionHandler::OnNotification()` 分发——未知通知**静默丢弃**（无 catch-all 处理器）。

**注意事项**：
- `notifications/cancelled` 在 `OnNotification()` 中硬编码处理，先于处理器表查找（并非 `SetNotificationHandler` 注册）。
- `logging/setLevel` 是**请求**（非通知）——新增服务端时必须注册到 `WireHandlers()`。
- 进度通知处理器调用 `ResetTimeoutByProgressToken()`，通过 `progress_token_map_` → `pending_` 查找将截止时间延长 30 秒。定义于 `McpSessionHandler`，在 `McpServer::WireHandlers()` 中接线。**客户端侧不自动接线**：`McpClient` 不注册任何通知处理器——客户端要处理 progress（含超时延长）须自行 `SetNotificationHandler`。

## 服务端选项与事件钩子

`ServerOptions` 暴露四层回调：简写（`on_method_called`、`on_protocol_error`）、完整消息（`on_request`、`on_response`、`on_error`、`on_notification`）、生命周期（`on_client_connected`、`on_initialized`）、传输层（`on_transport_close`、`on_transport_error`）。做认证/审计/限流时，通过 `incoming_filters`/`outgoing_filters` 注入 `FilterPipeline`。

`McpServer::GetClientCapabilities()/GetClientInfo()` 返回 `shared_ptr<const T>`（非裸指针）——调用方须持有返回值再访问。

## 并发与生命周期（易翻车点）

- **IO 线程回调内调用 `Close()` 会 self-join**：stdio/SSE/HTTP 传输的 IO 线程会直接执行用户回调（`on_transport_close`/`on_transport_error`），用户在回调里调 `McpServer::Close()` 会触发传输 `Close()` join 自身线程（`std::thread::join()` 抛异常）。所有传输与 `McpSessionHandler` 的 `Close()` 必须用 `detail::JoinThreadSafely`（`include/mcp/detail/ThreadUtils.hpp`：self 时 detach，否则 join）。
- **`PipeHandle::Read` 返回 0 不一定是 EOF**：`PosixPipe` 用 poll 轮询（100ms 超时），无数据时返回 0。读循环必须用 `IsEof()` 区分"超时无数据"（继续轮询，检查 `running_`）与"真 EOF"（退出），否则正常空闲时被误判为断连。
- **handler 抛 `McpError` 保留错误码**：`OnRequest` 对 `McpError` 直接回 `e.Code()` 错误响应；其他异常一律 `InternalError`（"handler error: ..."）。新 handler 抛 `McpError(InvalidParams, ...)` 即得正确的 JSON-RPC 错误码（如非法 cursor）。
- **`TransportBase::SetConnected` 仅 `Initial→Connected`**（CAS 校验）；已 `Disconnected` 后调用被忽略并记 Warning。
- **`HttpServer::SetHandler` 必须在 `Start()` 之前**：运行期调用抛 `std::logic_error`。

## 存储与认证行为（失败语义）

- **`FileTaskStore`**：`Flush()` 失败抛 `std::runtime_error` 并回滚内存修改（`CreateTask`/`UpdateTask`/`CancelTask`/`SetTaskStatus` 返回 `false` 仅表示任务不存在）；加载损坏文件会备份为 `<path>.corrupt` 再继续，不会覆盖原数据。
- **`AtomicJsonFile::WriteAtomic`**：临时文件名为 `<path>.tmp.<pid>`（多进程不踩踏），写后 `fsync`（POSIX）/`FlushFileBuffers`（Windows）再 rename；失败返回 false。
- **`FileTokenCache`**：Windows 上 DPAPI 解密失败**不回落明文**（记录 Error 并忽略缓存，重新认证即可）。
- **OAuth**：`ValidateTokenIssuer` 强制 RFC 9207——token/refresh 响应缺 `iss` 或与 metadata issuer 不匹配即拒绝；`RefreshTokens` 响应缺 `access_token` 视为失败（不回退旧 token）。

## 编码规范

- **C++17**、`#pragma once`、4 空格缩进。无自动格式化工具。
- 类型/函数：PascalCase。常量：`k` + PascalCase。成员：snake_case + 下划线（`io_ctx_`）。
- 命名空间：扁平 `mcp`。子命名空间：`mcp::methods`、`mcp::notifications`。
- **公共头文件无外部 JSON**：`JsonValue`（基于 `std::variant`）是唯一 JSON 类型。解析用 simdjson DOM（内部）。序列化为手写 `Dump()`。
- `Prompt` 无 `annotations` 字段——按规范。
- 服务端在收到 `notifications/initialized` 前，用 `initialized_` 标志守护所有处理器。
- `ContentVariant` 为 `std::variant<TextContent, ImageContent, AudioContent, EmbeddedResource, ResourceLink>`——按 `type` 字符串分派。
- `JsonRpcErrorResponse::id` 是 `optional<RequestId>`（按 JSON-RPC 2.0 §5.1）。
- 日志级别经 `MCP_LOG_LEVEL` 环境变量：0=Off，1=Error，2=Warning，3=Info，4=Debug，5=Trace。
- 带参数的工具有 `ToolOptions::InputSchema(JsonValue s)`（默认空 schema）。
- `StreamableHttpClientTransport::Name()` 在 `options_.name` 为空时返回 `"streamable-http"`。
- JSON 字段/协议键常量放 `src/detail/JsonFields.hpp`（`detail::kXxxKey`，含 `kMeta*Key` 系列）——新增键加在那里，禁止硬编码 `"io.modelcontextprotocol/..."` 字符串。

## 测试

`InMemoryTransport` 是**同步**的——消息在 `Send()`/`AsyncReceive()` 时交付（`MessageChannel` 是有界队列，默认 64），无外部事件循环。

**OpenSSL 陷阱**：PKCE 内置 SHA-256 回退（`include/mcp/detail/sha256.hpp`），但 `src/client/auth/OAuthClientProvider.cpp` 有无保护的 `#include <openssl/rand.h>`——未安装 OpenSSL 开发头文件的机器无论如何都无法编译 `mcp-client`。

**测试注意事项**：
- 集成测试的 `RunWithTimeout`：body 挂起超过 10s 会 `std::_Exit(1)` 使进程直接失败（快于永久阻塞）——新增集成测试应继续用它。
- 关键协议行为已有盲区守护测试：`RejectsRequestsBeforeInitialized`、`InitializeEchoesClientVersion`（回显旧版版本号）、`ProgressNotificationExtendsDeadline`、`IncomingFilterInterceptsRequests`——改动这些行为时测试会失败。
- `WireCodec::ValidateResponse`/`StampOutgoingRequest`/`ExtractIncomingMeta` 生产代码无调用者但**有测试守护**（WireCodecTests/Conformance）——不是死代码，勿删。

| 套件 | 目标 | 关键文件 |
|-------|--------|----------|
| JsonRpcTest | `mcp-core-tests` | 序列化 + 变体分派 |
| McpTypesTest | `mcp-core-tests` | 类型往返 |
| WireCodecTest | `mcp-wire-codec-tests` | 按时代门控的编解码器 |
| McpServerTest | `mcp-server-tests` | 注册、能力 |
| McpClientTest | `mcp-client-tests` | 客户端创建、连接模式 |
| OAuthTest | `mcp-oauth-tests` | PKCE、令牌缓存 |
| TransportTest | `mcp-transport-tests` | InMemory + 经 `dynamic_cast<TransportBase*>` 访问状态机 |
| HttpServer/EventStore/StreamableHttp | `mcp-http-tests` | HTTP 服务器、SSE、头 |
| Conformance | `mcp-conformance-tests` | MCP 规范符合性 |
| ClientServerFixture | `mcp-integration-tests` | 经 InMemoryTransport 的客户端-服务端往返 |
| MessageFilterTest | `mcp-message-filter-tests` | FilterPipeline 链、停止、修改 |
| FileTokenCacheTest | `mcp-token-cache-tests` | 持久化、损坏处理 |
| FileTaskStoreTest | `mcp-task-store-tests` | 任务 CRUD、状态迁移 |
| StreamableHttpTransportTest | `mcp-streamable-http-tests` | 客户端/服务端构造、头校验 |
| WebSocketTransportTest | `mcp-websocket-tests` | 构造、默认名称 |

## 依赖（自动拉取）

| 依赖 | 版本 | 说明 |
|-----|---------|-------|
| libhv | 1.3.4 | HTTP 客户端/服务端、WebSocket、事件循环；`hv_static` 目标 |
| simdjson | 3.12.3 | JSON 解析（内部，不出现于公共头文件） |
| GoogleTest | 1.15.2 | 仅当 `MCP_BUILD_TESTS=ON` 时 |
| OpenSSL | 系统 | 可选：TLS、PKCE SHA-256（回退到内置实现） |

无 asio。无 nlohmann-json。两者已完全移除。

## 文档

`docs/` 是 vitepress 站点（本地预览用 `pnpm dev`，构建用 `pnpm build`）。双语：`en/` 与 `zh/` 目录。SDK 用它作为面向用户的文档；生成的 API 文档在 `docs/.vitepress/dist/`。注意：文档 CI（docs.yml）在 push 到 `master` 时部署 GitHub Pages，而 ci.yml 门控在 `develop`。

## 项目知识库

`wiki/` 是源码知识库（与 `docs/` 在线文档分离）。规则：每个概念一个 `.md` 文件 + YAML frontmatter；`index.md` 是目录、`log.md` 是摘要（最近 7 条）、`changelog/<YYYY-MM-DD>-log.md` 按天记日志（条目精确到小时）；全部内容基于源码实现细节，严禁推测，无法核实处标 `> [!todo] 待补充`；更新仅影响受影响的页面，彻底清除过时描述；互链：每页至少 1 条出站/入站相对路径链接。

目录：`modules/`（库）、`classes/`（关键类）、`transports/`（传输实现）、`concepts/`（跨层概念）、`build.md`、`tests.md`。入口：`wiki/index.md`。知识库页面直接引用源码行号，与代码同步更新（功能完成/交付指南/提交前统一更新，微调不更新）。

## 提交

带 scope 的 Conventional Commits：`feat(transport):`、`fix(server):` 等。scope：`client`、`server`、`protocol`、`transport`、`http`、`core`、`build`、`test`、`examples`。
