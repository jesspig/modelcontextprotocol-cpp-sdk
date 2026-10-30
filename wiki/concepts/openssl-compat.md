---
type: Concept
title: OpenSSL 兼容与版本策略
description: OpenSSL 3.x/4.x 支持矩阵、配置期版本校验、CI 显式锁定、周度 canary 与主版本升级清单。
tags: [openssl, build, ci, dependency, tls]
timestamp: 2026-10-30T22:58:50+08:00
resource: cmake/FetchDependencies.cmake
---

# OpenSSL 兼容与版本策略

项目把 OpenSSL 视为**可选系统依赖**，但把「用哪个主版本」变成显式契约：支持 3.x/4.x，不支持的主版本在 configure 阶段失败，必跑 CI 显式锁定版本，最新版由独立 canary 预警。

## 支持矩阵

| 版本 | 状态 | 说明 |
| --- | --- | --- |
| 3.0.x | 支持 | Ubuntu 24.04 `libssl-dev`；由发行版维护安全补丁 |
| 3.5.x LTS | 推荐 | 上游支持到 2030-04-08；Homebrew `openssl@3` 当前解析版本 |
| 3.6.x | 支持 | 非 LTS；Windows runner 镜像当前版本 |
| 4.0.x | 支持 | 非 LTS，2027-05-14 EOL；X509 const 变更引入版本 |
| 4.1.x | 支持 | 非 LTS，2027-11-23 EOL；Homebrew `openssl@4` 当前解析版本 |
| 4.2.x LTS | 计划 | 计划 2027-04 发布，届时按升级清单评估 |

参考：[OpenSSL 发布策略](https://www.openssl.org/about/releasestrat.html)、[OpenSSL Roadmap](https://openssl-library.org/roadmap/)。

## 配置语义

- `MCP_WITH_OPENSSL=AUTO`：找到即启用；未找到关闭 TLS。
- `MCP_WITH_OPENSSL=ON`：未找到 configure 失败；找到但主版本不在 {3,4} 也失败。
- `MCP_WITH_OPENSSL=OFF`：不探测、不链接、不定义 `MCP_HAVE_OPENSSL`。
- `MCP_OPENSSL_EXPECTED_MAJOR`：仅在 CI 使用；非空时断言实际主版本，不匹配即 configure 失败。
- 版本校验由 `cmake/OpenSslVersion.cmake` 的 `mcp_validate_openssl_version` 与 `mcp_assert_openssl_major` 承担；独立检查脚本为 `scripts/check-openssl-version.cmake`。

## 失败模式

| 场景 | 行为 |
| --- | --- |
| 未找到 OpenSSL，`AUTO` | TLS 关闭；PKCE 回退内置 SHA-256；构建继续 |
| 未找到 OpenSSL，`ON` | configure 失败 |
| 找到 3.x/4.x | 启用 TLS；CI 继续断言预期主版本 |
| 找到 2.x/5.x 或无法解析 | configure 失败，消息含支持范围与 `MCP_WITH_OPENSSL=OFF` 提示 |
| 找到的主版本与 `MCP_OPENSSL_EXPECTED_MAJOR` 不符 | configure 失败，消息含实际版本与预期主版本 |

不支持的主版本**不会**静默关闭 TLS；这是与 `AUTO` 未找到场景的关键区别。

## CI 策略

- `ci.yml`：11 个矩阵项；Linux/Windows 断言 3.x，macOS debug 使用 `openssl@4`、release 使用 `openssl@3`。
- auto 腿使用 `MCP_WITH_OPENSSL=ON`，OpenSSL 缺失即失败，避免 CI 静默编译掉 TLS 路径。
- macOS 安装步骤显式 `brew install "openssl@${major}"` 并传 `OPENSSL_ROOT_DIR`；必跑 CI 禁止 `brew install openssl` 滚动别名。
- 一个 Linux job 运行 `scripts/check-openssl-version.cmake`，验证 3.0.13/4.0.3/4.1.0 放行、5.0.0 拒绝。
- `openssl-canary.yml`：每周一 03:00 UTC + 手动触发；macOS 用 Homebrew 最新版、Linux 用发行版 OpenSSL；失败不进入分支保护。

## 主版本升级清单

1. 读 canary run，确认失败点与新版本变更（发布说明、头文件签名）。
2. 更新 `mcp_validate_openssl_version` 的支持范围与 `scripts/check-openssl-version.cmake` 的正反用例。
3. 更新 `ci.yml` 矩阵的 `openssl_major`/formula 与期望主版本。
4. 跑本地全量测试；推送分支开 PR，确认 11/11 绿与 canary 绿。
5. 更新 `docs/{zh,en}/guide/installation.md`、`wiki/build.md`、本页与 changelog。
6. 主版本 EOL 前至少一个季度完成升级；不在必跑 CI 中保留已 EOL 的唯一版本。

## 不内置源码的决策

不把 OpenSSL 源码作为子模块或 FetchContent 默认依赖。原因：OpenSSL 4.x 无 CMake 构建系统，源码内置需要 Perl 与 make（Windows 还需 NASM）；仓库体积、11 个 CI job 的构建成本、CVE 跟进与静态符号冲突都由本项目承担。当前策略用「系统依赖 + 显式范围 + 确定 CI + canary」获得可预期的稳定性；只有出现「用户不安装 OpenSSL 也必须使用 TLS」或「完全离线交付」的硬性需求时，才重新评估 vcpkg/Conan，再评估源码内置。

## 相关文件

- `cmake/FetchDependencies.cmake`
- `cmake/OpenSslVersion.cmake`
- `scripts/check-openssl-version.cmake`
- `.github/workflows/ci.yml`
- `.github/workflows/openssl-canary.yml`
- `tests/unit/TlsTestCertificate.hpp`
