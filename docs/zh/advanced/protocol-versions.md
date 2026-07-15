# 协议版本

SDK 通过双 `WireCodec` 架构支持两个 MCP 协议时代。

## 版本对比

| 版本 | 状态 | 关键特性 |
|-------------|---------|--------------|
| 2024-11-05 | 旧版 | 原始规范 |
| 2025-03-26 | 旧版 | 稳定握手 |
| 2025-06-18 | 旧版 | 中间版本 |
| 2025-11-25 | 旧版 | `initialize` 握手，独立的服务器到客户端请求 |
| 2026-07-28 | 当前 | `server/discover`、每请求 `_meta`、MRTR、`subscriptions/listen` |

## WireCodec

`WireCodec` 工厂通过字符串比较自动选择正确的编解码器：

```cpp
auto codec = MakeWireCodec("2026-07-28");
// 如果版本 >= "2026-07-28"，返回 Rev2026Codec
// 否则回退至 Rev2025Codec 用于旧版本
```

### 时代之间的关键差异

| 方面 | 2025-11-25 | 2026-07-28 |
|--------|-----------|------------|
| 连接 | `initialize` 握手 | `server/discover` 每请求 `_meta` |
| 能力 | 一次性协商 | 通过 `_meta` 每请求指定 |
| 采样 | 独立请求 | 已移除（使用 Elicitation） |
| 日志 | `logging/setLevel` RPC | 每请求 `_meta.logLevel` |
| 订阅 | `subscribe`/`unsubscribe` | `subscriptions/listen` 流 |
| 错误码 | 直接值 | 重新映射（`-32001` → `-32020` 等） |
| 结果 | 普通 JSON | 带 `resultType` 字段的类型化结果 |

## 错误码重新映射（2026 时代）

| 代码 | 名称 | 2025 值 | 2026 值 |
|------|------|-----------|-----------|
| HeaderMismatch | 头部不匹配 | -32001 | -32020 |
| MissingRequiredClientCapability | 缺少必要能力 | -32003 | -32021 |
| UnsupportedProtocolVersion | 版本不匹配 | -32004 | -32022 |
