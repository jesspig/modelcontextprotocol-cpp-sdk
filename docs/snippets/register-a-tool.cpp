#include <mcp/Content.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/Meta.hpp>
#include <mcp/Transport.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <iostream>
#include <memory>
#include <string>

using namespace mcp;

using Ctx = RequestContext<CallToolRequestParams>;

// region minimal
void smoke_minimal() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    server->RegisterTool(
        "ping",
        ToolOptions{},
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "pong"});
            return result;
        });

    server->Run();
}
// endregion

// region schema
void smoke_schema() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    ToolOptions options;
    options.Description("Return a weather briefing for a city")
        .Title("Weather briefing")
        .InputSchema(JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"properties", JsonValue(JsonValue::Object{
                {"city", JsonValue(JsonValue::Object{
                    {"type", JsonValue("string")},
                    {"minLength", JsonValue(1)},
                    {"maxLength", JsonValue(64)},
                })},
                {"days", JsonValue(JsonValue::Object{
                    {"type", JsonValue("integer")},
                    {"minimum", JsonValue(1)},
                    {"maximum", JsonValue(7)},
                })},
            })},
            {"required", JsonValue(JsonValue::Array{JsonValue("city")})},
            {"additionalProperties", JsonValue(false)},
        }));

    server->RegisterTool(
        "briefing",
        options,
        [](const Ctx& ctx) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "Sunny, 22C"});
            return result;
        });

    server->Run();
}
// endregion

// region hints
void smoke_hints() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    ToolOptions options;
    options.Description("Re-index the search corpus");
    options.read_only_hint = false;
    options.destructive = true;
    options.idempotent = true;
    options.open_world_hint = false;
    options.icons.push_back(Icon{"file://icons/index.svg", "image/svg+xml"});

    server->RegisterTool(
        "reindex",
        options,
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "reindexed"});
            return result;
        });

    server->Run();
}
// endregion

// region context
void smoke_context() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    server->RegisterTool(
        "briefing",
        ToolOptions{}.Description("Return a briefing for a city"),
        [](const Ctx& ctx) {
            const CallToolRequestParams& params = ctx.Params();
            std::string city = params.name;
            if (params.arguments) {
                const JsonValue* value = params.arguments->Find("city");
                if (value && value->IsString()) {
                    city = value->GetString();
                }
            }

            ctx.Log(LoggingLevel::Info, "briefing requested for " + city);

            if (ctx.IsCancellationRequested()) {
                CallToolResult cancelled;
                cancelled.is_error = true;
                cancelled.content.push_back(TextContent{"text", "cancelled"});
                return cancelled;
            }

            const std::string version = ctx.Server().GetNegotiatedProtocolVersion();

            CallToolResult result;
            result.content.push_back(
                TextContent{"text", city + " briefing (protocol " + version + ")"});
            return result;
        });

    server->Run();
}
// endregion

// region structured
void smoke_structured() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    ToolOptions options;
    options.Description("Summarize a document")
        .InputSchema(JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"properties", JsonValue(JsonValue::Object{
                {"body", JsonValue(JsonValue::Object{{"type", JsonValue("string")}})},
            })},
            {"required", JsonValue(JsonValue::Array{JsonValue("body")})},
        }))
        .output_schema = JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"properties", JsonValue(JsonValue::Object{
                {"summary", JsonValue(JsonValue::Object{{"type", JsonValue("string")}})},
                {"words", JsonValue(JsonValue::Object{{"type", JsonValue("integer")}})},
            })},
            {"required", JsonValue(JsonValue::Array{JsonValue("summary")})},
        });

    server->RegisterTool(
        "summarize",
        options,
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "See structuredContent"});
            result.structured_content = JsonValue(JsonValue::Object{
                {"summary", JsonValue("A short document about weather.")},
                {"words", JsonValue(6)},
            });
            return result;
        });

    server->Run();
}
// endregion

// region registration-errors
void smoke_registration_errors() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    auto register_ping = [&server] {
        server->RegisterTool(
            "ping",
            ToolOptions{}.Description("First registration wins"),
            [](const Ctx&) { return CallToolResult{}; });
    };

    register_ping();

    try {
        register_ping();
    } catch (const McpError& error) {
        if (error.Code() != McpErrorCode::InvalidParams) {
            throw;
        }
        std::cerr << "registration rejected: " << error.ErrorMessage() << std::endl;
    }

    server->Run();
}
// endregion

// region full
void smoke_full() {
    ServerOptions server_options;
    server_options.server_info = Implementation{"register-a-tool-demo", "1.0.0"};
    server_options.server_instructions = "Call briefing with {\"city\":\"...\"}.";
    server_options.max_tool_input_elements = 16;

    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server, server_options);

    ToolOptions options;
    options.Description("Return a weather briefing for a city")
        .Title("Weather briefing")
        .InputSchema(JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"properties", JsonValue(JsonValue::Object{
                {"city", JsonValue(JsonValue::Object{
                    {"type", JsonValue("string")},
                    {"minLength", JsonValue(1)},
                    {"maxLength", JsonValue(64)},
                })},
                {"days", JsonValue(JsonValue::Object{
                    {"type", JsonValue("integer")},
                    {"minimum", JsonValue(1)},
                    {"maximum", JsonValue(7)},
                })},
            })},
            {"required", JsonValue(JsonValue::Array{JsonValue("city")})},
            {"additionalProperties", JsonValue(false)},
        }))
        .output_schema = JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"properties", JsonValue(JsonValue::Object{
                {"summary", JsonValue(JsonValue::Object{{"type", JsonValue("string")}})},
                {"days", JsonValue(JsonValue::Object{{"type", JsonValue("integer")}})},
            })},
            {"required", JsonValue(JsonValue::Array{JsonValue("summary")})},
        });
    options.read_only_hint = true;
    options.idempotent = true;
    options.open_world_hint = false;

    server->RegisterTool(
        "briefing",
        options,
        [](const Ctx& ctx) {
            const std::optional<JsonValue>& arguments = ctx.Params().arguments;
            const JsonValue* city = arguments ? arguments->Find("city") : nullptr;
            const JsonValue* days = arguments ? arguments->Find("days") : nullptr;

            ctx.Log(LoggingLevel::Info, "briefing invoked");

            if (!city || !city->IsString()) {
                CallToolResult failure;
                failure.is_error = true;
                failure.content.push_back(
                    TextContent{"text", "city must be a string"});
                return failure;
            }

            CallToolResult result;
            result.content.push_back(
                TextContent{"text", city->GetString() + ": sunny"});
            result.structured_content = JsonValue(JsonValue::Object{
                {"summary", JsonValue(city->GetString() + ": sunny")},
                {"days", JsonValue(days && days->IsInt() ? static_cast<int>(days->GetInt()) : 1)},
            });
            return result;
        });

    server->Run();
}
// endregion
