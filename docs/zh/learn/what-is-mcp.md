# 什么是 MCP

模型上下文协议（Model Context Protocol）是一套开放标准，规定「LLM 应用」与「外部数据源、工具」之间如何互相发现与调用。它只规定消息格式与方法语义：不规定实现语言，不规定宿主如何拼装上下文，也不规定服务端如何存储数据。

本层文档回答「这是什么、为什么这样设计、本 SDK 的实现边界在哪里」。具体怎么写代码属于参考层与操作层，见 [/zh/reference/index](/zh/reference/index)。

## 三个角色

| 角色 | 职责 | 本 SDK 是否提供 |
|------|------|----------------|
| 宿主（host） | 承载 LLM、持有用户界面、决定是否调用某个能力 | 否 |
| 客户端（client） | 与恰好一个服务端维持一条连接，双向转发消息 | 是：`McpClient` |
| 服务端（server） | 暴露能力并应答来自对端的请求 | 是：`McpServer` |

一个宿主通常同时持有多个客户端，每个客户端连一个服务端。「客户端」是宿主内部的连接器，不是面向用户的应用程序，这是初次接触该协议时最容易混淆的一点。

## 消息层

客户端与服务端之间的全部往来都是 JSON-RPC 2.0 消息，本 SDK 用四个结构表达（`include/mcp/JsonRpc.hpp`）：请求（带 `id`）、通知（不带 `id`）、成功响应、错误响应。协议自身的方法名、错误码、能力声明都建立在这四类信封之上，因此任何未被 SDK 建模的扩展方法仍可用 `SendRequest` 原样发出。

## 能力支持矩阵

| 能力 | 提出方 | 当前可用性 | 权威页 |
|------|--------|-----------|--------|
| 工具（tools） | 服务端 | 完整，注册即声明能力 | [/zh/reference/server](/zh/reference/server) |
| 资源（resources）与资源模板 | 服务端 | 完整，注册即声明能力 | [/zh/reference/server](/zh/reference/server) |
| 提示（prompts） | 服务端 | 完整，注册即声明能力 | [/zh/reference/server](/zh/reference/server) |
| 参数补全（completion） | 服务端 | 需另设补全回调，未设则该入口按「方法不存在」拒绝 | [/zh/reference/server](/zh/reference/server) |
| 技能（skills） | 服务端 | 需显式开关，且仅现代协议时代可用 | [/zh/reference/auth-and-extensions](/zh/reference/auth-and-extensions) |
| 任务（tasks） | 服务端 | 需显式开关加任务存储，双时代可用 | [/zh/reference/auth-and-extensions](/zh/reference/auth-and-extensions) |
| 用户输入收集（elicitation） | 服务端发起、客户端应答 | 完整；表单与 URL 两种模式 | [/zh/reference/client](/zh/reference/client) |
| 采样（sampling） | 服务端发起、客户端应答 | 可调用但已废弃（SEP-2577），新代码用 elicitation | [/zh/reference/client](/zh/reference/client) |
| 根目录（roots） | 客户端应答 | 可调用但已废弃（SEP-2577） | [/zh/reference/client](/zh/reference/client) |

采样与根目录的废弃标记落在客户端的设置入口上（`McpClient::SetSamplingHandler`、`McpClient::SetRootsHandler` 带 `[[deprecated]]`），字段与旧时代的线路行为都还在：**没有「已移除」**，在旧时代连接上它们仍然工作。现代时代里 `sampling/createMessage` 不再作为独立请求存在，只能作为多轮往返（MRTR）的输入请求出现，且服务端会先确认客户端声明过采样能力。详见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 两个协议时代

本 SDK 同时支持五个协议版本，分属两个时代：旧时代用 `initialize` 握手并允许服务端直发反向请求，现代时代（`2026-07-28` 起）用 `server/discover`、每请求 `_meta`、MRTR 与订阅流。任何能力可用性讨论都必须先说明时代，否则结论没有意义。版本矩阵只在本页之外的一份文档里维护：[/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

## 本 SDK 的定位

- **两端都做**：`McpServer` 与 `McpClient` 是平级的两个门面，不存在「只能写服务端」。
- **全静态库**：没有动态库、没有插件机制、没有需要随包分发的 DLL。链接方式即集成方式，见 [/zh/learn/architecture](/zh/learn/architecture)。
- **自带网络栈**：TCP/TLS/HTTP/WebSocket 均为自研实现，不依赖 asio；OpenSSL 是唯一可选的系统级外部依赖（缺失时仅禁用 TLS，其余功能完整）。

## 本 SDK 不做什么

- **不提供宿主实现**：没有宿主类型、没有会话编排器、没有 LLM 调用封装；把客户端接进你的应用是宿主侧的工作。
- **不做服务端列表发现**：连接对象由调用方构造并传入（`McpClient::Create(transport, options)`），SDK 不会去注册表或网络里查找有哪些服务端。协议里的 `server/discover` 是单条连接上的握手方法，与服务发现无关。
- **不内置鉴权体系**：只提供 Bearer 令牌校验钩子、OAuth 客户端流程与可选 DPoP；身份提供方、令牌签发与策略存储都在 SDK 之外。
- **不管理进程生命周期以外的编排**：子进程 stdio 传输会拉起并回收子进程，但没有集群、重启、限流与路由层。

## 延伸阅读

- [/zh/learn/architecture](/zh/learn/architecture) — 库分层、门面与引擎、线程模型
- [/zh/learn/transports](/zh/learn/transports) — 五种传输的选型与有状态语义
- [/zh/learn/protocol-versions](/zh/learn/protocol-versions) — 双时代可用性的唯一权威表
- [/zh/learn/extensions](/zh/learn/extensions) — 扩展开关族与显式声明要求
- [/zh/reference/types](/zh/reference/types) — 类型与字段表
