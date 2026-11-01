# 参考层总览

参考层只回答三件事：**签名是什么、字段默认值是多少、出错会怎样**。所有「怎么做」的步骤在任务层，所有「为什么这样设计」的说明在概念层。本页给出阅读约定与页面到源码的映射。

## 阅读约定

| 约定 | 含义 |
|---|---|
| 证据列 | `文件名:行号`，指向仓库当前内容；行号会随改动漂移，以符号名为准定位 |
| 时代标记 | 某些能力只在某一协议时代可用，结论一律回链 [协议版本](/zh/learn/protocol-versions)，各页不重复维护矩阵 |
| 废弃标记 | 标注「已废弃」的接口仍存在、仍可调用的，会写明「可调用但已废弃」与替代方案；只有真正删除的才写「已移除」 |
| 内部符号 | `detail/` 下的类型与函数不属于用户 API，参考页只在解释行为时提及，不列入接口表 |

## 类型与所有权惯例

| 惯例 | 事实 | 证据 |
|---|---|---|
| 工厂返回值 | `McpServer::Create` / `McpClient::Create` 返回 `std::unique_ptr` | `include/mcp/server/McpServer.hpp:34`、`include/mcp/client/McpClient.hpp:34` |
| 传输所有权 | `IClientTransport::Connect()` 返回 `std::shared_ptr`；传给工厂的传输是 `std::shared_ptr<ITransport>` | `include/mcp/Transport.hpp:68-72` |
| JSON 值类型 | `JsonValue` 是本仓库自研类，**不是 nlohmann/json**；取值用 `Find`/`IsString`/`IsNumber`/`GetInt`/`Dump` 等 | `include/mcp/JsonValue.hpp:19-22`，字段表见 [类型总表](/zh/reference/types) |
| 可选字段 | 统一用 `std::optional`，不使用哨兵值 | 各 Options 页逐字段标注 |
| 按值传参 | `JsonValue` 参数按值传递 | `include/mcp/JsonRpc.hpp:15-88` |
| 错误抛出 | 一律抛 `McpError(McpErrorCode, message)`，继承 `std::runtime_error`；用户 handler 异常按各层收敛规则处理 | `include/mcp/McpError.hpp:13`，收敛规则见 [错误码](/zh/reference/errors) |

## 页面到源码的映射

| 参考页 | 覆盖范围 | 主要源码 |
|---|---|---|
| [服务端门面](/zh/reference/server) | `McpServer` 全方法、注册 API、`RequestContext`、`ServerOptions`、能力推导、工具调用错误语义 | `include/mcp/server/` |
| [客户端门面](/zh/reference/client) | `McpClient` 全方法、请求句柄与取消、MRTR、被动侧 handler、`ClientOptions`、协商档位 | `include/mcp/client/` |
| [传输层](/zh/reference/transports) | 传输契约与五种传输的类与 Options 全字段、HTTP 服务端底座 | `include/mcp/Transport.hpp`、`include/mcp/transport/`、`include/mcp/http/` |
| [类型总表](/zh/reference/types) | `JsonValue`、内容与各 `*Result`/`*RequestParams`、能力、任务与 MRTR 类型、日志与 span | `include/mcp/McpTypes.hpp` 等核心头 |
| [错误码](/zh/reference/errors) | `McpErrorCode` 全表、抛出点、时代重映射、`RequestOutcome` 分型 | `include/mcp/ErrorCodes.hpp`、`src/protocol/` |
| [进程与存储](/zh/reference/process-and-storage) | `mcp::Spawn` 与 `SpawnedProcess`、任务/事件/会话存储、`HttpServer` | `include/mcp/process/`、`include/mcp/storage/`、`include/mcp/http/` |
| [授权与扩展](/zh/reference/auth-and-extensions) | OAuth 与 PKCE、DPoP、token 缓存、server card/apps/triggers、技能校验、方法名与版本常量 | `include/mcp/ext/`、`include/mcp/client/auth/` |

## 相关页面

- [概念层：协议版本](/zh/learn/protocol-versions) — 双时代可用性的唯一权威表
- [概念层：架构](/zh/learn/architecture) — 库分层、线程模型与能力推导边界
- [概念层：扩展开关](/zh/learn/extensions) — 各 `enable_*` 开关的显式声明要求
