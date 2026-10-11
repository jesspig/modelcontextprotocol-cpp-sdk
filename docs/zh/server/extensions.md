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

## 扩展约定

| 字段 | 说明 |
|-------|-------------|
| 键 | 反向 DNS 标识符（例如 `"io.modelcontextprotocol/tasks"`） |
| 值 | 包含扩展配置的自由格式 JSON 对象 |
