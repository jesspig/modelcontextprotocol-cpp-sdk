# JSON Schema Validation Reference

This page describes the two JSON Schema validation entry points and their behavioral boundary. Both live in the internal `detail/` layer: the three-argument legacy `ValidateJsonSchema` is inline in `src/detail/JsonSchemaValidator.hpp`, while the new 2020-12 engine is declared in `src/detail/SchemaValidation.hpp` and implemented in `src/detail/SchemaValidation.cpp`. They are **not** installed user API under `include/mcp/`; this page records their semantics for source reading and for diagnosing validation differences. The complete error-code table is in the [Errors reference](/en/reference/errors) and the `JsonValue` field table in [Types](/en/reference/types).

::: warning
The three-argument `ValidateJsonSchema` keeps its **legacy behavior by default**: `unevaluatedProperties`/`unevaluatedItems` are ignored, an invalid `pattern` is skipped rather than rejected, and numbers are compared through `double`. There is no automatic upgrade — 2020-12 semantics must be requested explicitly.
:::

## Entry points

| Entry | Location | Default | Semantics |
|---|---|---|---|
| `ValidateJsonSchema(instance, schema, error_out)` | `src/detail/JsonSchemaValidator.hpp` | Old entry, legacy behavior | Preserves the original draft behavior; invalid patterns do not fail the schema, unevaluated keywords are ignored, numbers go through `double` |
| `ValidateJsonSchema(instance, schema, error_out, Legacy, options?)` | `src/detail/SchemaValidation.hpp` | Explicit legacy | Equivalent to the three-argument entry, for switching modes at one call site; `Legacy` is an internal `SchemaMode` value |
| `ValidateJsonSchema(instance, schema, error_out, Modern2020, options?)` / `ValidateJsonSchema2020(...)` | `src/detail/SchemaValidation.hpp` | Explicit new engine | Internal 2020-12 compile + evaluation core; unsupported dialects, invalid patterns and unresolvable references are rejected at compile time |
| `Compile(schema, options?)` on `SchemaCompiler` → `CompiledSchema`; `Validate(compiled, instance, error_out)` on `EvaluationEngine` | `src/detail/SchemaValidation.hpp` | — | Compile once, validate many times; `CompiledSchema` owns its schema copies and compiled RE2 objects, is move-only |
| `CompareJsonSchemaEngines(instance, schema, options?)` | `src/detail/SchemaValidation.hpp` | — | Shadow comparison: runs both engines and returns `SchemaShadowResult{legacy_valid, modern_valid, diverged}` without changing either verdict |

The internal `SchemaMode` enum has the values `Legacy` and `Modern2020`. Every added default preserves old behavior: the old entry keeps returning old results, and new capabilities must be named explicitly.

## 2020-12 semantics in the new engine

- **References and scope**: an offline resource/anchor/dynamicAnchor graph; `$ref`, `$dynamicRef`, `$recursiveRef` and dynamic scope. A `$dynamicRef` enters dynamic resolution only when its initial target carries a matching `$dynamicAnchor`, then searches the dynamic scope from the **outermost** frame inwards so a scope that has already been left is not reused.
- **Annotations and unevaluated keywords**: `unevaluatedProperties` / `unevaluatedItems` merge successful annotations from adjacent applicators (`allOf`/`anyOf`/`oneOf`/`if`/`then`/`else`/`dependentSchemas`/`contains`, etc.); `prefixItems`, `dependentRequired`, `dependentSchemas`, `minContains`/`maxContains` follow 2020-12.
- **Exact numbers**: decimal fixed-point/rational comparison, so `9007199254740993` and `9007199254740992` stay distinct, `multipleOf: 0.1` accepts `0.3`, negative zero equals integer zero, and large exponents are budget-bounded and fail closed when out of range.
- **format / content**: annotation only by default — no automatic assertion or decoding. the internal `assert_format` option opts into assertions for a few formats; `contentEncoding`/`contentMediaType`/`contentSchema` never decode or validate content by default.
- **Fail closed**: dialects, a keyword allowlist, reference resolvability and pattern compilability are checked at compile time. Any over-budget or unsupported input throws `McpError(InvalidParams, ...)` with an `SL.*` classifier and the schema location; there is no partial pass.

## RE2 integration and pinned versions

| Item | Frozen value / note |
|---|---|
| RE2 | `2025-11-05 @ 927f5d53caf8111721e734cf24724686bb745f55`, BSD-3-Clause; `.gitmodules` records the official GitHub URL and that commit |
| Abseil | `20260817.0 @ 2065f4ded0558c6f89fee67c8e5228feb4eb960e`, Apache-2.0; `ABSL_MSVC_STATIC_RUNTIME=OFF` keeps the CRT consistent with the SDK |
| Build options | `RE2_INSTALL=OFF` (otherwise `install(EXPORT)` references unexported absl targets), `RE2_BUILD_TESTING=OFF`, `RE2_USE_ICU=OFF`, `USEPCRE=OFF`, `BUILD_SHARED_LIBS=OFF` |
| Matching | RE2's `PartialMatch` implements the JSON Schema search semantics; UTF-8; `max_mem` is explicitly set to 8388608 bytes rather than relying on the default |
| Dialect limits | RE2 is not full ECMA-262: no lookaround or backreferences; an invalid pattern fails schema compilation (the legacy fail-open behavior belongs only to the old entry) |
| Compatibility mapping | the adapter maps common ECMA-262 long property names to RE2's built-in short aliases (`\p{Letter}`→`\p{L}`, `\p{digit}`→`\p{Nd}`, etc.); unknown long names are left untouched and rejected by RE2, and ICU is not enabled for this |

RE2 and Abseil types never appear in a public header; they are used only inside `src/detail/SchemaValidation.cpp`.

## Offline references and registry

- Offline by default: any non-fragment `http`/`https`/`file` reference is rejected with `SL.RemoteRef`; no DNS lookup and no network request are performed.
- External documents can be supplied explicitly through the internal `registry` option (URI → `JsonValue`). Registry documents count towards the document/byte budgets and take part in compile-time reference resolution.
- `$id` accepts relative URIs with RFC 3986 dot-segment normalization; references resolve against the current resource base, and fragments are percent-decoded before JSON Pointer or anchor lookup.

## Resource budgets (T-03 B-37..B-51)

The integer thresholds live in `SchemaBudgets`. On overflow, evaluation stops before further allocation, resources are released, and the error maps to `InvalidParams` (-32602) with an `SL.*` classifier.

| ID | Item | Limit | ID | Item | Limit |
|---|---|---:|---|---|---:|
| B-37 | schema document bytes | 1048576 B | B-45 | `uniqueItems` elements | 1024 items |
| B-38 | total schema bytes | 4194304 B | B-46 | expanded/evaluated nodes | 100000 nodes |
| B-39 | external documents (incl. root) | 16 docs | B-47 | RE2 pattern bytes | 4096 B |
| B-40 | reference graph depth | 64 levels | B-48 | RE2 subject bytes | 65536 B |
| B-41 | schema structure depth | 32 levels | B-49 | RE2 `max_mem` | 8388608 B |
| B-42 | schema nodes | 100000 nodes | B-50 | RE2 compile-cache entries | 1024 entries |
| B-43 | evaluation steps | 1000000 steps | B-51 | allowed pattern fail-open | 0 |
| B-44 | evaluation time | 2000 ms | | | |

## Unsupported dialects and `$vocabulary`

`$schema` accepts only the standard 2020-12 http/https URIs. Custom metaschemas and `$vocabulary` (for example a metaschema without the validation vocabulary) are rejected at **compile time** with an error such as `SL.UnsupportedDialect: schema dialect '<URI>' is not supported; custom metaschemas and $vocabulary are rejected at compile time at #`. This is a deliberate fail-closed choice: silently ignoring vocabularies that cannot be enabled or disabled would create false passes.

## Official Test Suite status

Pinned suite commit `e2a68a15cd490750fd6b95e604e4054d7d978d1f`. The driver requires the `MCP_JSON_SCHEMA_SUITE_DIR` compile definition and is enabled with `MCP_RUN_JSON_SCHEMA_SUITE=1`; it is Skipped in the default ctest run. The latest run: 405 groups / 1300 cases / 142 skipped / 2 failures. Both remaining failures are deliberate fail-closed rejections of custom metaschema `$vocabulary` cases and are documented; **full official-suite conformance is not claimed**.

## Source files behind this page

- `src/detail/SchemaValidation.hpp`, `src/detail/SchemaValidation.cpp`
- `src/detail/JsonSchemaValidator.hpp`, `src/detail/schema/ExactNumber.hpp`
- `tests/unit/JsonSchema2020Tests.cpp` (local positive/negative cases and the official-suite driver)
- `third_party/re2/`, `third_party/abseil-cpp/` (pinned sources)
