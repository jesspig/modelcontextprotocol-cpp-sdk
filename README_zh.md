# MCP C++ SDK

> **English version**: [README.md](README.md)

[Model Context Protocol (MCP)](https://modelcontextprotocol.io) 的 C++17 实现，提供客户端和服务器库，用于构建基于 MCP 的 AI 工具集成。

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-windows%20%7C%20linux%20%7C%20macos-lightgrey.svg)

<details>
<summary>目录</summary>

- [什么是 MCP？](#什么是-mcp)
- [环境要求](#环境要求)
- [安装](#安装)
- [快速开始](#快速开始)
- [架构](#架构)
- [传输层](#传输层)
- [使用方式](#使用方式)
- [OAuth 支持](#oauth-支持)
- [协议版本](#协议版本)
- [一致性测试](#一致性测试)
- [示例](#示例)
- [相关项目](#相关项目)
- [不支持声明](#不支持声明)
- [许可证](#许可证)

</details>

## 什么是 MCP？

[Model Context Protocol](https://modelcontextprotocol.io) 允许你以安全、标准化的方式构建服务器，向 LLM 应用暴露数据和功能。使用此 SDK 你可以：

- **构建 MCP 服务器**，向任何 MCP 主机暴露工具、资源和提示词
- **构建 MCP 客户端**，连接到任何 MCP 服务器
- **支持所有标准传输层**：stdio、Streamable HTTP、SSE、WebSocket

## 环境要求

| 依赖          | 最低版本 | 说明                          |
|---------------|---------|-------------------------------|
| CMake         | 3.28    | 生成器：推荐 Ninja            |
| C++ 编译器    | C++17   | MSVC、Clang、GCC             |
| OpenSSL       | (可选)  | TLS 必需（WebSocket、SSE HTTPS）；OAuth PKCE 无需 OpenSSL（内置 SHA-256 回退）。安装：`vcpkg install openssl` / `apt install libssl-dev` / `brew install openssl` |

支持平台：**Windows** (MSVC、clang-cl)、**Linux** (GCC、Clang)、**macOS** (Clang)。

## 安装

在你的 CMake 项目中通过 `FetchContent` 使用：

```cmake
include(FetchContent)
FetchContent_Declare(
    mcp-cpp-sdk
    GIT_REPOSITORY https://github.com/jesspig/modelcontextprotocol-cpp-sdk.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(mcp-cpp-sdk)

target_link_libraries(your_target PRIVATE mcp-client mcp-server)
```

可用库目标：`mcp-core`、`mcp-transport`、`mcp-protocol`、`mcp-server`、`mcp-client`、`mcp-http`。所有库均为静态库。

## 快速开始

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

配置预设：`debug`、`release`。需要 Ninja 生成器。

## 架构

```
┌─────────────────────────────────────┐
│  mcp-server        mcp-client       │  服务器 & 客户端 API
├─────────────────────────────────────┤
│  mcp-protocol                       │  WireCodec、McpSessionHandler
├─────────────────────────────────────┤
│  mcp-transport       mcp-http       │  传输层、Streamable HTTP、SSE
├─────────────────────────────────────┤
│  mcp-core                           │  类型、JSON-RPC、接口
└─────────────────────────────────────┘
```

库依赖链：`mcp-core` (STATIC) → `mcp-transport` → `mcp-protocol` → `mcp-server | mcp-client`。`mcp-http` 依赖 `mcp-transport`。所有库均为静态库。

- **mcp-core** — 核心类型、JSON-RPC 消息结构、错误码、能力声明、内容类型、传输层接口。
- **mcp-transport** — 传输层实现：StdioServerTransport、StdioClientTransport、SseClientTransport、InMemoryTransport、WebSocketClientTransport、StreamableHttpServerTransport、StreamableHttpClientTransport。平台相关 I/O 在 `detail/` 目录下（posix、win32）。
- **mcp-protocol** — `McpSessionHandler`（JSON-RPC 引擎）、双时代 `WireCodec`（2025-11-25 / 2026-07-28）、请求/响应关联与超时、`MessageFilter` 管道。
- **mcp-server** — `McpServer`，含工具/资源/提示词注册、`IMcpTaskStore`（含 `FileTaskStore`）、MRTR（`InputRequiredResult`）、服务器到客户端的 elicit、订阅管理。
- **mcp-client** — `McpClient`，含服务器发现（`server/discover` → initialize 回退）、版本协商、OAuth（PKCE/CIMD，DCR 回退）、MRTR（InputRequired 循环）、工具缓存、`FileTokenCache`。
- **mcp-http** — 用于 Streamable HTTP 模式的 HTTP 服务器：`HttpServer`、`EventStore`（SSE 事件持久化）、`StreamableHttpServerTransport`、`StreamableHttpClientTransport`。

### 所有传输层

| 传输层          | 客户端 | 服务器 | 说明                                           |
|----------------|--------|--------|------------------------------------------------|
| Stdio          | 是     | 是     | stdin/stdout 管道，子进程通信                  |
| Streamable HTTP| 是     | 是     | HTTP POST 配合 JSON/会话模式响应               |
| SSE            | 是     | 是¹   | 用于推送通知的服务器发送事件                  |
| WebSocket      | 是     | 否     | TCP 双向通信（自研 WebSocketClient）          |
| InMemory       | 是     | 是     | 进程内传输，用于测试                           |

> ¹ 服务端 SSE 通过 `StreamableHttpServerTransport` 提供，当 `enable_legacy_sse` 为 `true` 时，在同一端点上通过 HTTP GET 提供 SSE 流。

## 使用方式

### 服务器

```cpp
#include <mcp/server/McpServer.hpp>
#include <mcp/transport/StdioServerTransport.hpp>

using namespace mcp;

int main() {
    auto transport = std::make_shared<StdioServerTransport>();

    ServerOptions opts;
    opts.server_info = Implementation{"MyServer", "1.0.0"};

    auto server = McpServer::Create(transport, opts);

    server->RegisterTool("echo",
        ToolOptions{}.Description("Echo input text back"),
        [](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
            std::string text;
            if (ctx.Params().arguments) {
                auto* v = ctx.Params().arguments->Find("text");
                if (v) text = v->GetString();
            }
            CallToolResult result;
            result.content.push_back(TextContent{"text", text});
            return result;
        });

    server->Run();
    return 0;
}
```

### 客户端

```cpp
#include <mcp/client/McpClient.hpp>
#include <mcp/transport/StdioClientTransport.hpp>

using namespace mcp;

StdioClientTransportOptions transport_opts;
transport_opts.command = "path/to/server";
auto factory = std::make_shared<StdioClientTransport>(transport_opts);
auto transport = factory->Connect();
ClientOptions opts;
opts.client_info = Implementation{"MyClient", "1.0.0"};

auto client = McpClient::Create(transport, opts);

auto tools = client->ListTools();
for (const auto& tool : tools.tools) {
    std::cout << tool.name << "\n";
}

auto result = client->CallTool("echo",
    JsonValue::Object{{"text", "Hello, MCP!"}});
```

## OAuth 支持

客户端支持 MCP OAuth 授权流程：

- **授权码 + PKCE** 流程，使用 S256 代码挑战
- CIMD 客户端标识与 DCR 回退（`client_id` 为 `http(s)://` URL 时拉取客户端元数据文档；仅未配置 `client_id` 时动态注册）
- Token 刷新和撤销
- 可插拔 Token 缓存（`ITokenCache`），内置持久化 `FileTokenCache`
- DPoP（RFC 9449）：Streamable HTTP 资源服务器侧已实现证明校验——默认关，按部署用实验开关 `enable_dpop`（`StreamableHttpServerOptions::bearer_auth->enable_dpop`）开启，且需 OpenSSL；客户端侧可切换绑定授权方案并签发证明（`OAuthClientOptions::enable_dpop`），但传输层尚未逐请求附加 `DPoP` 证明

```cpp
OAuthClientOptions oauth_opts;
oauth_opts.server_url = "https://auth.server.com/.well-known/oauth-authorization-server";
oauth_opts.client_id = "client-id";
oauth_opts.redirect_uri = "http://localhost:3000/callback";

OAuthClientProvider auth(oauth_opts);
auth.Authenticate();
```

## 协议版本

| 版本        | 状态    | 主要特性                                    |
|------------|---------|---------------------------------------------|
| 2025-11-25 | 遗留版本 | `initialize` 握手、独立 sampling/roots/list |
| 2026-07-28 | 当前版本 | `server/discover`、每请求 `_meta`、MRTR（`InputRequiredResult`）、`subscriptions/listen` |

`WireCodec` 工厂通过简单的字符串比较（`version >= "2026-07-28"`）自动选择正确的编解码器。

## 一致性测试

**113 个一致性测试**，覆盖两个时代的协议类型序列化：

- JSON-RPC 消息往返（请求、通知、响应、错误）
- WireCodec 时代门控（2025 与 2026 方法/通知集）
- 工具、资源、提示词序列化（含注解和图标）
- 内容变体分发（文本、图片、音频、嵌入资源）
- Elicitation 和 ElicitResultTyped\<T\>
- MRTR（InputRequiredResult、InputRequests、工厂辅助函数）
- 结构化元数据（RequestMetaObject、NotificationMetaObject）
- 扩展能力、ResultType 枚举、SubscriptionFilter
- 任务（获取/更新/取消）、日志（8 级别）
- 分页、缓存、协议版本辅助函数

## 示例

可运行示例在 [`examples/`](examples/) 目录中：

| 示例                                | 说明                              |
|------------------------------------|-----------------------------------|
| [EchoServer](examples/EchoServer/)   | 最小服务器，含工具、资源和提示词    |
| [WeatherServer](examples/WeatherServer/) | 集成外部 API 的服务器        |
| [SimpleClient](examples/SimpleClient/) | 连接服务器的进程内客户端      |

构建和运行：

```bash
cmake --preset debug -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug
# 运行 echo 服务器：
build/debug/examples/EchoServer/EchoServer
```

## 参考实现

本 SDK 参考了官方 MCP 协议规范和参考实现：

| 语言       | 仓库                                                            | 分级    |
|-----------|----------------------------------------------------------------|--------|
| Python    | [modelcontextprotocol/python-sdk](https://github.com/modelcontextprotocol/python-sdk) | Tier 1 |
| TypeScript| [modelcontextprotocol/typescript-sdk](https://github.com/modelcontextprotocol/typescript-sdk) | Tier 1 |
| Go        | [modelcontextprotocol/go-sdk](https://github.com/modelcontextprotocol/go-sdk) | Tier 1 |
| C#        | [modelcontextprotocol/csharp-sdk](https://github.com/modelcontextprotocol/csharp-sdk) | Tier 1 |
| Rust      | [modelcontextprotocol/rust-sdk](https://github.com/modelcontextprotocol/rust-sdk) | Tier 1 |
| Java      | [modelcontextprotocol/java-sdk](https://github.com/modelcontextprotocol/java-sdk) | Tier 2 |
| Ruby      | [modelcontextprotocol/ruby-sdk](https://github.com/modelcontextprotocol/ruby-sdk) | Tier 1 |

> 分级依据[官方 SDK 列表（2026-07-28 修订版）](https://modelcontextprotocol.io/docs/2026-07-28/sdk)：Java 仍为 Tier 2，Ruby 为新晋 Tier 1 成员，故保留原六 SDK 口径并增补 Ruby 行。分级规则见 [SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers)——Tasks 等协议扩展任何分级均不强制要求。

### Ruby SDK 对照注记

官方 Ruby SDK（`mcp` gem，2026-10-11 時 `main` 分支现状；最新发布 `1.7.0`，日期 2026-10-05，见 [CHANGELOG.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/CHANGELOG.md) 与 [version.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/version.rb)）相对本 SDK 的分维度实现要点：

| 维度 | Ruby（`mcp` gem） | 本 SDK |
|------|------------------|--------|
| 规范版本 | 2025-06-18、2025-11-25、2026-07-28（[ROADMAP.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/ROADMAP.md)，Current Status） | 2024-11-05…2026-07-28，双时代编解码（见上文协议版本） |
| `server/discover` | 现代时代永不经 `initialize` 协商（CHANGELOG §1.2.0）；`server/discover` 定稿并带现代客户端支持；stdio 按传输 era lock，Streamable HTTP 走无会话现代路径（[methods.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/methods.rb)） | Auto 模式按传输分类回退（[version-negotiation](wiki/concepts/version-negotiation.md)） |
| 多轮请求 | SEP-2322 `input_required` 结果（`inputRequests`/`inputResponses` + `requestState`），legacy 线上亦可履行（CHANGELOG §0.24.0/§1.2.0）；`resultType` 仅 `complete`/`input_required`（[result_type.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/result_type.rb)） | `InputRequiredResult` + elicitation 循环（[mrtr](wiki/concepts/mrtr.md)）；另有任务句柄专用的 `task` 结果类型 |
| 请求头 | 发送 SEP-2243 `Mcp-Method`/`Mcp-Name`，`x-mcp-header` 参数镜像为 `Mcp-Param-*`（CHANGELOG §0.22.0/§1.2.0；[mcp_param_headers.rb](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/lib/mcp/client/mcp_param_headers.rb)）；现代路径强制要求 `Mcp-Method` | `Mcp-Name` 校验含 tasks `taskId` 比对（[mcp-param-headers](wiki/concepts/mcp-param-headers.md)、[tasks-extension](wiki/concepts/tasks-extension.md)） |
| 缓存 | 服务端标注 SEP-2549 `ttlMs`/`cacheScope`（CHANGELOG §0.24.0/§1.2.0）；无客户端响应缓存——`lib/mcp/client/` 下仅 elicitation、http、mcp_param_headers、modern_envelope、oauth（+oauth/ 子目录）、paginated_result、stdio、tool，且 `client/http.rb` 全文无 `ttlMs`/`cacheScope`/缓存命中逻辑（2026-10-13 对 `main` 分支核实） | 客户端响应缓存，同时消费 2026 时代顶层 `ttlMs`/`cacheScope` 与 2025 时代嵌套 `cacheHint`，TTL + public/private 双分区（[response-cache](wiki/concepts/response-cache.md)） |
| 认证 | 完整 OAuth 2.1 客户端（DCR、PKCE、CIMD、`client_credentials`、`private_key_jwt`、`jwt-bearer`、RFC 9207 `iss`、PRM 优先发现；CHANGELOG §0.17.0–§1.6.0）+ 服务端资源服务器保护（[README Features](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/README.md)） | PKCE/CIMD（DCR 回退）+ `FileTokenCache`，服务端 Bearer 鉴权，可选的服务端 DPoP 证明校验（[dpop](wiki/concepts/dpop.md)；见上文 OAuth 支持） |
| 任务（SEP-2663） | 未实现——“remains planned”（[ROADMAP.md](https://github.com/modelcontextprotocol/ruby-sdk/blob/main/ROADMAP.md)，[#391](https://github.com/modelcontextprotocol/ruby-sdk/issues/391)） | `tasks/get\|update\|cancel` 双时代 + `FileTaskStore`（[tasks-extension](wiki/concepts/tasks-extension.md)） |
| 技能（SEP-2640） | 未实现【假设：`methods.rb` 方法全集、`lib/mcp/`、`lib/mcp/client/`、`test/mcp/` 均无 skills 条目，CHANGELOG 全版本无 skills 记载】 | `skills/list\|get` + `resources/directory/read`，仅 2026 时代（[skills-extension](wiki/concepts/skills-extension.md)） |

## 不支持声明

以下条目在本轮明确声明为不支持；当对端发送它们时，本 SDK 优雅降级（忽略未知字段或回退到基线路径），而不会中断会话。

- **应用界面渲染**——服务器推送的 UI 组件不渲染，仅作不透明数据透传。
- **EMA**——未实现 EMA 协商，对端回退到基线能力协商。
- **DPoP 客户端逐请求证明**——服务端证明校验已实现（默认关，实验开关 `enable_dpop`，需 OpenSSL，见上文 OAuth 支持），但传输层从不逐请求附加 `DPoP` 证明，服务端下发的 `DPoP-Nonce` 也未回灌做 nonce 重试，故 DPoP 绑定令牌尚不能驱动 Streamable HTTP 会话；开关关闭时纯 Bearer 路径行为不变（[dpop](wiki/concepts/dpop.md)）。
- **jwt-bearer**——已支持 `private_key_jwt` 客户端认证（配置 `private_key_pem` 且授权服务器在 `token_endpoint_auth_methods_supported` 中声明 `private_key_jwt` 时，向令牌请求发送 ES256 客户端断言；需 OpenSSL），但不支持授权方 `jwt-bearer` 授权，公开客户端 PKCE/CIMD 流程仍为受支持路径。
- **ETag**——不存储、不比对 `ETag` / `If-None-Match` 校验器，缓存响应仅按 TTL 治理。
- **渐进式发现**——未实现增量能力发现，恒用完整 `server/discover`（现代）/ `initialize`（遗留）握手。
- **文件上传**——未实现可续传文件上传原语，文件内容仅经现有工具/资源载荷流转。
- **服务器卡片**——不呈现服务器卡片，握手中的 `serverInfo` 仍为身份来源。

### 本轮破坏性变更

按 2026-07-28 规范修订，每条变更一句话概述并附迁移法。

- **URL elicitation 现代语义**——`url` elicitation 仅遵循现代时代语义。迁移：现代时代下 `ElicitUrl` 不再等待 `elicitationId` + `notifications/elicitation/complete`，而转为 MRTR 输入往返（`inputRequests["elicitation"]`，配置 `request_state_key` 时带 `requestState` 签名），经 `tasks/update` `inputResponses` 履行，客户端用 `SetUrlElicitationHandler` 承接（默认拒绝）；遗留 `elicitationId` 等待路径不变。
- **`_meta` 省略**——空的每请求 `_meta` 改为线上了省略，不再发送空对象。迁移：对端须把缺失的 `params._meta` 视为“无元数据”，不得假设空对象必到；发送侧无需补空对象；遗留 `SendRequest` 在 `params` 为空/null 时直接丢弃该字段。
- **重定向收紧**——HTTP 重定向处理比遗留行为更严格。迁移：默认策略 `HttpRedirectPolicy::SameOriginOnly`（同 scheme/host/port，允许 `http`→`https` 升级；跨源跳转被拒绝且剥离 `Authorization` 头，`301`/`302` POST 转 GET，`303` 一律转 GET）；经 `HttpRequestSpec::redirect_policy`（`FollowAll`/`NoFollow`）与 `max_redirects`（默认 5）逐调用覆盖，或经 `SseClientTransportOptions` / `HttpClientTransportOptions` 逐传输覆盖。
- **默认上限**——对端省略对应限制时适用内置默认上限。迁移：HTTP 响应 8 MB（`HttpRequestSpec::max_response_bytes`）、SSE 事件 1 MB（SSE/Streamable HTTP 客户端选项的 `max_sse_event_bytes`）、CIMD 文档 1 MB（硬上限，best-effort 回退）、stdio 帧 8 MB（`StdioClientTransportOptions::max_frame_bytes` / `StdioServerTransport` 构造参数）、服务端请求体 4 MB（`max_request_body_bytes`）、工具参数 `ServerOptions::max_tool_input_elements`（超限返回 `is_error` 工具结果而非协议错误）、技能 manifest 512 文件 / 16 MB 总量（客户端 `CheckSkillLimits`）；对端确需超限时调大对应旋钮，或对载荷分片/分页。
- **异步取消**——`notifications/cancelled` 投递异步，与被取消调用无序。迁移：待处理请求回调现跑在分离线程（仅线程创建失败时回退内联），调用方须按最终一致处理取消——handler 保持幂等并轮询 `RequestContext::IsCancellationRequested()` 协作退出，不得假设取消先于响应到达。

> 来源：[官方 SDK 列表（2026-07-28 修订版）](https://modelcontextprotocol.io/docs/2026-07-28/sdk)；[SDK Tiering System](https://modelcontextprotocol.io/community/sdk-tiers)——实验性功能与协议扩展（如 Tasks、MCP Apps）任何分级均不强制要求。

## 许可证

MIT — 参见 [LICENSE](LICENSE)。
