# Call Tools and Read Resources

`tools/call` and `resources/read` are the two outbound paths a client uses most. They look alike but fail differently: a tool-level failure is an `isError` field inside a normal result, while only a protocol-level failure throws `McpError`. Signatures and field tables live only in [/en/reference/client](/en/reference/client) and [/en/reference/types](/en/reference/types); this page covers the procedure and how to read the outcomes.

## Prerequisites

- You already have a connected client and its negotiation snapshot: see [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server).
- Include paths must carry the subdirectory: `mcp/client/McpClient.hpp`, `mcp/client/ClientOptions.hpp`, `mcp/McpTypes.hpp`, `mcp/Content.hpp`, `mcp/JsonValue.hpp`, `mcp/McpError.hpp`.
- The server-side counterparts are [/en/howto/register-a-tool](/en/howto/register-a-tool) and [/en/howto/serve-a-resource](/en/howto/serve-a-resource); the code table is [/en/reference/errors](/en/reference/errors).
- Peer capabilities (`capabilities.tools` / `capabilities.resources`) are already fixed at connect time — `GetServerCapabilities()` is enough to check them.

## Step 1: List Tools — Pick One of Three Entry Points

<<< @/snippets/call-tools-and-read-resources.cpp#list-tools

| Entry point | Pagination | Cache and etag | Sends `query`／`pageSize` |
|---|---|---|---|
| `ListTools()` (no cursor) | merges pages internally, up to 64 | yes | no |
| `ListTools(cursor)` | single page, you continue via `next_cursor` | yes, keyed per cursor | no |
| `ListToolsAll()` | loops `ListTools(cursor)` | yes | no |
| `SearchTools(query, page_size)` | sends `tools/list` page by page directly | no | only when `enable_progressive_discovery` is set |

Calling `ListTools()` without a cursor already merges the pages (`src/client/McpClient.cpp:124-172`, `1215-1235`), which makes `ListToolsAll()` equivalent to it (`McpClientTest.ListToolsAutoPaginates` returns 3 tools, `McpClientTest.ListToolsAllSinglePageEqualsPlainList` compares both shapes). Pagination that never converges past 64 pages throws `ProtocolViolation` with the message `list pagination did not converge within 64 pages` (`src/client/McpClient.cpp:52`, `1237-1250`). The `Tool` fields (`name`／`title`／`description`／`input_schema`／`output_schema`／`annotations`／`icons`) are in [/en/reference/types](/en/reference/types).

## Step 2: Build `arguments` from `inputSchema`

<<< @/snippets/call-tools-and-read-resources.cpp#required-arguments

The client never validates inputs: `CallTool` only packs `name` and `arguments` into the request (`src/client/McpClient.cpp:1331-1333`). Validation happens on the server, and a missing required key or a type mismatch comes back as `isError` inside a **normal result**, with text prefixed `invalid arguments: ` (`src/server/McpServer.cpp:2616-2623`). A misspelled tool name is a different animal: a JSON-RPC error `InvalidParams` whose message is `tool not found: <name>` (`src/server/McpServer.cpp:2144-2151`).

## Step 3: Call the Tool, and Separate the Two Failure Channels

<<< @/snippets/call-tools-and-read-resources.cpp#call-tool

| Channel | Triggered by | What you write |
|---|---|---|
| `result.is_error == true` | arguments off-schema, or the tool handler throws (`McpError` keeps its message, any other exception yields fixed text) (`src/server/McpServer.cpp:2633-2650`) | read the text out of `content`; do not catch |
| `McpError` thrown | the peer returned a JSON-RPC error: unknown tool name, `structuredContent` not matching `outputSchema`, MRTR rounds or budget exhausted | `try`／`catch`; the MRTR path keeps `data` (`src/client/McpClient.cpp:1101-1108`) |

`isError` is just a result field, deserialized straight into `CallToolResult::is_error` (`src/core/McpTypesResults.cpp:199-215`). When the server finds `structuredContent` incompatible with the declared `outputSchema` it converts that into `InvalidParams` (`src/server/McpServer.cpp:2671-2681`), so on the client `CallTool` throws `McpError` (`McpClientTest.ToolOutputSchemaValidation`). The timeout does not come from `RequestOptions::timeout`: `CallTool` uses `input_required_config->round_timeout` (600s by default, `src/client/McpClient.cpp:1335-1337`), and on `HeaderMismatch` the client clears its cache, re-fetches `tools/list` and retries once verbatim (`:1343-1349`).

## Step 4: Read the Five Content Variants

<<< @/snippets/call-tools-and-read-resources.cpp#read-content

| Variant | `type` discriminator | Payload field | Notes |
|---|---|---|---|
| `TextContent` | `text` | `text` | `mime_type` optional |
| `ImageContent` | `image` | `data` (base64) | `mime_type` mandatory |
| `AudioContent` | `audio` | `data` (base64) | `mime_type` mandatory |
| `EmbeddedResource` | `resource` | `resource` (again discriminated by `text`／`blob`) | resource contents inlined |
| `ResourceLink` | `resource_link` | `uri`, `name` | a reference only — follow up with `resources/read` |

Dispatch is by the `type` string and an unknown value throws `DeserializeFailed` (`src/core/Content.cpp:282-291`); `ResourceContents` is discriminated by the presence of a `text` or `blob` field, and neither present throws `DeserializeFailed` too (`src/core/Content.cpp:120-126`). Pull values with `std::get_if<T>(&item)`. `structured_content` is a raw `JsonValue` the client never validates — use `Find(...)` or `Dump()` as needed.

## Step 5: List Resources and Read One

<<< @/snippets/call-tools-and-read-resources.cpp#list-resources
<<< @/snippets/call-tools-and-read-resources.cpp#read-resource

`ListResources`／`ListResourcesAll` mirror the tool side; template entries expose the `uri_template` field (`ResourceTemplate`). The client offers **no** template expansion: the RFC 6570 matching in `src/detail/UriTemplate.hpp` runs only on the server, so you must turn `docs://users/{userId}/profile` into a literal URI yourself before calling `ReadResource` (`include/mcp/McpTypes.hpp:74-82`; nothing in `src/client/` expands templates). Every `contents` entry carries its own `uri`; text arrives in `TextResourceContents::text` and binary in `BlobResourceContents::blob` — base64 as sent, which the SDK neither decodes nor validates.

## Step 6: Cache, Freshness and etag

<<< @/snippets/call-tools-and-read-resources.cpp#cache-modes

`ReadResource` is the only resource method that accepts `CacheableRequestOptions`; `ListTools`／`ListResources`／`ListPrompts` always use the default read path (`src/client/McpClient.cpp:1441-1468`, `1215-1235`):

| `cache_mode` | Reads cache | Writes cache | Use for |
|---|---|---|---|
| unset (`"use"`) | yes, bounded by `max_age_ms` | yes | ordinary reads |
| `"bypass"` | no | no | one absolutely fresh value |
| `"refresh"` | no | yes | contents changed: force a fetch and update the cache |

An entry is only stored when the server put `cacheHint.ttlMs > 0` in the response (`src/client/McpClient.cpp:1145-1163`), and the TTL is capped at 24 hours (`src/detail/ResponseCache.hpp:23`, `:42`). With `cacheScope` equal to `"private"` entries are partitioned by the value `cache_auth_context()` returns, and a change clears every private entry (`src/client/McpClient.cpp:1155-1174`, `McpClientTest.PrivateCachePartitionedByAuthContext`). etag reuse is additionally gated by a **process-wide** switch, which an `McpServer` in the same process overwrites from its own `ServerOptions::enable_etag_cache` (`src/core/McpTypesResults.cpp:50`, `src/server/McpServer.cpp:661`, `src/client/McpClient.cpp:1178`).

## Step 7: Subscribe to Resource Updates

<<< @/snippets/call-tools-and-read-resources.cpp#subscribe-updates

Each era has its own entry point. Legacy: `SubscribeResource(uri)` sends `resources/subscribe` with a fixed 60s timeout through `DoSendRequest`, so errors throw `McpError` with `data` dropped (`src/client/McpClient.cpp:1472-1479`). Modern: `SubscribeAsync(params)` sends `subscriptions/listen` and waits for the acknowledged frame carrying the same subscription id in `_meta` — if the response arrives first and is an error it is rethrown as-is, and if no frame arrives within 5000ms it throws `InternalError` (`src/client/McpClient.cpp:1927-1990`). Calling the wrong era's method gets rejected by the peer's era gate with `MethodNotFound` (`src/protocol/WireCodec.cpp:24-39`, `src/protocol/McpSessionHandler.cpp:414-417`).

Once an update arrives, the built-in handler invalidates the cache entry for exactly that URI (`src/client/McpClient.cpp:757-766`) and `*_list_changed` clears the whole table (`:751-756`) — provided you did not replace those handlers with your own `SetNotificationHandler()`, because that table holds one slot per method (`src/protocol/McpSessionHandler.cpp:1193-1196`).

## Progress callback

To see movement before a slow call returns, attach a handler to `RequestOptions::on_progress`; when you do not pass a `progressToken` explicitly the client allocates one, and the callback is detached when the request finishes.

<<< @/snippets/call-tools-and-read-resources.cpp#progress

## Complete code

The compilable file is `docs/snippets/call-tools-and-read-resources.cpp`, with regions `list-tools`, `read-content`, `required-arguments`, `call-tool`, `progress`, `list-resources`, `read-resource`, `cache-modes` and `subscribe-updates`. The `progress` region demonstrates `RequestOptions::on_progress`: a non-empty callback registers a progress token and detaches it when the call returns, and each progress frame re-extends that request's timeout (`src/client/McpClient.cpp:937-973`, `1323-1329`).

## Verification

Snippets are only syntax-checked (`/Zs` on the MSVC frontend). From the worktree root:

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-call-tools-and-read-resources
```

Observed: exit code 0, one output line `Building CXX object ...call-tools-and-read-resources.cpp.obj`, no `error:` lines. Behavioural verification:

```bash
ctest --preset debug -R 'McpClientTest\.(ListToolsAutoPaginates|ListToolsAllAggregatesCursorChain|ListToolsAllSinglePageEqualsPlainList|ListToolsCachesAndInvalidatesOnListChanged|ListToolsCachesCursorPagesSeparately|ReadResourceCachesPerUri|CacheClampsTtlTo24Hours|CachePrivateEntryHitsInSameConnection|PrivateCachePartitionedByAuthContext|CloseDropsPrivateCacheEntries|ResourceUpdatedInvalidatesOnlyThatUri|ToolOutputSchemaValidation|CallToolReceivesProgressNotifications|ListResourcesTemplatesPromptsAllAggregate)$'
```

Observed: `[==========] 14 tests from 1 test suite ran` followed by `[  PASSED  ] 14 tests` (run directly via `build/debug/tests/unit/mcp-client-tests.exe`; each gtest case maps to one registered ctest test). Key assertions:

| Case | Asserted value |
|---|---|
| `McpClientTest.ListToolsAutoPaginates` | `ListTools()` yields 3 tools (`tool-1`…`tool-3`), `ListTools("c1")` yields 1 |
| `McpClientTest.ListToolsCachesAndInvalidatesOnListChanged` | server hit counts 1 → 1 → 2 (a re-fetch only after `tools/list_changed`) |
| `McpClientTest.ToolOutputSchemaValidation` | `CallTool` throws `McpError` |
| `McpClientTest.CallToolReceivesProgressNotifications` | `progress == 0.5`, `*total == 1.0`, `*message == "half"`, integer token 1 |
| `McpClientTest.ReadResourceCachesPerUri` | two reads of one URI send only one request |

## Era differences

This table covers only the differences that matter to calling and reading; the full method and notification matrix is in [/en/learn/protocol-versions](/en/learn/protocol-versions).

| Aspect | Legacy (before 2026-07-28) | Modern (2026-07-28 and later) |
|---|---|---|
| `tools/list`, `resources/list`, `resources/templates/list`, `resources/read`, `tools/call` | available in both eras with the same shape | same methods, but responses must carry `resultType` and list results must say `complete` (`src/protocol/WireCodec.cpp:205-221`) |
| `cacheHint` shape | object `cacheHint{ttlMs,cacheScope}` | flattened to top-level `ttlMs`／`cacheScope` with `cacheHint` erased; the client reads both shapes (`src/protocol/WireCodec.cpp:255-269`, `src/client/McpClient.cpp:92-104`) |
| Resource subscription | `resources/subscribe`／`unsubscribe` | `subscriptions/listen` plus the acknowledged frame; the legacy methods are not in the method set (`src/protocol/WireCodec.cpp:24-39`) |
| Internal error codes | passed through unchanged | `RequestTimeout`／`ConnectionRefused`／`TlsHandshakeFailed` are rewritten to `InternalError` (`src/protocol/WireCodec.cpp:271-283`) |

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| A tool failed but my `catch` never ran | `isError` is a field of a normal result and never throws | check `result.is_error` first, then read the content |
| A bad argument produced `-32602`, so I blamed the server | unknown names raise JSON-RPC errors, bad arguments yield `isError` | branch on the prefix: `tool not found:` versus `invalid arguments:` |
| I assumed `ListTools()` returns only the first page | without a cursor it already merges every page | pass a cursor explicitly when you want manual paging |
| The server registered a new tool but the client never sees it | the stale table is still cached, or you replaced the built-in `tools/list_changed` handler | wait for the TTL, reconnect, or stop registering that method name |
| Repeated `ReadResource` keeps returning old content | the server supplied `cacheHint.ttlMs`, so the cache answers | pass `cache_mode = "refresh"` (or `"bypass"`) |
| etag never takes effect | a `ServerOptions::enable_etag_cache = false` in the same process flipped the global switch | keep both sides consistent, or run server and client in separate processes |
| `SubscribeResource` throws `-32601` on a modern session | `resources/subscribe` is a legacy-only method | use `SubscribeAsync`; see Step 7 |

## Next steps

- Progress, timeouts and cancellation: [/en/howto/report-progress](/en/howto/report-progress), [/en/howto/cancel-a-request](/en/howto/cancel-a-request)
- Task-based invocation: [/en/howto/run-long-tasks](/en/howto/run-long-tasks)
- When the peer needs more input: [/en/howto/collect-user-input](/en/howto/collect-user-input), [/en/howto/request-extra-input](/en/howto/request-extra-input)
- Full error code table: [/en/reference/errors](/en/reference/errors)
- Connection and session snapshot: [/en/howto/connect-to-a-server](/en/howto/connect-to-a-server)

## Source behind this page

- `include/mcp/client/McpClient.hpp`
- `include/mcp/McpTypes.hpp` (`Tool`, `Resource`, `ResourceTemplate`, `CallToolResult`, `ReadResourceResult`, `RequestOptions`, `CacheableRequestOptions`, `SubscriptionsListenRequestParams`)
- `include/mcp/Content.hpp` (the five `ContentVariant` members, the two `ResourceContents` members)
- `include/mcp/JsonValue.hpp`
- `src/client/McpClient.cpp` (list pagination and caching, MRTR calls, content deserialization entry points, subscriptions)
- `src/core/McpTypesResults.cpp` (`isError`／`structuredContent` serialization and restore)
- `src/core/Content.cpp` (`type` dispatch and `DeserializeFailed`)
- `src/detail/ResponseCache.hpp` (TTL cap, public and private scopes)
- `src/protocol/WireCodec.cpp` (per-era shape checks and error-code rewriting)
- `src/server/McpServer.cpp` (where `isError` and `outputSchema` validation actually happen)
- `docs/snippets/call-tools-and-read-resources.cpp`
