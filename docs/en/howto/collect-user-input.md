# Collect Input From the User

Asking the user for structured input (`form`) or sending them to an off-site page (`url`) during one tool call needs only `McpServer::Elicit` and `McpServer::ElicitUrl`; the hard part is session addressing and the fact that the two protocol eras behave differently. Signatures and field tables live in [/en/reference/server](/en/reference/server) and [/en/reference/types](/en/reference/types), the mechanism in [/en/learn/protocol-versions](/en/learn/protocol-versions) and [/en/howto/request-extra-input](/en/howto/request-extra-input).

## Prerequisites

- Headers (include path is only `-I include`, so always spell out the subdirectory): `mcp/server/McpServer.hpp`, `mcp/server/RequestContext.hpp`, `mcp/server/ServerOptions.hpp`, `mcp/client/McpClient.hpp`, `mcp/client/ClientOptions.hpp`, `mcp/McpTypes.hpp`, `mcp/McpError.hpp`, `mcp/JsonValue.hpp`, `mcp/Implementation.hpp`, `mcp/Capabilities.hpp`.
- Both entry points are declared at `include/mcp/server/McpServer.hpp:78-83`:

| Method | Signature | Waiting limit |
|---|---|---|
| `Elicit` | `std::future<ElicitResult> Elicit(const ElicitRequestParams&, std::string_view session_id = {})` | No timeout parameter: takes `ServerOptions::input_required_config->round_timeout`, otherwise the internal default of 600s (`src/server/McpServer.cpp:1173-1177`, `McpServer.cpp:48`) |
| `ElicitUrl` | `std::future<ElicitResult> ElicitUrl(const std::string& url, const std::string& message, std::chrono::seconds timeout = std::chrono::seconds(600), std::string_view session_id = {})` | Explicit `timeout`; the watchdog settles the future with `RequestTimeout` (`-32001`) (`src/server/McpServer.cpp:1246-1279`) |

- The client must be able to answer `elicitation/create`: both `ElicitationHandler` and `UrlElicitationHandler` return `ElicitResult` (`include/mcp/client/ClientOptions.hpp:26-30`) and must be placed in `ClientOptions` before `McpClient::Create` — the advertised capabilities are derived in the constructor by `DeriveClientCapabilities` (`src/client/McpClient.cpp:360-390`, `:620-622`).
- The tool handler must have access to `ctx.SessionId()` (`include/mcp/server/RequestContext.hpp:62`).

## Step 1: Pass the session id all the way down

Capabilities and the negotiated version reported by `initialize` reach the server's global members only when the session id is empty; otherwise they always land in the per-`mcp-session-id` session table (`src/server/McpServer.cpp:3194-3198`). `Elicit`/`ElicitUrl` resolve the session with `EffectiveSessionId(session_id)` (`McpServer.cpp:1060-1068`), and when `session_id` is omitted the fallback order is: explicit session → global capabilities already set → that one session if there is exactly one non-empty session → everything else falls back to global, where per-session capabilities cannot be read.

> [!warning]
> Omitting `session_id` under concurrent sessions of a stateful Streamable HTTP server throws `McpError(-32021 MissingRequiredClientCapability)` in the legacy era (`src/server/McpServer.cpp:1151-1167`). Inside tool handlers always write `ctx.Server().Elicit(params, ctx.SessionId())` — see the real in-repo usage at `examples/conformance/server/InteractionTools.cpp:111`.

## Step 2: Wire up the client handlers

<<< @/snippets/collect-user-input.cpp#client-handlers

The two handlers split by `mode`: `elicitation_handler` receives `form`, `url_elicitation_handler` receives `url`. You must decide the tri-state yourself — `ElicitResult.action` may only be `accept`/`decline`/`cancel`, anything else makes the client throw `HandlerError` (`src/client/McpClient.cpp:285-294`); in `url` mode a missing handler does not fail, the client synthesizes `action="decline"` (`:296-305`); in `form` mode a missing handler replies `MethodNotFound` (`:737-740`).

> [!tip]
> `SetLegacyUrlElicitationHandler` (`include/mcp/client/McpClient.hpp:119-121`) is deprecated: it hard-codes the callback result to `accept` and never yields `content`. New code uses the `ElicitResult`-returning `url_elicitation_handler` instead.

## Step 3: Collecting with form mode

<<< @/snippets/collect-user-input.cpp#form-schema
<<< @/snippets/collect-user-input.cpp#form-tool-handler

`requested_schema` is a plain JSON Schema object and each key under `properties` becomes a key of the final `content`; in `form` mode the client fills schema `default` values into `content`, but only when the action is `accept` and the client capability `elicitation.form.applyDefaults == true` (`src/client/McpClient.cpp:239-258`).

The handler skeleton is "read back first, then ask": in the modern era `Elicit` does not send a request at all but throws the internal signal `ElicitationInputRequired` (`src/server/McpServer.cpp:1142-1148`), which the three paths of `HandleCallTool` turn into an `input_required` result (`:2369`, `:2520`, `:2630`); after the client answers, **the same handler runs again from the top** and the answer sits in `ctx.Params().input_responses`. Code that treats `Elicit(...).get()` as "this always yields the answer" re-asks on every round until the client's `max_rounds` runs out.

<<< @/snippets/collect-user-input.cpp#read-back

The read-back logic is identical for both eras: in the legacy era `input_responses` is absent and the handler asks; from the second round of the modern era the read-back branch hits, and `input_responses["elicitation"]` holds the bare result `{action, content}` (`src/core/McpTypesParams.cpp:277-282`).

## Step 4: Collecting with url mode

<<< @/snippets/collect-user-input.cpp#url-tool-handler

`ElicitUrl(url, message, timeout, session_id)` only tells the user where to go; it returns no form content. Three details:

- In the legacy era the server auto-generates `elicitation_id` (`src/server/McpServer.cpp:1213-1220`) and sends it as `elicitationId`; in the modern era the field is never serialized at all (`src/core/McpTypesParams.cpp:158-170`).
- In the legacy era the server's future is settled by `notifications/elicitation/complete` with a fixed `action="accept"` and empty `content` (`src/server/McpServer.cpp:1464-1487`) — a `decline`/`cancel` returned by the client handler never reaches the server. The real tri-state is only available through modern MRTR (`src/client/McpClient.cpp:1024-1027`).
- Both a timeout (`-32001`) and a closed connection settle the future with an exception, the latter via `AbandonPendingUrlElicitation` (`src/server/McpServer.cpp:1293-1304`), so `.get()` must sit inside a `try`.

## Step 5: Register the tools and run

<<< @/snippets/collect-user-input.cpp#server-options
<<< @/snippets/collect-user-input.cpp#server-registration

On the server, `input_required_config` has exactly one point of use: it supplies the waiting limit for `Elicit` in the legacy era; `max_rounds` is the client's loop bound (`src/client/McpClient.cpp:1076-1134`) and the server never consumes it (`src/server/McpServer.cpp:1173-1177`).

<<< @/snippets/collect-user-input.cpp#end-to-end

## Full code

Server identity and options, registration of both tools, the two client handlers and one end-to-end call are all given region by region in the five steps above; the compilable file is `docs/snippets/collect-user-input.cpp`, with regions `form-schema`, `read-back`, `outcome`, `form-tool-handler`, `url-tool-handler`, `server-options`, `server-registration`, `client-handlers`, `end-to-end`.

## Verification

Snippets are syntax-checked only (`/Zs` on the MSVC frontend, `-fsyntax-only` elsewhere). From the worktree root:

```bash
cmake -S . -B build/p2c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2c --target mcp-doc-snippet-collect-user-input
```

Observed output (clang-cl frontend, Windows): configure ends with `[mcp-docs] syntax-checking 10 snippet(s) with '/Zs'`, and the build prints

```text
[0/2] Re-checking globbed directories...
[1/2] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-collect-user-input.dir\__\__\docs\snippets\collect-user-input.cpp.obj
```

Exit code 0 with no `error:` lines means pass; the `clang-cl: warning: argument unused during compilation: '-c'` line is inherent to combining `/Zs` with Ninja and has nothing to do with this page's code.

Behavioural checks need the test targets (`cmake --preset debug && cmake --build --preset debug`):

```bash
ctest --preset debug -R 'McpServerTest.(ElicitationMissingCapability32021|ModernElicitRoutesThroughMrtrInputRoundTrip|ElicitUrlTimesOutWhenNoCompleteArrives|LegacyElicitResolvesCapabilitiesForSingleStatefulSession)'
```

You should see all four cases `Passed` and a final `100% tests passed out of 4` (the `0 tests failed` clause only appears when something fails). They cover `-32021` gating, the modern route through MRTR, the URL watchdog timeout and the single-session implicit fallback (case names are greppable in `tests/unit/McpServerTests.cpp`).

## Era differences

The discriminator is the lexicographic comparison `>= "2026-07-28"` (`IsModernProtocolVersion`) and it applies per session.

| Aspect | Legacy (before 2026-07-28) | Modern (2026-07-28 and later) |
|---|---|---|
| Outgoing shape | Server sends an `elicitation/create` request directly and blocks on the response | No direct send: throws `ElicitationInputRequired`, `HandleCallTool` converts it into `resultType="input_required"`, the client fulfils it through MRTR |
| Capability gating | Missing `elicitation.form`/`elicitation.url` throws `-32021`, `data.requiredCapabilities.elicitation` names the missing one | No capability gating at all, session capabilities are not even read |
| `elicitationId` | Present; `ElicitUrl` generates `elicit-<n>` | Never serialized, never present |
| URL mode tri-state | Server always observes `accept` (no `content`); `decline`/`cancel` are lost | The client handler's action propagates verbatim into `input_responses` |
| Handler lifetime | Entered once, returns once, blocks in place | The same handler re-runs from the top each round and must read `input_responses` |
| Waiting limit | `input_required_config->round_timeout`, else 600s | Governed by the client's `round_timeout`/`max_rounds`; the server does not wait |

## Common failures

| Symptom | Cause | Fix |
|---|---|---|
| Legacy session replies `-32021` with `data.requiredCapabilities.elicitation` containing `form` | The client registered no `elicitation_handler`, so the capability was never derived, or `SetElicitationHandler()` was called after `Create` | Install handlers via `ClientOptions` — capabilities are frozen at construction (`src/client/McpClient.cpp:620-622`) |
| The same code works single-user but fails `-32021` under stateful HTTP concurrency | `session_id` omitted and the session count is not 1, so `EffectiveSessionId` falls back to global capabilities (never written) | Pass `ctx.SessionId()`, see Step 1 |
| The client keeps asking the same question in the modern era and finally reports `MRTR: exceeded max_rounds` | The handler calls `Elicit` without reading `input_responses` first, so it re-asks every round | Follow the read-back-then-ask skeleton in Step 3 |
| The second question in one call overwrites the first answer | Both `Elicit` and `ElicitUrl` use the fixed key `"elicitation"` (`src/server/McpServer.cpp:97-98`) | Build distinct keys with MRTR instead, see [/en/howto/request-extra-input](/en/howto/request-extra-input) |
| The error seems caught, yet the modern era never receives user input | Wrapping `.get()` in `catch (const std::exception&)` swallows the MRTR signal as an ordinary error | Catch only `McpError`; `examples/conformance/server/main.cpp:159-161` pins the stateful conformance server to the legacy era, so the broad catch in `InteractionTools.cpp` is valid there and must not be copied into general code |
| No `content` in `url` mode | That mode returns an action only, and in the legacy era the complete notification pins it to `accept` | Use `form` when you need values; a URL only means "go finish something off-site" |
| The server treats `ElicitUrl` as an unbounded wait | The watchdog settles with `-32001` once it expires (default 600s) | Pass a shorter `timeout` explicitly and handle `McpError` |

## Next steps

- Multiple inputs and state carried across rounds: [/en/howto/request-extra-input](/en/howto/request-extra-input)
- Argument validation and tool registration: [/en/howto/validate-tool-input](/en/howto/validate-tool-input), [/en/howto/register-a-tool](/en/howto/register-a-tool)
- Long-running work and progress: [/en/howto/run-long-tasks](/en/howto/run-long-tasks), [/en/howto/report-progress](/en/howto/report-progress)
- Full error-code table: [/en/reference/errors](/en/reference/errors)

## Source files behind this page

- `include/mcp/server/McpServer.hpp`
- `src/server/McpServer.cpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/client/ClientOptions.hpp`
- `include/mcp/client/McpClient.hpp`
- `src/client/McpClient.cpp`
- `src/core/McpTypesParams.cpp`
- `docs/snippets/collect-user-input.cpp`
