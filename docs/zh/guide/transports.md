# 传输层

| 传输层            | 客户端 | 服务器 | 描述                                     |
|------------------|--------|--------|------------------------------------------|
| Stdio           | 是     | 是     | stdin/stdout 管道，子进程通信              |
| Streamable HTTP | 是     | 是     | HTTP POST 配合流式服务器发送事件           |
| SSE             | 是     | 否     | 用于推送通知的服务器发送事件                |
| WebSocket       | 是     | 否     | 基于 TCP 的双向通信（简化的 RFC 6455）     |
| InMemory        | 是     | 是     | 进程内通信，用于测试                       |

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

## 无状态传输

`StreamableHttpServerTransport` 的 `IsStateless() == true`。当检测到时，服务器会禁用 MRTR（`InputRequiredResult`）——请求必须通过 `_meta` 和 `requestState`（用于跨请求状态恢复的不透明令牌）携带完整上下文。
