# 更新摘要

- 2026-08-14 — 0.3.0 发布前全面文档核查：版本默认值更新为 0.3.0（CMakeLists/client_info/server_version/server_info），3 子代理并行核查 wiki 29 页 + docs 中英 43 页；测试目标数修正为 16（unit 13 实测）/ 392 用例 / 912 断言，OpenSSL 改述可选依赖，废弃 SamplingHandler/RootsHandler 示例移除，错误码映射表修正，见 [/changelog/2026-08-14-log.md](/changelog/2026-08-14-log.md)
- 2026-08-14 — 自研测试框架替换 googletest：`tests/framework/`（mcp-test + mcp-test-main：TEST/TEST_F、22 断言宏、ToString、过滤、`--list-tests`）+ `McpDiscoverTests.cmake` 逐用例注册，SelfTests 14 例，15 目标 / 392 用例双配置全绿，`--gtest_filter`/`ctest -R` 兼容，googletest 依赖移除（configure 无需联网），文档同步清理，见 [/changelog/2026-08-14-log.md](/changelog/2026-08-14-log.md)
- 2026-08-14 — 增量审计复核：3 子代理配对审计，修正 12 页 + index（core 行号/84 对序列化、protocol 客户端 4 通知处理器、mrtr 三件套字段、CancelTask 写 error_message、Mcp-Method 生成/回显区分、ITransport 4 纯虚、SSE 回调捕获等），测试 15 目标 / 378 用例 / 881 断言（含 mcp-net-tests），见 [/changelog/2026-08-14-log.md](/changelog/2026-08-14-log.md)
- 2026-08-13 — 自研网络栈替换 libhv：TcpSocket/TlsSocket/HttpClient/WebSocketClient/Sha1 + 自研 HttpServerImpl，4 使用方替换，378 用例全绿，基准达标，libhv 依赖与补丁移除，文档同步清理，见 [/changelog/2026-08-13-log.md](/changelog/2026-08-13-log.md)
- 2026-08-13 — 自研 JSON 解析器替换 simdjson：递归下降解析器（深度上限 512、错误带 offset）、JsonParserTests 边界矩阵 75 例、mcp-json-bench 基准，文档同步清理，见 [/changelog/2026-08-13-log.md](/changelog/2026-08-13-log.md)
- 2026-08-13 — AGENTS.md 复核修正：一致性测试 113→111、协议版本 6、OpenSSL 硬依赖改述、补 self-join 陷阱，新增 wiki 维护规则章节，见 [/changelog/2026-08-13-log.md](/changelog/2026-08-13-log.md)
- 2026-08-13 — 增量更新：代码大重构同步（Win32 Overlapped、时代注册表严格对齐、响应单 worker、缓存平铺、Auto 回退对齐、FileTaskStore 双锁等），3 子代理审计更新 26 页，测试 271→278，见 [/changelog/2026-08-13-log.md](/changelog/2026-08-13-log.md)
- 2026-08-13 — 初始构建：全量审计 7 层源码（core/protocol/transport/http/client/server/build+tests），建立 wiki/ 知识库（28 页 + 索引），见 [/changelog/2026-08-13-log.md](/changelog/2026-08-13-log.md)
