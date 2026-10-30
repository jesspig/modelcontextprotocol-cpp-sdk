---
type: Build
title: 构建系统
description: CMake 预设、编译器探测、Unity/LTO/缓存优化、conformance fixture 开关、系统依赖（仅可选 OpenSSL，由 MCP_WITH_OPENSSL 三态开关控制）。
tags: [cmake, ninja, unity, lto, conformance]
timestamp: 2026-10-18T13:48:17+08:00
resource: CMakePresets.json
---

# 构建系统

```bash
cmake --preset debug                 # 配置（Ninja，Debug）
cmake --build --preset debug         # 构建
cmake --build --preset debug --target mcp-transport-bench
ctest --preset debug --output-on-failure
```

仅 Ninja 生成器。`cmake_minimum_required(3.28...4.2)`，C++17 强制。

- 非 ctest 基准目标：`mcp-transport-bench`（[TransportBench.cpp](../tests/bench/TransportBench.cpp)），覆盖自研 HTTP 响应头解析、吞吐、并发、延迟响应和连接复用；基准不计入测试目标/用例统计

## 预设与开关

| 项 | 值 |
|----|----|
| 预设 | `debug` / `release`（对应 buildPresets/testPresets 各 2 枚） |
| 默认关闭 | `MCP_BUILD_TESTS`、`MCP_BUILD_EXAMPLES`、`MCP_BUILD_CONFORMANCE` 三开关在 `CMakeLists.txt:52-54` 默认全 OFF；预设仅置 `MCP_BUILD_TESTS=ON` / `MCP_BUILD_EXAMPLES=OFF`（无 `CONFORMANCE`，见 `CMakePresets.json:9-14`）；CI conformance job 双开（`conformance.yml:38`），`CMakeLists.txt:234-238` 以 `NOT EXAMPLES` 防 client fixture 重加 |
| Werror | 仅 `-DMCP_WERROR=ON`（CI 自动添加；MSVC `/WX`） |
| OpenSSL 开关 | `-DMCP_WITH_OPENSSL=AUTO\|ON\|OFF`，默认 `AUTO`：`AUTO` 按 `find_package(OpenSSL)` 结果启用，`ON` 未找到即 configure 失败，`OFF` 强制禁用（装有 OpenSSL 也不启用）；启用时由单一 INTERFACE 目标 `mcp-openssl` 统一向 `mcp-transport`/`mcp-client`/`mcp-ext` 施加 `MCP_HAVE_OPENSSL` 定义与链接，构建系统不做 `__has_include` 头文件嗅探 |
| `MCP_IS_CI` | 由环境变量 `CI` 定义与否决定 |
| job pool | 自动调优：compile = `min(mem/1500MB, cpu-2)`（下限 1）、link ≈ `mem/4000MB`（上限 2）；可用 `MCP_COMPILE_JOBS`/`MCP_LINK_JOBS` 覆盖 |

## 不易察觉的事实

- **Unity（jumbo）构建默认开启**，`-DMCP_UNITY_BUILD=OFF` 覆盖；批大小 `min(mem/500MB, (cpu+1)/2)` 下限 2，单核自动禁用
- Unity 开关按目标分化（`CMakeLists.txt:114-209`）：`mcp-core` / `mcp-server` / `mcp-client` / `mcp-http` / `mcp-ext` 全开，`mcp-transport` 与 `mcp-protocol` 显式关闭（匿名命名空间符号冲突）；`mcp-client` 另用 `UNITY_BUILD_UNIQUE_ID ON`（OAuth 符号）
- **编译器自动探测**在 `project()` 之前：Win 找 clang-cl（LLVM 两个安装路径），Linux/macOS 按序找 `clang++-19...clang++`；`CMAKE_CXX_COMPILER` 已设置则跳过
- **LTO 仅 Release**：clang-cl/MSVC 走 LTCG，Clang 走 ThinLTO，GCC 走 IPO
- **缓存**：sccache > ccache（ccache 跳过 MSVC）
- **`-march=native` 仅本地且非 Apple**（`MCP_IS_CI` 与 `APPLE` 门控），本地构建的二进制不可跨机分发
- MSVC 系编译标志：`/utf-8 /bigobj /W4 /wd4100 /wd4324 /wd4244 /wd4267 /EHsc` + 宏 `_CRT_SECURE_NO_WARNINGS`、`_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS`、`_WIN32_WINNT=0x0A00`、**`NOMINMAX` + `NOGDI`**（clang-cl 与 MSVC 两分支同加；`NOGDI` 排除 `wingdi.h` 的 GDI 定义（含与 `JsonValue::GetObject` 冲突的 `GetObject` 宏），`NOMINMAX` 排除 `min`/`max` 宏；配合全仓 `(std::min)`/`(std::max)` 括号防御纵深）
- Clang/GCC：`-Wall -Wextra -Wpedantic -Wno-unused-parameter`
- **GCC 独有 `-Wdangling-else`**：属 `-Wall` 成员（clang 不启用，显式加同参数亦不报该形态，实测 clang 22 + 真实 gtest 头静默）——断言宏（`EXPECT_*`/`ASSERT_*`）自带 `if/else`，作无括号单语句体时（`if (c) EXPECT_EQ(a, b);`）只有 Linux-gcc 两个 job 报 `-Werror`；CI run 38764091884 即因 `tests/unit/HttpServerTests.cpp:327` 该形态失败，现三处同类已补 `{}`，详见 [/AGENTS.md](../AGENTS.md) 关键陷阱第 11 条
- 非 Ninja 生成器提示警告；MSVC cl.exe + Ninja 自动加 `/lldlink`
- 配置期生成 `build_config.txt` 摘要

## 依赖（无第三方拉取）

| 依赖 | 版本 | 说明 |
|------|------|------|
| OpenSSL | 系统 3.x/4.x | 可选；启用由 `MCP_WITH_OPENSSL` 三态开关（默认 `AUTO`）+ `find_package(OpenSSL)` 结果决定，启用时经 `mcp-openssl` 定义 `MCP_HAVE_OPENSSL`（TLS 与 PKCE 随机数 `RAND_bytes`）并链接；PKCE 的 SHA-256 恒为内置实现（[sha256.hpp](../include/mcp/detail/sha256.hpp)）；支持 3.x/4.x，不支持主版本 configure 失败，详见 [/concepts/openssl-compat.md](/concepts/openssl-compat.md) |

## CI 工作流（.github/workflows/）

- `ci.yml`（**1 build job × 11 矩阵**：Windows/Linux-clang/Linux-gcc/macOS × debug/release，`ci.yml:13-61`，fail-fast false；conformance 另见 `conformance.yml:18,122` 的 server/client 双 job × 双 spec 共 4 leg）：actions 升版——checkout/cache @v5；**删除 Setup Ninja 步骤**（GitHub 镜像预装）；sccache 统一 `mozilla-actions/sccache-action@v0.0.11`（node24 兼容，替换 v0.0.7）三平台，`SCCACHE_DIR` 统一 `~/.cache/sccache`（缓存路径单一化，Windows 不再用 `AppData\Local\Mozilla\sccache`）；OpenSSL：Linux 显式 `libssl-dev`；macOS debug 显式 `openssl@4`、release 显式 `openssl@3`（`HOMEBREW_NO_AUTO_UPDATE`/`HOMEBREW_NO_INSTALL_CLEANUP`，configure 传 `OPENSSL_ROOT_DIR` 与 `MCP_OPENSSL_EXPECTED_MAJOR`）；auto 腿使用 `MCP_WITH_OPENSSL=ON`，缺失即失败；Linux 一个 job 跑 `scripts/check-openssl-version.cmake` 正反用例；**Windows 删除 choco openssl**（镜像预装，Configure 阶段自动探测 `C:\Program Files\OpenSSL*` 传 `OPENSSL_ROOT_DIR`）
- `openssl-canary.yml`（OpenSSL 最新版预警）：每周一 03:00 UTC + workflow_dispatch；macOS 用 `brew install openssl`（滚动最新）、Linux 用发行版 `libssl-dev`；构建与测试失败只红 canary，不进入分支保护。
- `678a733 fix(构建)`（OpenSSL 3.5 兼容与 Linux 链接顺序）：`EC_KEY_get_curve_name`（3.5 已移除）→ `EC_GROUP_get_curve_name(EC_KEY_get0_group(…))`；`mcp-ext` 补声明 `mcp-transport` 依赖（`Triggers.cpp` 用 `HttpClient`，GNU ld 单向扫描致未定义引用）；`AppendUtf8CodePoint` 加 `#ifdef _WIN32`（仅 Windows 宽字符环境变量转换路径使用，Linux `-Werror` 报未使用）
- `conformance.yml`（官方 conformance suite，push/PR 到 `develop`）：server/client 双 job，referee pin `@modelcontextprotocol/conformance@0.2.0-alpha.12`（`conformance.yml:14`，alpha 通道最新；requirements 内核与 alpha.11 一致，升级只收 referee 修补）；按 `CONFORMANCE_SPEC_VERSION` 映射 per-leg 基线与参数（`conformance.yml:59-62`、`run-conformance.sh:11-17`，未知版本回退 `tests/conformance/baseline.yaml`，见 `baseline.yaml:6`）：2026 切 `--stateless` + `--requirements 2026-07-28`（modern），2025 走 `--spec-version`（legacy 有状态 wire）；退出码语义由 referee 保证，不用 `continue-on-error`；本地复现 `scripts/run-conformance.sh server|client`，fixture 与基线说明见 [/tests.md](/tests.md)
- `docs.yml`（VitePress 发布）：触发分支 `master`（唯一允许分支，参见 AGENTS.md docs.yml 部署分支铁律）；pnpm/action-setup@v5、setup-node@v5、cache@v5、configure-pages@v6、upload-pages-artifact@v4、deploy-pages@v5；cache key 修正为 `hashFiles('docs/**', 'docs/pnpm-lock.yaml')`（原路径错误导致缓存永不命中）

## 相关页面

- [/tests.md](tests.md) — 测试体系
- [/modules/core.md](modules/core.md) — 编译影响面最大的库
- [/AGENTS.md](../AGENTS.md) — 仓库指南中的构建速查
