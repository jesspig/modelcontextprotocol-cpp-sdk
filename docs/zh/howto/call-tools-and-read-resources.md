# 调用工具与读取资源

`tools/call` 与 `resources/read` 是客户端最常用的两条出站路径，形状相似但失败语义完全不同：工具层失败是一个正常响应里的 `isError` 字段，协议层失败才抛 `McpError`。签名与字段表只在 [/zh/reference/client](/zh/reference/client) 与 [/zh/reference/types](/zh/reference/types) 维护，本篇只讲操作与判读。

## 前置条件

- 已经连上一个服务端并拿到协商快照，见 [/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)。
- 头文件按子目录全路径写：`mcp/client/McpClient.hpp`、`mcp/client/ClientOptions.hpp`、`mcp/McpTypes.hpp`、`mcp/Content.hpp`、`mcp/JsonValue.hpp`、`mcp/McpError.hpp`。
- 服务端侧的对应注册入口见 [/zh/howto/register-a-tool](/zh/howto/register-a-tool) 与 [/zh/howto/serve-a-resource](/zh/howto/serve-a-resource)；错误码全表见 [/zh/reference/errors](/zh/reference/errors)。
- 工具与资源的对端能力（`capabilities.tools`／`capabilities.resources`）在连接期就已确定，用 `GetServerCapabilities()` 判空即可。

## 第一步：列工具——三个入口选一个

<<< @/snippets/call-tools-and-read-resources.cpp#list-tools

| 入口 | 翻页 | 走缓存与 etag | 下发 `query`／`pageSize` |
|---|---|---|---|
| `ListTools()`（不给 cursor） | 内部自动翻到空，最多 64 页 | 是 | 否 |
| `ListTools(cursor)` | 单页，`next_cursor` 由你自己续 | 是，按 cursor 分键 | 否 |
| `ListToolsAll()` | 逐页调 `ListTools(cursor)` | 是 | 否 |
| `SearchTools(query, page_size)` | 逐页直发 `tools/list` | 否 | 仅 `enable_progressive_discovery` 为真时 |

`ListTools()` 不传 cursor 时已经把分页合并完（`src/client/McpClient.cpp:124-172`、`1215-1235`），所以 `ListToolsAll()` 与它等价（`McpClientTest.ListToolsAutoPaginates` 取到 3 个工具、`ListToolsAllSinglePageEqualsPlainList` 两者同结果）。超过 64 页仍不收敛抛 `ProtocolViolation`，消息 `list pagination did not converge within 64 pages`（`src/client/McpClient.cpp:52`、`1237-1250`）。`Tool` 的字段（`name`／`title`／`description`／`input_schema`／`output_schema`／`annotations`／`icons`）见 [/zh/reference/types](/zh/reference/types)。

## 第二步：按 `inputSchema` 组装 arguments

<<< @/snippets/call-tools-and-read-resources.cpp#required-arguments

客户端不校验入参：`CallTool` 只把 `name` 与 `arguments` 原样拼进请求（`src/client/McpClient.cpp:1331-1333`）。校验发生在服务端，缺必填或类型不符回的是**正常结果**里的 `isError`，文本以 `invalid arguments: ` 开头（`src/server/McpServer.cpp:2616-2623`）。名字拼错则完全不同——那是 JSON-RPC 错误 `InvalidParams`，消息 `tool not found: <name>`（`src/server/McpServer.cpp:2144-2151`）。

## 第三步：调用工具，并分清两条失败通道

<<< @/snippets/call-tools-and-read-resources.cpp#call-tool

| 通道 | 触发 | 你要写的代码 |
|---|---|---|
| `result.is_error == true` | 参数不合 schema、工具 handler 抛异常（`McpError` 回原文、其它异常回固定文案）（`src/server/McpServer.cpp:2633-2650`） | 读 `content` 里的文本，别 catch 异常 |
| 抛 `McpError` | 对端返回 JSON-RPC 错误：工具名不存在、`outputSchema` 与 `structuredContent` 不匹配、MRTR 轮数或预算耗尽 | `try`／`catch`；MRTR 路径保留 `data`（`src/client/McpClient.cpp:1101-1108`） |

`isError` 只是结果字段，反序列化时原样落到 `CallToolResult::is_error`（`src/core/McpTypesResults.cpp:199-215`）。服务端把 `structuredContent` 与工具声明的 `outputSchema` 对不上时直接转成 `InvalidParams`（`src/server/McpServer.cpp:2671-2681`），客户端侧表现为 `CallTool` 抛 `McpError`（`McpClientTest.ToolOutputSchemaValidation`）。超时不看 `RequestOptions::timeout`，`CallTool` 用 `input_required_config->round_timeout`（默认 600s，`src/client/McpClient.cpp:1335-1337`）；收到 `HeaderMismatch` 时清缓存、重取一次 `tools/list` 再原样重试（`:1343-1349`）。

## 第四步：读五种内容变体

<<< @/snippets/call-tools-and-read-resources.cpp#read-content

| 变体 | 判别 `type` | 内容字段 | 备注 |
|---|---|---|---|
| `TextContent` | `text` | `text` | 可选 `mime_type` |
| `ImageContent` | `image` | `data`（base64） | `mime_type` 必填 |
| `AudioContent` | `audio` | `data`（base64） | `mime_type` 必填 |
| `EmbeddedResource` | `resource` | `resource`（再按 `text`／`blob` 判别） | 内联的资源内容 |
| `ResourceLink` | `resource_link` | `uri`、`name` | 只是引用，要再发 `resources/read` |

分派靠 `type` 字符串，认不出即抛 `DeserializeFailed`（`src/core/Content.cpp:282-291`）；`ResourceContents` 靠 `text`／`blob` 字段是否存在判别，两者都缺同样抛 `DeserializeFailed`（`src/core/Content.cpp:120-126`）。用 `std::get_if<T>(&item)` 取，`structured_content` 是裸 `JsonValue`，客户端不做任何校验，按需 `Find(...)` 或 `Dump()`。

## 第五步：列资源与读资源

<<< @/snippets/call-tools-and-read-resources.cpp#list-resources
<<< @/snippets/call-tools-and-read-resources.cpp#read-resource

`ListResources`／`ListResourcesAll` 与工具侧同构；模板列表条目给的是 `uri_template` 字段（`ResourceTemplate`）。客户端**没有**模板展开：`src/detail/UriTemplate.hpp` 的 RFC 6570 匹配只在服务端跑，你得自己把 `docs://users/{userId}/profile` 拼成字面 URI 再 `ReadResource`（`include/mcp/McpTypes.hpp:74-82`、`src/client/` 内无任何模板展开调用）。读取结果的每个 `contents` 条目都带自己的 `uri`，文本走 `TextResourceContents::text`，二进制走 `BlobResourceContents::blob`——base64 原样给你，SDK 既不解码也不校验。

## 第六步：缓存、新鲜度与 etag

<<< @/snippets/call-tools-and-read-resources.cpp#cache-modes

`ReadResource` 是唯一吃 `CacheableRequestOptions` 的资源方法，`ListTools`／`ListResources`／`ListPrompts` 只能走默认读取（`src/client/McpClient.cpp:1441-1468`、`1215-1235`）：

| `cache_mode` | 读缓存 | 写缓存 | 用途 |
|---|---|---|---|
| 缺省（`"use"`） | 读，受 `max_age_ms` 约束 | 写 | 常规读 |
| `"bypass"` | 不读 | 不写 | 拿一次绝对真值 |
| `"refresh"` | 不读 | 写 | 内容已变，强制回源并更新缓存 |

只有服务端在响应里给了 `cacheHint.ttlMs > 0` 才真的写入缓存（`src/client/McpClient.cpp:1145-1163`），TTL 上限 24 小时（`src/detail/ResponseCache.hpp:23`、`:42`）；`cacheScope` 为 `"private"` 时条目按 `cache_auth_context` 的返回值分区，上下文一变就清空全部私有条目（`src/client/McpClient.cpp:1155-1174`、`McpClientTest.PrivateCachePartitionedByAuthContext`）。etag 复用还额外受一个**进程级**开关约束，而同进程里 `McpServer` 会按自己的 `ServerOptions::enable_etag_cache` 改写它（`src/core/McpTypesResults.cpp:50`、`src/server/McpServer.cpp:661`、`src/client/McpClient.cpp:1178`）。

## 第七步：订阅资源更新

<<< @/snippets/call-tools-and-read-resources.cpp#subscribe-updates

两个时代的入口不同：legacy 用 `SubscribeResource(uri)` 发 `resources/subscribe`（60s 超时，走 `DoSendRequest`，错误抛 `McpError` 且丢 `data`，`src/client/McpClient.cpp:1472-1479`）；modern 用 `SubscribeAsync(params)` 发 `subscriptions/listen`，并等 `_meta` 里带同一订阅 id 的 acknowledged 帧——响应先到且是错误就原样抛，5000ms 内等不到帧抛 `InternalError`（`src/client/McpClient.cpp:1927-1990`）。跨时代调用会被对端的 era 校验挡下并回 `MethodNotFound`（`src/protocol/WireCodec.cpp:24-39`、`src/protocol/McpSessionHandler.cpp:414-417`）。

更新到达后，内置处理器会把 `resources/updated` 里那个 URI 的缓存条目失效（`src/client/McpClient.cpp:757-766`），`*_list_changed` 则清全表（`:751-756`）。前提是你没有用同名 `SetNotificationHandler()` 顶掉它们——那张表每个方法只有一个槽位（`src/protocol/McpSessionHandler.cpp:1193-1196`）。

## 进度回调

慢调用想在返回前看到动静，就在 `RequestOptions::on_progress` 上挂回调；不显式给 `progressToken` 时客户端会自动分配一个，回调在请求结束时摘除。

<<< @/snippets/call-tools-and-read-resources.cpp#progress

## 完整代码

可编译的完整文件是 `docs/snippets/call-tools-and-read-resources.cpp`，区域名依次为 `list-tools`、`read-content`、`required-arguments`、`call-tool`、`progress`、`list-resources`、`read-resource`、`cache-modes`、`subscribe-updates`；`progress` 区域演示 `RequestOptions::on_progress`（非空即自动登记进度 token 并在返回时摘除，同时续期该请求的超时，`src/client/McpClient.cpp:937-973`、`1323-1329`）。

## 验证

片段只做语法校验（MSVC 前端 `/Zs`），在工作树根执行：

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-call-tools-and-read-resources
```

实测返回码 0，仅输出一行 `Building CXX object ...call-tools-and-read-resources.cpp.obj`，无 `error:` 行。行为级验证：

```bash
ctest --preset debug -R 'McpClientTest\.(ListToolsAutoPaginates|ListToolsAllAggregatesCursorChain|ListToolsAllSinglePageEqualsPlainList|ListToolsCachesAndInvalidatesOnListChanged|ListToolsCachesCursorPagesSeparately|ReadResourceCachesPerUri|CacheClampsTtlTo24Hours|CachePrivateEntryHitsInSameConnection|PrivateCachePartitionedByAuthContext|CloseDropsPrivateCacheEntries|ResourceUpdatedInvalidatesOnlyThatUri|ToolOutputSchemaValidation|CallToolReceivesProgressNotifications|ListResourcesTemplatesPromptsAllAggregate)$'
```

实测：`[==========] 14 tests from 1 test suite ran` 后跟 `[  PASSED  ] 14 tests`（直接跑 `build/debug/tests/unit/mcp-client-tests.exe`，每条 gtest 用例对应一个 ctest 注册项）。关键断言：

| 用例 | 断言值 |
|---|---|
| `McpClientTest.ListToolsAutoPaginates` | `ListTools()` 得 3 个工具（`tool-1`…`tool-3`），`ListTools("c1")` 只得 1 个 |
| `McpClientTest.ListToolsCachesAndInvalidatesOnListChanged` | 服务端命中次数 1 → 1 → 2（收到 `tools/list_changed` 后才回源） |
| `McpClientTest.ToolOutputSchemaValidation` | `CallTool` 抛 `McpError` |
| `McpClientTest.CallToolReceivesProgressNotifications` | `progress == 0.5`、`*total == 1.0`、`*message == "half"`、token 为整数 1 |
| `McpClientTest.ReadResourceCachesPerUri` | 同一 URI 两次读取只发一次请求 |

## 时代差异

本表只列与调用、读取相关的差异；方法与通知的完整双时代矩阵见 [/zh/learn/protocol-versions](/zh/learn/protocol-versions)。

| 维度 | legacy（早于 2026-07-28） | modern（2026-07-28 及以后） |
|---|---|---|
| `tools/list`、`resources/list`、`resources/templates/list`、`resources/read`、`tools/call` | 双时代同名同形 | 同左，但响应必须带 `resultType`，列表类必须为 `complete`（`src/protocol/WireCodec.cpp:205-221`） |
| `cacheHint` 形状 | 对象 `cacheHint{ttlMs,cacheScope}` | 编码时摊平成顶层 `ttlMs`／`cacheScope` 并删掉 `cacheHint`；客户端两种形状都读（`src/protocol/WireCodec.cpp:255-269`、`src/client/McpClient.cpp:92-104`） |
| 资源订阅 | `resources/subscribe`／`unsubscribe` | `subscriptions/listen` + acknowledged 帧，旧方法不在方法集（`src/protocol/WireCodec.cpp:24-39`） |
| 内部错误码 | 原样透出 | `RequestTimeout`／`ConnectionRefused`／`TlsHandshakeFailed` 被改写为 `InternalError`（`src/protocol/WireCodec.cpp:271-283`） |

## 常见错误

| 症状 | 成因 | 处置 |
|---|---|---|
| 工具失败却没进 `catch` | `isError` 是正常结果的字段，不抛异常 | 先看 `result.is_error`，再按内容判读 |
| 参数写错拿到 `-32602`，以为服务端坏了 | 名字不存在才走 JSON-RPC 错误，参数错走 `isError` | 按消息前缀区分：`tool not found:` 对 `invalid arguments:` |
| 以为 `ListTools()` 只返回第一页 | 不给 cursor 时已自动翻页聚合 | 要手工分页就显式传 cursor |
| 服务端注册了新工具但客户端看不到 | 缓存里还留着旧表，或你顶掉了内置的 `tools/list_changed` 处理器 | 等 TTL 过期、重连，或别用同名 `SetNotificationHandler` |
| 反复 `ReadResource` 读到旧内容 | 服务端给了 `cacheHint.ttlMs`，命中缓存不回源 | 传 `cache_mode = "refresh"`（或 `"bypass"`） |
| etag 从不生效 | 同进程的 `ServerOptions::enable_etag_cache = false` 改写了进程级开关 | 两侧一致，或把服务端与客户端分进程 |
| 现代会话里 `SubscribeResource` 抛 `-32601` | `resources/subscribe` 是 legacy 专属方法 | 改用 `SubscribeAsync`；见第七步 |

## 下一步

- 进度、超时与取消：[/zh/howto/report-progress](/zh/howto/report-progress)、[/zh/howto/cancel-a-request](/zh/howto/cancel-a-request)
- 任务化调用：[/zh/howto/run-long-tasks](/zh/howto/run-long-tasks)
- 需要用户补输入：[/zh/howto/collect-user-input](/zh/howto/collect-user-input)、[/zh/howto/request-extra-input](/zh/howto/request-extra-input)
- 错误判读全表：[/zh/reference/errors](/zh/reference/errors)
- 连接与会话快照：[/zh/howto/connect-to-a-server](/zh/howto/connect-to-a-server)

## 本页对应源码

- `include/mcp/client/McpClient.hpp`
- `include/mcp/McpTypes.hpp`（`Tool`、`Resource`、`ResourceTemplate`、`CallToolResult`、`ReadResourceResult`、`RequestOptions`、`CacheableRequestOptions`、`SubscriptionsListenRequestParams`）
- `include/mcp/Content.hpp`（`ContentVariant` 五变体、`ResourceContents` 两变体）
- `include/mcp/JsonValue.hpp`
- `src/client/McpClient.cpp`（列表翻页与缓存、MRTR 调用、内容反序列化入口、订阅）
- `src/core/McpTypesResults.cpp`（`isError`／`structuredContent` 的序列化与还原）
- `src/core/Content.cpp`（`type` 分派与 `DeserializeFailed`）
- `src/detail/ResponseCache.hpp`（TTL 上限、公私有作用域）
- `src/protocol/WireCodec.cpp`（按时代的形状校验与错误码改写）
- `src/server/McpServer.cpp`（`isError` 与 `outputSchema` 校验的实际发生地）
- `docs/snippets/call-tools-and-read-resources.cpp`
