#include <mcp/server/McpServer.hpp>
#include <mcp/McpError.hpp>

#include <asio/post.hpp>
#include <asio/signal_set.hpp>

#include <thread>
#include <set>
#include <algorithm>
#include <mutex>

namespace mcp {

static bool RequireInitialized(bool initialized, std::promise<nlohmann::json>& p) {
    if (!initialized) {
        p.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
        return false;
    }
    return true;
}

// ====================================================================
// Factory
// ====================================================================
std::unique_ptr<McpServer> McpServer::Create(
    std::shared_ptr<ITransport> transport,
    const ServerOptions& options,
    asio::io_context* io_ctx)
{
    return std::unique_ptr<McpServer>(
        new McpServer(std::move(transport), options, io_ctx));
}

McpServer::McpServer(
    std::shared_ptr<ITransport> transport,
    ServerOptions options,
    asio::io_context* external_io_ctx)
    : io_ctx_owner_(external_io_ctx ? nullptr : std::make_unique<asio::io_context>())
    , io_ctx_ptr_(external_io_ctx ? external_io_ctx : io_ctx_owner_.get())
    , transport_(std::move(transport))
    , options_(std::move(options))
{
    auto codec = MakeWireCodec(
        options_.protocol_version.value_or(std::string(kLatestProtocolVersion)));
    handler_ = std::make_shared<McpSessionHandler>(
        transport_, std::move(codec), true);

    // Detect stateless transport
    is_stateless_ = transport_->IsStateless();

    // Wire built-in handlers
    WireHandlers();

    // Derive capabilities from registered primitives
    DeriveCapabilities();

    // Negotiate protocol version
    if (options_.protocol_version) {
        handler_->SetNegotiatedProtocolVersion(*options_.protocol_version);
    }

    // Wire request state verifier if configured
    if (options_.request_state_verifier) {
        handler_->SetRequestStateVerifier(options_.request_state_verifier);
    }

    // Start the session handler
    handler_->Start();
}

// ====================================================================
// Lifecycle
// ====================================================================
void McpServer::Run() {
    io_ctx_ptr_->run();
}

void McpServer::Close() {
    {
        std::lock_guard<std::mutex> lock(pending_async_mutex_);
        for (auto& fut : pending_async_futures_) {
            fut.wait();
        }
        pending_async_futures_.clear();
    }
    handler_->Close();
    io_ctx_ptr_->stop();
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
    handler_->SendNotification(
        notifications::kToolListChanged, nlohmann::json::object());
}

void McpServer::SendResourceListChanged() {
    handler_->SendNotification(
        notifications::kResourceListChanged, nlohmann::json::object());
}

void McpServer::SendPromptListChanged() {
    handler_->SendNotification(
        notifications::kPromptListChanged, nlohmann::json::object());
}

void McpServer::SendLoggingMessage(LoggingLevel level, std::string_view data) {
    nlohmann::json params;
    params["level"] = level;
    params["data"] = data;
    params["logger"] = "mcp-server";
    handler_->SendNotification(notifications::kMessage, std::move(params));
}

void McpServer::SendLoggingMessage(LoggingLevel level, std::string_view data, std::optional<LoggingLevel> min_level) {
    if (min_level && static_cast<int>(level) < static_cast<int>(*min_level))
        return;
    SendLoggingMessage(level, data);
}

// ====================================================================
// Elicitation
// ====================================================================
std::future<ElicitResult> McpServer::Elicit(const ElicitRequestParams& params) {
    RequestMeta meta;
    auto vers = handler_->NegotiatedProtocolVersion();
    meta.protocol_version = vers.empty()
        ? std::string(kLatestProtocolVersion) : std::string(vers);

    auto future = handler_->SendRequest(
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
// Handlers auto-wiring
// ====================================================================
void McpServer::WireHandlers() {
    // ── tools/list ──
    if (!tools_.empty()) {
        handler_->SetRequestHandler(methods::kListTools,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleListTools(req, std::move(p));
            });
    }

    // ── tools/call ──
    handler_->SetRequestHandler(methods::kCallTool,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleCallTool(req, std::move(p));
        });

    // ── resources/list ──
    if (!resources_.empty() && std::any_of(resources_.begin(), resources_.end(),
            [](const auto& r) { return !r.is_template; })) {
        handler_->SetRequestHandler(methods::kListResources,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleListResources(req, std::move(p));
            });
    }

    // ── resources/templates/list ──
    if (!resources_.empty() && std::any_of(resources_.begin(), resources_.end(),
            [](const auto& r) { return r.is_template; })) {
        handler_->SetRequestHandler(methods::kListResourceTemplates,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleListResourceTemplates(req, std::move(p));
            });
    }

    // ── resources/read ──
    if (!resources_.empty()) {
        handler_->SetRequestHandler(methods::kReadResource,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleReadResource(req, std::move(p));
            });
    }

    // ── prompts/list ──
    if (!prompts_.empty()) {
        handler_->SetRequestHandler(methods::kListPrompts,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                HandleListPrompts(req, std::move(p));
            });
    }

    // ── prompts/get ──
    handler_->SetRequestHandler(methods::kGetPrompt,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleGetPrompt(req, std::move(p));
        });

    // ── initialize ──
    handler_->SetRequestHandler(methods::kInitialize,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleInitialize(req, std::move(p));
        });

    // ── server/discover ──
    handler_->SetRequestHandler(methods::kDiscover,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleDiscover(req, std::move(p));
        });

    // ── ping ──
    handler_->SetRequestHandler(methods::kPing,
        [this](const JsonRpcRequest&, std::promise<nlohmann::json> p) {
            if (!RequireInitialized(initialized_, p)) return;
            PingResult r;
            p.set_value(nlohmann::json(r));
        });

    // ── resources/subscribe / unsubscribe (2025-era) ──
    if (!resources_.empty()) {
        handler_->SetRequestHandler(methods::kSubscribeResource,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                if (!RequireInitialized(initialized_, p)) return;
                SubscribeRequestParams params;
                if (req.params) from_json(*req.params, params);
                Subscription sub{params.uri, {}};
                handler_->AddSubscription(sub);
                EmptyResult r;
                p.set_value(nlohmann::json(r));
            });

        handler_->SetRequestHandler(methods::kUnsubscribeResource,
            [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                if (!RequireInitialized(initialized_, p)) return;
                UnsubscribeRequestParams params;
                if (req.params) from_json(*req.params, params);
                handler_->RemoveSubscription(params.uri);
                EmptyResult r;
                p.set_value(nlohmann::json(r));
            });
    }

    // ── server/extensions/list ──
    handler_->SetRequestHandler(methods::kListExtensions,
        [this](const JsonRpcRequest&, std::promise<nlohmann::json> p) {
            if (!RequireInitialized(initialized_, p)) return;
            nlohmann::json j = nlohmann::json::object();
            j["extensions"] = nlohmann::json::array();
            p.set_value(std::move(j));
        });

    // ── notifications/initialized ──
    handler_->SetNotificationHandler(notifications::kInitialized,
        [this](const JsonRpcNotification&) {
            initialized_ = true;
        });

    // ── completion/complete ──
    handler_->SetRequestHandler(methods::kComplete,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleComplete(req, std::move(p));
        });

    // ── tasks/get, tasks/update, tasks/cancel (2026+ only) ──
    auto& store = options_.task_store;
    if (store) {
        handler_->SetRequestHandler(methods::kGetTask,
            [this, store](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                if (handler_->NegotiatedProtocolVersion() < "2026-07-28") {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound, "tasks/get not available in this protocol version")));
                    return;
                }
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

        handler_->SetRequestHandler(methods::kUpdateTask,
            [this, store](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                if (handler_->NegotiatedProtocolVersion() < "2026-07-28") {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound, "tasks/update not available in this protocol version")));
                    return;
                }
                UpdateTaskRequestParams params;
                if (req.params) from_json(*req.params, params);
                store->UpdateTask(params.task_id, params.result);
                UpdateTaskResult r;
                p.set_value(nlohmann::json(r));
            });

        handler_->SetRequestHandler(methods::kCancelTask,
            [this, store](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
                if (handler_->NegotiatedProtocolVersion() < "2026-07-28") {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound, "tasks/cancel not available in this protocol version")));
                    return;
                }
                CancelTaskRequestParams params;
                if (req.params) from_json(*req.params, params);
                store->CancelTask(params.task_id, params.reason);
                CancelTaskResult r;
                p.set_value(nlohmann::json(r));
            });
    }

    // ── subscriptions/listen (2026 era only) ──
    handler_->SetRequestHandler(methods::kSubscribe,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            HandleSubscriptionsListen(req, std::move(p));
        });
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
        capabilities_.extensions = std::map<std::string, nlohmann::json>{};
    }
}

// ====================================================================
// Handler implementations
// ====================================================================
void McpServer::HandleListTools(
    const JsonRpcRequest& /*req*/, std::promise<nlohmann::json> promise)
{
    if (!initialized_) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
        return;
    }
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
    if (!initialized_) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
        return;
    }

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
    auto log_fn = [this](LoggingLevel level, std::string_view data) {
        SendLoggingMessage(level, data);
    };
    auto ctx = RequestContext<CallToolRequestParams>(
        *this, req, std::move(params), *io_ctx_ptr_, std::move(log_fn));

    auto tool = it->second;

    auto captured_promise = std::make_shared<std::promise<nlohmann::json>>(std::move(promise));
    auto fut = std::async(std::launch::async,
        [tool, ctx = std::move(ctx), captured_promise]() mutable {
            auto result_promise = std::make_shared<std::promise<CallToolResult>>();
            auto result_future = result_promise->get_future();
            tool->InvokeAsync(ctx, std::move(*result_promise));
            try {
                auto result = result_future.get();
                nlohmann::json j = result;
                captured_promise->set_value(std::move(j));
            } catch (...) {
                captured_promise->set_exception(std::current_exception());
            }
        });

    // Store future for lifecycle management; clean up completed futures
    std::lock_guard<std::mutex> lock(pending_async_mutex_);
    pending_async_futures_.push_back(fut.share());
    pending_async_futures_.erase(
        std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
            [](const auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
        pending_async_futures_.end());
}

void McpServer::HandleListResources(
    const JsonRpcRequest& /*req*/, std::promise<nlohmann::json> promise)
{
    if (!initialized_) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
        return;
    }
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
    if (!initialized_) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
        return;
    }
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
    if (!initialized_) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
        return;
    }
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
    if (!initialized_) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
        return;
    }
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
    if (!initialized_) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
        return;
    }
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

void McpServer::SetCompletionHandler(CompletionHandler handler) {
    completion_handler_ = std::move(handler);
}

void McpServer::HandleComplete(
    const JsonRpcRequest& req, std::promise<nlohmann::json> promise)
{
    if (!initialized_) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
        return;
    }
    CompleteRequestParams params;
    if (req.params) from_json(*req.params, params);

    if (completion_handler_) {
        try {
            auto result = completion_handler_(params);
            nlohmann::json j = result;
            promise.set_value(std::move(j));
            return;
        } catch (...) {
            promise.set_exception(std::current_exception());
            return;
        }
    }

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
    if (options_.server_info) {
        result.server_info = Implementation{
            options_.server_info->name,
            options_.server_info->version
        };
    } else {
        result.server_info = Implementation{"mcp-server", "0.1.0"};
    }
    if (options_.server_instructions) {
        result.instructions = options_.server_instructions;
    }
    nlohmann::json j = result;
    promise.set_value(std::move(j));
}

void McpServer::HandleInitialize(
    const JsonRpcRequest& req, std::promise<nlohmann::json> promise)
{
    InitializeRequestParams params;
    if (req.params) {
        from_json(*req.params, params);
    }

    // Store client info
    client_capabilities_ = params.capabilities;
    client_info_ = params.client_info;

    // Negotiate protocol version
    if (options_.protocol_version) {
        handler_->SetNegotiatedProtocolVersion(*options_.protocol_version);
    }

    InitializeResult result;
    result.protocol_version = handler_->NegotiatedProtocolVersion().empty()
        ? std::string(kLatestProtocolVersion)
        : std::string(handler_->NegotiatedProtocolVersion());
    result.capabilities = capabilities_;
    if (options_.server_info) {
        result.server_info = Implementation{
            options_.server_info->name,
            options_.server_info->version
        };
    } else {
        result.server_info = Implementation{"mcp-server", "0.1.0"};
    }
    if (options_.server_instructions) {
        result.instructions = options_.server_instructions;
    }
    nlohmann::json j = result;
    promise.set_value(std::move(j));
}

void McpServer::HandleSubscriptionsListen(
    const JsonRpcRequest& req, std::promise<nlohmann::json> promise)
{
    if (handler_->NegotiatedProtocolVersion() < "2026-07-28") {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "subscriptions/listen not available in this protocol version")));
        return;
    }

    SubscriptionsListenRequestParams params;
    if (req.params) from_json(*req.params, params);

    auto meta = handler_->ExtractIncomingMeta(req);

    SubscriptionEntry entry;
    entry.id = std::to_string(
        std::hash<std::string>{}(req.method + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count())));
    entry.filter = std::move(params.notifications);
    entry.created_at = std::chrono::steady_clock::now();

    if (meta.subscription_id) {
        entry.session_id = *meta.subscription_id;
    }

    handler_->AddSubscriptionEntry(std::move(entry));

    nlohmann::json result = nlohmann::json::object();
    promise.set_value(std::move(result));
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
    return handler_->NegotiatedProtocolVersion();
}

const ServerCapabilities& McpServer::GetCapabilities() const {
    return capabilities_;
}

bool McpServer::IsMrtrSupported() const {
    // MRTR requires stateful transport
    if (is_stateless_) return false;
    auto* caps = GetClientCapabilities();
    return caps && caps->elicitation.has_value();
}

} // namespace mcp
