# 什么是 MCP？

[模型上下文协议](https://modelcontextprotocol.io)（Model Context Protocol）是一个开放标准，让你能够以安全、标准化的方式构建向 LLM 应用暴露数据和功能的服务器。可以把它想象成 AI 界的 USB-C 端口——AI 应用与其所需工具/数据之间的通用接口。

## 核心概念

**服务器（Server）** — 向连接的客户端提供能力（工具、资源、提示）。

**客户端（Client）** — 连接服务器，代表 LLM 发现并调用其能力。

**传输层（Transport）** — 承载 JSON-RPC 消息的底层通信层（stdio、HTTP、WebSocket）。

## 基本元语

| 元语         | 方向           | 描述                                  |
|--------------|---------------|---------------------------------------|
| 工具（Tools）      | 客户端 → LLM    | LLM 可调用的函数，用于执行操作             |
| 资源（Resources）  | 客户端 → LLM    | LLM 可读取的结构化数据                    |
| 提示（Prompts）    | 服务器 → LLM    | 常见交互模式的模板                        |
| 启发式收集（Elicitation）| 服务器 → LLM    | 请求用户输入（表单或 URL 导航）           |

## 协议版本

SDK 支持五个协议版本，分属两个时代：

| 时代 | 版本 | 握手方式 |
|------|------|----------|
| 旧版 | `2024-11-05`、`2025-03-26`、`2025-06-18`、`2025-11-25` | `initialize` |
| 现代 | `2026-07-28` | `server/discover` |

旧版使用 `initialize` 握手和独立的服务器到客户端请求（采样/根目录/启发式收集）。现代版（2026-07-28）使用无状态 `server/discover`、每请求 `_meta` 信封、MRTR（`InputRequiredResult`）和 `subscriptions/listen`。
