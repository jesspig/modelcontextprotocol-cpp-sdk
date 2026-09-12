# Prompts

Prompts are reusable templates that guide LLM interactions. They can have arguments for dynamic content.

## Registering a Prompt

```cpp
server->RegisterPrompt(
    "code_review",
    PromptOptions{}.Description("Review code changes"),
    [](const std::string& name,
       const std::optional<JsonValue>& args) -> GetPromptResult {
        GetPromptResult result;

        std::string diff;
        if (args) {
            auto* v = args->Find("diff");
            if (v) diff = v->GetString();
        }
        PromptMessage msg;
        msg.role = "user";
        msg.content = TextContent{"text",
            "Review the following code changes:\n" + diff};
        result.messages.push_back(msg);

        return result;
    });
```

## Prompt Arguments

Arguments can be declared directly on `PromptOptions` via the fluent `.Arguments(...)` setter:

```cpp
PromptArgument arg;
arg.name = "diff";
arg.description = "The git diff to review";
arg.required = true;

server->RegisterPrompt(
    "code_review",
    PromptOptions{}.Description("Review code changes").Arguments({arg}),
    handler);
```

The arguments are propagated to the protocol-level `Prompt` struct and returned via `prompts/list`. For full control you can also set `arguments` on a `Prompt` struct directly:

```cpp
Prompt p;
p.name = "code_review";
p.description = "Review code changes";
p.arguments = {arg};
```

::: note
`PromptOptions` fields (`description`, `title`, `icons`, `arguments`) are propagated to the protocol-level `Prompt` struct returned via `prompts/list`.
:::
