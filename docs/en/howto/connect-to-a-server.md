# Connect to a Server

`McpClient::Create()` does everything in one call — attach a transport, negotiate the protocol version, start the message thread — so the session is ready when it returns. The tricky part is telling `IClientTransport` apart from `ITransport`, and knowing which packets each negotiation mode actually sends. Signatures and field tables live only in [/en/reference/client](/en/reference/client); this page covers the procedure.

## Prerequisites

- You can build and link `mcp-client`: see [/en/start/installation](/en/start/installation). For one complete in-process round trip, run `examples/SimpleClient` as shown in [/en/start/quickstart](/en/start/quickstart).
- Include paths must carry the subdirectory (the build only gets `-I include`): `mcp/client/McpClient.hpp`, `mcp/client/ClientOptions.hpp`, `mcp/Transport.hpp`, `mcp/transport/StdioClientTransport.hpp`, `mcp/transport/StreamableHttpClientTransport.hpp`, `mcp/transport/InMemoryTransport.hpp`, `mcp/Capabilities.hpp`, `mcp/Implementation.hpp`.
- Transport option fields are in [/en/reference/transports](/en/reference/transports), every `ClientOptions` field in [/en/reference/client](/en/reference/client); neither table is repeated here.
- Background: layering and threading in [/en/learn/architecture](/en/learn/architecture), transport choice in [/en/learn/transports](/en/learn/transports).

## Step 1: Pick a Transport and Call `Connect()` Yourself

| Transport | Constructor | What `Connect()` actually starts | Good for |
|---|---|---|---|
| stdio child process | `StdioClientTransport` | spawns the process and takes over its stdin/stdout | a local executable server |
| Streamable HTTP | `StreamableHttpClientTransport` | opens the HTTP session and, when enabled, a listen stream | a remote server |
| in-process | `InMemoryTransport::CreatePair()` | nothing to connect — take `.client` directly | unit tests and self-talk |

`McpClient::Create()` takes a `std::shared_ptr<ITransport>`, not an `IClientTransport` (`include/mcp/client/McpClient.hpp:34-36`, `include/mcp/Transport.hpp:68-73`). `Connect()` is where work happens: the stdio implementation starts the process and hands the pipes to the session object (`src/transport/StdioClientTransport.cpp:188-220`), the HTTP implementation builds the session and calls `Start()` (`src/http/StreamableHttpClientTransport.cpp:2282-2286`).

<<< @/snippets/connect-to-a-server.cpp#stdio-transport
<<< @/snippets/connect-to-a-server.cpp#http-transport
<<< @/snippets/connect-to-a-server.cpp#in-memory-pair

`Pid()` has no value before `Connect()` and a positive one afterwards (`TransportTest.StdioPidAvailable`); `stderr_path` and `on_stderr` are mutually exclusive — setting both makes `Connect()` throw `InvalidParams` right away (`src/transport/StdioClientTransport.cpp:189-192`).

## Step 2: Assemble `ClientOptions`

<<< @/snippets/connect-to-a-server.cpp#client-options

- To answer a server-initiated `elicitation/create`, the handler must be inside `ClientOptions` before `Create()`: the outgoing capability set is fixed in the constructor by `DeriveClientCapabilities` (`src/client/McpClient.cpp:618-622`), so calling `SetElicitationHandler()` afterwards cannot change the capabilities the peer already saw.
- `connect_mode` defaults to `Auto`; `initialization_timeout` (60s) only bounds the `initialize` handshake and `discover_probe_timeout` (5s) only bounds the `server/discover` probe (`include/mcp/client/ClientOptions.hpp:47-51`).
- `on_transport_close`／`on_transport_error` are wired only when the transport derives from `TransportBase`; otherwise the SDK logs a Warning and ignores them (`src/client/McpClient.cpp:643-669`). All three bundled transports qualify.
- `enable_etag_cache` defaults to **true** on the client (`include/mcp/client/ClientOptions.hpp:75`), the opposite of the server default.

## Step 3: `Create()` Negotiates, and Throws on Failure

<<< @/snippets/connect-to-a-server.cpp#open-client
<<< @/snippets/connect-to-a-server.cpp#negotiate-failure

`Create()` first constructs the object (which already runs `handler_->Start()` and the message thread), then negotiates synchronously (`src/client/McpClient.cpp:611-636`, `679-691`). If negotiation throws, the local `unique_ptr` is destroyed during unwinding, which closes the session and the transport — in the stdio case the freshly spawned child process is reaped too, so there is nothing to clean up by hand.

| Mode | Packets sent | Success looks like | Failure looks like |
|---|---|---|---|
| `Auto` | `server/discover` first, then a version chosen from it | `IsModernProtocol()` is true | probe failure is classified per transport: stdio-like falls back to `initialize`, network transports throw |
| `Legacy` | `initialize` + `notifications/initialized` | `IsModernProtocol()` is false | handshake timeout throws, bounded by `initialization_timeout` |
| `Pin` | **nothing at all**, the result is synthesised locally | `is_modern` is forced to true | cannot fail, and learns nothing about the peer |

Evidence: `include/mcp/client/ClientOptions.hpp:32-36`, `src/client/McpClient.cpp:411-473`, `475-590`; the fallback classification is detailed in [/en/learn/protocol-versions](/en/learn/protocol-versions).

## Step 4: Read the Negotiation Snapshot, and Do Not Treat `Pin` as the Cheap Option

<<< @/snippets/connect-to-a-server.cpp#negotiated-snapshot

`GetServerCapabilities()` and `GetServerInfo()` return references into the negotiation result and must not outlive the client; `GetInstructions()` yields a `std::optional<std::string>` by value and `GetNegotiatedProtocolVersion()` a view over the internal string (`src/client/McpClient.cpp:818-833`). Three consequences:

- In `Pin` mode the synthesised discover result carries an empty `capabilities` object and never assigns `server_info`: every capability is unset and `GetServerInfo().name` is an empty string (`src/client/McpClient.cpp:429-442`, `McpClientTest.InitialState`). If your code decides whether to send `tools/call` from the capability table, don't use `Pin`.
- `IsModernProtocol()` returns the flag written during negotiation, which `Pin` forces to true — even when you pinned `2025-11-25`. To test the real era, use `IsModernProtocolVersion(GetNegotiatedProtocolVersion())`, i.e. lexicographic `>= "2026-07-28"` (`src/client/McpClient.cpp:834-837`, `include/mcp/ProtocolVersion.hpp:21-23`).
- Of the five protocol versions only `2026-07-28` counts as modern; `Auto` normally lands on it, while `Legacy` returns whatever version the peer declared in `initialize` (`include/mcp/ProtocolVersion.hpp:13-19`).

## Step 5: Catch Notifications Sent by the Server

<<< @/snippets/connect-to-a-server.cpp#passive-handlers

`SetLoggingHandler()` is the dedicated entry point for `notifications/message`: registering it takes over that notification, and an exception from your handler only reaches the internal log without hurting the connection (`src/client/McpClient.cpp:920-935`). `SetNotificationHandler(method, handler)` registers any notification by method name, but it writes into **one assignment-per-method table** (`src/protocol/McpSessionHandler.cpp:1193-1196`): the constructor already installed built-in handlers for `tools/list_changed`, `resources/list_changed`, `prompts/list_changed`, `resources/updated` and `notifications/progress`, so registering the same method replaces the built-in behaviour — and cache invalidation disappears with it. Server-initiated requests and the subscription channel are covered in [/en/howto/handle-server-initiated-requests](/en/howto/handle-server-initiated-requests).

## Step 6: Closing, and Requests Still in Flight

<<< @/snippets/connect-to-a-server.cpp#shutdown

- `Close()` is idempotent: a second call is stopped by the `closed_` flag (`src/protocol/McpSessionHandler.cpp:238-241`).
- `Close()` joins the message, timeout and response threads, then calls `transport_->Close()` (`src/protocol/McpSessionHandler.cpp:243-280`). Closing a stdio session waits `shutdown_timeout` for the child and `terminate_process_tree` decides whether grandchildren go too (`TransportTest.StdioProcessTreeTerminated`); see [/en/howto/manage-subprocesses](/en/howto/manage-subprocesses).
- Requests still pending at close time are settled with `ConnectionClosed` (`-32000`, message `connection closed`) instead of blocking forever (`src/protocol/McpSessionHandler.cpp:267-278`).
- `Close()` clears only the private-scope response cache entries and resets the auth context; public entries survive (`src/client/McpClient.cpp:693-698`, `McpClientTest.CloseDropsPrivateCacheEntries`).
- The destructor calls `Close()` again and detaches the transport callbacks (`src/client/McpClient.cpp:638-641`), so a `unique_ptr` going out of scope disconnects.

::: warning
`on_transport_close`／`on_transport_error` and every notification handler run on the transport's IO thread. Calling `Close()` from such a callback joins the current thread and self-deadlocks. Dispatch the shutdown onto your own thread instead; for stuck-session triage see [/en/howto/debug-a-stuck-server](/en/howto/debug-a-stuck-server).
:::

## Complete code

The compilable file is `docs/snippets/connect-to-a-server.cpp`, with regions `stdio-transport`, `http-transport`, `in-memory-pair`, `client-options`, `open-client`, `negotiate-failure`, `negotiated-snapshot`, `passive-handlers` and `shutdown` — each referenced once across the six steps above.

## Verification

Snippets are only syntax-checked (`/Zs` on the MSVC frontend). From the worktree root:

```bash
cmake -S . -B build/p3a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p3a --target mcp-doc-snippet-connect-to-a-server
```

Observed output: configure ends with `[mcp-docs] syntax-checking 12 snippet(s) with '/Zs'`, the build prints `[1/2] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-connect-to-a-server.dir\__\__\docs\snippets\connect-to-a-server.cpp.obj`, exit code 0 and no `error:` lines.

Behavioural verification needs the test targets (`cmake --preset debug && cmake --build --preset debug`):

```bash
ctest --preset debug -R 'McpClientTest\.(ClientOptionsDefaults|InitialState|PinNegotiationUsesPinnedVersion|LegacyNegotiationUsesInitialize|AutoNegotiationDiscoversModern|AutoNegotiationFallsBackToInitialize|CreateAndClose|SetNotificationHandlerFiresAfterCreate)$|TransportTest\.(StdioPidAvailable|StdioStderrCaptured)$'
```

Observed: the `McpClientTest` group printed `[==========] 8 tests from 1 test suite ran` followed by `[  PASSED  ] 8 tests`, and the `TransportTest` group printed `[  PASSED  ] 2 tests` (run directly via `build/debug/tests/unit/mcp-client-tests.exe` and `mcp-transport-tests.exe`; each gtest case maps to one registered ctest test, 10 in total). Four key assertions:

| Case | Asserted value |
|---|---|
| `McpClientTest.ClientOptionsDefaults` | `connect_mode == Auto`, `client_info.name == "mcp-cpp-client"`, timeouts 60 and 5 |
| `McpClientTest.AutoNegotiationDiscoversModern` | `IsModernProtocol()` true, version `2026-07-28`, `GetServerInfo().name == "test-server"` |
| `McpClientTest.InitialState` | under `Pin`, `GetServerInfo().name` is an empty string |
| `TransportTest.StdioPidAvailable` | `Pid()` has no value before `Connect()`, a positive one after |

## Era differences

The test is lexicographic `>= "2026-07-28"`, applied per session.

| Aspect | Legacy (before 2026-07-28) | Modern (2026-07-28 and later) |
|---|---|---|
| Session bootstrap | `initialize` + `notifications/initialized` | `server/discover`, no `initialize` handshake |
| `_meta` required on requests | no | yes — a missing `_meta` is `InvalidRequest`; the SDK backfills it on the way out (`src/protocol/WireCodec.cpp:81-100`, `186-203`) |
| `resultType` required on responses | no | yes — list results must say `complete` (`src/protocol/WireCodec.cpp:205-221`) |
| Initialised precondition on the server | requests before `initialized` are rejected (`src/server/McpServer.cpp:587-592`) | the check is skipped |
| Subscription channel | `resources/subscribe` (`src/protocol/WireCodec.cpp:24-32`) | `subscriptions/listen` plus the acknowledged frame (`:34-39`) |

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| Passing the `StdioClientTransport` object to `Create()` does not compile | `Create()` only accepts `std::shared_ptr<ITransport>` | call `auto session = transport.Connect();` first, then pass `session` |
| Connecting to a remote HTTP server throws `-32001` or `-32000` without ever trying `initialize` | `Auto` does not fall back for network transports | set `ConnectMode::Legacy` when the peer is known to be old (`src/client/McpClient.cpp:475-590`) |
| `GetServerCapabilities()` comes back empty and your code concludes the server has no tools | `Pin` mode synthesises an empty capability table | use `Auto` or `Legacy` when you need real capabilities; `Pin` only forces a version |
| `IsModernProtocol()` is true yet `tools/list` still uses the old shape | `Pin` always sets `is_modern`, regardless of the pinned version | branch on `IsModernProtocolVersion(GetNegotiatedProtocolVersion())` |
| The server says the client lacks elicitation | `SetElicitationHandler()` was called after `Create()` | put the handler in `ClientOptions`; capabilities are declared at construction |
| `on_transport_close` never fires | a custom transport that does not derive from `TransportBase` | use a bundled transport, or signal from your own close path (`src/client/McpClient.cpp:643-649`) |
| The process hangs in `Close()` on exit | `Close()` called from an IO-thread callback, joining itself | move the shutdown onto another thread; see the warning in Step 6 |

## Next steps

- Getting data after the connection: [/en/howto/call-tools-and-read-resources](/en/howto/call-tools-and-read-resources)
- Answering server-initiated requests: [/en/howto/handle-server-initiated-requests](/en/howto/handle-server-initiated-requests)
- Authenticated connections: [/en/howto/add-oauth-to-a-client](/en/howto/add-oauth-to-a-client)
- Operating the child-process transport: [/en/howto/manage-subprocesses](/en/howto/manage-subprocesses)
- Cancelling a request in flight: [/en/howto/cancel-a-request](/en/howto/cancel-a-request)
- Full facade signature and field tables: [/en/reference/client](/en/reference/client)

## Source behind this page

- `include/mcp/client/McpClient.hpp`
- `include/mcp/client/ClientOptions.hpp`
- `include/mcp/Transport.hpp`
- `include/mcp/Capabilities.hpp`
- `include/mcp/ProtocolVersion.hpp`
- `include/mcp/transport/StdioClientTransport.hpp`
- `include/mcp/transport/StreamableHttpClientTransport.hpp`
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/client/McpClient.cpp` (construction, three negotiation modes, snapshot readers, shutdown)
- `src/protocol/McpSessionHandler.cpp` (`Start`/`Close`, notification handler table, in-flight settlement)
- `src/protocol/WireCodec.cpp` (per-era wire validation)
- `src/transport/StdioClientTransport.cpp`, `src/http/StreamableHttpClientTransport.cpp`
- `docs/snippets/connect-to-a-server.cpp`
