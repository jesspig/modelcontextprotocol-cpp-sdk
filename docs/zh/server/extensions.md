# 扩展

协议扩展通过 `ClientCapabilities` 和 `ServerCapabilities` 上的 `extensions` 映射进行协商。每个条目将扩展标识符映射到其配置值。

## 服务端

```cpp
ServerCapabilities caps;
caps.extensions["io.modelcontextprotocol/tasks"] = {
    {"supported", true},
    {"version", "1.0.0"}
};

auto server = McpServer::Create(transport,
    ServerOptions{}.Capabilities(caps));
```

## 客户端

```cpp
ClientCapabilities caps;
caps.extensions["io.modelcontextprotocol/tasks"] = {
    {"supported", true}
};

auto client = McpClient::Create(transport,
    ClientOptions{}.Capabilities(caps));
```

## 扩展约定

| 字段 | 说明 |
|-------|-------------|
| 键 | 反向 DNS 标识符（例如 `"io.modelcontextprotocol/tasks"`） |
| 值 | 包含扩展配置的自由格式 JSON 对象 |
