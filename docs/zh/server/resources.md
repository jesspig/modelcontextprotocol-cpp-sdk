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

::: note
`ResourceOptions` 中的字段（`description`、`title`、`mime_type`、`icons`）会传播到通过 `resources/list` 和 `resources/templates/list` 返回的协议级 `Resource` 和 `ResourceTemplate` 结构中。
:::

## 资源变更通知

资源内容或资源列表发生变化时，由服务端主动通知客户端：

```cpp
// 某个资源的内容变化——只投递给订阅了该 uri 的订阅者
server->SendResourceUpdated("file:///app/config.json");

// 资源列表本身变化（新增/删除资源）
server->SendResourceListChanged();
```

`SendResourceUpdated(uri)` 发布 `notifications/resources/updated`，`SendResourceListChanged()` 发布 `notifications/resources/list_changed`。分发方式随协商的协议时代自适应：

- **2026-07-28 及以后**：订阅经 `subscriptions/listen` 携带显式过滤器，通知只投递给过滤器匹配的订阅者。
- **2025 及更早**：`resources/subscribe` 没有过滤器可参照，通知按广播语义投递。
