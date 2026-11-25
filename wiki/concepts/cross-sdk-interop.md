---
type: concept
title: 跨 SDK 双向互操作矩阵
description: 用官方 TypeScript 与 Python SDK 验证 C++ 客户端/服务端的 Tasks、Skills、OAuth 与 Streamable HTTP 互操作，含精确版本与结论边界。
tags: [interop, typescript-sdk, python-sdk, oauth, tasks, streamable-http]
timestamp: 2026-11-25T07:33:00Z
---

# 跨 SDK 双向互操作矩阵

T-23（ST-05）以官方 SDK 精确版本验证四个方向的互操作。证据根目录 `.handoff/evidence/T-23/`（gitignore），复跑入口 [run-interop.sh](../../scripts/run-interop.sh)，fixture 源码在 [tests/interop](../../tests/interop)。

## 锁定版本

| 组件 | 精确版本 | 校验 |
| ---- | -------- | ---- |
| 官方 TS SDK | `@modelcontextprotocol/sdk` 1.32.2 | npm integrity `sha512-SWsVtH/...`，shasum `6ede940e...` |
| 官方 Python SDK | `mcp` 2.3.0 | wheel SHA256 `dd0c44c0...` |
| Node | v22.23.3（npm 10.9.9） | nvm 精确安装 |
| Python | 3.14.4 | venv 固定依赖 |
| C++ | MCP_SDK_VERSION 0.3.12 | WSL clang++ 21.1.8 构建 conformance fixture |

无 `latest`、无版本漂移；完整清单见 `.handoff/evidence/T-23/sdk-versions.txt`。

## 四方向结论

| 方向 | 结果 | 关键证据 |
| ---- | ---- | -------- |
| C++ server ↔ TS client | 核心 9/9、Tasks 原始协议 7/7、OAuth 6/6、HTTP 6/6 PASS | `cpp-server-ts-client/`、`tasks/`、`oauth/`、`http/` |
| C++ server ↔ Python client | 核心 9/9、OAuth 5/5 PASS；Tasks/Skills 不适用 | `cpp-server-python-client/`、`oauth/` |
| TS server ↔ C++ client | initialize / tools_call / HTTP 头 / 错误映射 4/4 exit 0（legacy 2025-11-25） | `ts-server-cpp-client/` |
| Python server ↔ C++ client | legacy 3/3 + modern 2026-07-28 1/1 exit 0 | `python-server-cpp-client/` |

REQUIRED 单元格 43 PASS / 0 FAIL / 6 N/A；每项 N/A 均给出官方 SDK 能力或协议版本依据，不以 skip 冒充通过。

## 能力边界与披露

- TS SDK 1.32.2 最高支持 2025-11-25，且无 `skills/*` 方法；C++ 的 skills 方法要求 ≥ 2026-07-28，故该单元格不适用。
- Python SDK 2.3.0 最高支持 2026-07-28，但 wheel 内无 Tasks/Skills 方法或类型，故相应类型化 API 单元格不适用。
- TS SDK 类型化 Tasks API 与官方 ext-tasks v0.2.3 线格式不一致（`ttl` vs `ttlMs`）：上游问题，最小复现与规避见 `.handoff/evidence/T-23/findings.txt` F-01；原始 JSON-RPC 经官方 SDK 传输可用。
- TS SDK 无 `server/discover`（2026-07-28 探测）：C++ client 的 Auto 模式不可用，fixture 以 `MCP_CONFORMANCE_FORCE_LEGACY=1` 跑通；见 F-03。
- OAuth 用本地授权服务器与合成令牌，覆盖发现、401 挑战、PKCE S256、令牌、resource/audience 与 401 恢复；不接真实账号/密钥。方向 3/4 的服务端 OAuth 未在本轮 fixture 启用，见 F-04。
- Windows 原生构建仍无 OpenSSL；WSL 侧 OpenSSL 3.5.5 可用，两者分别记录，不互相替代。

## 复跑

```bash
cmake --preset debug -DMCP_BUILD_CONFORMANCE=ON -DMCP_BUILD_EXAMPLES=ON
cmake --build --preset debug --target conformance-server conformance-client
bash scripts/run-interop.sh all
```

脚本固定端口、有界等待；`MCP_INTEROP_CPP_DIR` 可指定 fixture 二进制目录，`MCP_INTEROP_OUT` 指定日志目录。安装阶段访问 npm/PyPI，运行阶段全部走 localhost。

相关：[tests.md](../tests.md)、[concepts/tasks-extension.md](tasks-extension.md)、[concepts/skills-extension.md](skills-extension.md)、[concepts/oauth.md](oauth.md)、[transports/streamable-http.md](../transports/streamable-http.md)。
