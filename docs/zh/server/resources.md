# 资源

资源暴露客户端可以读取的结构化数据。它们类似于 REST API 中的 GET 端点。

## 注册资源

```cpp
server->RegisterResource(
    "config",
    "file:///app/config.json",
    ResourceOptions{}.Description("应用配置").MimeType("application/json"),
    [](const std::string& uri) -> ReadResourceResult {
        ReadResourceResult result;
        TextResourceContents contents;
        contents.uri = uri;
        contents.text = R"({"debug": true, "port": 8080})";
        contents.mime_type = "application/json";
        result.contents.push_back(contents);
        return result;
    });
```

## 资源模板

资源模板使用包含 `{variables}` 的 URI 模式：

```cpp
server->RegisterResourceTemplate(
    "user-data",
    "file:///users/{userId}/profile",
    ResourceOptions{}.Description("用户资料数据"),
    [](const std::string& uri,
       const std::map<std::string, std::string>& vars) -> ReadResourceResult {
        auto userId = vars.at("userId");
        // 获取用户数据...
    });
```

## 资源注解

资源注解控制可见性和优先级：

```cpp
ResourceOptions opts;
opts.description = "敏感配置";
opts.mime_type = "application/json";
// 注解通过 MCP 协议间接设置
```
