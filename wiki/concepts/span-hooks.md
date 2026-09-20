---
type: Concept
title: Span 观测钩子
description: 无 OpenTelemetry 依赖的同步 span 事件钩子，覆盖服务端请求、传输接收和传输发送边界。
tags: [可观测性, span, traceparent, tracestate, baggage]
timestamp: 2026-09-20T03:14:18+08:00
resource: include/mcp/SpanHooks.hpp
---

# Span 观测钩子

`SpanHooks.hpp` 提供无第三方 tracing 依赖的 `SpanHandler`。它把会话引擎中的关键边界以成对事件交给调用方，调用方可以在外部接入 OpenTelemetry 或其他观测系统。

## 事件模型

- `SpanKind::ServerRequest` 表示入站请求处理；`TransportReceive` 和 `TransportSend` 分别表示消息接收与发送。
- `SpanPhase::Begin` 与 `End` 按 `(kind, request_id)` 配对；结束事件的 `failed` 标记反映拒绝、异常或传输失败。
- `SpanEvent` 携带 name、method、request_id，以及可选 `traceparent`、`tracestate`、`baggage`。服务端请求事件从入站 `_meta` 复制这些 trace context。

## 接入与线程约束

- `McpSessionHandler::SetSpanHandler` 必须在 `Start()` 前调用；`ClientOptions::span_handler` 会在 `McpClient` 启动会话前注入同一钩子。
- 钩子在发射事件的线程同步执行，调用方必须保证线程安全、不得抛出异常，也不得在钩子内调用 `Close()`。
- 未设置钩子时不创建事件对象，不增加这些路径的构造、分配或间接调用；设置后每个 Begin 都有对应 End，包括请求被拒绝的路径。

## 验证

`tests/unit/SpanHookTests.cpp` 当前包含 5 个用例，归入 `mcp-wire-codec-tests`，覆盖未注入回归、服务端请求边界、传输边界、trace context 传递和拒绝请求收尾。

## 相关页面

- [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md) — 钩子挂接点
- [/classes/mcp-client.md](../classes/mcp-client.md) — `ClientOptions` 注入
- [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) — 入站 trace 元数据
- [/concepts/concurrency.md](../concepts/concurrency.md) — 发射线程与生命周期
- [/concepts/logging.md](../concepts/logging.md) — 传统日志钩子
