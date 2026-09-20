# MRTR（多轮往返请求）

MRTR（SEP-2322）允许服务器处理程序在工具执行期间向客户端请求额外输入，而无需离开 JSON-RPC 上下文。这取代了旧有的服务器到客户端独立请求模式。

## 工作原理

在无状态（2026 时代）模式中，服务端返回一个 `InputRequiredResult`，其 `resultType: "input_required"`。客户端通过其 `ElicitationHandler` 在内部解析响应，然后使用 `inputResponses` 和 `requestState` 重试原始工具调用：

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
  │        "name": {        │
  │          method: ...,   │
  │          params: {...}  │
  │        }                │
  │      },                 │
  │      requestState: "..."│
  │    }                    │
  │                         │
  │  (客户端通过             │
  │   ElicitationHandler    │
  │   解析)                 │
  │                         │
  ├── tools/call ──────────►│  (使用 inputResponses
  │    (携带 inputResponses│   + requestState 重试)
  │     + requestState)     │
  │◄── 结果：{...}           │
```

`InputRequests` 是「服务端分配的键 → 请求对象」的映射（`std::map<std::string, InputRequest, std::less<>>`）。每个 `InputRequest` 由 `method` 与 `params` 组成：

| 字段 | 类型 | 用途 |
|-------|------|---------|
| `method` | `std::string` | 请求方法：`elicitation/create`、`sampling/createMessage`、`roots/list` |
| `params` | `JsonValue` | 该方法的请求参数对象 |

键由服务端自行取语义名（如 `"name"`、`"confirm"`、`"paths"`）；客户端在 `inputResponses` 中以**相同的键**回填对应的裸结果。

## 服务端

工具处理程序通过 `Elicit` 请求用户输入：

```cpp
// 通过 RequestContext 使用 Elicit（在工具处理程序内部）
auto elicit_result = ctx.Server().Elicit(
    ElicitRequestParams{"确认订单?", /* requested_schema */}).get();

CallToolResult result;
result.content.push_back(TextContent{"text",
    elicit_result.content ? "已确认" : "已取消"});
return result;
```

`Elicit` 方法返回 `std::future<ElicitResult>`。

用户可以手动使用 `ElicitResultTyped<T>` 构造类型化结果：

```cpp
ElicitResultTyped<JsonValue> typed;
typed.action = "accept";
JsonValue obj(JsonValue::object_tag);
obj["confirmed"] = JsonValue(true);
typed.content = std::move(obj);
```

## 客户端

客户端通过 `SetElicitationHandler` 处理 MRTR：

```cpp
client->SetElicitationHandler(
    [](const ElicitRequestParams& params) -> ElicitResult {
        ElicitResult result;
        result.action = "accept";
        JsonValue obj(JsonValue::object_tag);
        obj["confirmed"] = JsonValue(true);
        result.content = std::move(obj);
        return result;
    });
```

客户端按每个输入请求的 `method` 分派：`elicitation/create` → `ElicitationHandler`，`sampling/createMessage` → `SamplingHandler`，`roots/list` → `RootsHandler`。对应 handler 未注册或方法未知时抛 `McpError`（`MethodNotFound`）。

`inputResponses` 的值是**裸结果**（elicitation 为 `{action, content}`，sampling 为 `{role, content, model, stopReason}`，roots 为 `{roots}`），不带 `resultType` / `meta` 信封。

## InputRequired 结果

推荐方式是在工具处理程序中直接设置 `CallToolResult::input_required`：

```cpp
CallToolResult result;
InputRequiredResult ir;
ElicitRequestParams params;
params.message = "提供值";
ir.input_requests["name"] = MakeInputRequestForElicitation(params);
result.input_required = std::move(ir);
return result;
```

配置了 `request_state_key` 时，服务端在发送前自动为 `input_required` 结果签名 `request_state`（处理程序无需也无法自行提供）；客户端携带 `inputResponses` + `requestState` 重试时，被篡改或过期的状态在进入处理程序前即被拒绝（-32602，`data.reason="invalid_request_state"`）。

签名与校验的底层助手在 `include/mcp/server/RequestState.hpp`（`MintRequestState` / `VerifyRequestState`）；需要自定义验证逻辑时可改为提供 `ServerOptions::request_state_verifier`。

## 辅助函数

```cpp
// 构建 elicitation 输入请求（返回 {method, params} 请求对象）
InputRequest request = MakeInputRequestForElicitation(params);
// request.method == "elicitation/create"
// 同系列：MakeInputRequestForSampling / MakeInputRequestForRoots

// 将 elicit 结果转换为裸输入响应（不含 resultType / meta）
JsonValue resp = MakeInputResponseFromElicitResult(result);
// 同系列：MakeInputResponseFromCreateMessageResult / MakeInputResponseFromListRootsResult

// 检查 JSON 结果是否表示 input_required
if (IsInputRequiredResult(raw_result)) {
    std::optional<InputRequests> input_requests = ExtractInputRequests(raw_result);
}
```

## 配置 MRTR

### 服务端

```cpp
ServerOptions opts;
opts.input_required_config = ServerOptions::InputRequiredConfig{
    .max_rounds = 10,
    .round_timeout = std::chrono::seconds(600),
    .legacy_shim = true
};
opts.request_state_key = "server-secret";              // 自动签名 requestState
opts.request_state_ttl = std::chrono::seconds(300);    // 状态有效期，0 = 不过期
```

### 客户端

```cpp
ClientOptions opts;
opts.input_required_config = ClientOptions::InputRequiredConfig{
    .auto_fulfill = true,
    .max_rounds = 10,
    .round_timeout = std::chrono::seconds(600),
    .max_total_timeout = std::chrono::seconds(0)
};
```

| 选项 | 服务端 | 客户端 | 描述 |
|--------|--------|--------|-------------|
| `max_rounds` | 是 | 是 | 最大 MRTR 轮数（默认：10） |
| `round_timeout` | 是 | 是 | 每轮超时（默认：600 秒） |
| `legacy_shim` | 是 | 否 | 占位字段，当前尚未生效 |
| `auto_fulfill` | 否 | 是 | 如可能则自动填充，无需提示 |
| `max_total_timeout` | 否 | 是 | 整个 MRTR 流程的硬性预算（默认 0 = 不设上限；`round_timeout` 作用于每轮） |
| `request_state_key` / `request_state_ttl` | 是 | 否 | 服务端请求状态签名密钥与有效期 |
