---
type: Build
title: 构建系统
description: CMake 预设、编译器探测、Unity/LTO/缓存优化、依赖拉取。
tags: [cmake, ninja, unity, lto]
timestamp: 2026-08-13T16:30:00+08:00
resource: CMakePresets.json
---

# 构建系统

```bash
cmake --preset debug                 # 配置（Ninja，Debug）
cmake --build --preset debug         # 构建
ctest --preset debug --output-on-failure
```

仅 Ninja 生成器。`cmake_minimum_required(3.28...4.2)`，C++17 强制。

## 预设与开关

| 项 | 值 |
|----|----|
| 预设 | `debug` / `release`（对应 buildPresets/testPresets 各 2 枚） |
| 默认关闭 | `MCP_BUILD_TESTS`、`MCP_BUILD_EXAMPLES`（预设中 tests=ON） |
| Werror | 仅 `-DMCP_WERROR=ON`（CI 自动添加；MSVC `/WX`） |
| `MCP_IS_CI` | 由环境变量 `CI` 定义与否决定 |
| job pool | 自动调优：compile ≈ `mem/1500MB`、link ≈ `mem/4000MB`（上限 2）；可用 `MCP_COMPILE_JOBS`/`MCP_LINK_JOBS` 覆盖 |

## 不易察觉的事实

- **Unity（jumbo）构建默认开启**，`-DMCP_UNITY_BUILD=OFF` 覆盖；批大小 `min(mem/500MB, (cpu+1)/2)` 下限 2，单核自动禁用
- `mcp-transport` 与 `mcp-protocol` 显式关闭 Unity（匿名命名空间符号冲突）；`mcp-client` 用 `UNITY_BUILD_UNIQUE_ID ON`（OAuth 符号）
- **编译器自动探测**在 `project()` 之前：Win 找 clang-cl（LLVM 两个安装路径），Linux/macOS 按序找 `clang++-19...clang++`；`CMAKE_CXX_COMPILER` 已设置则跳过
- **LTO 仅 Release**：clang-cl/MSVC 走 LTCG，Clang 走 ThinLTO，GCC 走 IPO
- **缓存**：sccache > ccache（ccache 跳过 MSVC）
- **`-march=native` 仅本地**（`MCP_IS_CI` 门控），debug 二进制不可移植出构建机
- MSVC 系编译标志：`/utf-8 /bigobj /W4 /wd4324 /wd4244 /wd4267 /EHsc` + `_WIN32_WINNT=0x0A00`
- Clang/GCC：`-Wall -Wextra -Wpedantic -Wno-unused-parameter`
- 非 Ninja 生成器提示警告；MSVC cl.exe + Ninja 自动加 `/lldlink`
- 配置期生成 `build_config.txt` 摘要

## 依赖（FetchContent 自动拉取）

| 依赖 | 版本 | 说明 |
|------|------|------|
| GoogleTest | 1.15.2 | 仅 `MCP_BUILD_TESTS=ON`；`BUILD_GMOCK=OFF` |
| OpenSSL | 系统 | 可选；`MCP_HAVE_OPENSSL` 定义，PKCE 失败回落内置 SHA-256 |

## 相关页面

- [/tests.md](tests.md) — 测试体系
- [/modules/core.md](modules/core.md) — 编译影响面最大的库
- [/AGENTS.md](../AGENTS.md) — 仓库指南中的构建速查
