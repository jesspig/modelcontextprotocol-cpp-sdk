# 扩展

协议扩展通过 `ClientCapabilities` 和 `ServerCapabilities` 上的 `extensions` 映射进行协商。每个条目将扩展标识符映射到其配置值。

## 服务端自动推导

在 `ServerOptions` 上设置 `task_store` 会自动启用扩展能力（空映射）：

```cpp
ServerOptions opts;
opts.task_store = std::make_shared<FileTaskStore>("tasks.json");

auto server = McpServer::Create(transport, opts);
```

`ServerOptions` 上的 `capabilities` 字段已声明但当前不会被服务端消费。仅在设置了 `task_store` 时才会自动推导扩展。

## 扩展约定

| 字段 | 说明 |
|-------|-------------|
| 键 | 反向 DNS 标识符（例如 `"io.modelcontextprotocol/tasks"`） |
| 值 | 包含扩展配置的自由格式 JSON 对象 |
