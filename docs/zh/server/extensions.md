# 扩展

协议扩展通过 `ClientCapabilities` 和 `ServerCapabilities` 上的 `extensions` 映射进行协商。每个条目将扩展标识符映射到其配置值。

## 服务端自动推导

在 `ServerOptions` 上设置 `task_store` 会自动声明任务扩展：

```cpp
auto transport = std::make_shared<StdioServerTransport>();

ServerOptions opts;
opts.task_store = std::make_shared<FileTaskStore>("tasks.json");

auto server = McpServer::Create(transport, opts);
```

`ServerOptions` 没有 `capabilities` 字段。能力始终自动推导：

| 条件 | 声明的扩展键 | 值 |
|------|-------------|-----|
| 设置了 `task_store` | `"io.modelcontextprotocol/tasks"` | 空对象 `{}` |
| 注册了至少一个技能（`RegisterSkill`）**且**具备 resources 能力 | `"io.modelcontextprotocol/skills"` | 空对象；`SetSkillsDirectoryRead(true)` 后含 `"directoryRead": true` |

客户端侧声明见 [客户端概述](/zh/client/overview)（`ClientOptions::capabilities.extensions` 或 `ClientOptions::extensions` 含任务扩展键）；2026 时代下未声明任务扩展却收到任务结果时客户端抛 `HandlerError`。

## 任务通知路由

2026 时代下任务状态通知（任务化 `tools/call` 的初始 Working 与终态、`tasks/update` 的 `tasks/completed`/`tasks/working`、`tasks/cancel` 的 `tasks/cancelled`）不再抑制，改经服务端订阅表按订阅路由：投递给全部现存订阅者（任务类通知不过滤器），每帧 `_meta` 携带 `io.modelcontextprotocol/subscriptionId`（`subscriptions/listen` 传入 ID 则回显，否则用服务端自增 ID）；尚无订阅者时直接丢弃，客户端靠 `PollTaskToCompletion` 轮询取终态（任务 `poll_interval_ms > 0` 时采用服务端指示间隔，上限 300s）。2025 时代保持广播（`notifications/tasks/*`）。

## 技能 URI 形态与验签助手

技能条目 `uri` 须为 `<scheme>://<名>/SKILL.md` 形态（scheme 无关：只要求非空 `://`、名段非空、`/SKILL.md` 结尾；`skill://` 仍为约定 scheme）。服务端只做形态放行，是否取用他源技能由客户端经 `mcp/client/SkillVerify.hpp` 五助手判定：`CheckSkillLimits`（manifest ≤ 512 文件、总量 ≤ 16 MB）、`VerifyFileBytes`（`size` 等长 + `sha256:` 摘要实算比对）、`VerifyFrontmatter`（两份一致且 `name` 等于 URI 名段）、`IsApprovalStillValid`（授权快照复核）、`ApprovalKey`（`"<host>\x1f<skill-uri>"` 授权键）。

## 扩展约定

| 字段 | 说明 |
|-------|-------------|
| 键 | 反向 DNS 标识符（例如 `"io.modelcontextprotocol/tasks"`） |
| 值 | 包含扩展配置的自由格式 JSON 对象 |
