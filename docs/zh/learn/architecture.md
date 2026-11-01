# 架构

本页说明库如何分层、门面类与底层引擎的分工、线程与 IO 模型，以及几条会决定文档其余部分写法（「为什么这里要警告」）的构建期事实。字段表与签名以参考层为准：[/zh/reference/index](/zh/reference/index)。

## 库分层与依赖链

全部产品库都是静态库，共七个目标，另有一个纯接口目标 `mcp-openssl` 负责可选依赖的落地：

```text
mcp-core ──┬── mcp-transport ──┬── mcp-protocol ──┬── mcp-server
           │                   │                  └── mcp-client
           │                   └── mcp-http
           └─────────────────────── mcp-ext ──────┘
```

| 目标 | 链接依赖 | 内容 |
|------|---------|------|
| `mcp-core` | 线程库 | JSON 值模型、JSON-RPC 信封、协议数据类型、错误码与方法常量、能力结构、传输接口 |
| `mcp-transport` | `mcp-core` | 各传输实现与自研网络栈（TCP/TLS/HTTP/WebSocket）、平台 IO、子进程 |
| `mcp-protocol` | `mcp-transport` | JSON-RPC 引擎 `McpSessionHandler` 与双时代编解码 `WireCodec` |
| `mcp-http` | `mcp-transport`、`mcp-ext` | 自研 HTTP 服务端、事件存储、会话存储、Streamable HTTP 双端 |
| `mcp-ext` | `mcp-core`、`mcp-transport` | apps、DPoP、server card、triggers 四个扩展的实现 |
| `mcp-server` | `mcp-protocol`、`mcp-ext` | `McpServer` 门面、注册 API、任务存储 |
| `mcp-client` | `mcp-protocol`、`mcp-ext` | `McpClient` 门面、版本协商、OAuth 与令牌缓存 |

`mcp-ext` 是被常见描述漏掉的一环：它不是可选组件，服务端、客户端与 HTTP 三个目标都直接依赖它，链接任一门面时必然一起进二进制。

## 门面与引擎

自顶向下四层，每层只做一件事：

| 层 | 类型 | 职责 |
|----|------|------|
| 门面 | `McpServer` / `McpClient` | 注册与调用 API、能力推导、扩展开关裁决 |
| 引擎 | `McpSessionHandler` | 消息循环、请求响应关联、超时与取消、过滤器管线、按会话持有协商版本 |
| 编解码 | `WireCodec` | 按协商版本选择时代编解码器，决定方法是否属于本时代、`_meta` 与 `resultType` 怎么落线 |
| 队列 | `MessageChannel` | 有界异步队列，连接引擎与传输；满则阻塞发送（背压） |

关键点：**编解码器属于引擎而非传输**。传输只搬 JSON 文本，时代差异全部在 `WireCodec` 里裁决，所以换传输不会改变协议行为，反之亦然。

## 线程与 IO 模型

没有 asio，只有裸线程加标准库同步原语。

| 组件 | 线程 |
|------|------|
| `McpSessionHandler` | 消息循环线程 + 超时检查线程（100ms 轮询）+ 单一响应回发线程 |
| stdio 传输 | 读循环线程 |
| SSE 客户端 | SSE 读线程 + 发送线程 |
| Streamable HTTP 客户端 | 发送线程 + GET 监听线程 + 通知/响应的短命分离线程 |
| HTTP 服务端 | accept 线程 + 每连接线程（上限 256）+ SSE keepalive 线程 |

请求处理程序在消息循环线程上被同步调用，响应由单一回发线程消费 promise——**不是每请求一个线程**。因此 handler 里长时间阻塞会拖住这条连接上的一切其它消息，包括超时检查之外的收包路径；要并行就自己在 handler 内把工作交给别的线程并把结果放进 promise。

## 回调内关闭会自我 join

stdio、SSE、HTTP 三种传输的 IO 线程直接执行用户回调（`on_transport_close`、`on_transport_error`）。在回调里调用 `Close()` 就是让线程 join 自己，会抛异常。SDK 内部一律经 `detail::JoinThreadSafely`：线程是自己时改为分离（detach），否则正常 join。这条约束的后果有两个方向：

- 分离后的线程仍会继续跑到自然退出，所以传输对象必须在读循环入口自持有，否则就是释放后使用。
- 关闭时仍存活的分离线程不再被等待，不要假设 `Close()` 返回后所有 IO 都已经停止。

## 能力由注册推导

`ServerOptions` 没有 `capabilities` 字段：服务端能力是注册行为的结果，不是声明的输入。工具、资源、提示三项在注册表非空时自动出现；日志与补全两项必须显式开关；扩展项由开关决定（见 [/zh/learn/extensions](/zh/learn/extensions)）。这样做的理由是避免「声明了能力但没有实现」这一类不一致：能对外声明的，必然是内部已经接好线的。

## 构建期事实

以下四条不是实现细节，而是文档中若干警告的来源，展开写法归操作层。

- **unity build 默认开启，但只覆盖 `mcp-core`/`mcp-server`/`mcp-client`/`mcp-http`/`mcp-ext`**；`mcp-protocol` 与 `mcp-transport` 显式关闭。新增源文件时同名静态符号是否冲突，取决于所在目标。
- **Release 构建自动开过程间优化**：Clang 走 ThinLTO、MSVC 与 clang-cl 走 LTCG、GCC 走 IPO；本地 Release 链接变慢是正常现象。
- **非 CI 且非 Apple 且非 Windows 的构建默认加 `-march=native`**（CI 判定读环境变量 `CI`）。产物因此不可跨机分发；要给别的机器用必须显式关掉。
- **OpenSSL 是可选依赖**（`MCP_WITH_OPENSSL=AUTO|ON|OFF`），可用性由单一接口目标施加编译宏与链接。禁止用「头文件能否被包含」来推断 OpenSSL 可用——头可见不等于已定义宏，更不等于已链接。

## 延伸阅读

- [/zh/learn/what-is-mcp](/zh/learn/what-is-mcp) — 协议角色与本 SDK 边界
- [/zh/learn/transports](/zh/learn/transports) — 传输分层与会话语义
- [/zh/learn/protocol-versions](/zh/learn/protocol-versions) — `WireCodec` 的双时代裁决
- [/zh/learn/extensions](/zh/learn/extensions) — 开关与能力推导的对应关系
- [/zh/reference/process-and-storage](/zh/reference/process-and-storage) — 子进程与存储类型
