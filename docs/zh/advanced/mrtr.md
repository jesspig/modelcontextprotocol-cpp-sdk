# MRTR（多轮往返请求）

MRTR（SEP-2322）允许服务器处理程序在工具执行期间向客户端请求额外输入，而无需离开 JSON-RPC 上下文。这取代了旧有的服务器到客户端独立请求模式。

## 工作原理

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
  ├── 解析 elicit ─────────►│  （客户端通过
  │                         │   ElicitationHandler 解析）
  │◄── 结果：{...}          │
```

## 服务端

工具处理程序可以通过调用 `Elicit` 或使用 MRTR 辅助函数来请求用户输入：

```cpp
// 直接使用 Elicit（服务端发送 elicit，等待响应）
server->RegisterTool("order_item",
    ToolOptions{}.Description("Place an order"),
    [&server](const RequestContext<CallToolRequestParams>& ctx) -> CallToolResult {
        // 需要用户确认
        ElicitRequestParams confirm;
        confirm.message = "Confirm order for $" + amount + "?";
        auto elicit_result = server->Elicit(confirm).get();

        CallToolResult result;
        result.content.push_back(TextContent{"text",
            elicit_result.values ? "Confirmed" : "Cancelled"});
        return result;
    });
```

## 客户端

`MrtrDriver` 自动处理 MRTR 循环：

```cpp
client->SetElicitationHandler(
    [](const ElicitRequestParams& params) -> ElicitResult {
        // 使用 params.message 提示用户
        // 根据用户输入返回结果
        ElicitResult result;
        result.values = nlohmann::json{{"confirmed", true}};
        return result;
    });
```

## InputRequired 结果

服务器也可以直接返回 `InputRequiredResult`（无状态模式）：

```cpp
InputRequiredResult ir;
ir.input_requests.elicit = InputRequestElicit{"Confirm purchase?"};
// 服务器将其作为 tools/call 的结果返回
// 客户端解析并使用 input_responses + requestState 重试
```

## 配置 MRTR

```cpp
ServerOptions opts;
opts.input_required_config = ServerOptions::InputRequiredConfig{
    .max_rounds = 8,
    .round_timeout = std::chrono::seconds(600),
    .legacy_shim = true
};
```
