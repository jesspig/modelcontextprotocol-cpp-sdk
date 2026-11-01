# 请求取消示例逐段走读

`examples/RequestCancellation` 是最小可运行的「超时 + 显式取消」演示：服务端故意**扣住 promise 不回话**，客户端分别用超时和 `Cancel()` 拿到两种不同终局码。源码：[`examples/RequestCancellation/main.cpp`](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/examples/RequestCancellation/main.cpp)。用法与语义在 [/zh/howto/cancel-a-request](/zh/howto/cancel-a-request)，本页只讲代码为什么这样写。

## 这个示例演示什么

- 一个**永不结算**的服务端请求处理器，是复现「卡住」的最省事办法。
- `McpSessionHandler` 直接装配（不经过 `McpServer`）时需要的三件套：传输、codec、请求处理器。
- 自定义方法要过 `WireCodec` 的时代白名单，因此示例包了一层 codec。
- 超时终局 `-32001` 与取消终局 `-32800` 的同屏对比。

## 为什么需要自定义 codec

`WireCodec` 会按协商时代裁剪已知方法与合法帧，未登记的 `demo/slow` 会被当成非法请求。示例的做法是**委托 + 局部放行**：

```cpp
bool HasRequestMethod(std::string_view method) const override {
    return IsCustom(method) || inner_->HasRequestMethod(method);
}

WireValidation ValidateRequest(
    std::string_view method, const JsonValue& raw) const override {
    return IsCustom(method)
        ? WireValidation::Ok
        : inner_->ValidateRequest(method, raw);
}
```

对应 `examples/RequestCancellation/main.cpp:27-40`（通知侧同形，`:31-34`、`:47-52`）。其余虚函数一律转给 `inner_`，包括 `EncodeErrorCode`（`:64-66`）与 `Era()`（`:68-70`）——**时代相关的错误码重映射必须原样保留**，否则这个演示就改变了被测行为。内层由 `MakeWireCodec(version)` 构造（`:22-25`）。

## 服务端扣住 promise

```cpp
std::vector<std::shared_ptr<std::promise<JsonValue>>> held_requests;
server->SetRequestHandler(kSlowMethod,
    [&held_requests](const JsonRpcRequest&, std::promise<JsonValue> promise) {
        held_requests.push_back(
            std::make_shared<std::promise<JsonValue>>(std::move(promise)));
    });
```

对应 `examples/RequestCancellation/main.cpp:123-128`。promise 被搬进 vector 保存，既不 `set_value` 也不 `set_exception`，于是**服务端永不回话**——SDK 不会替你补错误响应（判定与后果见 [/zh/howto/debug-a-stuck-server](/zh/howto/debug-a-stuck-server)）。

装配本身用 `std::make_shared<McpSessionHandler>(std::move(pair.server), std::make_unique<DemoCodec>(kLegacyProtocolVersion))`（`:117-119`），并注册 `initialize` 处理器返回 `kLegacyProtocolVersion`（`:80-89`），最后 `server->Start()`（`:129`）。选 legacy 时代是有意的：这样取消会以 `notifications/cancelled` 发出，能在演示里看到线上行为；modern 下由实现 `IRequestCanceller` 的传输关请求流。

## 客户端两条路径

```cpp
ClientOptions client_options;
client_options.connect_mode = ConnectMode::Legacy;
auto client = McpClient::Create(std::move(pair.client), client_options);

RequestOptions timeout_options;
timeout_options.timeout = std::chrono::milliseconds(200);
auto timed = client->SendRequest(
    kSlowMethod, JsonValue(JsonValue::object_tag), timeout_options);
```

对应 `examples/RequestCancellation/main.cpp:131-138`。`SendRequest` 的三参形态（方法、参数、`RequestOptions`）返回 `RequestHandle`，句柄里同时有请求 `id` 和 `future<RequestOutcome>`。

取消路径则显式调用句柄：

```cpp
auto cancelled = client->SendRequest(
    kSlowMethod, JsonValue(JsonValue::object_tag), RequestOptions{});
std::cout << "cancel handle id: " << RequestIdText(cancelled.id) << std::endl;
cancelled.Cancel("user requested cancellation");
PrintOutcome(cancelled.future.get());
```

对应 `:142-146`。默认 `RequestOptions{}` 的超时是 60 秒（`kDefaultRequestTimeout{60000}`，`include/mcp/McpTypes.hpp:26`），所以这条不会在演示时长内自己到期，只能被 `Cancel` 结算——这正是两条路径能同屏对比的原因。

## 打印终局的判型写法

```cpp
void PrintOutcome(const RequestOutcome& outcome) {
    if (const auto* value = std::get_if<JsonValue>(&outcome)) {
        std::cout << "result: " << value->Dump() << std::endl;
        return;
    }
    const auto& error = std::get<ErrorData>(outcome);
    std::cout << "error: " << error.message
              << " (code " << static_cast<int32_t>(error.code) << ")"
              << std::endl;
}
```

对应 `examples/RequestCancellation/main.cpp:102-111`。`RequestOutcome` 是 `std::variant<JsonValue, ErrorData>`，**先判成功再取错误**；`error.code` 是枚举，打印要显式 `static_cast<int32_t>`。请求 ID 是 `std::variant<int64_t, std::string>`，示例用 `std::visit` + `if constexpr` 转文本（`:91-100`），这也是唯一安全的写法。

## 运行与实测输出

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_EXAMPLES=ON
cmake --build build/debug --target RequestCancellation
build/debug/examples/RequestCancellation/RequestCancellation
```

实测输出（Windows，clang-cl，返回码 0）：

```text
timeout handle id: 2
error: request timed out (code -32001)
cancel handle id: 3
error: request cancelled (code -32800)
```

两行终局码不同，且都来自**本地结算**：超时那条的 `cancel_on_timeout` 默认 `true`，因此顺带发了取消通知，但码仍是 `-32001`。结尾示例依次 `client->Close()` 与 `server->Close()`（`:148-149`）。

## 与 how-to 的对应关系

| 想做的事 | 去看 |
|---|---|
| 取消、超时与三条失败通道的完整语义 | [/zh/howto/cancel-a-request](/zh/howto/cancel-a-request) |
| 错误码分型与重映射 | [/zh/howto/handle-errors](/zh/howto/handle-errors) |
| 请求永无响应时的排障顺序 | [/zh/howto/debug-a-stuck-server](/zh/howto/debug-a-stuck-server) |
| 长活儿改成可轮询的任务 | [/zh/howto/run-long-tasks](/zh/howto/run-long-tasks) |
| 错误码与终局形态全表 | [/zh/reference/errors](/zh/reference/errors) |

## 本页对应源码

- `examples/RequestCancellation/main.cpp`
- `examples/RequestCancellation/CMakeLists.txt`
- `include/mcp/client/RequestHandle.hpp`、`include/mcp/protocol/IncomingRequestMeta.hpp`
- `include/mcp/protocol/McpSessionHandler.hpp`、`include/mcp/protocol/WireCodec.hpp`
- `include/mcp/transport/IRequestCanceller.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `include/mcp/ErrorCodes.hpp`
