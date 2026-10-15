#include "ConformanceServer.hpp"

#include <mcp/McpError.hpp>
#include <mcp/detail/sha256.hpp>
#include <mcp/storage/FileTaskStore.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace mcp::conformance {
namespace {

constexpr const char* kWeatherSkillName = "conformance-weather";
constexpr const char* kWeatherSkillDescription =
    "Conformance weather skill for SEP-2640 exercised over skill protocol URIs";
constexpr const char* kWeatherSkillBody =
    "Report synthetic weather for a requested city.";
constexpr const char* kWeatherNotesBody = "Celsius unless asked otherwise.";
constexpr const char* kGreeterSkillName = "conformance-greeter";
constexpr const char* kGreeterSkillDescription =
    "Conformance greeter skill for SEP-2640 with a references document";
constexpr const char* kGreeterSkillBody = "Greet the user by name.";
constexpr const char* kGreeterUsageBody = "Say hello, then stop.";
constexpr const char* kDynamicSkillName = "conformance-dynamic";
constexpr const char* kDynamicSkillDescription =
    "Conformance dynamic skill whose files resolve at read time";

std::string HexEncode(std::string_view bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xF]);
    }
    return out;
}

std::string Sha256Digest(std::string_view content)
{
    return "sha256:" + HexEncode(detail::Sha256::Hash(content));
}

std::string RenderSkillFile(std::string_view name,
    std::string_view description, std::string_view body)
{
    std::string file = "---\nname: ";
    file += name;
    file += "\ndescription: ";
    file += description;
    file += "\n---\n";
    file += body;
    file += "\n";
    return file;
}

JsonValue MakeFrontmatter(std::string_view name, std::string_view description)
{
    JsonValue frontmatter(JsonValue::object_tag);
    frontmatter["name"] = JsonValue(std::string(name));
    frontmatter["description"] = JsonValue(std::string(description));
    return frontmatter;
}

SkillResource MakeFileEntry(std::string_view uri, std::string_view content)
{
    SkillResource entry;
    entry.uri = std::string(uri);
    entry.digest = Sha256Digest(content);
    entry.size = static_cast<int64_t>(content.size());
    return entry;
}

ReadResourceResult ServeText(
    std::string_view uri, std::string text, std::string_view mime_type)
{
    TextResourceContents contents;
    contents.uri = std::string(uri);
    contents.mime_type = std::string(mime_type);
    contents.text = std::move(text);
    ReadResourceResult result;
    result.contents.push_back(ResourceContents{std::move(contents)});
    return result;
}

CallToolResult Greet(const ToolContext& ctx)
{
    std::string name = "World";
    if (ctx.Params().arguments) {
        if (auto* value = ctx.Params().arguments->Find("name");
            value && value->IsString()) {
            name = value->GetString();
        }
    }
    return MakeTextResult("Hello, " + name + "!");
}

CallToolResult SlowCompute(const ToolContext& ctx)
{
    int64_t seconds = 0;
    std::string label = "job";
    if (ctx.Params().arguments) {
        if (auto* value = ctx.Params().arguments->Find("seconds");
            value && value->IsInt()) {
            seconds = value->GetInt();
        }
        if (auto* text = ctx.Params().arguments->Find("label");
            text && text->IsString()) {
            label = text->GetString();
        }
    }
    if (seconds < 0) seconds = 0;
    if (seconds > 30) seconds = 30;
    for (int64_t i = 0; i < seconds * 10; ++i) {
        if (ctx.IsCancellationRequested()) return MakeTextResult("cancelled");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return MakeTextResult(
        "computed " + label + " in " + std::to_string(seconds) + "s");
}

CallToolResult FailingJob(const ToolContext&)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    CallToolResult result;
    result.is_error = true;
    result.content.push_back(
        TextContent{"text", "failing_job reported a tool error"});
    return result;
}

CallToolResult ProtocolErrorJob(const ToolContext&)
{
    throw McpError(McpErrorCode::InternalError,
        "protocol_error_job encountered a protocol-level failure");
}

const JsonValue* AcceptedResponseContent(
    const std::optional<JsonValue>& responses, const char* key)
{
    if (!responses) return nullptr;
    auto* entry = responses->Find(key);
    if (!entry || !entry->IsObject()) return nullptr;
    if (auto* action = entry->Find("action")) {
        if (!action->IsString() || std::string(action->GetString()) != "accept") {
            return nullptr;
        }
        auto* content = entry->Find("content");
        return (content && content->IsObject()) ? content : nullptr;
    }
    return entry;
}

JsonValue MakeConfirmRequestedSchema()
{
    JsonValue confirm(JsonValue::object_tag);
    confirm["type"] = JsonValue("boolean");
    JsonValue properties(JsonValue::object_tag);
    properties["confirm"] = std::move(confirm);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    schema["required"] = JsonValue(JsonValue::array_tag);
    schema["required"].PushBack(JsonValue("confirm"));
    return schema;
}

JsonValue MakeNameRequestedSchema()
{
    JsonValue name(JsonValue::object_tag);
    name["type"] = JsonValue("string");
    JsonValue properties(JsonValue::object_tag);
    properties["name"] = std::move(name);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    schema["required"] = JsonValue(JsonValue::array_tag);
    schema["required"].PushBack(JsonValue("name"));
    return schema;
}

InputRequest MakeConfirmDeleteRequest()
{
    ElicitRequestParams params;
    params.message = "Confirm deletion of the requested file";
    params.requested_schema = MakeConfirmRequestedSchema();
    return MakeInputRequestForElicitation(params);
}

InputRequest MakeUserNameRequest(const char* message)
{
    ElicitRequestParams params;
    params.message = message;
    params.requested_schema = MakeNameRequestedSchema();
    return MakeInputRequestForElicitation(params);
}

CallToolResult ConfirmDelete(const ToolContext& ctx)
{
    std::string filename = "file";
    if (ctx.Params().arguments) {
        if (auto* value = ctx.Params().arguments->Find("filename");
            value && value->IsString()) {
            filename = value->GetString();
        }
    }
    if (auto* content = AcceptedResponseContent(
            ctx.Params().input_responses, "confirm_delete")) {
        if (auto* confirm = content->Find("confirm");
            confirm && confirm->IsBool() && confirm->GetBool()) {
            return MakeTextResult("Deleted " + filename);
        }
    }
    CallToolResult result;
    InputRequiredResult ir;
    ir.input_requests["confirm_delete"] = MakeConfirmDeleteRequest();
    result.input_required = std::move(ir);
    return result;
}

CallToolResult MultiInput(const ToolContext& ctx)
{
    const auto& responses = ctx.Params().input_responses;
    auto* first = AcceptedResponseContent(responses, "first");
    auto* second = AcceptedResponseContent(responses, "second");
    auto* first_name = first ? first->Find("name") : nullptr;
    auto* second_name = second ? second->Find("name") : nullptr;
    if (first_name && first_name->IsString() && second_name && second_name->IsString()) {
        return MakeTextResult("Confirmed both inputs: " + first_name->GetString()
            + ", " + second_name->GetString());
    }
    CallToolResult result;
    InputRequiredResult ir;
    ir.input_requests["first"] = MakeUserNameRequest("First input: what is your name?");
    ir.input_requests["second"] = MakeUserNameRequest("Second input: what is your name?");
    result.input_required = std::move(ir);
    return result;
}

CallToolResult TestToolWithTask(const ToolContext& ctx)
{
    auto* content = AcceptedResponseContent(
        ctx.Params().input_responses, "user_name");
    if (content) {
        if (auto* name = content->Find("name"); name && name->IsString()) {
            return MakeTextResult("Hello, " + name->GetString() + "!");
        }
    }
    CallToolResult result;
    InputRequiredResult ir;
    ir.input_requests["user_name"] = MakeUserNameRequest("What is your name?");
    result.input_required = std::move(ir);
    return result;
}

JsonValue MakeNameSchema()
{
    JsonValue name(JsonValue::object_tag);
    name["type"] = JsonValue("string");
    name["description"] = JsonValue("Name to greet");
    JsonValue properties(JsonValue::object_tag);
    properties["name"] = std::move(name);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    return schema;
}

JsonValue MakeSlowComputeSchema()
{
    JsonValue seconds(JsonValue::object_tag);
    seconds["type"] = JsonValue("integer");
    seconds["description"] = JsonValue("Seconds to compute");
    JsonValue label(JsonValue::object_tag);
    label["type"] = JsonValue("string");
    label["description"] = JsonValue("Label for the computation");
    JsonValue properties(JsonValue::object_tag);
    properties["seconds"] = std::move(seconds);
    properties["label"] = std::move(label);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    return schema;
}

JsonValue MakeFilenameSchema()
{
    JsonValue filename(JsonValue::object_tag);
    filename["type"] = JsonValue("string");
    filename["description"] = JsonValue("File to delete");
    JsonValue properties(JsonValue::object_tag);
    properties["filename"] = std::move(filename);
    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);
    return schema;
}

ToolOptions MakeTaskToolOptions()
{
    ToolExecution execution;
    execution.mode = ToolExecutionMode::Task;
    ToolOptions options;
    options.execution = execution;
    return options;
}

ToolOptions MakeRequiredTaskToolOptions()
{
    ToolOptions options = MakeTaskToolOptions();
    options.execution->task_support = TaskSupport::Required;
    return options;
}

} // namespace

void RegisterConformanceSkills(McpServer& server)
{
    const std::string weather_uri =
        std::string("skill://") + kWeatherSkillName + "/SKILL.md";
    const std::string weather_file = RenderSkillFile(
        kWeatherSkillName, kWeatherSkillDescription, kWeatherSkillBody);
    const std::string weather_notes_uri =
        std::string("skill://") + kWeatherSkillName + "/references/notes.md";
    const std::string greeter_uri =
        std::string("skill://") + kGreeterSkillName + "/SKILL.md";
    const std::string greeter_file = RenderSkillFile(
        kGreeterSkillName, kGreeterSkillDescription, kGreeterSkillBody);
    const std::string greeter_usage_uri =
        std::string("skill://") + kGreeterSkillName + "/references/usage.md";
    const std::string dynamic_uri =
        std::string("skill://") + kDynamicSkillName + "/SKILL.md";

    Skill weather;
    weather.uri = weather_uri;
    weather.frontmatter =
        MakeFrontmatter(kWeatherSkillName, kWeatherSkillDescription);
    weather.resources = std::vector<SkillResource>{
        MakeFileEntry(weather_uri, weather_file),
        MakeFileEntry(weather_notes_uri, kWeatherNotesBody)};
    server.RegisterSkill(std::move(weather));

    Skill greeter;
    greeter.uri = greeter_uri;
    greeter.frontmatter =
        MakeFrontmatter(kGreeterSkillName, kGreeterSkillDescription);
    greeter.resources = std::vector<SkillResource>{
        MakeFileEntry(greeter_uri, greeter_file),
        MakeFileEntry(greeter_usage_uri, kGreeterUsageBody)};
    server.RegisterSkill(std::move(greeter));

    Skill dynamic;
    dynamic.uri = dynamic_uri;
    dynamic.frontmatter =
        MakeFrontmatter(kDynamicSkillName, kDynamicSkillDescription);
    dynamic.resources = std::string("dynamic");
    server.RegisterSkill(std::move(dynamic));

    server.SetSkillsDirectoryRead(true);

    server.RegisterResource(kWeatherSkillName, weather_uri,
        ResourceOptions{}
            .Title(kWeatherSkillName)
            .Description(kWeatherSkillDescription)
            .MimeType("text/markdown"),
        [weather_file, weather_uri](const std::string&) {
            return ServeText(weather_uri, weather_file, "text/markdown");
        });

    server.RegisterResource(kGreeterSkillName, greeter_uri,
        ResourceOptions{}
            .Title(kGreeterSkillName)
            .Description(kGreeterSkillDescription)
            .MimeType("text/markdown"),
        [greeter_file, greeter_uri](const std::string&) {
            return ServeText(greeter_uri, greeter_file, "text/markdown");
        });

    server.RegisterResource("conformance-weather-notes", weather_notes_uri,
        ResourceOptions{}
            .Title("Weather notes")
            .Description("Supporting references document for the weather skill")
            .MimeType("text/plain"),
        [weather_notes_uri](const std::string&) {
            return ServeText(
                weather_notes_uri, kWeatherNotesBody, "text/plain");
        });

    server.RegisterResource("conformance-greeter-usage", greeter_usage_uri,
        ResourceOptions{}
            .Title("Greeter usage")
            .Description("Supporting references document for the greeter skill")
            .MimeType("text/plain"),
        [greeter_usage_uri](const std::string&) {
            return ServeText(
                greeter_usage_uri, kGreeterUsageBody, "text/plain");
        });
}

void RegisterConformanceTaskTools(McpServer& server)
{
    server.RegisterTool("greet",
        ToolOptions{}
            .Description("SEP-2663: sync-only tool returning a greeting")
            .InputSchema(MakeNameSchema()),
        std::function<CallToolResult(const ToolContext&)>(&Greet));

    server.RegisterTool("slow_compute",
        MakeTaskToolOptions()
            .Description("SEP-2663: task-supporting tool sleeping seconds")
            .InputSchema(MakeSlowComputeSchema()),
        std::function<CallToolResult(const ToolContext&)>(&SlowCompute));

    server.RegisterTool("failing_job",
        MakeRequiredTaskToolOptions().Description(
            "SEP-2663: task-supporting tool always reporting a tool error"),
        std::function<CallToolResult(const ToolContext&)>(&FailingJob));

    server.RegisterTool("protocol_error_job",
        MakeTaskToolOptions().Description(
            "SEP-2663: task-supporting tool panicking into a protocol error"),
        std::function<CallToolResult(const ToolContext&)>(&ProtocolErrorJob));

    server.RegisterTool("confirm_delete",
        MakeTaskToolOptions()
            .Description("SEP-2663: task-supporting tool emitting a single "
                "elicitation/create inputRequest then completing on response")
            .InputSchema(MakeFilenameSchema()),
        std::function<CallToolResult(const ToolContext&)>(&ConfirmDelete));

    server.RegisterTool("multi_input",
        MakeTaskToolOptions().Description(
            "SEP-2663: task-supporting tool fanning out two elicitation/create "
            "inputRequests in parallel for the partial-fulfillment check"),
        std::function<CallToolResult(const ToolContext&)>(&MultiInput));

    server.RegisterTool("test_tool_with_task",
        MakeRequiredTaskToolOptions().Description(
            "SEP-2663: MRTR loop gathering user_name then escalating to async "
            "with the gathered name reflected in the final task result"),
        std::function<CallToolResult(const ToolContext&)>(&TestToolWithTask));
}

std::shared_ptr<IMcpTaskStore> MakeConformanceTaskStore()
{
    auto path =
        std::filesystem::temp_directory_path() / "mcp-conformance-tasks.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return std::make_shared<FileTaskStore>(path);
}

} // namespace mcp::conformance
