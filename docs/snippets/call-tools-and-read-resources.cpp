#include <mcp/Content.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/client/McpClient.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace mcp;

namespace call_tools_and_read_resources {

// region list-tools
std::vector<std::string> ToolNames(McpClient& client) {
    ListToolsResult tools = client.ListTools();
    std::vector<std::string> names;
    for (const auto& tool : tools.tools) names.push_back(tool.name);
    return names;
}

ListToolsResult FirstToolsPage(McpClient& client) {
    return client.ListTools();
}

ListToolsResult NextPage(McpClient& client, const ListToolsResult& previous) {
    if (!previous.next_cursor || previous.next_cursor->empty()) return previous;
    return client.ListTools(*previous.next_cursor);
}

ListToolsResult SearchWeather(McpClient& client) {
    return client.SearchTools(std::optional<std::string>("weather"),
        std::optional<int64_t>(20));
}
// endregion

// region read-content
std::string DescribeContent(const std::vector<ContentVariant>& content) {
    std::string text;
    for (const auto& item : content) {
        if (const auto* entry = std::get_if<TextContent>(&item)) {
            text += entry->text;
        } else if (const auto* image = std::get_if<ImageContent>(&item)) {
            text += "[image:" + image->mime_type + "]";
        } else if (const auto* audio = std::get_if<AudioContent>(&item)) {
            text += "[audio:" + audio->mime_type + "]";
        } else if (const auto* embedded = std::get_if<EmbeddedResource>(&item)) {
            if (const auto* body = std::get_if<TextResourceContents>(&embedded->resource)) {
                text += body->text;
            }
        } else if (const auto* link = std::get_if<ResourceLink>(&item)) {
            text += "[link:" + link->uri + "]";
        }
    }
    return text;
}

std::string FailureText(const CallToolResult& result) {
    return result.is_error ? DescribeContent(result.content) : std::string();
}
// endregion

// region required-arguments
std::vector<std::string> RequiredArgumentNames(const Tool& tool) {
    std::vector<std::string> names;
    const JsonValue* required = tool.input_schema.Find("required");
    if (!required || !required->IsArray()) return names;
    for (const auto& item : required->GetArray()) {
        if (item.IsString()) names.push_back(item.GetString());
    }
    return names;
}

bool AcceptsCity(const Tool& tool) {
    const JsonValue* properties = tool.input_schema.Find("properties");
    return properties && properties->IsObject() &&
        properties->Find("city") != nullptr;
}
// endregion

// region call-tool
JsonValue MakeCityArgument(const std::string& city) {
    JsonValue arguments(JsonValue::object_tag);
    arguments["city"] = JsonValue(city);
    return arguments;
}

std::string CallWeather(McpClient& client) {
    CallToolResult result = client.CallTool("weather", MakeCityArgument("Oslo"));
    if (result.is_error) return FailureText(result);
    if (result.structured_content) return result.structured_content->Dump();
    return DescribeContent(result.content);
}

std::string CallServerTime(McpClient& client) {
    return DescribeContent(client.CallTool("server-time").content);
}
// endregion

// region progress
void CallWithProgress(McpClient& client) {
    RequestOptions options;
    options.on_progress = [](const ProgressNotificationParams& progress) {
        const double total = progress.total.has_value() ? *progress.total : 0.0;
        const double ratio = total > 0.0 ? progress.progress / total : progress.progress;
        (void)ratio;
    };
    client.CallTool("render", JsonValue(JsonValue::object_tag), options);
}
// endregion

// region list-resources
std::vector<std::string> ResourceUris(McpClient& client) {
    std::vector<std::string> uris;
    for (const auto& resource : client.ListResourcesAll().resources) {
        uris.push_back(resource.uri);
    }
    for (const auto& entry : client.ListResourceTemplatesAll().resource_templates) {
        uris.push_back(entry.uri_template);
    }
    return uris;
}

std::optional<std::string> MimeTypeOf(McpClient& client, const std::string& uri) {
    for (const auto& resource : client.ListResources().resources) {
        if (resource.uri == uri) return resource.mime_type;
    }
    return std::nullopt;
}
// endregion

// region read-resource
std::string FirstText(const ReadResourceResult& result) {
    for (const auto& entry : result.contents) {
        if (const auto* text = std::get_if<TextResourceContents>(&entry)) {
            return text->text;
        }
    }
    return std::string();
}

std::string FirstBlob(const ReadResourceResult& result) {
    for (const auto& entry : result.contents) {
        if (const auto* blob = std::get_if<BlobResourceContents>(&entry)) {
            return blob->blob;
        }
    }
    return std::string();
}

std::string ReadReleaseNotes(McpClient& client) {
    return FirstText(client.ReadResource("docs://release-notes"));
}

std::string ReadUserProfile(McpClient& client, const std::string& user_id) {
    return FirstText(client.ReadResource("docs://users/" + user_id + "/profile"));
}

std::string ReadLogoBytes(McpClient& client) {
    return FirstBlob(client.ReadResource("files://logo.png"));
}
// endregion

// region cache-modes
void ReadBypassingCache(McpClient& client) {
    CacheableRequestOptions bypass;
    bypass.cache_mode = "bypass";
    client.ReadResource("docs://release-notes", bypass);

    CacheableRequestOptions refresh;
    refresh.cache_mode = "refresh";
    client.ReadResource("docs://release-notes", refresh);

    CacheableRequestOptions fresh;
    fresh.max_age_ms = int64_t(5000);
    client.ReadResource("docs://release-notes", fresh);
}
// endregion

// region subscribe-updates
void WatchReleaseNotes(McpClient& client) {
    client.SubscribeResource("docs://release-notes");

    SubscriptionsListenRequestParams params;
    params.notifications.resource_subscriptions = {"docs://release-notes"};
    params.notifications.tools_list_changed = true;
    client.SubscribeAsync(params);
}
// endregion

} // namespace call_tools_and_read_resources
