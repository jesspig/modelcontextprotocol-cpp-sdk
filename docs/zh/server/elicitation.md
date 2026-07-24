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
if (result.values) {
    auto street = (*result.values)["street"];
}
```

## 类型化辅助结构

`ElicitResultTyped<T>` 是一个用户侧的类型安全封装，用于将启发式收集结果反序列化为结构化类型：

```cpp
struct AddressForm {
    std::string street;
    std::string city;
    std::string zip_code;
};

ElicitResult raw = future.get();
ElicitResultTyped<AddressForm> typed;
if (raw.values) {
    typed.action = "accept";
    typed.content = AddressForm{
        (*raw.values)["street"].GetString(),
        (*raw.values)["city"].GetString(),
        (*raw.values)["zip_code"].GetString()
    };
}

if (typed.is_accepted() && typed.content) {
    auto& addr = *typed.content;
    // ...
}
```

## 启发式收集结果

`ElicitResult` 继承自 `Result`，包含以下字段：

| 字段 | 类型 | 说明 |
|-------|------|-------------|
| `values` | `std::optional<JsonValue>` | 提交的表单数据（接受时存在） |
| `result_type` | `ResultType` | `Complete`（已接受）或 `InputRequired`（已拒绝/待处理） |

`ElicitResultTyped<T>` 提供用户侧的操作模型：

| 成员 | 类型 | 说明 |
|--------|------|-------------|
| `action` | `std::string` | `"accept"`、`"decline"` 或 `"cancel"`（默认） |
| `content` | `std::optional<T>` | 反序列化的值（接受时存在） |
