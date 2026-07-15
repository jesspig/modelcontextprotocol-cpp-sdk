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

## 列表变更通知

当提示在运行时发生变更时，通知客户端：

```cpp
server->SendPromptListChanged();
```
