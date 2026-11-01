# 传输

传输只负责把 JSON-RPC 信封搬到对端，不裁决协议语义——时代差异全部在编解码层，见 [/zh/learn/architecture](/zh/learn/architecture)。本页讲选型、有状态与无状态的分界、会话 id 的来源，以及传输类别如何影响版本协商的回退。字段表在 [/zh/reference/transports](/zh/reference/transports)。

## 五种传输

| 传输 | 客户端 | 服务端 | 承载方式 | 适用 |
|------|--------|--------|---------|------|
| stdio | `StdioClientTransport` | `StdioServerTransport` | 子进程的标准输入输出管道 | 本机一对一，宿主拉起服务端进程 |
| Streamable HTTP | `StreamableHttpClientTransport` | `StreamableHttpServerTransport` | HTTP POST 承载消息，可选 GET 流承载通知与流式结果 | 远程部署的默认选择 |
| SSE | `SseClientTransport` | 无独立服务端类 | 一条 GET 长连接推送 + POST 上行 | 只对接仍用旧 SSE 端点的服务端 |
| WebSocket | `WebSocketClientTransport` | 无 | TCP 上的双向帧 | 对端只提供 WebSocket 时 |
| InMemory | `InMemoryTransport::CreatePair()` 的两端 | 同左 | 同进程内两条队列互连 | 单元测试与同进程装配 |

两处必须写清的事实：**SSE 与 WebSocket 都只有客户端实现**。服务端侧的 SSE 流由 `StreamableHttpServerTransport` 在同一端点上按 `enable_legacy_sse` 开启（默认关闭，关闭时不注册 GET 路由，监听流与事件回放随之不可用），不存在单独的「SSE 服务端类」。把 InMemory 当成网络替代品或指望 WebSocket 连本 SDK 的服务端，都会在最开始就撞上不存在的类。

## 怎么选

- 服务端与宿主在同一台机器、由宿主负责拉起进程：stdio。没有端口、没有鉴权面、生命周期随进程结束。
- 服务端独立部署、要多客户端或跨网络：Streamable HTTP。其余三种都是「对端已有既定形态」时的兼容路径。
- 测试里想跳过序列化与网络：InMemory，它同样走完整的引擎与编解码路径，能验证协议行为。

## 有状态与无状态

分界线是「服务端是否保存跨请求的会话」。

- **有状态**：服务端为每个客户端会话保存协商版本、能力、初始化状态与订阅表；后续请求必须带同一个会话标识，否则被判为过期会话。
- **无状态**：每个请求独立处理，服务端不保留会话；请求自带全部上下文（现代时代靠每请求 `_meta` 携带版本与客户端信息，见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)）。

Streamable HTTP 服务端默认无状态（`StreamableHttpServerOptions::stateless` 默认 `true`）；客户端侧默认有状态（`HttpClientTransportOptions::stateless` 默认 `false`），必须与服务端一致，否则客户端会持续发送一个服务端从不认识的会话头。stdio、SSE、WebSocket、InMemory 四种传输没有「模式」概念：一条连接就是一条会话，等价于有状态。

## 会话 id 从哪里来

会话 id 的唯一来源是 HTTP 头 `mcp-session-id`：

- 有状态模式下，服务端在 `initialize` 成功时生成 id、写入会话存储，并在响应头回写同一个值；后续请求凭该头路由到同一会话。
- 头缺失且不是 `initialize` 请求时，会话 id 就是空串——落到空串即表示「没有会话上下文」。
- 会话未知或已过期返回 404 与错误码 `-32009`（SessionExpired）。
- 无状态模式直接跳过全部解析：会话 id 恒为空串。
- 解析出的值写进消息自身的会话字段，而不是传输对象的 `SessionId()`；非 HTTP 传输不解析会话头，两处都是空串。服务端把空串会话统一归并到自身的默认会话状态，因此本地传输天然是「一个实例一个会话」。

会话 id 的容量与闲置回收由服务端配置里的会话存储决定，不属于协议。

## `IsStateless()` 改变了什么

`ITransport::IsStateless()` 是传输向门面披露模式的唯一通道，服务端在构造时读一次并据此收窄功能。影响面：

- **多轮往返（MRTR）不可用**：无状态时 `McpServer::IsMrtrSupported()` 直接返回 `false`——MRTR 需要服务端记住「这一轮在等谁的输入」，无状态服务端记不住。要用 elicitation 反复收集输入，必须有状态。
- **通知交付路径改变**：无状态时不做 SSE 广播、不追加事件存储，只有与挂起请求关联的响应以 JSON 交付。
- **一批方法在无状态下被额外拒绝**：无状态且请求头版本未被钉在旧时代时，`ping`、`logging/setLevel`、`resources/subscribe`、`resources/unsubscribe` 一律返回「方法不存在」；`initialize` 同样被拒，但只在版本头是现代版本时——显式钉住旧时代的请求仍可走握手。反向请求（用户输入收集、采样、根目录）不在这条门里——它们是否可发由协议时代决定，见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。
- **版本头必须自洽**：无状态模式下请求头里的版本与 `_meta` 里的版本不一致会直接判为头不匹配。

## 协商回退按传输分类

客户端 Auto 模式（默认）先探测 `server/discover`，探测失败怎么处理取决于传输类别，而不是协议本身：

| 探测结果 | stdio 类传输 | 网络类传输 |
|---------|-------------|-----------|
| 探测超时 | 回退 `initialize` | 抛请求超时错误 |
| 请求异常 | 回退 `initialize` | 抛连接关闭错误 |
| 版本不支持且服务端 `supported` 含最新版本 | 用最新版本纠正重发一次 | 同左 |
| 纠正重发再次被拒 | 硬错误，不再回退 | 同左 |
| 其它错误码 | 回退 `initialize` | 回退 `initialize` |

「stdio 类」按运行时类型判定，只包含进程内传输与 stdio 客户端会话传输两种；其余一律算网络类。理由是本地进程要么活着要么死了，探测不通基本等于对端不认识这个方法；而网络探测不通可能是丢包、延迟或对端故障，静默回退会掩盖真实故障，因此宁可直接抛错。纠正重发只有一次预算，第二次拒绝即硬错误——这是为了防止两端互相试探形成循环。

三种连接模式与回退后的时代归属见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 延伸阅读

- [/zh/reference/transports](/zh/reference/transports) — 各传输配置结构体的字段表
- [/zh/learn/protocol-versions](/zh/learn/protocol-versions) — 时代判定与双时代矩阵
- [/zh/learn/architecture](/zh/learn/architecture) — 传输与引擎的线程模型
- [/zh/learn/extensions](/zh/learn/extensions) — 无状态如何影响扩展可用性
