# 提示

提示是可复用的模板，用于指导 LLM 交互。它们可以带有参数以实现动态内容。

## 注册提示

```cpp
server->RegisterPrompt(
    "code_review",
    PromptOptions{}.Description("审查代码变更"),
    [](const std::string& name,
       const std::optional<nlohmann::json>& args) -> GetPromptResult {
        GetPromptResult result;

        PromptMessage msg;
        msg.role = "user";
        msg.content = TextContent{"text",
            "请审查以下代码变更：\n" +
            (args ? args->value("diff", "") : "")};
        result.messages.push_back(msg);

        return result;
    });
```

## 提示参数

```cpp
PromptArgument arg;
arg.name = "diff";
arg.description = "要审查的 git diff";
arg.required = true;

Prompt p;
p.name = "code_review";
p.description = "审查代码变更";
p.arguments = {arg};
```

::: note
`PromptOptions` 会被注册 API 接受，但 `description` 当前不会传播到通过 `prompts/list` 暴露的协议级提示。`ListPromptsResult` 中返回的 `Prompt` 结构仅包含 `name`。
:::
