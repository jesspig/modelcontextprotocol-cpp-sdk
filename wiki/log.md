# 更新摘要

- 2026-10-13 — private_key_jwt 客户端断言（ES256）落地：`OAuthClientOptions` 增 `private_key_pem` / `signing_algorithm` / `client_assertion_clock_seconds`，元数据 `token_endpoint_auth_methods_supported` 含 `private_key_jwt` 时三个令牌请求改发 `client_assertion_type` + ES256 签名 `client_assertion`（不再发 `client_secret`）；签名复用新增的 `mcp::ext::dpop::detail::SignEs256JwtWithPem`（与 `CreateProof` 同源、无 OpenSSL 抛 InternalError），测试 +5（结构断言 + 无 OpenSSL 抛错路径 + 未声明时回退 `client_secret_post`），docs 中英与 README 口径同步，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — Streamable HTTP 客户端逐请求头注入口 + DPoP 发送侧接线：新增 `HttpClientTransportOptions::apply_per_request_headers` / `observe_response_challenge` 两回调（六处发请求点全覆盖、Win32/POSIX 共用判定），401/400 `use_dpop_nonce` 首轮恰好重试一次并重算 proof（与 auth 挑战、HeaderMismatch 刷新共用单一预算），`OAuthClientProvider::ApplyDPoPProofHeader` / `ObserveDPoPNonce` 适配器落地（htu 剥 query、ath 绑定、逐请求新 jti/iat），测试 +6，dpop 页「未接线项」节删除，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — DPoP 能力文档化 + README 口径统一（纯文档）：新建 [/concepts/dpop.md](/concepts/dpop.md)（服务端 DPoP 校验链、OpenSSL 可选依赖缺失抛错语义、SEP-1932 逐请求 proof 与 nonce 重试缺口、client 发送侧接线进展），README 中英「DPoP 既不发送也不校验」与 ttlMs/cacheScope「待核实」两处矛盾表述按源码与 Ruby `main` 分支实测改写；`experimental-flags`/`tier1-parity`/`modules/http`/`concurrency` 四页的 `enable_listen_stream` 口径统一为「stateful 默认开、stateless 默认关、显式 true 仍可强制开」，tier1-parity 遗留 2/3/4（`maxToolInputElements`、`ttlMs`/`cacheScope` 消费、listen 默认值）按源码改判已实现，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — gap-and-ci-green 分支 wiki 收尾（纯文档）：tier1-parity 页增 A-T2 真实流（client 夹具 +1143 行 OAuth 真实流，modern 删 21 / legacy 删 10 转绿）+ B2 证伪转 B3 + CI 三修复（DPoP 虚构 API、inline 链接、解析器显式栈）+ 生产缺口记账 6 项，tests.md 口径 848/3606（C-T1 握手断言 +1）+ 四 leg 新数（server legacy 67/67、server modern 194+1、client legacy 179+4、client modern 383 + 保留 10 条）+ 基线现状，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — T11 文档收尾与遗留记账（纯文档）：新建 Tier1 技术对标声明页 [/concepts/tier1-parity.md](/concepts/tier1-parity.md)（TS v2.3.1/Python v2.3.0/C# v2.2.0/Go v1.8.0/Rust v3.5.1，仅技术对标、无官方 Tier 申报意向；WebSocket 非标超集；9 项遗留记账），tests.md 口径 848/3605（分布已回填） + 四 leg（server legacy 67/67、server modern 194+1、client legacy 11+43、client modern 66+68）+ 专项（request_state 5/5、`$ref` 1/1、schema 9/9），`http-custom-headers` 18/18 已删基线，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — T9 单连接语义与可观测锁定：生产零改动（stateless 单连接三元组 + 写完即关 + DELETE 405 + span 全路径配对均已具备），测试 +5（streamable-http +3 默认/405/独立连接、span-hook +2 成功与拒绝 End 回填 trace，待主代理实测），span-hooks 页增 OTel 差异节，详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-13 — T8 Apps 扩展与 Server Card 接线 + tasks 隐式移除：生产 5 文件（ext 双头增扩展标识常量、ServerOptions 增 `server_card`、McpServer 增 `GetServerCard`、McpServer.cpp 能力推导/apps 资源读门/任务四处去隐式），测试 6 处显式化 + 新增矩阵与显式 tasks 两用例（待主代理实测），详见 [/changelog/2026-10-13-log.md](/changelog/2026-10-13-log.md)

- 2026-10-12 — 前瞻六开关文档同步（ad547f9 后续批次）：`experimental-flags.md` 六开关去“保留未接线”标注（etag/渐进/文件/触发器/dpop 已实现默认关语义 + 构造期同步节，interceptor 为无门控别名；客户端增至 4 开关），docs 中英 extensions.md 同表镜像，tests.md 仅留用例数待回填占位；只改 6 个 md，详见 [/changelog/2026-10-12-log.md](/changelog/2026-10-12-log.md)

- 2026-10-11 — 配置分发与部署形态：详见 [/changelog/2026-10-11-log.md](/changelog/2026-10-11-log.md)
