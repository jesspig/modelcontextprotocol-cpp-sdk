# Prompts

Prompts are reusable templates that guide LLM interactions. They can have arguments for dynamic content.

## Registering a Prompt

```cpp
server->RegisterPrompt(
    "code_review",
    PromptOptions{}.Description("Review code changes"),
    [](const std::string& name,
       const std::optional<nlohmann::json>& args) -> GetPromptResult {
        GetPromptResult result;

        PromptMessage msg;
        msg.role = "user";
        msg.content = TextContent{"text",
            "Review the following code changes:\n" +
            (args ? args->value("diff", "") : "")};
        result.messages.push_back(msg);

        return result;
    });
```

## Prompt Arguments

```cpp
PromptArgument arg;
arg.name = "diff";
arg.description = "The git diff to review";
arg.required = true;

Prompt p;
p.name = "code_review";
p.description = "Review code changes";
p.arguments = {arg};
```

::: note
`PromptOptions` is accepted by the registration API but `description` is currently not propagated to the protocol-level prompt exposed via `prompts/list`. The `Prompt` struct returned in `ListPromptsResult` only includes `name`.
:::
