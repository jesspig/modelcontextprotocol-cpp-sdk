# JSON 解析基准基线

对比自研解析器（`src/core/JsonParser.cpp`）与 simdjson v3.12.3。环境：Windows 11，clang-cl（MSVC 目标），同机同配置先后运行 `mcp-json-bench`。

## Debug 构建

| 样本 | 大小 | simdjson | 自研 | 比值 |
|---|---|---|---|---|
| tools/call | 100 B | 4.26 MB/s | 5.48 MB/s | 129% |
| tools/list | 5152 B | 4.27 MB/s | 4.47 MB/s | 105% |
| initialize | 152 B | 5.15 MB/s | 6.53 MB/s | 127% |
| error | 99 B | 4.46 MB/s | 5.55 MB/s | 124% |
| deep-nested | 103 B | 1.68 MB/s | 1.94 MB/s | 115% |

## Release 构建（LTO）

| 样本 | 大小 | simdjson | 自研 | 比值 |
|---|---|---|---|---|
| tools/call | 100 B | 126.27 MB/s | 140.62 MB/s | 111% |
| tools/list | 5152 B | 71.93 MB/s | 62.71 MB/s | 87% |
| initialize | 152 B | 148.29 MB/s | 144.59 MB/s | 98% |
| error | 99 B | 116.83 MB/s | 135.84 MB/s | 116% |
| deep-nested | 103 B | 24.64 MB/s | 25.20 MB/s | 102% |

## 结论

验收标准为「KB 级消息解析吞吐不低于 simdjson 基线的 25%」。全部样本达标：最差 87%（Release tools/list），多数样本超过 simdjson 基线。

# HTTP 基准对比（自研客户端 × libhv 服务器 → 自研客户端 × 自研服务器）

环境：Windows 11，clang-cl（MSVC 目标）。`mcp-http-bench`：GET RTT（keep-alive 复用 200 次）、POST 1KB×2000、并发 8 线程×50、SSE 广播 1000 事件。

## Debug 构建

| 指标 | libhv 服务器 | 自研服务器 | 比值 |
|---|---|---|---|
| GetRtt | 0.666 ms/op | 0.757 ms/op | 114% |
| PostThroughput | 2.10 MB/s | 1.44 MB/s | 69% |
| Concurrent(8x50) | 62.3 ms | 72.2 ms | 116% |
| SseStream(1000) | 15.87 s | 15.74 s | 99% |

## Release 构建（LTO）

| 指标 | 自研服务器 |
|---|---|
| GetRtt | 0.600 ms/op |
| PostThroughput | 1.92 MB/s |
| Concurrent(8x50) | 53.6 ms |
| SseStream(1000) | 15.79 s |

## 结论

验收标准：RTT 同数量级（≤2×）、吞吐 ≥50%、并发无超时失败。全部达标（Debug 最差 69%，Release 更优；SSE 持平）。libhv 服务器时代未记录 Release 数据，Release 列仅自研数据。
