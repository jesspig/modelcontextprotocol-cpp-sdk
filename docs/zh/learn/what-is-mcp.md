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

## 与官方 SDK 的分级对照

本 SDK 参考了官方 MCP 协议规范和参考实现：

| 语言 | 仓库 | 分级 |
|------|------|------|
| Python | [modelcontextprotocol/python-sdk](https://github.com/modelcontextprotocol/python-sdk) | Tier 1 |
| TypeScript | [modelcontextprotocol/typescript-sdk](https://github.com/modelcontextprotocol/typescript-sdk) | Tier 1 |
| Go | [modelcontextprotocol/go-sdk](https://github.com/modelcontextprotocol/go-sdk) | Tier 1 |
| C# | [modelcontextprotocol/csharp-sdk](https://github.com/modelcontextprotocol/csharp-sdk) | Tier 1 |
| Rust | [modelcontextprotocol/rust-sdk](https://github.com/modelcontextprotocol/rust-sdk) | Tier 1 |
| Ruby | [modelcontextprotocol/ruby-sdk](https://github.com/modelcontextprotocol/ruby-sdk) | Tier 1 |
| Java | [modelcontextprotocol/java-sdk](https://github.com/modelcontextprotocol/java-sdk) | Tier 2 |
| Swift | [modelcontextprotocol/swift-sdk](https://github.com/modelcontextprotocol/swift-sdk) | Tier 3 |
| PHP | [modelcontextprotocol/php-sdk](https://github.com/modelcontextprotocol/php-sdk) | Tier 3 |
| Kotlin | [modelcontextprotocol/kotlin-sdk](https://github.com/modelcontextprotocol/kotlin-sdk) | Tier 3 |

分级依据[官方 SDK 列表（2026-07-28 修订版）](https://modelcontextprotocol.io/docs/2026-07-28/sdk)：6 个 Tier 1（TypeScript、Python、C#、Go、Rust、Ruby——Rust 经 [PR #3287](https://github.com/modelcontextprotocol/modelcontextprotocol/pull/3287) 于 2026-08-21 晋升），Java 为 Tier 2，Swift/PHP/Kotlin 为 Tier 3。分级规则见 [SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers)——实验性功能与协议扩展（如 Tasks、MCP Apps）任何分级均不强制要求。

### Ruby SDK 对照注记

官方 Ruby SDK（`mcp` gem，2026-10-11 时 `main` 分支现状；最新发布 `1.7.0`，日期 2026-10-05，见 [CHANGELOG.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/CHANGELOG.md) 与 [version.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/version.rb)）相对本 SDK 的分维度实现要点：

| 维度 | Ruby（`mcp` gem） | 本 SDK |
|------|------------------|--------|
| 规范版本 | 2025-06-18、2025-11-25、2026-07-28（[ROADMAP.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/ROADMAP.md)，Current Status） | 2024-11-05…2026-07-28，双时代编解码（见[协议版本](/zh/learn/protocol-versions)） |
| `server/discover` | 现代时代永不经 `initialize` 协商（CHANGELOG §1.2.0）；`server/discover` 定稿并带现代客户端支持；stdio 按传输 era lock，Streamable HTTP 走无会话现代路径（[methods.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/methods.rb)） | Auto 模式按传输分类回退（[版本协商](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/version-negotiation.md)） |
| 多轮请求 | SEP-2322 `input_required` 结果（`inputRequests`/`inputResponses` + `requestState`），legacy 线上亦可履行（CHANGELOG §0.24.0/§1.2.0）；`resultType` 仅 `complete`/`input_required`（[result_type.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/result_type.rb)） | `InputRequiredResult` + elicitation 循环（[MRTR](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/mrtr.md)）；另有任务句柄专用的 `task` 结果类型 |
| 请求头 | 发送 SEP-2243 `Mcp-Method`/`Mcp-Name`，`x-mcp-header` 参数镜像为 `Mcp-Param-*`（CHANGELOG §0.22.0/§1.2.0；[mcp_param_headers.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/client/mcp_param_headers.rb)）；现代路径强制要求 `Mcp-Method` | `Mcp-Name` 校验含 tasks `taskId` 比对（[MCP 参数头](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/mcp-param-headers.md)、[任务扩展](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/tasks-extension.md)） |
| 缓存 | 服务端标注 SEP-2549 `ttlMs`/`cacheScope`（CHANGELOG §0.24.0/§1.2.0）；无客户端响应缓存——`lib/mcp/client/` 下仅 elicitation、http、mcp_param_headers、modern_envelope、oauth（+oauth/ 子目录）、paginated_result、stdio、tool，且 `client/http.rb` 全文无 `ttlMs`/`cacheScope`/缓存命中逻辑（2026-10-13 对 `main` 分支核实） | 客户端响应缓存，同时消费 2026 时代顶层 `ttlMs`/`cacheScope` 与 2025 时代嵌套 `cacheHint`，TTL + public/private 双分区（[响应缓存](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/response-cache.md)） |
| 认证 | 完整 OAuth 2.1 客户端（DCR、PKCE、CIMD、`client_credentials`、`private_key_jwt`、`jwt-bearer`、RFC 9207 `iss`、PRM 优先发现；CHANGELOG §0.17.0–§1.6.0）+ 服务端资源服务器保护（[README Features](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/README.md)） | PKCE/CIMD（DCR 回退）+ `FileTokenCache`，服务端 Bearer 鉴权，可选的服务端 DPoP 证明校验（[DPoP](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/dpop.md)；见[为客户端接入 OAuth](/zh/howto/add-oauth-to-a-client)） |
| 任务（SEP-2663） | 未实现——“remains planned”（[ROADMAP.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/ROADMAP.md)，[#391](https://github.com/modelcontextprotocol/ruby-sdk/issues/391)） | `tasks/get\|update\|cancel` 双时代 + `FileTaskStore`（[任务扩展](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/tasks-extension.md)） |
| 技能（SEP-2640） | 未实现【假设：`methods.rb` 方法全集、`lib/mcp/`、`lib/mcp/client/`、`test/mcp/` 均无 skills 条目，CHANGELOG 全版本无 skills 记载】 | `skills/list\|get` + `resources/directory/read`，仅 2026 时代（[技能扩展](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/wiki/concepts/skills-extension.md)） |

## 本 SDK 不做什么

- **不提供宿主实现**：没有宿主类型、没有会话编排器、没有 LLM 调用封装；把客户端接进你的应用是宿主侧的工作。
- **不做服务端列表发现**：连接对象由调用方构造并传入（`McpClient::Create(transport, options)`），SDK 不会去注册表或网络里查找有哪些服务端。协议里的 `server/discover` 是单条连接上的握手方法，与服务发现无关。
- **不内置鉴权体系**：只提供 Bearer 令牌校验钩子、OAuth 客户端流程与可选 DPoP；身份提供方、令牌签发与策略存储都在 SDK 之外。
- **不管理进程生命周期以外的编排**：子进程 stdio 传输会拉起并回收子进程，但没有集群、重启、限流与路由层。

### 明确不支持的协议特性

以下条目明确声明为不支持；当对端发送它们时，本 SDK 优雅降级（忽略未知字段或回退到基线路径），而不会中断会话。

- **应用界面渲染**：服务器推送的 UI 组件不渲染，仅作不透明数据透传。
- **完整 sampling**：sampling 仅为 legacy partial：已废弃的 `sampling_handler` 只覆盖基本 form 与单内容采样，modern 2026 时代不视为完整能力；完整 sampling 不在支持范围内，也不列为 Tier 1 收口项。
- **EMA**：未实现 EMA 协商，对端回退到基线能力协商。
- **DPoP 客户端逐请求证明**：服务端证明校验已实现（默认关，实验开关 `enable_dpop`，需 OpenSSL，见[为客户端接入 OAuth](/zh/howto/add-oauth-to-a-client)）；未绑定 `apply_per_request_headers`/`observe_response_challenge` 两回调时不附证明，已配置时可驱动 DPoP 会话并单次 `use_dpop_nonce` 重试；开关关闭时纯 Bearer 路径行为不变。
- **jwt-bearer**：已支持 `private_key_jwt` 客户端认证（配置 `private_key_pem` 且授权服务器在 `token_endpoint_auth_methods_supported` 中声明 `private_key_jwt` 时，向令牌请求发送 ES256 客户端断言；需 OpenSSL），但不支持授权方 `jwt-bearer` 授权，公开客户端 PKCE/CIMD 流程仍为受支持路径。
- **文件上传**：未实现可续传文件上传原语，文件内容仅经现有工具/资源载荷流转。

## 延伸阅读

- [/zh/learn/architecture](/zh/learn/architecture) — 库分层、门面与引擎、线程模型
- [/zh/learn/transports](/zh/learn/transports) — 五种传输的选型与有状态语义
- [/zh/learn/protocol-versions](/zh/learn/protocol-versions) — 双时代可用性的唯一权威表
- [/zh/learn/extensions](/zh/learn/extensions) — 扩展开关族与显式声明要求
- [/zh/reference/types](/zh/reference/types) — 类型与字段表
- [/zh/howto/register-a-tool](/zh/howto/register-a-tool) — 工具元语的落地写法
- [/zh/howto/serve-a-resource](/zh/howto/serve-a-resource) — 资源与模板的暴露方式
