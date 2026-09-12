# 启发式收集

启发式收集允许服务端在工具执行期间请求用户额外输入。它取代了已弃用的采样机制（SEP-2577）。

## 表单模式

通过 JSON Schema 表单请求结构化用户输入：

```cpp
ElicitRequestParams params;
params.message = "请提供您的收货地址";
params.requested_schema = JsonValue::Parse(R"({
    "type": "object",
    "properties": {
        "street": {"type": "string"},
        "city": {"type": "string"},
        "zip": {"type": "string"}
    },
    "required": ["street", "city", "zip"]
})");

auto future = server->Elicit(params);
auto result = future.get();
if (result.content) {
    auto street = (*result.content)["street"];
}
```

## 启发式收集结果

`ElicitResult` 继承自 `Result`，包含：

| 字段 | 类型 | 说明 |
|-------|------|------|
| `action` | `string` | 用户决定：`"accept"`、`"decline"` 或 `"cancel"` |
| `content` | `optional<JsonValue>` | 提交的表单数据（接受时存在） |

继承的 `result_type`（`Complete` 或 `InputRequired`）指示输入是否已完成或仍在等待。

## URL 模式

`ElicitUrl` 让客户端把用户引导到站外 URL 完成授权等操作（`mode: "url"`，自动生成 `elicitation_id`）：

```cpp
auto future = server->ElicitUrl(
    "https://auth.example.com/oauth/authorize",
    "请在浏览器中完成登录",
    std::chrono::seconds(600));  // 默认 600s，超时抛 McpError(RequestTimeout)
auto result = future.get();
```

客户端通过 `SetUrlElicitationHandler` 接收 URL 收集请求（打开浏览器或展示链接）：

```cpp
client->SetUrlElicitationHandler(
    [](const ElicitRequestParams& params) {
        // params.mode == "url"，含 params.url 与 params.elicitation_id
    });
```

## 类型化辅助结构

`ElicitResultTyped<T>` 是一个用户侧的便利结构体，用于封装反序列化结果：

```cpp
struct AddressForm {
    std::string street;
    std::string city;
    std::string zip_code;
};

ElicitResult raw = future.get();
ElicitResultTyped<AddressForm> typed;
if (raw.content) {
    typed.action = "accept";
    typed.content = AddressForm{
        (*raw.content)["street"].GetString(),
        (*raw.content)["city"].GetString(),
        (*raw.content)["zip_code"].GetString()
    };
}

if (typed.is_accepted() && typed.content) {
    auto& addr = *typed.content;
    // ...
}
```

`ElicitResultTyped<T>` 成员：

| 成员 | 类型 | 说明 |
|--------|------|------|
| `action` | `string` | `"accept"`、`"decline"` 或 `"cancel"`（默认） |
| `content` | `optional<T>` | 反序列化的值（接受时存在） |
| `is_accepted()` | `bool` | 当 `action == "accept"` 时返回 `true` |

注意：`ElicitResultTyped<T>` 不由任何 API 返回——请从原始的 `ElicitResult` 手动构造。
