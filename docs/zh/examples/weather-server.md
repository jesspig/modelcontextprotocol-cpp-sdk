# Weather 服务端逐段走读

`examples/WeatherServer` 演示**带输入 schema 的多工具服务端**：两个工具都声明 `inputSchema` 与 `required`，因此入参在进入 handler 前就被 SDK 校验。它和 `EchoServer` 的差别正是本页要讲的重点：有没有 schema，代码里哪些判空可以省、哪些不能省。源码：[`examples/WeatherServer/main.cpp`](https://github.com/jesspig/modelcontextprotocol-cpp-sdk/blob/master/examples/WeatherServer/main.cpp)。

## 这个示例演示什么

- 用 `ToolOptions` 的**链式 setter** 一次性给出 `Description` 与 `InputSchema`。
- 用 `JsonValue::Object{...}` / `JsonValue::Array{...}` 现场构造 JSON Schema（`JsonValue` 是仓库自研类型，不是 nlohmann）。
- 两个工具对同一份 `arguments` 采取两种读取风格，并说明各自依赖的前提。
- `server_instructions` 写给模型看的使用指引（工具选择提示）。

## 装配与指令

```cpp
ServerOptions opts;
opts.server_info = Implementation{"WeatherServer", "1.0.0"};
opts.server_instructions =
    "Get weather alerts and forecasts for US states. "
    "Call get_alerts for weather alerts, get_forecast for detailed forecasts.";
```

对应 `examples/WeatherServer/main.cpp:24-28`，传输与创建同 `EchoServer`（`:22-30`）。`instructions` 是给客户端/模型的路标，教它**何时该选哪个工具**；它不参与任何校验，写清用途才有价值。数据源是文件级常量表 `kWeatherData`（`:12-19`），含一个 `"default"` 兜底键——示例用它演示「查不到也要给可读回话」而不是抛错。

## 用链式 setter 声明输入 schema

```cpp
server->RegisterTool("get_alerts",
    ToolOptions{}
        .Description("Get weather alerts for a US state")
        .InputSchema(JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"properties", JsonValue(JsonValue::Object{
                {"state", JsonValue(JsonValue::Object{
                    {"type", JsonValue("string")},
                    {"description", JsonValue("Two-letter US state code")}})},
            })},
            {"required", JsonValue(JsonValue::Array{JsonValue("state")})},
        })),
    ...);
```

对应 `examples/WeatherServer/main.cpp:32-43`。要点：

- `Description` 与 `InputSchema` 都返回 `*this`，可串起来；`ToolOptions` 其余字段（如 `read_only_hint`）是直接成员赋值，没有 setter。
- `JsonValue::Object{...}` / `JsonValue::Array{...}` 是构造入口，`JsonValue("string")` 建字符串节点。
- 真实工具名仍是 `RegisterTool` 的第一个实参 `"get_alerts"`。

## 有 schema 之后 SDK 做了什么

`input_schema` 一旦声明，SDK 在把调用交给你的 handler **之前**按它校验 `arguments`：类型不符或缺必填项时直接回 `is_error` 结果，文本以 `invalid arguments: ` 开头（`src/server/McpServer.cpp:553-559`、`:2359`、`:2507`、`:2616`）。

这解释了两个 handler 的读取差异：

```cpp
auto& params = ctx.Params();
std::string state;
if (params.arguments && params.arguments->Contains("state")) {
    state = (*params.arguments)["state"].GetString();
}
```

对应 `examples/WeatherServer/main.cpp:46-50`。这里**没有**判 `IsString()`，能这么写的前提正是 schema 声明了 `state` 为 `string` 且 `required`——校验器已经挡住非字符串。`EchoServer` 没有 schema，同样的写法就会在未预期入参上抛（`GetString` 对非字符串节点是错误用法）。

## 两个 handler 的取参差异

`get_forecast` 用了完整的防御式读取（`examples/WeatherServer/main.cpp:76-85`）：

```cpp
std::string state;
double latitude = 0, longitude = 0;
if (params.arguments) {
    if (auto* v = params.arguments->Find("state"); v && v->IsString())
        state = v->GetString();
    if (auto* v = params.arguments->Find("latitude"); v && v->IsNumber())
        latitude = v->IsDouble() ? v->GetDouble() : static_cast<double>(v->GetInt());
    if (auto* v = params.arguments->Find("longitude"); v && v->IsNumber())
        longitude = v->IsDouble() ? v->GetDouble() : static_cast<double>(v->GetInt());
}
```

两个细节值得单独记：

- `Find` 返回 `JsonValue*`（找不到为 `nullptr`），配合 `if (auto* v = ...; v && v->IsString())` 是仓库惯用写法。
- **整数与浮点在同一节点上是两种表示**：`IsNumber()` 为真时还要用 `IsDouble()` 区分取 `GetDouble()` 还是 `GetInt()`。JSON 里 `40` 与 `40.5` 都会通过 `"type": "number"` 校验，不做这个分支就会在整数入参上拿到错值。

结果装配（`:86-91`）把经纬度直接拼进文本；`get_alerts` 用 `kWeatherData.find(state)` 命中或回 `"default"`（`:52-55`），两者都只回 `content`，没有声明 `output_schema`。

## 运行与观察

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMCP_BUILD_EXAMPLES=ON
cmake --build build/debug --target WeatherServer
build/debug/examples/WeatherServer/WeatherServer
```

实测输出（Windows，clang-cl）：

```
WeatherServer starting on stdio...
```

横幅在 stderr（`examples/WeatherServer/main.cpp:94`），随后 `server->Run()` 阻塞（`:95`）。想立刻看到校验效果，用任意客户端调 `get_alerts` 并**故意漏掉 `state`**：返回的应是 `is_error` 且文本以 `invalid arguments: ` 开头，而不是你的 handler 被调用。

## 与 how-to 的对应关系

| 想做的事 | 去看 |
|---|---|
| 工具注册全形态（标题、注解、图标） | [/zh/howto/register-a-tool](/zh/howto/register-a-tool) |
| 入参校验与失败返回 | [/zh/howto/validate-tool-input](/zh/howto/validate-tool-input) |
| 结构化输出（`outputSchema`） | [/zh/howto/register-a-tool](/zh/howto/register-a-tool) |
| 错误如何回给模型 | [/zh/howto/handle-errors](/zh/howto/handle-errors) |

## 本页对应源码

- `examples/WeatherServer/main.cpp`
- `examples/WeatherServer/CMakeLists.txt`
- `include/mcp/JsonValue.hpp`（`Object`/`Array` 构造、`Find`、`IsString`、`IsNumber`、`IsDouble`、`GetInt`、`GetDouble`）
- `include/mcp/McpTypes.hpp`（`ToolOptions`、`CallToolRequestParams`）
- `src/server/McpServer.cpp`（入参校验与 `invalid arguments:` 前缀）
