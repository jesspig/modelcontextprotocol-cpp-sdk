# 参考层总览

参考层只回答三件事：**签名是什么、字段默认值是多少、出错会怎样**。所有「怎么做」的步骤在任务层，所有「为什么这样设计」的说明在概念层。本页给出阅读约定与页面到源码的映射。

## 阅读约定

| 约定 | 含义 |
|---|---|
| 源码位置 | 各页页尾「本页对应源码」列出相关文件；不标行号——行号随改码漂移，请以符号名在文件内检索 |
| 时代标记 | 某些能力只在某一协议时代可用，结论一律回链 [协议版本](/zh/learn/protocol-versions)，各页不重复维护矩阵 |
| 废弃标记 | 标注「已废弃」的接口仍存在、仍可调用的，会写明「可调用但已废弃」与替代方案；只有真正删除的才写「已移除」 |
| 内部符号 | `detail/` 下的类型与函数不属于用户 API，参考页只在解释行为时提及，不列入接口表 |

## 类型与所有权惯例

| 惯例 | 事实 |
|---|---|
| 工厂返回值 | `McpServer::Create` / `McpClient::Create` 返回 `std::unique_ptr` |
| 传输所有权 | `IClientTransport::Connect()` 返回 `std::shared_ptr`；传给工厂的传输是 `std::shared_ptr<ITransport>` |
| JSON 值类型 | `JsonValue` 是本仓库自研类，**不是 nlohmann/json**；取值用 `Find`/`IsString`/`IsNumber`/`GetInt`/`Dump` 等，字段表见 [类型总表](/zh/reference/types) |
| 可选字段 | 统一用 `std::optional`，不使用哨兵值 |
| 按值传参 | `JsonValue` 参数按值传递 |
| 错误抛出 | 一律抛 `McpError(McpErrorCode, message)`，继承 `std::runtime_error`；用户 handler 异常按各层收敛规则处理，见 [错误码](/zh/reference/errors) |

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
- [任务层：注册一个工具](/zh/howto/register-a-tool) — 从字段表走向可运行代码
- [任务层：承接服务端发起的请求](/zh/howto/handle-server-initiated-requests) — 被动侧回调与续轮
