# 传输层

| 传输层            | 客户端 | 服务器 | 描述                                     |
|------------------|--------|--------|------------------------------------------|
| Stdio           | 是     | 是     | stdin/stdout 管道，子进程通信              |
| Streamable HTTP | 是     | 是     | HTTP POST 配合流式服务器发送事件           |
| SSE             | 是     | 是¹   | 用于推送通知的服务器发送事件                |
| WebSocket       | 是     | 否     | 基于 TCP 的双向通信（简化的 RFC 6455）     |
| InMemory        | 是     | 是     | 进程内通信，用于测试                       |

## Streamable HTTP

Streamable HTTP 传输实现了 MCP Streamable HTTP 规范。每个会话使用一个内部的 `StreamableHttpSessionTransport`（封装 `ITransport`），配备独立的 `asio::io_context`。

**请求流程**：客户端通过 HTTP POST 发送 JSON-RPC 消息。服务端验证 `Mcp-Method` 和 `Mcp-Name` 头是否与消息体匹配（SEP-2243），在响应中回显这些头，并将 `Mcp-Param-*` 头提取到 `_meta.x-mcp-headers`。如果响应是 JSON，服务端在**无状态**模式下同步响应（通过 `std::promise` 进行待处理响应关联），或在会话模式下返回 `202 Accepted`。如果响应是 SSE（`text/event-stream`），服务端保持连接打开并流式推送事件。

**SSE 读取循环**：在客户端，当 POST 响应的 `Content-Type` 为 `text/event-stream` 时，后台线程（`SseReadLoop`）读取数据块，按 `\n\n` 分隔符拆分，解析 `data:` 行，并将 `JsonRpcMessage` 入队到 `MessageChannel`。

**请求头**（`StreamableHttpClientTransport`）：
- `MCP-Protocol-Version: 2026-07-28` — 始终发送
- `Mcp-Method` — 动态生成，从 JSON-RPC 消息体的 method 字段提取
- `Mcp-Param-*` — 提取原始参数用于中间件路由（仅支持字符串、整数、布尔值）
- `Accept: application/json, text/event-stream` — 允许服务端选择响应模式

## 传输层接口

```
ITransport（会话连接）
  └── TransportBase（三状态：初始 → 已连接 → 已断开）
      ├── StdioServerTransport
      ├── InMemoryTransportImpl
      ├── StreamableHttpServerTransport
      └── WebSocketTransport

IClientTransport（连接工厂）
  ├── StdioClientTransport
  ├── SseClientTransport
  ├── StreamableHttpClientTransport
  └── WebSocketClientTransport
```

## 关键类型

### `HttpTransportMode`
控制 HTTP 客户端传输的连接方式：

| 值              | 描述                                                |
|-----------------|-----------------------------------------------------|
| `AutoDetect`    | 先探测 `server/discover`；回退到 SSE POST           |
| `StreamableHttp`| 直接使用 Streamable HTTP（需要 2026-07-28+）        |
| `Sse`           | 仅使用传统 SSE POST                                 |

### `StdioClientTransportOptions`
| 字段                           | 类型                              | 默认值   | 描述                    |
|-------------------------------|-----------------------------------|----------|-------------------------|
| `command`                     | `std::string`                     | 必填     | 子进程命令              |
| `arguments`                   | `std::vector<std::string>`        | `{}`     | 命令行参数              |
| `name`                        | `std::string`                     | `"stdio"`| 传输层名称              |
| `working_directory`           | `std::string`                     | `""`     | 子进程工作目录          |
| `inherit_environment_variables` | `bool`                          | `true`   | 继承父进程环境          |
| `environment_variables`       | `std::map<std::string, std::string>` | `{}`  | 附加环境变量            |

### `HttpClientTransportOptions`
| 字段                 | 类型                                     | 默认值        | 描述                    |
|---------------------|------------------------------------------|--------------|-------------------------|
| `endpoint`          | `std::string`                            | 必填         | 服务端端点 URL          |
| `transport_mode`    | `HttpTransportMode`                      | `AutoDetect` | 连接模式                |
| `name`              | `std::string`                            | `""`         | 传输层名称              |
| `known_session_id`  | `std::string`                            | `""`         | 用于恢复的会话 ID       |
| `additional_headers`| `std::map<std::string, std::string>`     | `{}`         | 附加 HTTP 头            |

### `StreamableHttpServerOptions`
| 字段               | 类型                               | 默认值          | 描述                          |
|-------------------|------------------------------------|-----------------|-------------------------------|
| `port`            | `uint16_t`                         | `3001`          | HTTP 服务端口                 |
| `endpoint`        | `std::string`                      | `"/mcp"`        | HTTP 端点路径                 |
| `stateless`       | `bool`                             | `false`         | 启用 2026-07-28 无状态模式    |
| `enable_legacy_sse` | `bool`                           | `true`          | 在 GET 上提供 SSE 流          |
| `event_store`     | `std::shared_ptr<EventStore>`      | `nullptr`       | 用于恢复的事件存储            |
| `server_name`     | `std::string`                      | `"mcp-server"`  | 用于发现的服务器名称          |
| `server_version`  | `std::string`                      | `"0.1.0"`       | 用于发现的服务器版本          |

### `InMemoryTransport::Pair`
```cpp
struct Pair {
    std::shared_ptr<ITransport> client;
    std::shared_ptr<ITransport> server;
};
```

> ¹ 服务端 SSE 通过 `StreamableHttpServerTransport` 提供，当 `enable_legacy_sse` 为 `true`（默认值）时，在同一端点上通过 HTTP GET 提供 SSE 流。

## 无状态模式

`StreamableHttpServerTransport` 支持无状态模式，由 `StreamableHttpServerOptions::stateless` 控制（默认 `false`）。当为 `true` 时，`IsStateless()` 返回 `true`，并且：

- **无会话**：每个请求独立；通过 `std::promise` 同步关联响应，超时时间 30 秒。
- **无 SSE**：服务端发起的通知通过 JSON 响应当行传递；跳过 `EventStore` 追加。
- **MRTR 禁用**：服务器禁用 `InputRequiredResult`——请求必须通过 `_meta` 和 `requestState`（用于跨请求状态恢复的不透明令牌）携带完整上下文。
