# Request Extra Input Across Rounds (MRTR)

When a tool discovers mid-flight that it still needs input, do not block the server and do not send `elicitation/create` directly: return an `input_required` result and let the client answer, then re-run the same handler with `inputResponses`. This page covers building the result, minting and verifying `requestState`, reading answers back, and how this differs from [/en/howto/collect-user-input](/en/howto/collect-user-input); field tables live in [/en/reference/types](/en/reference/types) and the mechanism in [/en/advanced/mrtr](/en/advanced/mrtr).

## Prerequisites

- Headers (full subdirectory paths): `mcp/server/McpServer.hpp`, `mcp/server/RequestContext.hpp`, `mcp/server/ServerOptions.hpp`, `mcp/server/RequestState.hpp`, `mcp/client/McpClient.hpp`, `mcp/client/ClientOptions.hpp`, `mcp/Methods.hpp`, `mcp/McpTypes.hpp`, `mcp/JsonValue.hpp`, `mcp/Implementation.hpp`.
- Result types in `include/mcp/McpTypes.hpp`: `InputRequest{method, params}` (`:245-248`), `InputRequests` (key → request, `:250`), `InputRequiredResult{input_requests, request_state}` (`:252-255`), `CallToolResult::input_required` (`:272`); the equivalent fields for resources and prompts are at `:296` and `:308`.
- Read-back fields on the request side: `CallToolRequestParams::input_responses` and `::request_state` (`:138-144`), reachable through `RequestContext::Params()`.
- Knobs: server `ServerOptions::request_state_key`/`request_state_ttl`/`request_state_verifier`/`input_required_config` (`include/mcp/server/ServerOptions.hpp:39-49`); client `ClientOptions::input_required_config` (present by default with `auto_fulfill=true`, `max_rounds=10`, `round_timeout=600s`, `include/mcp/client/ClientOptions.hpp:55-63`).

## Step 1: Choose the route before writing code

| Scenario | Choice | Reason |
|---|---|---|
| One input, done as soon as it arrives, server-side blocking acceptable | Direct `Elicit`/`ElicitUrl` | Shortest code, and the only shape available in the legacy era |
| Several inputs in one round | Hand-built MRTR with multiple keys | `Elicit`/`ElicitUrl` both write to the fixed key `"elicitation"` (`src/server/McpServer.cpp:97-98`), so the second overwrites the first |
| You must remember what was already answered, or which phase you are in | MRTR + `requestState` | The direct route has no carrier for state |
| Stateless deployment | MRTR only | `McpServer::IsMrtrSupported()` returns `false` when stateless (`src/server/McpServer.cpp:3351-3355`), and the modern direct call already becomes MRTR anyway |
| Waiting on the user may take long | MRTR | No server thread parks; the deadline is the client's `round_timeout`/`max_rounds` |

## Step 2: Build the `input_required` result

<<< @/snippets/request-extra-input.cpp#input-request
<<< @/snippets/request-extra-input.cpp#input-required-result

Keys in `input_requests` are yours to name and must stay stable across rounds (`src/client/McpClient.cpp:1021-1031` echoes the very same keys back), so use readable, deterministic names. On the wire `resultType` becomes `"input_required"` and `inputRequests` plus `requestState` are emitted alongside (`src/core/McpTypesResults.cpp:179-196`); `content`/`isError` are serialized regardless, so do not put meaningful payload in that branch.

`ReadResourceResult` and `GetPromptResult` carry the same `input_required` field, and `HandleReadResource`/`HandleGetPrompt` apply the identical signing logic (`src/server/McpServer.cpp:2829-2838`, `:2921-2930`).

## Step 3: Read `inputResponses` back on the next round

<<< @/snippets/request-extra-input.cpp#read-back

Values are **bare results**: elicitation is `{action, content}` (`src/core/McpTypesParams.cpp:277-282`), sampling `{role, content, model, stopReason}`, roots `{roots}` — never wrapped in a `resultType`/`meta` envelope. Because the handler re-runs from the top every round, "read back → decide → figure out what is still missing" must be the first thing in the function; side effects (writing files, charging money) either move to the final round or have to be idempotent.

## Step 4: Mint and verify `requestState`

<<< @/snippets/request-extra-input.cpp#state-payload

What the handler puts in `request_state` is only an **unsigned payload** (a JSON string): with `request_state_key` configured, the server decodes that payload, adds `iat`, then HMAC-SHA256-signs it before sending (`src/server/McpServer.cpp:2403-2410`, `src/server/McpServer.cpp:410-421`) — so "the handler signs it itself" is wrong, one server-side mint is enough. Inside the tool, read it back with `detail::DecodeRequestStatePayload` (`include/mcp/server/RequestState.hpp:83-98`), which recovers the payload whether or not it is signed.

<<< @/snippets/request-extra-input.cpp#server-signing
<<< @/snippets/request-extra-input.cpp#custom-verifier

`request_state_ttl` bounds the age of `iat`, with `0` meaning no expiry check (`include/mcp/server/RequestState.hpp:74-79`); supplying `request_state_verifier` replaces the built-in HMAC verification outright (`src/server/McpServer.cpp:686-695`). Verification happens before your handler is entered: a failure replies `InvalidParams` with `data.reason = "invalid_request_state"` (`src/protocol/McpSessionHandler.cpp:466-475`) and no tool side effect runs.

> [!danger]
> With neither `request_state_key` nor `request_state_verifier` configured, `requestState` is never verified — it is a free-form string coming back from the client. Any handler that derives phase, amounts or permissions from it must configure at least one of the two.

## Step 5: Register the tool, continue on the client

<<< @/snippets/request-extra-input.cpp#full-tool
<<< @/snippets/request-extra-input.cpp#tool-registration

<<< @/snippets/request-extra-input.cpp#client-auto

The default configuration suffices: with `auto_fulfill=true`, once the client sees `resultType=="input_required"` it fulfils each key through the matching handler and re-sends `tools/call` with `inputResponses` + `requestState`, up to `max_rounds` rounds (`src/client/McpClient.cpp:1113-1131`). If a matching handler is missing nothing is filled in and the intermediate `input_required` result is returned to the caller as-is (`:1000-1003` — that existence check only applies to `form` mode, since `url` mode synthesizes `decline`).

<<< @/snippets/request-extra-input.cpp#manual-round

In manual mode (`input_required_config = std::nullopt` or `auto_fulfill=false`) the result stops in `CallToolResult::input_required`, and **you cannot send the second round through `CallTool()`**: that overload only consumes `name`/`arguments`/`meta`/progress, and `RequestOptions::input_responses` and `::request_state` are never written into the request (`src/client/McpClient.cpp:1312-1356`). The working path is to assemble `CallToolRequestParams` yourself (with `input_responses`/`request_state`) and call `SendRequest(methods::kCallTool, SerializeCallToolRequestParams(params))` (`src/core/McpTypesParams.cpp:45-53`).

## Full code

`docs/snippets/request-extra-input.cpp` is the compilable file, with regions `form-schema`, `input-request`, `read-back`, `state-payload`, `input-required-result`, `full-tool`, `server-signing`, `custom-verifier`, `tool-registration`, `client-auto`, `manual-round`. The in-repo reference implementation on the same topic is `examples/conformance/server/MrtrTools.cpp:123-295` (one tool each for single key, multiple keys, signed state and multi-round).

## Verification

Snippets are syntax-checked only (`/Zs` on the MSVC frontend, `-fsyntax-only` elsewhere):

```bash
cmake -S . -B build/p2c -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2c --target mcp-doc-snippet-request-extra-input
```

Observed output (clang-cl frontend): configure ends with `[mcp-docs] syntax-checking 10 snippet(s) with '/Zs'`, the build prints `[1/2] Building CXX object tests\docs\CMakeFiles\mcp-doc-snippet-request-extra-input.dir\__\__\docs\snippets\request-extra-input.cpp.obj`, exit code 0 with no `error:` lines; `argument unused during compilation: '-c'` is inherent to combining `/Zs` with Ninja.

Behavioural checks need the test targets:

```bash
ctest --preset debug -R 'McpServerTest.(MrtrToolInputRequiredElicitationRoundTrip|MrtrMintedRequestStateRoundTrip|MrtrForgedRequestStateRejectedWithReason)|McpClientTest.MrtrDefaultWithoutHandlersReturnsInputRequired'
```

You should see all four cases `Passed` and a final `100% tests passed, 0 tests failed out of 4`: bare read-back, server-signed round trip, forged state rejected with `data.reason`, and untouched return when a handler is missing.

## Era differences

| Aspect | Legacy (before 2026-07-28) | Modern (2026-07-28 and later) |
|---|---|---|
| Hand-written `input_required` | Usable: serialization and read-back are not era-gated (`src/core/McpTypesResults.cpp:179-196`) | Usable, and the only shape that can obtain user input |
| `Elicit`/`ElicitUrl` | Sends `elicitation/create` directly, gated by client capabilities | No direct send: throws `ElicitationInputRequired`, converted into `input_required` under the fixed key `"elicitation"` |
| `elicitationId` | Present in URL-mode requests | Never serialized |
| Server waiting | Parks on the future with `round_timeout` (600s when unset) | Does not wait; rounds and deadlines belong to the client |
| State carrier | None (only in-call blocking) | `requestState`, with signing and lifetime controlled by server options |
| Retry closure | Not applicable | The retried `tools/call` carries both `inputResponses` and `requestState`, mirroring task resumption (`tasks/update` rejects both fields together, `src/server/McpServer.cpp:1586-1591`) |

## Common failures

| Symptom | Cause | Fix |
|---|---|---|
| The client asks the same question every round and ends with `MRTR: exceeded max_rounds` | The handler re-asks without reading `input_responses`/`request_state` first | Put the read-back first as in Step 3 and let state decide the current round |
| Next round replies `-32602` with `data.reason="invalid_request_state"` and the handler never runs | The client echoed a modified or expired `requestState` (`request_state_ttl`), or you rewrote the signed string inside the handler | Echo the server's string verbatim; widen `ttl` or install `request_state_verifier` if you need other rules |
| With a key configured the state field turns into `base64url.hex` and `Find("round")` returns nothing | The signed `requestState` is not bare JSON | Read it via `detail::DecodeRequestStatePayload`, never `JsonValue::Parse` |
| State can be forged whenever `request_state_key` is unset | The server neither signs nor verifies | See the danger block in Step 4: configure at least one of the two |
| With `auto_fulfill=false` the manual second round never carries answers | `CallTool()` does not pass `RequestOptions::input_responses`/`request_state` through | Use `SendRequest` + `SerializeCallToolRequestParams`, see Step 5 |
| The second input request overwrites the first answer | Several `InputRequest` entries share one key, or everything goes through `Elicit` (fixed key) | Give each input its own key; reserve `Elicit` for single-input calls |
| You expect `content` but get an empty array | The `input_required` branch still serializes `content`/`isError`, yet no final result exists this round | Fill only `input_required` in that branch and keep content for the last round |
| You wait for `-32021` in the modern era and add a fallback for it | The modern path performs no capability gating; missing capability shows up as nobody answering the round | Prefer `McpServer::IsMrtrSupported()` for pre-flight, mindful that it resolves the session globally or by the single session |

## Next steps

- The direct route for a single input: [/en/howto/collect-user-input](/en/howto/collect-user-input)
- Tool registration and argument validation: [/en/howto/register-a-tool](/en/howto/register-a-tool), [/en/howto/validate-tool-input](/en/howto/validate-tool-input)
- Long-running task flows and progress: [/en/howto/run-long-tasks](/en/howto/run-long-tasks), [/en/howto/report-progress](/en/howto/report-progress)
- Full error-code table: [/en/reference/errors](/en/reference/errors)

## Source files behind this page

- `include/mcp/McpTypes.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/server/RequestState.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `src/server/McpServer.cpp`
- `src/protocol/McpSessionHandler.cpp`
- `src/core/McpTypesParams.cpp`
- `src/core/McpTypesResults.cpp`
- `include/mcp/client/ClientOptions.hpp`
- `src/client/McpClient.cpp`
- `examples/conformance/server/MrtrTools.cpp`
- `docs/snippets/request-extra-input.cpp`
