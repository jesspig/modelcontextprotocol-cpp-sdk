# 跑起 Streamable HTTP 服务端

把一个服务端挂在 HTTP 端口上，让远程客户端连进来：装配传输、管住会话、决定有状态还是无状态。

## 前置条件

- 链接 `mcp-server` 与 `mcp-http`（依赖自动传递，见[安装与集成](/zh/start/installation)）。
- 明文 HTTP。TLS 不由这层传输提供，需要 TLS 时把服务放在反向代理后面。
- 端口默认 `3001`、端点默认 `/mcp`（`include/mcp/transport/StreamableHttpServerTransport.hpp:29-41`）。

## 有状态还是无状态

`StreamableHttpServerOptions::stateless` **默认是 `true`**（同文件 `:43`）。两种模式的差别不是「有没有会话」这么轻：

| 维度 | 有状态 | 无状态 |
|---|---|---|
| 会话标识 | `initialize` 生成并经响应头 `mcp-session-id` 回发（`src/http/StreamableHttpServerTransport.cpp:1172`） | 不签发会话，每个请求自带上下文 |
| 后续请求 | 必须带 `mcp-session-id`，缺失或未知回 404（`:541-543`、`:783-786`） | 无需会话头 |
| 服务端保存的状态 | 按会话隔离协商版本、客户端能力、订阅与任务 owner | 每请求独立 |
| 闲置回收 | 超过 `detail::kSessionIdleTimeoutMs` 判过期（`:818`、`:868`、`include/mcp/http/SessionStore.hpp:104`） | 不涉及 |
| 典型协议时代 | 走 `initialize` 握手，协商结果落在旧时代 | 走 `server/discover`，可用现代时代 |

选型规则：需要服务端主动推（订阅、任务状态、日志广播）或要复用一次握手的上下文，用有状态；只想被当无连接 API 调用、方便横向扩容，用无状态。

## 步骤一：装配有状态服务端

关键字段只有三个：`stateless = false`、`session_store` 给一个实现、`allowed_hosts`/`allowed_origins` 白名单。完整写法见本页「完整代码」的 `stateful` 段。

- `session_store` 置空时会用传输内部的本地会话表；要跨进程共享或自定义过期策略才注入 `SessionStore` 实现（内置 `InMemorySessionStore`）。
- Host/Origin 校验失败会被拒，别把白名单留空当作「允许所有」。

## 步骤二：会话生命周期

1. 客户端 `POST /mcp` 发 `initialize`，不带会话头。
2. 服务端生成会话 id，在响应头回 `mcp-session-id`。
3. 之后每个请求都要带上这个头。**会话头值未知或已过期**回 404，响应体里的码是 `SessionExpired`（`-32009`，`:783-790`）；**完全不带头**的非 `initialize` 请求不会被当成未知会话，而是按空会话处理并因缺少握手上下文回 400（`:795-798`，实测）。
4. 会话被关闭时，它名下仍在等待的挂起请求会以 JSON-RPC 错误 `SessionExpired`（`-32009`，`:947-952`）结算，不会永远吊着。
5. `DELETE` 只关闭该会话，清掉它的挂起请求与事件回放记录，不影响同端口其他会话。

## 步骤三：无状态与保持打开的 POST 流

<<< @/snippets/run-a-streamable-http-server.cpp#stateless-hold-open

`listen_hold_open` 让服务端把 progress、日志等通知按 `progressToken` 注回所属 POST 的 SSE 流，客户端因此能在同一个请求里既拿到通知又拿到最终响应；无状态模式下不开这个开关，服务端主动通知就没有落点。

## 步骤四：断线回放与旧版 SSE

- `event_store`（可注入 `FileEventStore`）负责按会话分区的事件追加与回放，客户端 `GET` 带 `Last-Event-ID` 时从那之后续传。
- `enable_legacy_sse` 打开后额外提供旧的 SSE 端点形态，给尚未支持 Streamable HTTP 的宿主用；`sse_keep_alive_ms`（默认 15000）控制心跳间隔。
- `max_request_body_bytes`、`max_sse_event_bytes` 是入站与出站体量上限，超限即拒，不静默截断。
- 需要校验 `Mcp-Param-*` 头与 `tools/call` body 一致时，把 `resolve_param_annotations` 接到 `McpServer::ResolveToolParamAnnotations`（见 `examples/conformance/server/main.cpp:149-153` 的真实接法）。

## 完整代码

<<< @/snippets/run-a-streamable-http-server.cpp#stateful

<<< @/snippets/run-a-streamable-http-server.cpp#stateless

## 验证

先做语法检查（这条命令已实测通过）：

```bash
cmake -S . -B build/docs-snippets -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/docs-snippets --target mcp-doc-snippet-run-a-streamable-http-server
```

再握手一次（下列状态码与响应头为实测：用走同一套 `StreamableHttpServerTransport` 代码路径的 `examples/conformance/server`，`--port` 自选后按同样顺序请求）：

```bash
curl.exe -i -X POST http://127.0.0.1:3001/mcp \
  -H "Content-Type: application/json" \
  -H "Accept: application/json, text/event-stream" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"curl","version":"0"}}}'
```

- 期望 `200`，响应头含 `mcp-session-id: sess-<32 位 hex>` 与 `mcp-protocol-version: 2025-11-25`，`content-type` 为 `text/event-stream`。
- 用该会话头再发 `notifications/initialized` → `202`；发 `tools/list` → `200`，能看到本页示例注册的 `ping`。
- 把会话头换成一个不存在的值 → `404`；去掉会话头 → `400`。

## 时代差异

有状态走 `initialize` 握手，协商结果只会落在旧时代；无状态配合 `listen_hold_open` 才能走现代时代的 `server/discover` 单请求模型。矩阵见[协议版本](/zh/learn/protocol-versions)。

## 常见错误

| 现象 | 成因 | 处置 |
|---|---|---|
| 404 `Not Found`，带内码 `-32009`（`Session expired`） | 会话头值是未知或已过期（闲置超 `kSessionIdleTimeoutMs`、或会话已 `DELETE`/关闭） | 客户端重发 `initialize` 取新会话 |
| 400 | 非 `initialize` 请求没带 `mcp-session-id`，被当作空会话却没有握手上下文 | 补上会话头；确认先发 `initialize` |
| `InvalidParams`，消息为 `unsupported protocol version`（`-32022`） | 声明的版本不在支持表内 | 用 `ServerOptions::supported_protocol_versions` 窄化前先确认对端能力 |
| 端口占用导致启动失败 | 默认 3001 已被占 | 改 `http_options.port` |
| 客户端收不到进度或日志 | 无状态且没开 `listen_hold_open` | 开该开关，或改有状态 + 订阅 |

## 下一步

- 让通知进同一条流：[上报进度](/zh/howto/report-progress)
- 长耗时工具改任务化：[跑长任务](/zh/howto/run-long-tasks)
- 全部字段与签名：[传输层参考](/zh/reference/transports)
- 传输选型原理：[传输层概念](/zh/learn/transports)

## 本页对应源码

- `include/mcp/transport/StreamableHttpServerTransport.hpp`、`src/http/StreamableHttpServerTransport.cpp`、`include/mcp/http/SessionStore.hpp`、`include/mcp/http/EventStore.hpp`、`include/mcp/storage/FileEventStore.hpp`、`examples/conformance/server/main.cpp`
