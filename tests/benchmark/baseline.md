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
