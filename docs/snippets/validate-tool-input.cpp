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

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace mcp;

using Ctx = RequestContext<CallToolRequestParams>;

inline bool KnownAccount(std::string_view id) {
    return id == "acct-1" || id == "acct-2";
}

// region builtin-validation
void smoke_builtin_validation() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    ToolOptions options;
    options.Description("Convert a temperature between units")
        .InputSchema(JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"properties", JsonValue(JsonValue::Object{
                {"celsius", JsonValue(JsonValue::Object{
                    {"type", JsonValue("number")},
                    {"minimum", JsonValue(-273.15)},
                })},
                {"unit", JsonValue(JsonValue::Object{
                    {"type", JsonValue("string")},
                    {"enum", JsonValue(JsonValue::Array{
                        JsonValue("C"), JsonValue("F"), JsonValue("K")})},
                })},
            })},
            {"required", JsonValue(JsonValue::Array{
                JsonValue("celsius"), JsonValue("unit")})},
            {"additionalProperties", JsonValue(false)},
        }));

    server->RegisterTool(
        "convert",
        options,
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "71.6F"});
            return result;
        });

    server->Run();
}
// endregion

// region handler-defense
void smoke_handler_defense() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    server->RegisterTool(
        "forecast",
        ToolOptions{}.Description("Forecast for a lat/lon box"),
        [](const Ctx& ctx) -> CallToolResult {
            const std::optional<JsonValue>& arguments = ctx.Params().arguments;
            if (!arguments || !arguments->IsObject()) {
                CallToolResult missing;
                missing.is_error = true;
                missing.content.push_back(TextContent{"text", "arguments must be an object"});
                return missing;
            }

            const JsonValue* latitude = arguments->Find("latitude");
            const JsonValue* longitude = arguments->Find("longitude");
            if (!latitude || !longitude || !latitude->IsNumber() || !longitude->IsNumber()) {
                CallToolResult invalid;
                invalid.is_error = true;
                invalid.content.push_back(TextContent{"text", "latitude/longitude must be numbers"});
                return invalid;
            }

            const double lat = latitude->IsDouble()
                ? latitude->GetDouble()
                : static_cast<double>(latitude->GetInt());
            const double lon = longitude->IsDouble()
                ? longitude->GetDouble()
                : static_cast<double>(longitude->GetInt());

            CallToolResult result;
            result.content.push_back(
                TextContent{"text", "Mild near " + std::to_string(lat) + ", " + std::to_string(lon)});
            return result;
        });

    server->Run();
}
// endregion

// region element-limit
void smoke_element_limit() {
    ServerOptions server_options;
    server_options.server_info = Implementation{"validate-tool-input-demo", "1.0.0"};
    server_options.max_tool_input_elements = 8;

    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server, server_options);

    server->RegisterTool(
        "bulk_tag",
        ToolOptions{}.Description("Tag a batch of records"),
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "tagged"});
            return result;
        });

    server->Run();
}
// endregion

// region local-ref
void smoke_local_ref() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    ToolOptions options;
    options.Description("Route between two cities")
        .InputSchema(JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"$defs", JsonValue(JsonValue::Object{
                {"cityName", JsonValue(JsonValue::Object{
                    {"type", JsonValue("string")},
                    {"minLength", JsonValue(1)},
                    {"maxLength", JsonValue(64)},
                })},
            })},
            {"properties", JsonValue(JsonValue::Object{
                {"origin", JsonValue(JsonValue::Object{
                    {"$ref", JsonValue("#/$defs/cityName")}})},
                {"destination", JsonValue(JsonValue::Object{
                    {"$ref", JsonValue("#/$defs/cityName")}})},
            })},
            {"required", JsonValue(JsonValue::Array{JsonValue("origin")})},
        }));

    server->RegisterTool(
        "route",
        options,
        [](const Ctx&) {
            CallToolResult result;
            result.content.push_back(TextContent{"text", "3h by rail"});
            return result;
        });

    server->Run();
}
// endregion

// region failure-modes
void smoke_failure_modes() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server);

    server->RegisterTool(
        "withdraw",
        ToolOptions{}.Description("Withdraw an amount from an account"),
        [](const Ctx& ctx) -> CallToolResult {
            const std::optional<JsonValue>& arguments = ctx.Params().arguments;
            const JsonValue* amount = arguments ? arguments->Find("amount") : nullptr;
            if (!amount || !amount->IsNumber()) {
                CallToolResult rejected;
                rejected.is_error = true;
                rejected.content.push_back(TextContent{"text", "amount must be a number"});
                return rejected;
            }

            const JsonValue* account = arguments ? arguments->Find("account") : nullptr;
            if (!account || !account->IsString() || !KnownAccount(account->GetString())) {
                CallToolResult unknown;
                unknown.is_error = true;
                unknown.content.push_back(TextContent{"text", "unknown account"});
                return unknown;
            }

            if (amount->IsInt() && amount->GetInt() > 1000000) {
                throw std::runtime_error("ledger backend unavailable");
            }

            if (amount->IsInt() && amount->GetInt() < 0) {
                throw McpError(McpErrorCode::InvalidParams, "amount must not be negative");
            }

            CallToolResult result;
            result.content.push_back(TextContent{"text", "withdrawn"});
            return result;
        });

    server->Run();
}
// endregion

// region full
void smoke_full() {
    ServerOptions server_options;
    server_options.server_info = Implementation{"validate-tool-input-demo", "1.0.0"};
    server_options.max_tool_input_elements = 16;

    auto pair = InMemoryTransport::CreatePair();
    auto server = McpServer::Create(pair.server, server_options);

    ToolOptions options;
    options.Description("Convert a temperature between units")
        .InputSchema(JsonValue(JsonValue::Object{
            {"type", JsonValue("object")},
            {"properties", JsonValue(JsonValue::Object{
                {"celsius", JsonValue(JsonValue::Object{
                    {"type", JsonValue("number")},
                    {"minimum", JsonValue(-273.15)},
                })},
                {"unit", JsonValue(JsonValue::Object{
                    {"type", JsonValue("string")},
                    {"enum", JsonValue(JsonValue::Array{
                        JsonValue("C"), JsonValue("F"), JsonValue("K")})},
                })},
            })},
            {"required", JsonValue(JsonValue::Array{
                JsonValue("celsius"), JsonValue("unit")})},
            {"additionalProperties", JsonValue(false)},
        }));

    server->RegisterTool(
        "convert",
        options,
        [](const Ctx& ctx) -> CallToolResult {
            const std::optional<JsonValue>& arguments = ctx.Params().arguments;
            const JsonValue* celsius = arguments ? arguments->Find("celsius") : nullptr;
            const JsonValue* unit = arguments ? arguments->Find("unit") : nullptr;
            if (!celsius || !celsius->IsNumber() || !unit || !unit->IsString()) {
                CallToolResult invalid;
                invalid.is_error = true;
                invalid.content.push_back(TextContent{"text", "celsius and unit are required"});
                return invalid;
            }

            const double degrees = celsius->IsDouble()
                ? celsius->GetDouble()
                : static_cast<double>(celsius->GetInt());
            const std::string scale = unit->GetString();

            if (scale != "C" && scale != "F" && scale != "K") {
                CallToolResult unsupported;
                unsupported.is_error = true;
                unsupported.content.push_back(
                    TextContent{"text", "unsupported unit: " + scale});
                return unsupported;
            }

            const double fahrenheit = degrees * 9.0 / 5.0 + 32.0;

            CallToolResult result;
            result.content.push_back(
                TextContent{"text", std::to_string(fahrenheit) + "F"});
            return result;
        });

    server->Run();
}
// endregion
