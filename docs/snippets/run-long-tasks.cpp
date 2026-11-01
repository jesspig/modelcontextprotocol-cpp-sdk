#include <mcp/Content.hpp>
#include <mcp/Implementation.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/McpTypes.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/McpTaskStore.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/storage/FileTaskStore.hpp>
#include <mcp/transport/InMemoryTransport.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace mcp::docs {
namespace {

CallToolResult TextResult(const std::string& text) {
    CallToolResult result;
    result.content.push_back(TextContent{"text", text});
    return result;
}

} // namespace

// region task-declaration
using TaskCtx = RequestContext<CallToolRequestParams>;

ToolOptions RenderReportOptions() {
    ToolExecution execution;
    execution.mode = ToolExecutionMode::Task;
    execution.task_support = TaskSupport::Optional;

    JsonValue seconds(JsonValue::object_tag);
    seconds["type"] = JsonValue("integer");
    seconds["description"] = JsonValue("How long the render takes");

    JsonValue properties(JsonValue::object_tag);
    properties["seconds"] = std::move(seconds);

    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);

    ToolOptions options;
    options.Description("Render a report row by row").InputSchema(std::move(schema));
    options.execution = std::move(execution);
    return options;
}
// endregion

// region task-cancellation
CallToolResult RenderReport(const TaskCtx& ctx) {
    int64_t steps = 3;
    if (ctx.Params().arguments) {
        if (auto* value = ctx.Params().arguments->Find("seconds");
            value && value->IsInt()) {
            steps = value->GetInt();
        }
    }

    for (int64_t step = 0; step < steps; ++step) {
        if (ctx.IsCancellationRequested()) {
            return TextResult("stopped before step " + std::to_string(step));
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return TextResult("report rendered");
}
// endregion

// region task-enable
std::unique_ptr<McpServer> CreateTaskServer(std::shared_ptr<ITransport> transport) {
    ServerOptions options;
    options.server_info = Implementation{"long-task-server", "1.0.0"};
    options.enable_tasks_extension = true;
    options.task_store = std::make_shared<FileTaskStore>(
        std::filesystem::temp_directory_path() / "mcp-doc-tasks.json");

    auto server = McpServer::Create(std::move(transport), options);
    server->RegisterTool(
        "render_report",
        RenderReportOptions(),
        std::function<CallToolResult(const TaskCtx&)>(&RenderReport));
    return server;
}

std::unique_ptr<McpClient> CreateTaskClient(std::shared_ptr<ITransport> transport) {
    ClientOptions options;
    options.client_info = Implementation{"long-task-client", "1.0.0"};
    options.connect_mode = ConnectMode::Auto;
    options.enable_tasks_extension = true;
    return McpClient::Create(std::move(transport), options);
}
// endregion

// region task-poll
GetTaskResult PollRenderReport(McpClient& client) {
    JsonValue arguments(JsonValue::object_tag);
    arguments["seconds"] = JsonValue(3);

    GetTaskResult handle = client.CallToolAsTask("render_report", arguments);
    if (handle.status != "working") {
        return handle;
    }
    return client.PollTaskToCompletion(
        handle.task_id, std::chrono::milliseconds(200), std::chrono::seconds(60));
}
// endregion

// region task-cancel
std::string CancelRenderReport(McpClient& client) {
    GetTaskResult handle = client.CallToolAsTask(
        "render_report", JsonValue(JsonValue::object_tag));
    client.CancelTask(handle.task_id, "user asked to stop");

    GetTaskResult settled = client.PollTaskToCompletion(
        handle.task_id, std::chrono::milliseconds(200), std::chrono::seconds(30));
    return settled.status;
}
// endregion

// region task-input-required
CallToolResult CollectName(const TaskCtx& ctx) {
    const std::optional<JsonValue>& responses = ctx.Params().input_responses;
    if (responses) {
        if (auto* entry = responses->Find("user_name"); entry && entry->IsObject()) {
            if (auto* content = entry->Find("content"); content && content->IsObject()) {
                if (auto* name = content->Find("name"); name && name->IsString()) {
                    return TextResult("hello " + name->GetString());
                }
            }
        }
    }

    JsonValue name(JsonValue::object_tag);
    name["type"] = JsonValue("string");

    JsonValue properties(JsonValue::object_tag);
    properties["name"] = std::move(name);

    JsonValue schema(JsonValue::object_tag);
    schema["type"] = JsonValue("object");
    schema["properties"] = std::move(properties);

    ElicitRequestParams elicit;
    elicit.message = "What is your name?";
    elicit.requested_schema = std::move(schema);

    InputRequiredResult pending;
    pending.input_requests["user_name"] = MakeInputRequestForElicitation(elicit);

    CallToolResult result;
    result.input_required = std::move(pending);
    return result;
}

void ResumeTaskWithInput(McpClient& client, const std::string& task_id) {
    JsonValue content(JsonValue::object_tag);
    content["name"] = JsonValue("Jess");

    JsonValue accepted(JsonValue::object_tag);
    accepted["action"] = JsonValue("accept");
    accepted["content"] = std::move(content);

    JsonValue responses(JsonValue::object_tag);
    responses["user_name"] = std::move(accepted);

    client.UpdateTask(task_id, std::nullopt, std::move(responses));
}
// endregion

// region full
void RunLongTaskFlow() {
    auto pair = InMemoryTransport::CreatePair();
    auto server = CreateTaskServer(pair.server);
    std::thread worker([&server] { server->Run(); });

    auto client = CreateTaskClient(pair.client);

    GetTaskResult finished = PollRenderReport(*client);
    std::cout << "polled status: " << finished.status << std::endl;

    GetTaskResult working = client->CallToolAsTask(
        "render_report", JsonValue(JsonValue::object_tag));
    client->CancelTask(working.task_id, "superseded by a newer request");
    GetTaskResult settled = client->PollTaskToCompletion(
        working.task_id, std::chrono::milliseconds(200), std::chrono::seconds(30));
    std::cout << "after cancel: " << settled.status << std::endl;

    client->Close();
    server->Close();
    worker.join();
}
// endregion

} // namespace mcp::docs
