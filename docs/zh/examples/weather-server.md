# 天气服务器示例

一个更接近实际的 MCP 服务器，封装外部天气 API。

源代码：[`examples/WeatherServer/`](https://github.com/modelcontextprotocol/cpp-sdk/tree/main/examples/WeatherServer)

## 特性

- **工具**：`get_forecast` — 获取某个地点的天气预报
- **工具**：`get_alerts` — 获取某个地区的天气预警
- **资源**：`weather:///{location}/current` — 当前天气状况
- **资源模板**：`weather:///{location}/forecast` — 预报数据

## 运行

```bash
cmake --preset debug
cmake --build --preset debug
build/debug/examples/WeatherServer/WeatherServer
```

## 关键概念

WeatherServer 演示了：

1. **外部 API 集成** — 在工具处理程序中调用 HTTP 端点
2. **资源模板** — URI 变量提取（`{location}`）
3. **结构化内容** — 在文本响应旁返回 JSON 数据
4. **错误处理** — 对无效位置返回结构化错误
