# Validate Tool Input

Put three layers of defence around a tool: let the SDK reject bad arguments against `input_schema` before your handler ever runs, keep semantic and type self-checks inside the handler, and finally cap the total payload with `ServerOptions::max_tool_input_elements`. The result: clients get a readable `is_error` the moment arguments are wrong, and your business code only ever sees well-shaped input. Signatures and field tables stay in [/en/reference/types](/en/reference/types) and [/en/reference/server](/en/reference/server).

## Prerequisites

- Headers: `mcp/server/McpServer.hpp`, `mcp/server/RequestContext.hpp`, `mcp/server/ServerOptions.hpp`, `mcp/McpTypes.hpp`, `mcp/JsonValue.hpp`, `mcp/Content.hpp`, `mcp/McpError.hpp`, `mcp/transport/InMemoryTransport.hpp` (or your production transport).
- Registration shape: either the `McpServer::RegisterTool` lambda overload or a `McpServerTool` object — validation happens SDK-side and is independent of the shape.
- Protocol era: identical across all five versions; the task-backed path validates first as well.
- The validator (`src/detail/JsonSchemaValidator.hpp`) is an internal implementation, not public API: constrain input only through `input_schema` and never `#include` it.

## Layer one: let the SDK reject arguments against input_schema

`HandleCallTool` calls `ValidateToolCallArguments` with `Tool::input_schema` before handing the request to your handler (`McpServer.cpp:553-561`, `2616-2623`). On failure the handler is **not called**; the server returns an `is_error` result whose text is `invalid arguments: <reason>`. A missing `arguments` participates as an empty object, so anything listed in `required` must actually be sent; and because the synthesized default schema carries `type: "object"`, non-object input is rejected too.

<<< @/snippets/validate-tool-input.cpp#builtin-validation

::: tip
Constraints such as `enum`, `minimum`, `maxLength` and `additionalProperties: false` are enforced by the SDK for free. Writing them into `description` and hoping the model complies is an anti-pattern.
:::

## Layer two: type defence inside the handler

A schema constrains shape, not meaning: unknown accounts, cross-field relationships, and enumerated values that are currently unavailable all need handler-side checks. `CallToolRequestParams::arguments` is a `std::optional<JsonValue>`, so test it before `Find`; and read numbers by checking `IsDouble()` versus `IsInt()` separately. `examples/WeatherServer/main.cpp:30-92` shows exactly this pattern.

<<< @/snippets/validate-tool-input.cpp#handler-defense

::: warning
Every `JsonValue` accessor throws (`McpError(McpErrorCode::DeserializeFailed, "JsonValue::GetString: expected string, got number")`), and the const `operator[]` throws `JsonValue: key not found: '<key>'` for a missing member — nothing is silently defaulted. `Find()` returns `const JsonValue*` and is `nullptr` on a miss, so the null check must come first.
:::

## Layer three: cap the number of argument elements

`ServerOptions::max_tool_input_elements` counts **one level deep**: object keys, array length, and 1 for a scalar (`McpServer.cpp:563-568`). Exceeding it returns `is_error` + `tool arguments exceed maximum input element count` (`2323-2328`), again without entering the handler.

<<< @/snippets/validate-tool-input.cpp#element-limit

Cost and limits: it is not a depth cap and not a byte cap, so a deeply nested payload passes; it only saves one traversal after parsing. Never treat it as a schema substitute — use both.

## The JSON Schema subset the validator supports

| Category | Supported keywords | Notes |
|---|---|---|
| Type and constants | `type`, `const`, `enum` | `type` accepts a string or an array; `integer` also accepts numbers with zero fractional part |
| Logical composition | `allOf`, `anyOf`, `oneOf`, `not`, `if`/`then`/`else` | `oneOf` requires exactly one match |
| Objects | `required`, `minProperties`, `maxProperties`, `properties`, `patternProperties`, `additionalProperties`, `propertyNames`, `dependentRequired`, `dependentSchemas`, `dependencies` | Errors carry the property path, e.g. `property 'city': type mismatch` |
| Arrays | `items` (single schema, or the array form with `additionalItems`), `prefixItems`, `contains` with `minContains`/`maxContains`, `uniqueItems`, `minItems`, `maxItems` | Errors look like `item 0: ...` |
| Strings | `minLength`, `maxLength`, `pattern` | Length counts UTF-8 code points; `pattern` uses `std::regex` in ECMAScript grammar and is an **unanchored search** |
| Numbers | `minimum`, `maximum`, `exclusiveMinimum`, `exclusiveMaximum`, `multipleOf` | The draft-04 boolean form of `exclusiveMinimum` is honoured |
| References | `$ref`, `$dynamicRef`, `$recursiveRef` | Only local `#` references, resolved via JSON Pointer or `$anchor`; `$defs` is just the container a pointer lands on |

Unrecognised keywords are **silently ignored** (fail-open): `format`, `contentEncoding`/`contentMediaType`, `unevaluatedProperties`/`unevaluatedItems` and `deprecated` take no part in the decision. Boolean schemas are legal: `true` accepts everything, `false` reports `boolean schema false`. Two hard limits: recursion caps at depth 32 (`kMaxJsonSchemaDepth`) and reports `schema depth exceeded`; a reference that does not start with `#` fails outright with `remote $ref not dereferenced '<uri>'`. For recursive reuse, write a local reference:

<<< @/snippets/validate-tool-input.cpp#local-ref

::: danger
Three traps: `pattern` is unanchored, so `^[a-z]+` lets trailing garbage through — write the full `^...$`; when a regex fails to compile the value is simply accepted, so never lean on a pattern for safety; a circular `$ref` throws `McpError(InvalidParams)` instead of returning `is_error`, and no response is ever emitted, leaving the client to time out (`src/detail/JsonSchemaValidator.hpp:205-209`).
:::

## Telling is_error apart from a protocol error

| Trigger | Wire shape | What the client sees |
|---|---|---|
| Built-in schema validation | Tool result with `is_error` | `invalid arguments: <reason>` |
| Element count above `max_tool_input_elements` | Tool result with `is_error` | `tool arguments exceed maximum input element count` |
| Handler returning failure | Tool result with `is_error` | The `content` text you wrote |
| Handler throwing `McpError` | Tool result with `is_error` | Only `ErrorMessage()`; the code and `data` are dropped (`McpServer.cpp:2633-2636`) |
| Handler throwing anything else | Tool result with `is_error` | Generic `Error executing tool <name>`; the original `what()` only reaches internal logs (`2637-2650`) |
| Tool name not registered | JSON-RPC protocol error `InvalidParams` | `tool not found: <name>` |
| Handler exception on the task-backed path | Task failure, not `is_error` | `McpError` keeps code/message/data, other exceptions become `InternalError` (`2524-2552`) |

<<< @/snippets/validate-tool-input.cpp#failure-modes

::: tip
Expected business failures belong in `is_error` (the model reads the text and can retry); only malformed inbound requests or genuine programming errors should throw. Never expect an error code to reach the peer.
:::

## Complete code

Schema constraints plus handler self-checks plus the element-count cap, combined:

<<< @/snippets/validate-tool-input.cpp#full

## Verification

```bash
cmake -S . -B build/p2a -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DMCP_BUILD_TESTS=OFF -DMCP_BUILD_DOCS_SNIPPETS=ON
cmake --build build/p2a --target mcp-doc-snippet-validate-tool-input
```

Expected output: configure prints `[mcp-docs] syntax-checking <N> snippet(s) with '/Zs'` (clang-cl) or `'-fsyntax-only'` (Clang/GCC), the build stops at `[N/N] Building CXX object ...` with exit code 0 and no `error:` line (clang-cl adds one `argument unused during compilation: '-c'` warning that has nothing to do with snippet content). Behaviourally, the validator itself has direct tests: `ctest --preset debug -R 'McpTypesTest.JsonSchemaValidator'`; registration-time failures use `ctest --preset debug -R 'McpServerTest.RegisterToolRejectsInvalidName'` and `ctest --preset debug -R 'McpServerTest.DuplicateRegistrationThrowsInvalidParams'`. Note that the `invalid arguments:` and element-count branches currently have an implementation path but no test asserting their text, so verify them yourself when touching `HandleCallTool`.

## Era differences

Where validation happens and what `is_error` means are identical in both protocol eras; the only divergence is how task-backed and MRTR results carry failure (exceptions keep their error code on the task path) — see [/en/learn/protocol-versions](/en/learn/protocol-versions).

## Common mistakes

| Symptom | Cause | Remedy |
|---|---|---|
| Every call fails with `invalid arguments: missing required property 'x'` | The client omits the field while the schema declares it in `required` | Make the field optional, or state the requirement in `description`/`title` |
| `invalid arguments: property 'x': boolean schema false` | `additionalProperties: false` rejected an extra key | Have the client send fewer keys, or relax to `additionalProperties: { "type": "string" }` |
| `format: date` does nothing | The validator does not support `format` | Express the same rule with a fully anchored `pattern` |
| `invalid arguments: unresolvable $ref` | The local JSON Pointer points at a path the schema does not contain (e.g. `#/definitions/x` while only `$defs` exists) | Keep shared subschemas in `$defs` and reference `#/$defs/<name>`; `$ref` accepts only `#`-prefixed local references |
| The call never responds and eventually times out | A circular `$ref` in the schema | Flatten with `items`/`prefixItems` instead of self-reference |
| Call returns `is_error` with `JsonValue: key not found: 'x'` or `JsonValue::GetString: expected string, got number` | Direct `operator[]` plus `GetString()` without checking presence and type | Follow layer two: `Find()` then `IsXxx()` before reading; the exception does become `is_error`, but you do not control the text |
| Payload size is not actually capped | Only `max_tool_input_elements` was set, and it counts one level | Add `maxItems` for concrete arrays and `maxLength` for strings in the schema |
| You wanted to send `-32602` but the client only got text | Throwing `McpError` from a handler drops the code | Put the detail into the message, or return an `is_error` result with structured text |

## Next steps

- Full tool registration flow: [/en/howto/register-a-tool](/en/howto/register-a-tool)
- Asking the user for missing parameters: [/en/howto/collect-user-input](/en/howto/collect-user-input), [/en/howto/request-extra-input](/en/howto/request-extra-input)
- Long tasks and failure reporting: [/en/howto/run-long-tasks](/en/howto/run-long-tasks)
- Error code table: [/en/reference/errors](/en/reference/errors); type fields: [/en/reference/types](/en/reference/types)

## Source behind this page

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/RequestContext.hpp`
- `include/mcp/server/ServerOptions.hpp`
- `include/mcp/server/McpServerTool.hpp`
- `include/mcp/McpTypes.hpp`
- `include/mcp/JsonValue.hpp`
- `include/mcp/McpError.hpp`
- `src/detail/JsonSchemaValidator.hpp` (internal validator; the single source of truth for its capabilities)
- `src/server/McpServer.cpp` (`ValidateToolCallArguments` and the `tools/call` branch)
- `examples/WeatherServer/main.cpp` (handler-side type defence sample)
