#include "ConformanceServer.hpp"

#include <functional>
#include <map>
#include <string>

namespace mcp::conformance {
namespace {

ReadResourceResult StaticText(const std::string& uri)
{
    TextResourceContents contents;
    contents.uri = uri;
    contents.mime_type = "text/plain";
    contents.text = "This is the content of the static text resource.";
    ReadResourceResult result;
    result.contents.push_back(ResourceContents{std::move(contents)});
    return result;
}

ReadResourceResult StaticBinary(const std::string& uri)
{
    BlobResourceContents contents;
    contents.uri = uri;
    contents.mime_type = "image/png";
    contents.blob = kTestImageBase64;
    ReadResourceResult result;
    result.contents.push_back(ResourceContents{std::move(contents)});
    return result;
}

ReadResourceResult Template(const std::string& uri,
    const std::map<std::string, std::string>& vars)
{
    std::string id;
    if (auto it = vars.find("id"); it != vars.end()) {
        id = it->second;
    }
    TextResourceContents contents;
    contents.uri = uri;
    contents.mime_type = "application/json";
    contents.text = R"({"id":")" + id + R"(","templateTest":true,"data":"Data for ID: )" + id + R"("})";
    ReadResourceResult result;
    result.contents.push_back(ResourceContents{std::move(contents)});
    return result;
}

ReadResourceResult WatchedResource(const std::string& uri)
{
    TextResourceContents contents;
    contents.uri = uri;
    contents.mime_type = "text/plain";
    contents.text = kWatchedResourceContent;
    ReadResourceResult result;
    result.contents.push_back(ResourceContents{std::move(contents)});
    return result;
}

GetPromptResult SimplePrompt(const std::string&, const std::optional<JsonValue>&)
{
    GetPromptResult result;
    PromptMessage message;
    message.role = "user";
    message.content = TextContent{"text", "This is a simple prompt for testing."};
    result.messages.push_back(std::move(message));
    return result;
}

GetPromptResult PromptWithArguments(const std::string&, const std::optional<JsonValue>& args)
{
    std::string arg1;
    std::string arg2;
    if (args) {
        if (auto* value = args->Find("arg1"); value && value->IsString()) arg1 = value->GetString();
        if (auto* value = args->Find("arg2"); value && value->IsString()) arg2 = value->GetString();
    }
    GetPromptResult result;
    PromptMessage message;
    message.role = "user";
    message.content = TextContent{"text",
        "Prompt with arguments: arg1='" + arg1 + "', arg2='" + arg2 + "'"};
    result.messages.push_back(std::move(message));
    return result;
}

GetPromptResult PromptWithEmbeddedResource(const std::string&,
    const std::optional<JsonValue>& args)
{
    std::string resource_uri;
    if (args) {
        if (auto* value = args->Find("resourceUri"); value && value->IsString()) {
            resource_uri = value->GetString();
        }
    }
    TextResourceContents contents;
    contents.uri = resource_uri;
    contents.mime_type = "text/plain";
    contents.text = "Embedded resource content for testing.";
    EmbeddedResource embedded;
    embedded.resource = ResourceContents{std::move(contents)};

    PromptMessage resource_message;
    resource_message.role = "user";
    resource_message.content = std::move(embedded);

    PromptMessage text_message;
    text_message.role = "user";
    text_message.content = TextContent{"text", "Please process the embedded resource above."};

    GetPromptResult result;
    result.messages.push_back(std::move(resource_message));
    result.messages.push_back(std::move(text_message));
    return result;
}

GetPromptResult InputRequiredResultPrompt(const std::string&,
    const std::optional<JsonValue>&)
{
    GetPromptResult result;
    PromptMessage message;
    message.role = "user";
    message.content = TextContent{"text",
        "V7 placeholder: input_required_result prompt"};
    result.messages.push_back(std::move(message));
    return result;
}

GetPromptResult PromptWithImage(const std::string&, const std::optional<JsonValue>&)
{
    PromptMessage image_message;
    image_message.role = "user";
    image_message.content = ImageContent{"image", kTestImageBase64, "image/png"};

    PromptMessage text_message;
    text_message.role = "user";
    text_message.content = TextContent{"text", "Please analyze the image above."};

    GetPromptResult result;
    result.messages.push_back(std::move(image_message));
    result.messages.push_back(std::move(text_message));
    return result;
}

} // namespace

void RegisterConformanceResources(McpServer& server)
{
    server.RegisterResource("static-text", "test://static-text",
        ResourceOptions{}
            .Title("Static Text Resource")
            .Description("A static text resource for testing")
            .MimeType("text/plain"),
        &StaticText);

    server.RegisterResource("static-binary", "test://static-binary",
        ResourceOptions{}
            .Title("Static Binary Resource")
            .Description("A static binary resource (image) for testing")
            .MimeType("image/png"),
        &StaticBinary);

    server.RegisterResourceTemplate("template", "test://template/{id}/data",
        ResourceOptions{}
            .Title("Resource Template")
            .Description("A resource template with parameter substitution")
            .MimeType("application/json"),
        &Template);

    server.RegisterResource("watched-resource", "test://watched-resource",
        ResourceOptions{}
            .Title("Watched Resource")
            .Description("Static resource registered for subscribe/unsubscribe testing")
            .MimeType("text/plain"),
        &WatchedResource);
}

void RegisterConformancePrompts(McpServer& server)
{
    server.RegisterPrompt("test_simple_prompt",
        PromptOptions{}
            .Title("Simple Test Prompt")
            .Description("A simple prompt without arguments"),
        &SimplePrompt);

    server.RegisterPrompt("test_prompt_with_arguments",
        PromptOptions{}
            .Title("Prompt With Arguments")
            .Description("A prompt with required arguments"),
        &PromptWithArguments);

    server.RegisterPrompt("test_prompt_with_embedded_resource",
        PromptOptions{}
            .Title("Prompt With Embedded Resource")
            .Description("A prompt that includes an embedded resource"),
        &PromptWithEmbeddedResource);

    server.RegisterPrompt("test_input_required_result_prompt",
        PromptOptions{}
            .Title("MRTR Prompt")
            .Description("MRTR (SEP-2322): prompt that requires elicitation input before rendering"),
        &InputRequiredResultPrompt);

    server.RegisterPrompt("test_prompt_with_image",
        PromptOptions{}
            .Title("Prompt With Image")
            .Description("A prompt that includes image content"),
        &PromptWithImage);
}

}
