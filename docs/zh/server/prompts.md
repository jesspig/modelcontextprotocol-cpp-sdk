# 提示

提示是可复用的模板，用于指导 LLM 交互。它们可以带有参数以实现动态内容。

## 注册提示

```cpp
server->RegisterPrompt(
    "code_review",
    PromptOptions{}.Description("审查代码变更"),
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
            "请审查以下代码变更：\n" + diff};
        result.messages.push_back(msg);

        return result;
    });
```

## 提示参数

参数可直接通过 `PromptOptions::Arguments()`（fluent 风格）声明，会传播到 `prompts/list`：

```cpp
PromptArgument arg;
arg.name = "diff";
arg.description = "要审查的 git diff";
arg.required = true;

server->RegisterPrompt(
    "code_review",
    PromptOptions{}.Description("审查代码变更").Arguments({arg}),
    [](const std::string& name,
       const std::optional<JsonValue>& args) -> GetPromptResult {
        // ...
    });
```

也可以直接构造协议级 `Prompt` 结构：

```cpp
Prompt p;
p.name = "code_review";
p.description = "审查代码变更";
p.arguments = {arg};
```

::: note
`PromptOptions` 中的字段（`description`、`title`、`icons`、`arguments`）会传播到通过 `prompts/list` 返回的协议级 `Prompt` 结构中。
:::
