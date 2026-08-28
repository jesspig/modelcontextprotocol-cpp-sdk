# 获取 SDK 内部日志

MCP C++ SDK 的内部运行日志默认关闭，便于在需要时按需开启以排查问题。

## 方式一：环境变量（最快）

设置 `MCP_LOG_LEVEL`（0–5，0=Off，4=Debug）后启动进程，日志直接输出到 `stderr`：

```bash
MCP_LOG_LEVEL=4 ./your_mcp_server
```

级别：`Off=0` `Error=1` `Warning=2` `Info=3` `Debug=4` `Trace=5`。调试排障一般用 `4`。

## 方式二：程序化钩子（集成进自有日志系统）

通过全局钩子捕获结构化 `LogRecord`，写入你自己的日志后端：

```cpp
#include <mcp/Log.hpp>

mcp::SetLogLevel(mcp::LogLevel::Debug);
mcp::SetLogHandler([](const mcp::LogRecord& r) {
    my_logger.log(static_cast<int>(r.level),
                  r.tag.empty() ? std::string(r.tag) : std::string(r.tag),
                  std::string(r.message));
});
// 需要恢复默认 stderr 时：
// mcp::SetLogHandler(nullptr);
```

注意：

- 钩子在发射日志的线程（含 IO 线程）**同步**调用，回调须线程安全。
- **不要在钩子内调用 `Close()`**，否则可能 self-join 死锁。
- 钩子内部抛出的异常会被 SDK 吞掉，不会向上传播。

## 绑定地址可配置

`HttpServer` 现支持 `HttpServerOptions::bind_host`（及 `StreamableHttpServerOptions::host`）指定监听地址，接受 IPv4/IPv6 字面量（如 `127.0.0.1` / `::1` / `::`，可用 `[]` 包围），空字符串表示仅监听 IPv4 所有接口（默认行为）。
