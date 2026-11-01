#include <mcp/Content.hpp>
#include <mcp/ErrorCodes.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// region full
using namespace mcp;

void ServeAPrompt() {
    auto read_argument = [](const std::optional<JsonValue>& args,
                            std::string_view key) -> std::string {
        if (!args) {
            return {};
        }
        const auto* value = args->Find(key);
        if (!value || !value->IsString()) {
            return {};
        }
        return value->GetString();
    };

    ServerOptions options;
    options.server_info = Implementation{"DocsPromptServer", "1.0.0"};
    options.declare_completions = true;

    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(std::move(pair.server), options);

    PromptArgument language;
    language.name = "language";
    language.description = "Language the snippet is written in";
    language.required = true;

    PromptArgument code;
    code.name = "code";
    code.description = "Snippet to review";
    code.required = true;

    server->RegisterPrompt("code-review",
        PromptOptions{}
            .Title("Code Review")
            .Description("Review a snippet written in one language")
            .Arguments({language, code}),
        [read_argument](const std::string& name,
                        const std::optional<JsonValue>& args) -> GetPromptResult {
            const std::string language_value = read_argument(args, "language");
            const std::string code_value = read_argument(args, "code");
            if (language_value.empty() || code_value.empty()) {
                throw McpError(McpErrorCode::InvalidParams,
                    "prompt '" + name + "' requires arguments 'language' and 'code'");
            }

            PromptMessage instruction;
            instruction.role = "user";
            instruction.content = TextContent{"text",
                "Review this " + language_value + " snippet for correctness and safety:\n"
                    + code_value};

            GetPromptResult result;
            result.description = "Review a " + language_value + " snippet";
            result.messages.push_back(std::move(instruction));
            return result;
        });

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
}
// endregion
