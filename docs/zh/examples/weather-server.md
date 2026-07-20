# 天气服务器示例

一个更接近实际的 MCP 服务器，演示多工具注册和模拟天气数据。

源代码：[`examples/WeatherServer/`](https://github.com/modelcontextprotocol/cpp-sdk/tree/main/examples/WeatherServer)

## 特性

- **工具**：`get_alerts` — 返回某个美国州的模拟天气预警
- **工具**：`get_forecast` — 返回带有位置参数的模拟天气预报

## 运行

```bash
cmake --preset debug
cmake --build --preset debug
build/debug/examples/WeatherServer/WeatherServer
```

## 关键概念

WeatherServer 演示了：

1. **多工具注册** — 注册两个不同的工具（`get_alerts`、`get_forecast`），具有不同的签名和逻辑
2. **工具参数** — 从 `CallToolRequestParams` 的 `arguments` 中读取输入值（`state`、`latitude`、`longitude`）
3. **基于文本的响应** — 从工具处理程序返回 `TextContent` 结果
4. **默认回退** — 在数据缺失时使用默认响应而非结构化错误
