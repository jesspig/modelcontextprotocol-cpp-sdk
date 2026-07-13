#include <mcp/server/McpServer.hpp>
#include <mcp/McpError.hpp>

#include <asio/post.hpp>
#include <asio/signal_set.hpp>

#include <set>

namespace mcp {

// ====================================================================
// Factory
// ====================================================================
std::unique_ptr<McpServer> McpServer::Create(
    std::unique_ptr<Transport> transport,
    const ServerOptions& options)
{
    return std::unique_ptr<McpServer>(
        new McpServer(std::move(transport), options));
}

McpServer::McpServer(
    std::unique_ptr<Transport> transport,
    ServerOptions options)
    : io_ctx_()
    , transport_(std::move(transport))
    , options_(std::move(options))
{
    protocol_ = std::make_shared<Protocol>(io_ctx_, std::move(transport_));

    // Wire built-in handlers
    WireHandlers();

    // Derive capabilities from registered primitives
    DeriveCapabilities();

    // Negotiate protocol version
    if (options_.protocol_version) {
        protocol_->SetNegotiatedProtocolVersion(*options_.protocol_version);
    }

    // Start the protocol engine
    protocol_->Start();
}

// ====================================================================
// Lifecycle
// ====================================================================
void McpServer::Run() {
    io_ctx_.run();
}

void McpServer::Close() {
    protocol_->Close();
    io_ctx_.stop();
}

// ====================================================================
// Tool registration
// ====================================================================
void McpServer::RegisterTool(std::shared_ptr<McpServerTool> tool) {
    const auto& t = tool->ProtocolTool();
    tools_[t.name] = std::move(tool);
    // Re-wire handlers
    WireHandlers();
    DeriveCapabilities();
}

// ====================================================================
// Resource registration
// ====================================================================
void McpServer::RegisterResource(
    std::string_view name,
    std::string_view uri,
    const ResourceOptions& /*opts*/,
    std::function<ReadResourceResult(const std::string&)> handler)
{
    ResourceEntry entry;
    entry.name = std::string(name);
    entry.uri_pattern = std::string(uri);
    entry.is_template = false;
    entry.handler = std::move(handler);
    resources_.push_back(std::move(entry));
    WireHandlers();
    DeriveCapabilities();
}

void McpServer::RegisterResourceTemplate(
    std::string_view name,
    std::string_view uri_template,
    const ResourceOptions& /*opts*/,
    std::function<ReadResourceResult(
        const std::string&,
        const std::map<std::string, std::string>&)> handler)
{
    ResourceEntry entry;
    entry.name = std::string(name);
    entry.uri_pattern = std::string(uri_template);
    entry.is_template = true;
    entry.template_handler = std::move(handler);
    resources_.push_back(std::move(entry));
    WireHandlers();
    DeriveCapabilities();
}

// ====================================================================
// Prompt registration
// ====================================================================
void McpServer::RegisterPrompt(
    std::string_view name,
    const PromptOptions& /*opts*/,
    std::function<GetPromptResult(
        const std::string&,
        const std::optional<nlohmann::json>&)> handler)
{
    PromptEntry entry;
    entry.name = std::string(name);
    entry.handler = std::move(handler);
    prompts_.push_back(std::move(entry));
    WireHandlers();
    DeriveCapabilities();
}

// ====================================================================
// Notifications
// ====================================================================
void McpServer::SendToolListChanged() {
    protocol_->SendNotification(
        notifications::kToolListChanged, nlohmann::json::object());
}

void McpServer::SendResourceListChanged() {
    protocol_->SendNotification(
        notifications::kResourceListChanged, nlohmann::json::object());
}

void McpServer::SendPromptListChanged() {
    protocol_->SendNotification(
        notifications::kPromptListChanged, nlohmann::json::object());
}

void McpServer::SendLoggingMessage(LoggingLevel level, std::string_view data) {
    nlohmann::json params;
    params["level"] = level;
    params["data"] = data;
    params["logger"] = "mcp-server";
    protocol_->SendNotification(notifications::kMessage, std::move(params));
}

// ====================================================================
// Elicitation
// ====================================================================
std::future<ElicitResult> McpServer::Elicit(const ElicitRequestParams& params) {
    RequestMeta meta;
    auto vers = protocol_->NegotiatedProtocolVersion();
    meta.protocol_version = vers.empty()
        ? std::string(kLatestProtocolVersion) : std::string(vers);

    auto future = protocol_->SendRequest(
        methods::kElicit, nlohmann::json(params), meta, std::chrono::seconds(600));

    // Wrap response
    auto result_future = std::async(std::launch::async, [future = std::move(future)]() mutable {
        auto json = future.get();
        // Check for errors
        if (json.contains("code") && json["code"].get<int32_t>() < 0) {
            throw McpError(
                static_cast<McpErrorCode>(json["code"].get<int32_t>()),
                json.value("message", "elicitation failed"));
        }
        return json.get<ElicitResult>();
    });

    return result_future;
}

// ====================================================================
// Extension registration
// ====================================================================
void McpServer::RegisterExtension(std::shared_ptr<Extension> extension) {
    extensions_.push_back(extension);

    // Register tools from extension
    for (auto& tool : extension->Tools()) {
        RegisterTool(tool);
    }

    // Register methods from extension
    for (auto& [method, handler] : extension->Methods()) {
        protocol_->SetRequestHandler(method,
            [handler](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                handler(req, std::move(p));
            });
    }

    WireHandlers();
    DeriveCapabilities();
}

// ====================================================================
// Handlers auto-wiring
// ====================================================================
void McpServer::WireHandlers() {
    // ── tools/list ──
    if (!tools_.empty()) {
        protocol_->SetRequestHandler(methods::kListTools,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleListTools(req, std::move(p));
            });
    }

    // ── tools/call ──
    protocol_->SetRequestHandler(methods::kCallTool,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleCallTool(req, std::move(p));
        });

    // ── resources/list ──
    if (!resources_.empty() && std::any_of(resources_.begin(), resources_.end(),
            [](const auto& r) { return !r.is_template; })) {
        protocol_->SetRequestHandler(methods::kListResources,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleListResources(req, std::move(p));
            });
    }

    // ── resources/templates/list ──
    if (!resources_.empty() && std::any_of(resources_.begin(), resources_.end(),
            [](const auto& r) { return r.is_template; })) {
        protocol_->SetRequestHandler(methods::kListResourceTemplates,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleListResourceTemplates(req, std::move(p));
            });
    }

    // ── resources/read ──
    if (!resources_.empty()) {
        protocol_->SetRequestHandler(methods::kReadResource,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleReadResource(req, std::move(p));
            });
    }

    // ── prompts/list ──
    if (!prompts_.empty()) {
        protocol_->SetRequestHandler(methods::kListPrompts,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleListPrompts(req, std::move(p));
            });
    }

    // ── prompts/get ──
    protocol_->SetRequestHandler(methods::kGetPrompt,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleGetPrompt(req, std::move(p));
        });

    // ── server/discover ──
    protocol_->SetRequestHandler(methods::kDiscover,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleDiscover(req, std::move(p));
        });

    // ── completion/complete ──
    protocol_->SetRequestHandler(methods::kComplete,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleComplete(req, std::move(p));
        });

    // ── server/extensions/list ──
    protocol_->SetRequestHandler(methods::kListExtensions,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            ExtensionListResult r;
            for (auto& ext : extensions_)
                r.extensions.push_back(ext->Identifier());
            nlohmann::json j = r;
            p.set_value(std::move(j));
        });

    // ── tasks/get, tasks/update, tasks/cancel ──
    auto& store = options_.task_store;
    if (store) {
        protocol_->SetRequestHandler(methods::kGetTask,
            [store](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                GetTaskRequestParams params;
                if (req.params) from_json(*req.params, params);
                auto task = store->GetTask(params.task_id);
                if (!task) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                GetTaskResult r;
                r.task_id = task->task_id;
                r.status = std::to_string(static_cast<int>(task->status));
                r.result = task->result;
                r.error_message = task->error_message;
                r.input_required = task->input_required;
                nlohmann::json j = r;
                p.set_value(std::move(j));
            });

        protocol_->SetRequestHandler(methods::kUpdateTask,
            [store](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                UpdateTaskRequestParams params;
                if (req.params) from_json(*req.params, params);
                store->UpdateTask(params.task_id, params.result);
                UpdateTaskResult r;
                p.set_value(nlohmann::json(r));
            });

        protocol_->SetRequestHandler(methods::kCancelTask,
            [store](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                CancelTaskRequestParams params;
                if (req.params) from_json(*req.params, params);
                store->CancelTask(params.task_id, params.reason);
                CancelTaskResult r;
                p.set_value(nlohmann::json(r));
            });
    }
}

// ====================================================================
// Capability derivation
// ====================================================================
void McpServer::DeriveCapabilities() {
    if (!tools_.empty()) {
        capabilities_.tools = ToolsCapability{};
        capabilities_.tools->list_changed = true;
    }
    if (!resources_.empty()) {
        capabilities_.resources = ResourcesCapability{};
        capabilities_.resources->subscribe = true;
        capabilities_.resources->list_changed = true;
    }
    if (!prompts_.empty()) {
        capabilities_.prompts = PromptsCapability{};
        capabilities_.prompts->list_changed = true;
    }
    if (options_.task_store) {
        capabilities_.tasks = TasksCapability{};
    }
}

// ====================================================================
// Handler implementations
// ====================================================================
void McpServer::HandleListTools(
    const JsonRpcRequest& /*req*/, std::promise<nlohmann::json> promise)
{
    ListToolsResult result;
    for (const auto& [name, tool_ptr] : tools_) {
        result.tools.push_back(tool_ptr->ProtocolTool());
    }
    nlohmann::json j = result;
    promise.set_value(std::move(j));
}

void McpServer::HandleCallTool(
    const JsonRpcRequest& req, std::promise<nlohmann::json> promise)
{
    // Parse params
    CallToolRequestParams params;
    if (req.params) {
        from_json(*req.params, params);
    }

    auto it = tools_.find(params.name);
    if (it == tools_.end()) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidParams,
                     "tool not found: " + params.name)));
        return;
    }

    // Build RequestContext and invoke
    auto ctx = RequestContext<CallToolRequestParams>(
        *this, req, std::move(params), io_ctx_);

    auto tool = it->second;
    std::thread([tool, ctx = std::move(ctx),
                 promise = std::move(promise)]() mutable {
        auto result_promise = std::make_shared<std::promise<CallToolResult>>();
        auto result_future = result_promise->get_future();
        tool->InvokeAsync(ctx, std::move(*result_promise));
        try {
            auto result = result_future.get();
            nlohmann::json j = result;
            promise.set_value(std::move(j));
        } catch (...) {
            promise.set_exception(std::current_exception());
        }
    }).detach();
}

void McpServer::HandleListResources(
    const JsonRpcRequest& /*req*/, std::promise<nlohmann::json> promise)
{
    ListResourcesResult result;
    for (const auto& entry : resources_) {
        if (entry.is_template) continue;
        Resource r;
        r.uri = entry.uri_pattern;
        r.name = entry.name;
        result.resources.push_back(std::move(r));
    }
    nlohmann::json j = result;
    promise.set_value(std::move(j));
}

void McpServer::HandleListResourceTemplates(
    const JsonRpcRequest& /*req*/, std::promise<nlohmann::json> promise)
{
    ListResourceTemplatesResult result;
    for (const auto& entry : resources_) {
        if (!entry.is_template) continue;
        ResourceTemplate rt;
        rt.uri_template = entry.uri_pattern;
        rt.name = entry.name;
        result.resource_templates.push_back(std::move(rt));
    }
    nlohmann::json j = result;
    promise.set_value(std::move(j));
}

void McpServer::HandleReadResource(
    const JsonRpcRequest& req, std::promise<nlohmann::json> promise)
{
    ReadResourceRequestParams params;
    if (req.params) {
        from_json(*req.params, params);
    }

    for (const auto& entry : resources_) {
        if (entry.is_template) continue;
        if (entry.uri_pattern == params.uri) {
            try {
                auto result = entry.handler(params.uri);
                nlohmann::json j = result;
                promise.set_value(std::move(j));
                return;
            } catch (const McpError&) {
                throw;
            } catch (...) {
                promise.set_exception(std::current_exception());
                return;
            }
        }
    }

    promise.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::InvalidParams,
                 "resource not found: " + params.uri)));
}

void McpServer::HandleListPrompts(
    const JsonRpcRequest& /*req*/, std::promise<nlohmann::json> promise)
{
    ListPromptsResult result;
    for (const auto& entry : prompts_) {
        Prompt p;
        p.name = entry.name;
        result.prompts.push_back(std::move(p));
    }
    nlohmann::json j = result;
    promise.set_value(std::move(j));
}

void McpServer::HandleGetPrompt(
    const JsonRpcRequest& req, std::promise<nlohmann::json> promise)
{
    GetPromptRequestParams params;
    if (req.params) {
        from_json(*req.params, params);
    }

    for (const auto& entry : prompts_) {
        if (entry.name == params.name) {
            try {
                auto result = entry.handler(params.name, params.arguments);
                nlohmann::json j = result;
                promise.set_value(std::move(j));
                return;
            } catch (...) {
                promise.set_exception(std::current_exception());
                return;
            }
        }
    }

    promise.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::InvalidParams,
                 "prompt not found: " + params.name)));
}

void McpServer::HandleComplete(
    const JsonRpcRequest& /*req*/, std::promise<nlohmann::json> promise)
{
    CompleteResult result;
    result.completion = nlohmann::json{{"values", nlohmann::json::array()}};
    nlohmann::json j = result;
    promise.set_value(std::move(j));
}

void McpServer::HandleDiscover(
    const JsonRpcRequest& /*req*/, std::promise<nlohmann::json> promise)
{
    DiscoverResult result;
    result.supported_versions = {
        "2025-11-25",
        kLatestProtocolVersion.data()
    };
    result.capabilities = capabilities_;
    result.server_info = Implementation{
        options_.server_info->name,
        options_.server_info->version
    };
    if (options_.server_instructions) {
        result.instructions = options_.server_instructions;
    }
    nlohmann::json j = result;
    promise.set_value(std::move(j));
}

// ====================================================================
// Properties
// ====================================================================
const ClientCapabilities* McpServer::GetClientCapabilities() const {
    return client_capabilities_ ? &*client_capabilities_ : nullptr;
}

const Implementation* McpServer::GetClientInfo() const {
    return client_info_ ? &*client_info_ : nullptr;
}

std::string_view McpServer::GetNegotiatedProtocolVersion() const {
    return protocol_->NegotiatedProtocolVersion();
}

const ServerCapabilities& McpServer::GetCapabilities() const {
    return capabilities_;
}

bool McpServer::IsMrtrSupported() const {
    auto* caps = GetClientCapabilities();
    return caps && caps->elicitation.has_value();
}

} // namespace mcp
