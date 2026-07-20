# MRTR（多轮往返请求）

MRTR（SEP-2322）允许服务器处理程序在工具执行期间向客户端请求额外输入，而无需离开 JSON-RPC 上下文。这取代了旧有的服务器到客户端独立请求模式。

## 工作原理

在无状态（2026 时代）模式中，服务端返回一个 `InputRequiredResult`，其 `resultType: "input_required"`。客户端通过其 `ElicitationHandler` 在内部解析响应，然后使用 `input_responses` 和 `requestState` 重试原始工具调用：

```
客户端                    服务器
  │                         │
  ├── tools/call ──────────►│
  │                         ├── 处理程序需要用户输入
  │◄── 结果：               │
  │    {                    │
  │      resultType:        │
  │        "input_required",│
  │      inputRequests: {   │
  │        elicit: {...}    │
  │      },                 │
  │      requestState: "..."│
  │    }                    │
  │                         │
  │  (客户端通过             │
  │   ElicitationHandler    │
  │   解析)                 │
  │                         │
  ├── tools/call ──────────►│  (使用 input_responses
  │    (携带 input_responses│   + requestState 重试)
  │     + requestState)     │
  │◄── 结果：{...}           │
```

`InputRequests` 结构体可以包含以下任一或两者：

| 字段 | 类型 | 用途 |
|-------|------|---------|
| `confirm` | `InputRequestElicit` | 简单的是/否确认 |
| `elicit` | `InputRequestElicit` | 自由格式的输入请求 |

每个 `InputRequestElicit` 包含一个 `message` 字符串和可选的 `requestedSchema`（JSON Schema）。

## 服务端

工具处理程序通过 `Elicit` 请求用户输入：

```cpp
// 直接使用 Elicit（服务端发送 elicit，等待响应）
auto elicit_result = server->Elicit(
    ElicitRequestParams{"确认订单?", /* requested_schema */}).get();

CallToolResult result;
result.content.push_back(TextContent{"text",
    elicit_result.values ? "已确认" : "已取消"});
return result;
```

带 JSON Schema 推断的类型化 elicit：

```cpp
struct OrderConfirmation {
    bool confirmed;
    std::string notes;
};

auto typed_result = server->Elicit<OrderConfirmation>(
    "请确认订单详情").get();
if (typed_result.is_accepted()) {
    // typed_result.content->confirmed, typed_result.content->notes
}
```

`Elicit` 方法返回 `std::future<ElicitResult>`（或类型化重载的 `std::future<ElicitResultTyped<T>>`）。

## 客户端

客户端通过 `SetElicitationHandler` 处理 MRTR：

```cpp
client->SetElicitationHandler(
    [](const ElicitRequestParams& params) -> ElicitResult {
        ElicitResult result;
        result.values = nlohmann::json{{"confirmed", true}};
        return result;
    });
```

## InputRequired 结果

服务器也可以直接返回 `InputRequiredResult`（无状态模式）：

```cpp
InputRequiredResult ir;
ir.input_requests.elicit = InputRequestElicit{"提供值"};
ir.request_state = "state-token";
// 服务器将其作为 tools/call 的结果返回
// 客户端解析并使用 input_responses + requestState 重试
```

## 辅助函数

```cpp
// 构建用于 elicitation 的输入请求负载
nlohmann::json req = MakeInputRequestForElicitation(params);
// req == {"method": "elicitation/create", "params": {...}}

// 将 elicit 结果转换为输入响应
nlohmann::json resp = MakeInputResponseFromElicitResult(result);

// 检查 JSON 结果是否表示 input_required
if (IsInputRequiredResult(raw_result)) {
    auto input_requests = ExtractInputRequests(raw_result);
}
```

## 配置 MRTR

### 服务端

```cpp
ServerOptions opts;
opts.input_required_config = ServerOptions::InputRequiredConfig{
    .max_rounds = 8,
    .round_timeout = std::chrono::seconds(600),
    .legacy_shim = true
};
```

### 客户端

```cpp
ClientOptions opts;
opts.input_required_config = ClientOptions::InputRequiredConfig{
    .auto_fulfill = true,
    .max_rounds = 8,
    .round_timeout = std::chrono::seconds(600)
};
```

| 选项 | 服务端 | 客户端 | 描述 |
|--------|--------|--------|-------------|
| `max_rounds` | 是 | 是 | 最大 MRTR 轮数（默认：8） |
| `round_timeout` | 是 | 是 | 每轮超时（默认：600 秒） |
| `legacy_shim` | 是 | 否 | 启用旧版 2025 时代兼容层 |
| `auto_fulfill` | 否 | 是 | 如可能则自动填充，无需提示 |
