# 启发式收集

启发式收集允许服务端在工具执行期间请求用户额外输入。它取代了已弃用的采样机制（SEP-2577）。

## 表单模式

通过 JSON Schema 表单请求结构化用户输入：

```cpp
ElicitRequestParams params;
params.message = "请提供您的收货地址";
params.requested_schema = R"({
    "type": "object",
    "properties": {
        "street": {"type": "string"},
        "city": {"type": "string"},
        "zip": {"type": "string"}
    },
    "required": ["street", "city", "zip"]
})"_json;

auto result = server->Elicit(params);
if (result.get().values) {
    auto street = (*result.get().values)["street"];
}
```

## 泛型类型化表单

使用 `Elicit<T>` 模板实现类型安全的启发式收集：

```cpp
struct AddressForm {
    std::string street;
    std::string city;
    std::string zip_code;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AddressForm, street, city, zip_code)

auto future = server->Elicit<AddressForm>("请提供您的收货地址");
auto result = future.get();
if (result.is_accepted() && result.content) {
    auto& addr = *result.content;
    std::cout << addr.street << ", " << addr.city << "\n";
}
```

## 启发式收集结果

结果有三种取值：

| 动作 | 含义 |
|--------|---------|
| `accept` | 用户提交了表单 |
| `decline` | 用户明确拒绝 |
| `cancel` | 用户未操作直接关闭（默认） |
