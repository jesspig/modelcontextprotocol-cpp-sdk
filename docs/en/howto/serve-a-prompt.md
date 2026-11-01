# Serve a Prompt

A prompt is a reusable message template: the client fetches it by name and fills in arguments. This guide covers declaring arguments, building the returned messages, reporting missing arguments, and wiring completion to a prompt argument.

## Prerequisites

- You can already bring up a server instance: see [Quick Start](/en/start/quickstart).
- You have a `std::shared_ptr<ITransport>`. For an in-process round trip, `InMemoryTransport::CreatePair()` hands back a matched `.client` and `.server`.
- Signatures and field tables live only in the reference layer and are not repeated here: `RegisterPrompt` and `SetCompletionHandler` in [Server Facade Reference](/en/reference/server), `PromptOptions`, `PromptArgument`, `GetPromptResult`, `CompleteRequestParams` and `CompleteResult` in [Type Reference](/en/reference/types).

## Step 1: Declare the Prompt and Its Arguments

```cpp
PromptArgument language;
language.name = "language";
language.description = "Language the snippet is written in";
language.required = true;

PromptArgument code;
code.name = "code";
code.description = "Snippet to review";
code.required = true;

PromptOptions prompt_options;
prompt_options.Title("Code Review")
    .Description("Review a snippet written in one language")
    .Arguments({language, code});
```

Handing `prompt_options` to `McpServer::RegisterPrompt("code-review", prompt_options, handler)` completes the registration.

- The registered name comes only from the `name` argument of `RegisterPrompt`; the `name` member of `PromptOptions` has no consumer at all and setting it changes nothing.
- The list passed to `Arguments()` goes into `prompts/list` verbatim; `required` is a `std::optional<bool>`, and leaving it empty drops the field from the wire (`src/core/McpTypesTools.cpp:192-198`).
- A duplicate name throws `McpError` (`InvalidParams`, `prompt already registered: <name>`), and an empty handler is rejected too (`src/server/McpServer.cpp:880-883`, `:893-898`).
- A successful registration declares the `prompts` capability with `list_changed` set to true, and emits one `list_changed` automatically while the instance is running (`src/server/McpServer.cpp:901-905`, `:1955-1958`).

## Step 2: Build the Messages in the Handler

```cpp
const std::string language_value = read_argument(args, "language");
const std::string code_value = read_argument(args, "code");

PromptMessage instruction;
instruction.role = "user";
instruction.content = TextContent{"text",
    "Review this " + language_value + " snippet for correctness and safety:\n"
        + code_value};

GetPromptResult result;
result.description = "Review a " + language_value + " snippet";
result.messages.push_back(std::move(instruction));
return result;
```

- The handler signature is `(const std::string& name, const std::optional<JsonValue>& args)`: `name` is the registered name that matched, `args` is the client's `arguments` object deserialized verbatim. When the client sends no `arguments`, `args` is `nullopt`.
- `read_argument` in the example is the argument-fetching lambda in the complete code: it returns an empty string when `args` is empty, the key is absent, or the value is not a string — `args->Find(key)` returns `nullptr` rather than throwing, so the empty check has to live in your fetching code.
- `role` goes onto the wire unchecked; per the protocol only `user` or `assistant` are valid.
- `content` is a `ContentVariant` and any of the five variants works: text, image, audio, embedded resource, resource link. See [Serve a Resource](/en/howto/serve-a-resource) for the embedded-resource variant.
- `messages` is always serialized, so an empty vector still goes out as `"messages": []` — the SDK does not stop you, and you have to guarantee the minimum count yourself.
- The prompt list paginates at 100 entries per page as well; anything beyond that needs a `cursor`.

## Step 3: Validate Missing Arguments Yourself

On the `prompts/get` path the SDK performs **no argument validation at all**: it checks neither `required` nor types, and `args` reaches your handler untouched. This differs from `tools/call`, which runs JSON Schema validation against `input_schema`. A missing argument must be raised by you, or the peer just receives a content-free message.

```cpp
if (language_value.empty() || code_value.empty()) {
    throw McpError(McpErrorCode::InvalidParams,
        "prompt '" + name + "' requires arguments 'language' and 'code'");
}
```

The thrown `McpError` is encoded verbatim as a JSON-RPC error response with code and message preserved; any other exception collapses to `InternalError` plus the fixed text `internal error` (see [Error Codes and Model](/en/reference/errors)).

::: tip
Declare and validate as a pair: `required = true` is metadata the client reads, and the actual rejection can only happen inside your handler.
:::

## Step 4: Attach Completion to a Prompt Argument

The completion capability is decided by `ServerOptions::declare_completions`, which defaults to `false` and is never derived from registrations. The `completion/complete` request handler is wired unconditionally, though, so the method stays reachable without the capability — the peer simply will not ask.

```cpp
options.declare_completions = true;

server->SetCompletionHandler(
    [](const CompleteRequestParams& params) -> CompleteResult {
        const auto* ref_type = params.ref.Find("type");
        const bool is_prompt_ref =
            ref_type && ref_type->IsString() && ref_type->GetString() == "ref/prompt";

        std::vector<std::string> candidates;
        if (is_prompt_ref && params.argument_name == "language") {
            candidates = {"cpp", "go", "python", "rust"};
        }

        JsonValue values(JsonValue::array_tag);
        for (const auto& candidate : candidates) {
            if (candidate.rfind(params.argument_value, 0) != 0) {
                continue;
            }
            values.PushBack(JsonValue(candidate));
        }

        const int64_t total = static_cast<int64_t>(values.Size());

        JsonValue completion(JsonValue::object_tag);
        completion["values"] = std::move(values);
        completion["total"] = JsonValue(total);
        completion["hasMore"] = JsonValue(false);

        CompleteResult result;
        result.completion = std::move(completion);
        return result;
    });
```

- One instance holds a single global completion callback, so you dispatch on `params.ref`: it is an unparsed `JsonValue` shaped like `{"type":"ref/prompt","name":"code-review"}`, and you must read `type` / `name` yourself.
- `params.argument_name` is the key being typed and `params.argument_value` the text entered so far; `params.context_arguments` carries the other arguments already fixed in the same request (values must be strings, otherwise the whole request is rejected as `InvalidParams` — `src/core/McpTypesParams.cpp:113-126`).
- `CompleteResult::completion` is a raw `JsonValue` too, and the SDK validates no shape. The convention is `values` / `total` / `hasMore`, and `total` has to be computed before `values` is moved out.
- Without `SetCompletionHandler`, `completion/complete` returns `MethodNotFound` with the message `No completion handler registered` (`src/server/McpServer.cpp:3111-3114`); passing an empty callback throws `InvalidParams` at registration time.

## Complete code

<<< @/snippets/serve-a-prompt.cpp#full

## Verification

```bash
ctest --preset debug -R 'McpServerTest\.(RegisterPrompt|ListPrompts|Prompts|Complete|InitializeDeclares|ExplicitCapability)'
```

All 6 cases should report `Passed`, with the final line `100% tests passed, 0 tests failed out of 6`. Key assertions:

| Case | Asserted value |
|---|---|
| `McpServerTest.ListPromptsIncludesArguments` | `prompts[0].arguments[0].name` equals `city`, `required` equals `true` |
| `McpServerTest.ExplicitCapabilityDeclarationDefaultsOff` | the `completions` capability has no value |
| `McpServerTest.InitializeDeclaresExplicitLoggingAndCompletions` | `completions` has a value once `declare_completions` is set |
| `McpServerTest.CompleteRequestCarriesContextArguments` | `context_arguments` reaches the callback verbatim |

## Era differences

`prompts/list`, `prompts/get`, `completion/complete` and the `prompts/list_changed` notification exist in both eras. In the legacy era a session that has not handshaked gets `InvalidRequest` (`Server not initialized`) for these three requests; the modern era lets them through directly. See [Protocol Versions](/en/learn/protocol-versions).

## Common mistakes

| Symptom | Cause | Fix |
|---|---|---|
| The client omits a required argument and still gets a content-free message | The SDK never checks `required`; absence silently becomes an empty string | Check for empty at the top of the handler and throw `McpError(InvalidParams, ...)` |
| `prompts/list` shows no `arguments` | `PromptOptions::Arguments()` was not called, or `required` stayed an empty `optional` | Pass the list explicitly; assign `true` / `false` whenever `required` matters |
| Changing `PromptOptions::name` has no effect | That member has no consumer | Rename through the first argument of `RegisterPrompt` only |
| The peer asks for completion and gets `-32601` | `SetCompletionHandler` was never called | Register the global callback and dispatch on `params.ref` |
| The completion capability is missing from the `initialize` result | `declare_completions` defaults to `false` and is not derived | Set it to true explicitly on `ServerOptions` |
| An unknown prompt name yields `prompt not found: <name>` | The name is not in the registry | Compare against the names actually returned by `prompts/list`, watching hyphens versus underscores |

## Next steps

- Embed resources and links in a prompt: [Serve a Resource](/en/howto/serve-a-resource)
- Ask the user instead when an argument is missing: [Request Extra Input](/en/howto/request-extra-input)
- Declaring and rejecting arguments: [Validate Tool Input](/en/howto/validate-tool-input)

## Source behind this page

- `include/mcp/server/McpServer.hpp`
- `include/mcp/server/ServerOptions.hpp` (`declare_completions`)
- `include/mcp/McpTypes.hpp` (`PromptOptions`, `PromptArgument`, `Prompt`, `PromptMessage`, `GetPromptResult`, `CompleteRequestParams`, `CompleteResult`)
- `include/mcp/Content.hpp` (the five `ContentVariant` alternatives)
- `include/mcp/transport/InMemoryTransport.hpp`
- `src/server/McpServer.cpp` (`RegisterPrompt`, `HandleListPrompts`, `HandleGetPrompt`, `SetCompletionHandler`, `HandleComplete`, capability derivation)
- `src/core/McpTypesTools.cpp` (`SerializePromptArgument`, `SerializePromptMessage`)
- `src/core/McpTypesParams.cpp` (`DeserializeCompleteRequestParams`)
