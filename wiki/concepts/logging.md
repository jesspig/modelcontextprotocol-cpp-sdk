---
type: Concept
title: 日志与可观测性
description: 日志级别、全局钩子（SetLogHandler/SetLogLevel/LogRecord）、诊断增强与 bind_host 可配置。
tags: [日志, 可观测性, 诊断, bind_host]
timestamp: 2026-08-28T15:00:00+08:00
resource: include/mcp/Log.hpp
---

# 日志与可观测性

全部内部日志经 `MCP_LOG` / `MCP_LOG_TAG` / `MCP_LOG_CTX` 宏，最终汇聚到 `LogWrite` / `LogMessage`（`include/mcp/Log.hpp`）。

## 级别与默认行为

- `LogLevel`：`Off(0)` / `Error(1)` / `Warning(2)` / `Info(3)` / `Debug(4)` / `Trace(5)`。
- 默认从环境变量 `MCP_LOG_LEVEL` 读取（未设置 = `Off`，即不开任何日志）。
- 新增 `mcp::SetLogLevel(LogLevel)`：运行时覆盖环境变量（原实现为静态缓存、不可运行时变更）。

## 全局日志钩子

- `mcp::SetLogHandler(LogHandler)`：设置自定义回调；传 `nullptr` 恢复默认 `stderr` 输出。
- `mcp::GetLogHandler()`：获取当前回调（无则空 callable）。
- `LogRecord` 结构化记录字段：`level`、`tag`、`message`、`file`、`line`、`context`（`LogContext*`，含 request_id/session_id/peer/method，可能为空）。`MCP_LOG_CTX` 会填充 `context`。
- 用户设置钩子后，日志**仅走钩子、不再写 stderr**（由用户完全掌控落点）；不设钩子时与历史逐字节一致（仅 `stderr`、受 `MCP_LOG_LEVEL` 控制）。

## 线程安全约束

- 钩子在**发射日志的线程（含 IO 线程）同步调用**，用户须保证线程安全。
- **禁止在钩子内调用 `Close()`**：IO 线程直接执行回调，回调里 `Close()` 会 self-join（见 [/concepts/concurrency.md](/concepts/concurrency.md)）。

## 诊断日志增强（本次新增）

- `HttpServer::Start` 绑定前打印 `binding <addr>:<port>`（`Info`）；bind 失败消息补充地址。
- DNS 重绑保护拒绝请求时，打印被拒 `Host` 值；无 `Host` 头时打印 `<missing>`，**不会因 `at()` 抛异常而崩溃**（403 状态码/文本不变）。

## 绑定地址可配置

- `HttpServerOptions::bind_host` 与 `StreamableHttpServerOptions::host`：监听地址可配置。
- 仅接受 IPv4 字面量（如 `127.0.0.1` / `0.0.0.0`）；空字符串 = 监听所有接口（`INADDR_ANY`，与历史一致）；不支持主机名解析与 IPv6。
- 非法 `bind_host` 抛 `std::runtime_error("HttpServer: invalid bind_host: ...")`。
