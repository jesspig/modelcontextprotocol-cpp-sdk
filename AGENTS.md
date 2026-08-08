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

`MessageChannel`（`include/mcp/protocol/MessageChannel.hpp`）是有界异步队列（`std::queue + mutex + condition_variable`），替代 `asio::experimental::channel`。

`InMemoryTransport::CreatePair()` 返回 `Pair{client, server}`（各为 `shared_ptr<ITransport>`）。用 `dynamic_cast<TransportBase*>` 访问状态机。

### HttpServer（`mcp-http`）

基于 libhv `HttpService` 的 PIMPL 模式。关键细节：`HttpServerOptions::on_connect` 与 `on_disconnect` **已接线**——分别在 SSE 客户端添加/移除时触发。

### 版本协商（关键）

- **`HandleInitialize` 必须回显客户端的旧版版本号**：返回客户端发送的版本（如 `"2025-11-25"`）。切勿返回 `kLatestProtocolVersion`（`"2026-07-28"`）——TS SDK v2 会校验 `result.protocolVersion` 是否在其旧版列表中。
- **现代版本（2026-07-28+）绝不通过 `initialize` 协商**：只能通过 `server/discover`。
- `McpClient` 连接模式：`Auto`（默认，探测 `server/discover`，失败回退 `initialize`）、`Legacy`（仅 initialize）、`Pin`（固定版本）。
- `SetNegotiatedProtocolVersion(version)` 存储版本并重建 `WireCodec`。

## 关键协议模式

- **2026-07-28+（现代）**：无状态。`server/discover` 取代 `initialize`/`initialized`。每请求 `_meta` 携带 `protocolVersion`、`clientInfo`、`clientCapabilities`、`logLevel`。
- **MRTR**：服务端发起的 elicitation 以 `InputRequiredResult` 内嵌。
- **订阅**：`subscriptions/listen` 取代 `resources/subscribe`。
- **缓存**：带 `ttlMs`/`cacheScope` 的 `CacheHint`。
- **Mcp-Method 头**：动态生成，取自 JSON-RPC 消息体的 method 字段（用于 Streamable HTTP + SSE）。

## 通知处理器

全部 17 种通知类型注册于 `WireCodec.cpp` 编解码器集合。服务端处理器在 `McpServer::WireHandlers()` 接线；客户端处理器在 `McpClient::WireClientHandlers()`。通知经 `McpSessionHandler::OnNotification()` 分发——未知通知**静默丢弃**（无 catch-all 处理器）。

**注意事项**：
- `notifications/cancelled` 在 `OnNotification()` 中硬编码处理，先于处理器表查找（并非 `SetNotificationHandler` 注册）。
- `logging/setLevel` 是**请求**（非通知）——新增服务端时必须注册到 `WireHandlers()`。
- 进度通知处理器调用 `ResetTimeoutByProgressToken()`，通过 `progress_token_map_` → `pending_` 查找将截止时间延长 30 秒。定义于 `McpSessionHandler`，在 `McpServer::WireHandlers()` 中接线。

## 服务端选项与事件钩子

`ServerOptions` 暴露四层回调：简写（`on_method_called`、`on_protocol_error`）、完整消息（`on_request`、`on_response`、`on_error`、`on_notification`）、生命周期（`on_client_connected`、`on_initialized`）、传输层（`on_transport_close`、`on_transport_error`）。做认证/审计/限流时，通过 `incoming_filters`/`outgoing_filters` 注入 `FilterPipeline`。

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

## 测试

`InMemoryTransport` 是**同步**的——消息在 `Send()`/`AsyncReceive()` 时交付（`MessageChannel` 是有界队列，默认 64），无外部事件循环。

**OpenSSL 陷阱**：PKCE 内置 SHA-256 回退（`include/mcp/detail/sha256.hpp`），但 `src/client/auth/OAuthClientProvider.cpp` 有无保护的 `#include <openssl/rand.h>`——未安装 OpenSSL 开发头文件的机器无论如何都无法编译 `mcp-client`。

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

## 提交

带 scope 的 Conventional Commits：`feat(transport):`、`fix(server):` 等。scope：`client`、`server`、`protocol`、`transport`、`http`、`core`、`build`、`test`、`examples`。
