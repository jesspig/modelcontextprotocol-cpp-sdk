# Request cancellation example, section by section

`examples/RequestCancellation` is the smallest runnable demonstration of timeout plus explicit cancellation: the server deliberately **withholds every promise**, and the client obtains two different terminal codes — one from a timeout, one from `Cancel()`. Source: [`examples/RequestCancellation/main.cpp`](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/examples/RequestCancellation/main.cpp). Semantics and usage are in [/en/howto/cancel-a-request](/en/howto/cancel-a-request); this page explains why the code looks the way it does.

## What this example demonstrates

- A server request handler that **never settles** is the cheapest way to reproduce a stuck request.
- The three pieces needed to assemble `McpSessionHandler` directly (bypassing `McpServer`): transport, codec, request handlers.
- Custom methods must pass the era whitelist in `WireCodec`, hence the codec wrapper.
- Timeout terminal `-32001` next to cancellation terminal `-32800`.

## Why a custom codec is needed

`WireCodec` prunes known methods and valid frames by negotiated era, so an unregistered `demo/slow` would be rejected as illegal. The example delegates and opens a single hole:

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

From `examples/RequestCancellation/main.cpp:27-40` (the notification side mirrors it at `:31-34` and `:47-52`). Every other virtual forwards to `inner_`, including `EncodeErrorCode` (`:64-66`) and `Era()` (`:68-70`) — **era-dependent error-code remapping must stay intact**, otherwise the demo would alter the behaviour under test. The inner codec comes from `MakeWireCodec(version)` (`:22-25`).

## The server withholds the promise

```cpp
std::vector<std::shared_ptr<std::promise<JsonValue>>> held_requests;
server->SetRequestHandler(kSlowMethod,
    [&held_requests](const JsonRpcRequest&, std::promise<JsonValue> promise) {
        held_requests.push_back(
            std::make_shared<std::promise<JsonValue>>(std::move(promise)));
    });
```

From `examples/RequestCancellation/main.cpp:123-128`. The promise is parked in a vector, neither `set_value` nor `set_exception`, so **the server never answers** — the SDK will not synthesise an error response for you (diagnosis and consequences in [/en/howto/debug-a-stuck-server](/en/howto/debug-a-stuck-server)).

The assembly itself is `std::make_shared<McpSessionHandler>(std::move(pair.server), std::make_unique<DemoCodec>(kLegacyProtocolVersion))` (`:117-119`), with an `initialize` handler returning `kLegacyProtocolVersion` (`:80-89`) and `server->Start()` at the end (`:129`). Choosing the legacy era is deliberate: cancellation then travels as `notifications/cancelled`, which is visible in the demo, whereas the modern era closes the request stream on transports implementing `IRequestCanceller`.

## The client's two paths

```cpp
ClientOptions client_options;
client_options.connect_mode = ConnectMode::Legacy;
auto client = McpClient::Create(std::move(pair.client), client_options);

RequestOptions timeout_options;
timeout_options.timeout = std::chrono::milliseconds(200);
auto timed = client->SendRequest(
    kSlowMethod, JsonValue(JsonValue::object_tag), timeout_options);
```

From `examples/RequestCancellation/main.cpp:131-138`. The three-argument `SendRequest` (method, params, `RequestOptions`) returns a `RequestHandle` carrying both the request `id` and a `future<RequestOutcome>`.

The cancellation path calls the handle explicitly:

```cpp
auto cancelled = client->SendRequest(
    kSlowMethod, JsonValue(JsonValue::object_tag), RequestOptions{});
std::cout << "cancel handle id: " << RequestIdText(cancelled.id) << std::endl;
cancelled.Cancel("user requested cancellation");
PrintOutcome(cancelled.future.get());
```

From `:142-146`. A default `RequestOptions{}` times out after 60 seconds (`kDefaultRequestTimeout{60000}`, `include/mcp/McpTypes.hpp:26`), so this one cannot expire within the demo and can only be settled by `Cancel` — which is exactly why both paths are comparable on one screen.

## Printing the terminal outcome

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

From `examples/RequestCancellation/main.cpp:102-111`. `RequestOutcome` is `std::variant<JsonValue, ErrorData>`, so **test success first, then pull the error**; `error.code` is an enum and needs an explicit `static_cast<int32_t>` to print. The request ID is a `std::variant<int64_t, std::string>`, and the example converts it with `std::visit` plus `if constexpr` (`:91-100`) — the only safe way to do it.

## Running and measured output

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_EXAMPLES=ON
cmake --build build/debug --target RequestCancellation
build/debug/examples/RequestCancellation/RequestCancellation
```

Measured output (Windows, clang-cl, return code 0):

```
timeout handle id: 2
error: request timed out (code -32001)
cancel handle id: 3
error: request cancelled (code -32800)
```

The two terminal codes differ and both are **settled locally**: the timeout line still had `cancel_on_timeout` at its default `true`, so a cancellation notice went out as a side effect while the code stayed `-32001`. The example finishes by closing the client and then the server (`:148-149`).

## Mapping to the how-to pages

| What you want to do | Go to |
|---|---|
| Full semantics of cancellation, timeouts and the three failure channels | [/en/howto/cancel-a-request](/en/howto/cancel-a-request) |
| Error-code typing and remapping | [/en/howto/handle-errors](/en/howto/handle-errors) |
| Diagnosing a request that never answers | [/en/howto/debug-a-stuck-server](/en/howto/debug-a-stuck-server) |
| Turning long work into a pollable task | [/en/howto/run-long-tasks](/en/howto/run-long-tasks) |
| Full table of error codes and terminal shapes | [/en/reference/errors](/en/reference/errors) |

## Source behind this page

- `examples/RequestCancellation/main.cpp`
- `examples/RequestCancellation/CMakeLists.txt`
- `include/mcp/client/RequestHandle.hpp`, `include/mcp/protocol/IncomingRequestMeta.hpp`
- `include/mcp/protocol/McpSessionHandler.hpp`, `include/mcp/protocol/WireCodec.hpp`
- `include/mcp/transport/IRequestCanceller.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `include/mcp/ErrorCodes.hpp`
