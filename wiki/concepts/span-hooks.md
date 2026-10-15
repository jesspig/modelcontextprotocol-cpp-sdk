---
type: Concept
title: Span 观测钩子
description: 无 OpenTelemetry 依赖的同步 span 事件钩子，覆盖服务端请求、传输接收和传输发送边界。
tags: [可观测性, span, traceparent, tracestate, baggage]
timestamp: 2026-10-14T18:07:07+08:00
resource: include/mcp/SpanHooks.hpp
---

# Span 观测钩子

`SpanHooks.hpp` 提供无第三方 tracing 依赖的 `SpanHandler`。它把会话引擎中的关键边界以成对事件交给调用方，调用方可以在外部接入 OpenTelemetry 或其他观测系统。

## 事件模型

- `SpanKind::ServerRequest` 表示入站请求处理；`TransportReceive` 和 `TransportSend` 分别表示消息接收与发送。
- `SpanPhase::Begin` 与 `End` 按 `(kind, request_id)` 配对；结束事件的 `failed` 标记反映拒绝、异常或传输失败。
- `SpanEvent` 携带 name、method、request_id，以及可选 `traceparent`、`tracestate`、`baggage`。服务端请求事件从入站 `_meta` 复制这些 trace context——Begin 与 End 携带同一份（End 经请求 `_meta` 原样回填，拒绝路径亦然）；拒绝、异常或传输失败的 End 置 `failed=true`，Begin 恒 `failed=false`。

## 接入与线程约束

- `McpSessionHandler::SetSpanHandler` 必须在 `Start()` 前调用；`ClientOptions::span_handler` 会在 `McpClient` 启动会话前注入同一钩子。
- 钩子在发射事件的线程同步执行，调用方必须保证线程安全、不得抛出异常，也不得在钩子内调用 `Close()`。
- 未设置钩子时不创建事件对象，不增加这些路径的构造、分配或间接调用；设置后每个 Begin 都有对应 End，包括请求被拒绝的路径。

## 拒收路径的 End 先行语义

- `OnRequest` 内拒收 early-return 一律走局部 `reject` 闭包：显式发 ServerRequest End（`failed=true`，trace context 原样回填）→ 置 `span_open=false`（抑制函数返回时栈上 guard 重发）→ 最后 `SendErrorResponse`。共 9 处（含两处 handler 异常 catch），与 `DeliverResponse` 三分支“先 `finish_span` 再 `SendMessage`”同序，成功路径与 `DeliverResponse` 一字未动
- 此前拒收路径是先 `SendErrorResponse`、栈上 guard 析构才补发 End，错误已上通道而 End 尚未发射，构成跨线程竞态——CI `windows-2022-debug` 上 `SpanHookTest.RejectedRequestEndPairsWithTraceContext` 因此出现 End 计数 0 vs 1 的偶发失败
- `VerifyCapability` 为配合该语义由“内部直接发送 + 返回 bool”改为“只判定不发送”，返回 `optional<ErrorData>`，由调用方经 `reject` 统一收尾（声明见 `McpSessionHandler.hpp`）

## 验证

`tests/unit/SpanHookTests.cpp` 当前包含 7 个用例，归入 `mcp-wire-codec-tests`，覆盖未注入回归、服务端请求边界、传输边界、trace context 传递（含成功 End 回填）、拒绝请求收尾（含 Begin/End 配对计数与拒绝 End 的 trace 回填）。

## OTel 差异说明

- 本 SDK 默认不引入 OpenTelemetry 依赖：可观测只经同步 `SpanHandler` 钩子外发，span 的创建、上报与采样由调用方在外部完成（取事件中的 W3C `traceparent`、`tracestate`、`baggage` 桥接）。
- 与 OTel 原生对端的互操作点仅为 `_meta` 明文字段：出站请求的追踪字段仅在 `RequestMeta` 显式设置时才序列化；入站追踪字段只复制到 span 事件，不做解析、校验或自动续接。

## 相关页面

- [/classes/mcp-session-handler.md](../classes/mcp-session-handler.md) — 钩子挂接点
- [/classes/mcp-client.md](../classes/mcp-client.md) — `ClientOptions` 注入
- [/concepts/meta-and-filters.md](../concepts/meta-and-filters.md) — 入站 trace 元数据
- [/concepts/concurrency.md](../concepts/concurrency.md) — 发射线程与生命周期
- [/concepts/logging.md](../concepts/logging.md) — 传统日志钩子
