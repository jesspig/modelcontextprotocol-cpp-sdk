#include <mcp/client/McpClient.hpp>
#include <mcp/McpError.hpp>

#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <thread>

namespace mcp {

// ====================================================================
// VersionNegotiation implementation
// ====================================================================
NegotiationResult VersionNegotiation::Negotiate(
    McpSessionHandler& handler, const ClientOptions& options)
{
    if (options.connect_mode == ConnectMode::Legacy) {
        // Force legacy initialize handshake
        auto init = HandshakeInitialize(
            handler, options.client_info, options.capabilities,
            options.initialization_timeout);
        NegotiationResult result;
        result.is_modern = false;
        result.negotiated_version = init.protocol_version;
        result.initialize = std::move(init);
        result.capabilities = result.initialize->capabilities;
        result.server_info = result.initialize->server_info;
        result.instructions = result.initialize->instructions;
        handler.SetNegotiatedProtocolVersion(result.negotiated_version);
        return result;
    }

    if (options.connect_mode == ConnectMode::Pin) {
        // Pin to specific version — skip negotiation, assume modern
        NegotiationResult result;
        result.is_modern = true;
        result.negotiated_version = options.pin_protocol_version.value_or(
            std::string(kLatestProtocolVersion));
        handler.SetNegotiatedProtocolVersion(result.negotiated_version);

        DiscoverResult disc;
        disc.supported_versions = {result.negotiated_version};
        disc.capabilities = ServerCapabilities{};
        disc.server_info = options.client_info;
        result.discover = std::move(disc);
        return result;
    }

    // Auto mode: probe server/discover, fallback to initialize
    auto discover = ProbeDiscover(
        handler, std::string(kLatestProtocolVersion),
        options.discover_probe_timeout, options);

    if (discover.has_value()) {
        NegotiationResult result;
        result.is_modern = true;
        result.discover = std::move(discover);
        result.negotiated_version = kLatestProtocolVersion.data();
        result.capabilities = result.discover->capabilities;
        result.server_info = result.discover->server_info;
        result.instructions = result.discover->instructions;
        handler.SetNegotiatedProtocolVersion(result.negotiated_version);
        return result;
    }

    // Fallback to initialize
    auto init = HandshakeInitialize(
        handler, options.client_info, options.capabilities,
        options.initialization_timeout);
    NegotiationResult result;
    result.is_modern = false;
    result.negotiated_version = init.protocol_version;
    result.initialize = std::move(init);
    result.capabilities = result.initialize->capabilities;
    result.server_info = result.initialize->server_info;
    result.instructions = result.initialize->instructions;
    handler.SetNegotiatedProtocolVersion(result.negotiated_version);
    return result;
}

std::optional<DiscoverResult> VersionNegotiation::ProbeDiscover(
    McpSessionHandler& handler,
    std::string_view preferred_version,
    std::chrono::seconds timeout,
    const ClientOptions& options)
{
    RequestMeta meta;
    meta.protocol_version = std::string(preferred_version);
    meta.client_info = options.client_info;
    meta.client_capabilities = options.capabilities.value_or(ClientCapabilities{});
    if (options.extensions) {
        meta.client_capabilities->extensions = options.extensions;
    }

    auto future = handler.SendRequest(
        methods::kDiscover, nlohmann::json::object(), meta, timeout);

    if (future.wait_for(timeout) == std::future_status::timeout) {
        return std::nullopt;
    }

    try {
        auto result = future.get();
        // Check for protocol errors in result
        if (result.contains("code") &&
            result["code"].get<int32_t>() < 0) {
            return std::nullopt;
        }
        return result.get<DiscoverResult>();
    } catch (...) {
        return std::nullopt;
    }
}

InitializeResult VersionNegotiation::HandshakeInitialize(
    McpSessionHandler& handler,
    const Implementation& client_info,
    const std::optional<ClientCapabilities>& capabilities,
    std::chrono::seconds timeout)
{
    InitializeRequestParams params;
    params.protocol_version = "2025-11-25";
    params.client_info = client_info;
    params.capabilities = capabilities.value_or(ClientCapabilities{});

    auto future = handler.SendRequest(
        methods::kInitialize, nlohmann::json(params), {}, timeout);

    auto result = future.get();

    handler.SendNotification(notifications::kInitialized, nlohmann::json::object());

    return result.get<InitializeResult>();
}

// ====================================================================
// McpClient construction / destruction
// ====================================================================
McpClient::McpClient(
    std::shared_ptr<ITransport> transport,
    ClientOptions options)
    : transport_(std::move(transport))
    , options_(std::move(options))
{
    auto codec = MakeWireCodec(std::string(kLatestProtocolVersion));
    handler_ = std::make_shared<McpSessionHandler>(
        transport_, std::move(codec), false);
    handler_->Start();
    WireClientHandlers();
}

McpClient::~McpClient() {
    Close();
}

std::unique_ptr<McpClient> McpClient::Create(
    std::shared_ptr<ITransport> transport,
    const ClientOptions& options)
{
    auto client = std::unique_ptr<McpClient>(
        new McpClient(std::move(transport), options));
    client->negotiation_ = client->NegotiateProtocol();
    return client;
}

// ====================================================================
// Negotiation
// ====================================================================
NegotiationResult McpClient::NegotiateProtocol() {
    return VersionNegotiation::Negotiate(*handler_, options_);
}

// ====================================================================
// Close
// ====================================================================
void McpClient::Close() {
    if (handler_) handler_->Close();
}

// ====================================================================
// Wire client-side handlers
// ====================================================================
void McpClient::WireClientHandlers() {
    // Sampling handler (deprecated)
    handler_->SetRequestHandler(methods::kCreateMessage,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            if (!sampling_handler_) {
                JsonRpcErrorResponse err;
                err.id = req.id;
                err.error.code = McpErrorCode::MethodNotFound;
                err.error.message = "sampling not supported";
                p.set_value(nlohmann::json(err));
                return;
            }
            CreateMessageRequestParams params;
            if (req.params) from_json(*req.params, params);
            try {
                auto result = (*sampling_handler_)(params);
                p.set_value(nlohmann::json(result));
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        });

    // Roots handler (deprecated)
    handler_->SetRequestHandler(methods::kListRoots,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            (void)req;
            if (!roots_handler_) {
                ListRootsResult empty;
                p.set_value(nlohmann::json(empty));
                return;
            }
            try {
                auto result = (*roots_handler_)(ListRootsRequestParams{});
                p.set_value(nlohmann::json(result));
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        });

    // Elicitation handler
    handler_->SetRequestHandler(methods::kElicit,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
            if (!elicitation_handler_) {
                JsonRpcErrorResponse err;
                err.id = req.id;
                err.error.code = McpErrorCode::MethodNotFound;
                err.error.message = "elicitation not supported";
                p.set_value(nlohmann::json(err));
                return;
            }
            ElicitRequestParams params;
            if (req.params) from_json(*req.params, params);
            try {
                auto result = (*elicitation_handler_)(params);
                p.set_value(nlohmann::json(result));
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        });

    // Notification handlers
    for (auto& notif_pair : notif_handlers_) {
        auto method = notif_pair.first;
        auto& notif_handler = notif_pair.second;
        auto nh = notif_handler;
        handler_->SetNotificationHandler(method,
            [nh](const JsonRpcNotification& notif) {
                nh(notif);
            });
    }
}

// ====================================================================
// Properties
// ====================================================================
const ServerCapabilities& McpClient::GetServerCapabilities() const {
    return negotiation_.capabilities;
}

const Implementation& McpClient::GetServerInfo() const {
    return negotiation_.server_info;
}

std::optional<std::string> McpClient::GetInstructions() const {
    return negotiation_.instructions;
}

std::string_view McpClient::GetNegotiatedProtocolVersion() const {
    return negotiation_.negotiated_version;
}

bool McpClient::IsModernProtocol() const {
    return negotiation_.is_modern;
}

// ====================================================================
// Client handlers
// ====================================================================
void McpClient::SetSamplingHandler(SamplingHandler handler) {
    sampling_handler_ = std::move(handler);
}

void McpClient::SetRootsHandler(RootsHandler handler) {
    roots_handler_ = std::move(handler);
}

void McpClient::SetElicitationHandler(ElicitationHandler handler) {
    elicitation_handler_ = std::move(handler);
}

void McpClient::SetNotificationHandler(
    std::string_view method, ClientNotificationHandler handler)
{
    notif_handlers_.emplace_back(std::string(method), std::move(handler));
}

// ====================================================================
// Tool cache
// ====================================================================
void McpClient::AddKnownTools(const std::vector<Tool>& tools) {
    for (const auto& t : tools) {
        known_tools_[t.name] = McpClientTool::FromProtocol(t);
    }
}

void McpClient::RemoveKnownTools(const std::vector<std::string>& names) {
    for (const auto& name : names) {
        known_tools_.erase(std::string(name));
    }
}

void McpClient::ClearKnownTools() {
    known_tools_.clear();
}

// ====================================================================
// Request helpers
// ====================================================================
template <typename TParams, typename TResult>
TResult McpClient::SendRequest(
    std::string_view method,
    TParams params,
    std::chrono::milliseconds timeout)
{
    nlohmann::json params_json;
    to_json(params_json, params);

    RequestMeta meta;
    meta.protocol_version = negotiation_.negotiated_version;
    meta.client_info = options_.client_info;
    if (options_.capabilities) {
        meta.client_capabilities = options_.capabilities;
    }
    if (options_.extensions) {
        if (!meta.client_capabilities) meta.client_capabilities = ClientCapabilities{};
        meta.client_capabilities->extensions = options_.extensions;
    }

    auto future = handler_->SendRequest(method, params_json, meta, timeout);
    auto result_json = future.get();

    // Check for error
    if (result_json.contains("code") && result_json["code"].get<int32_t>() < 0) {
        throw McpError(
            static_cast<McpErrorCode>(result_json["code"].get<int32_t>()),
            result_json.value("message", "request failed"));
    }

    TResult result;
    from_json(result_json, result);
    return result;
}

// ====================================================================
// MRTR helper: attempt to fulfill input_required responses via elicitation handler
// ====================================================================
static bool TryFulfillInputRequired(
    const nlohmann::json& result_json,
    const ClientOptions& options,
    const std::optional<std::function<ElicitResult(const ElicitRequestParams&)>>& elicitation_handler,
    nlohmann::json& out_input_responses,
    std::optional<std::string>& out_request_state)
{
    auto rt = result_json.find("resultType");
    if (rt == result_json.end() || rt->get<std::string>() != "input_required") {
        return false;
    }

    if (!options.input_required_config || !options.input_required_config->auto_fulfill) {
        return false;
    }

    InputRequiredResult input_req;
    from_json(result_json, input_req);

    if (!input_req.input_requests.elicit && !input_req.input_requests.confirm) {
        return false;
    }

    nlohmann::json responses = nlohmann::json::object();

    if (input_req.input_requests.elicit && elicitation_handler) {
        auto& elicit_req = *input_req.input_requests.elicit;
        ElicitRequestParams ep;
        ep.message = elicit_req.message;
        ep.requested_schema = elicit_req.requested_schema;
        auto elicit_result = (*elicitation_handler)(ep);
        if (elicit_result.values)
            responses["elicit"] = *elicit_result.values;
    }

    if (input_req.input_requests.confirm && elicitation_handler) {
        auto& confirm_req = *input_req.input_requests.confirm;
        ElicitRequestParams ep;
        ep.message = confirm_req.message;
        ep.requested_schema = confirm_req.requested_schema;
        auto confirm_result = (*elicitation_handler)(ep);
        if (confirm_result.values)
            responses["confirm"] = *confirm_result.values;
    }

    out_input_responses = std::move(responses);
    out_request_state = input_req.request_state;
    return true;
}

nlohmann::json McpClient::SendRequestWithMrtr(
    std::string_view method,
    nlohmann::json params_json,
    const RequestMeta& meta,
    std::chrono::milliseconds timeout)
{
    auto& cfg = options_.input_required_config;
    int max_rounds = cfg ? cfg->max_rounds : 0;
    auto effective_timeout = cfg ? cfg->round_timeout : timeout;

    for (int round = 0; round <= max_rounds; ++round) {
        auto future = handler_->SendRequest(method, params_json, meta, effective_timeout);
        auto result_json = future.get();

        // Check for protocol errors
        if (result_json.contains("code") && result_json["code"].get<int32_t>() < 0) {
            throw McpError(
                static_cast<McpErrorCode>(result_json["code"].get<int32_t>()),
                result_json.value("message", "request failed"));
        }

        // Check for input_required (MRTR)
        nlohmann::json input_responses;
        std::optional<std::string> request_state;
        if (TryFulfillInputRequired(result_json, options_, elicitation_handler_,
                input_responses, request_state)) {
            // Inject input_responses for next round
            params_json["inputResponses"] = std::move(input_responses);
            if (request_state) params_json["requestState"] = *request_state;
            continue;
        }

        // Complete result or non-MRTR — return raw JSON
        return result_json;
    }

    throw McpError(McpErrorCode::InternalError,
        "MRTR: exceeded max_rounds (" + std::to_string(max_rounds) + ")");
}

// ====================================================================
// Tools
// ====================================================================
ListToolsResult McpClient::ListTools(
    const CacheableRequestOptions& options,
    std::optional<std::string> cursor)
{
    (void)options;
    ListToolsRequestParams params;
    params.cursor = std::move(cursor);
    return SendRequest<ListToolsRequestParams, ListToolsResult>(
        methods::kListTools, params);
}

CallToolResult McpClient::CallTool(
    std::string_view name,
    std::optional<nlohmann::json> arguments,
    const RequestOptions& options)
{
    CallToolRequestParams params;
    params.name = std::string(name);
    params.arguments = std::move(arguments);

    auto& cfg = options_.input_required_config;
    int max_rounds = cfg ? cfg->max_rounds : 0;
    auto round_timeout = cfg ? cfg->round_timeout : std::chrono::seconds(600);
    bool auto_fulfill = cfg ? cfg->auto_fulfill : false;

    // Send with meta
    RequestMeta meta;
    meta.protocol_version = negotiation_.negotiated_version;
    meta.client_info = options_.client_info;
    if (options_.capabilities) meta.client_capabilities = options_.capabilities;
    if (options_.extensions) {
        if (!meta.client_capabilities) meta.client_capabilities = ClientCapabilities{};
        meta.client_capabilities->extensions = options_.extensions;
    }
    if (options.meta) meta.extensions = options.meta;

    for (int round = 0; round <= max_rounds; ++round) {
        // Build request params JSON manually so we can inject input_responses
        nlohmann::json req_json;
        req_json["name"] = params.name;
        if (params.arguments) req_json["arguments"] = *params.arguments;
        if (params.input_responses) req_json["inputResponses"] = *params.input_responses;
        if (params.request_state)   req_json["requestState"] = *params.request_state;

        auto future = handler_->SendRequest(
            methods::kCallTool, req_json, meta, round_timeout);
        auto result_json = future.get();

        // Check for protocol errors
        if (result_json.contains("code") && result_json["code"].get<int32_t>() < 0) {
            throw McpError(
                static_cast<McpErrorCode>(result_json["code"].get<int32_t>()),
                result_json.value("message", "request failed"));
        }

        // Check for input_required (MRTR)
        if (auto rt = result_json.find("resultType"); rt != result_json.end()
            && rt->get<std::string>() == "input_required" && auto_fulfill)
        {
            // Parse InputRequiredResult
            InputRequiredResult input_req;
            from_json(result_json, input_req);

            if (!input_req.input_requests.elicit && !input_req.input_requests.confirm) {
                // No actionable requests — stop the loop
                CallToolResult fallback;
                from_json(result_json, fallback);
                return fallback;
            }

            // Fulfill each request
            nlohmann::json responses = nlohmann::json::object();

            if (input_req.input_requests.elicit && elicitation_handler_) {
                auto& elicit_req = *input_req.input_requests.elicit;
                ElicitRequestParams ep;
                ep.message = elicit_req.message;
                ep.requested_schema = elicit_req.requested_schema;
                auto elicit_result = (*elicitation_handler_)(ep);
                if (elicit_result.values)
                    responses["elicit"] = *elicit_result.values;
            }

            if (input_req.input_requests.confirm && elicitation_handler_) {
                auto& confirm_req = *input_req.input_requests.confirm;
                ElicitRequestParams ep;
                ep.message = confirm_req.message;
                ep.requested_schema = confirm_req.requested_schema;
                auto confirm_result = (*elicitation_handler_)(ep);
                if (confirm_result.values)
                    responses["confirm"] = *confirm_result.values;
            }

            // Set up retry with responses
            params.input_responses = std::move(responses);
            params.request_state = input_req.request_state;
            continue;
        }

        // Check for task result type (扩展任务)
        if (auto rt = result_json.find("resultType"); rt != result_json.end()
            && rt->get<std::string>() == "task")
        {
            auto task_id = result_json["taskId"].get<std::string>();
            auto task_result = PollTaskToCompletionAsync(task_id);

            if (task_result.status == "failed") {
                throw McpError(McpErrorCode::InternalError,
                    "Task failed: " + task_id +
                    (task_result.error_message ? ": " + *task_result.error_message : ""));
            }

            if (task_result.status == "cancelled") {
                throw McpError(McpErrorCode::InternalError,
                    "Task cancelled: " + task_id);
            }

            CallToolResult result;
            if (task_result.result) {
                from_json(*task_result.result, result);
            }
            return result;
        }

        // Complete result — deserialize and return
        CallToolResult result;
        from_json(result_json, result);
        return result;
    }

    throw McpError(McpErrorCode::InternalError,
        "MRTR: exceeded max_rounds (" + std::to_string(max_rounds) + ")");
}

// ====================================================================
// Resources
// ====================================================================
ListResourcesResult McpClient::ListResources(
    const CacheableRequestOptions& options,
    std::optional<std::string> cursor)
{
    (void)options;
    ListResourcesRequestParams params;
    params.cursor = std::move(cursor);
    return SendRequest<ListResourcesRequestParams, ListResourcesResult>(
        methods::kListResources, params);
}

ListResourceTemplatesResult McpClient::ListResourceTemplates(
    const CacheableRequestOptions& options,
    std::optional<std::string> cursor)
{
    (void)options;
    ListResourceTemplatesRequestParams params;
    params.cursor = std::move(cursor);
    return SendRequest<ListResourceTemplatesRequestParams, ListResourceTemplatesResult>(
        methods::kListResourceTemplates, params);
}

ReadResourceResult McpClient::ReadResource(
    std::string_view uri, const CacheableRequestOptions& options)
{
    RequestMeta meta;
    meta.protocol_version = negotiation_.negotiated_version;
    meta.client_info = options_.client_info;
    if (options_.capabilities) meta.client_capabilities = options_.capabilities;
    if (options_.extensions) {
        if (!meta.client_capabilities) meta.client_capabilities = ClientCapabilities{};
        meta.client_capabilities->extensions = options_.extensions;
    }
    if (options.meta) meta.extensions = options.meta;

    nlohmann::json req_json;
    req_json["uri"] = std::string(uri);

    auto timeout = options.read_timeout_ms
        ? std::chrono::milliseconds(*options.read_timeout_ms)
        : std::chrono::seconds(30);
    auto result_json = SendRequestWithMrtr(
        methods::kReadResource, std::move(req_json), meta, timeout);
    return result_json.get<ReadResourceResult>();
}

EmptyResult McpClient::SubscribeResource(std::string_view uri) {
    SubscribeRequestParams params;
    params.uri = std::string(uri);
    return SendRequest<SubscribeRequestParams, EmptyResult>(
        methods::kSubscribeResource, params);
}

EmptyResult McpClient::UnsubscribeResource(std::string_view uri) {
    UnsubscribeRequestParams params;
    params.uri = std::string(uri);
    return SendRequest<UnsubscribeRequestParams, EmptyResult>(
        methods::kUnsubscribeResource, params);
}

// ====================================================================
// Prompts
// ====================================================================
ListPromptsResult McpClient::ListPrompts(
    const CacheableRequestOptions& options,
    std::optional<std::string> cursor)
{
    (void)options;
    ListPromptsRequestParams params;
    params.cursor = std::move(cursor);
    return SendRequest<ListPromptsRequestParams, ListPromptsResult>(
        methods::kListPrompts, params);
}

GetPromptResult McpClient::GetPrompt(
    std::string_view name,
    std::optional<nlohmann::json> arguments,
    const RequestOptions& options)
{
    RequestMeta meta;
    meta.protocol_version = negotiation_.negotiated_version;
    meta.client_info = options_.client_info;
    if (options_.capabilities) meta.client_capabilities = options_.capabilities;
    if (options_.extensions) {
        if (!meta.client_capabilities) meta.client_capabilities = ClientCapabilities{};
        meta.client_capabilities->extensions = options_.extensions;
    }
    if (options.meta) meta.extensions = options.meta;

    nlohmann::json req_json;
    req_json["name"] = std::string(name);
    if (arguments) req_json["arguments"] = *arguments;

    auto timeout = options.read_timeout_ms
        ? std::chrono::milliseconds(*options.read_timeout_ms)
        : std::chrono::seconds(30);
    auto result_json = SendRequestWithMrtr(
        methods::kGetPrompt, std::move(req_json), meta, timeout);

    // Check for task result type
    if (auto rt = result_json.find("resultType"); rt != result_json.end()
        && rt->get<std::string>() == "task")
    {
        auto task_id = result_json["taskId"].get<std::string>();
        auto task_result = PollTaskToCompletionAsync(task_id);

        if (task_result.status == "failed") {
            throw McpError(McpErrorCode::InternalError,
                "Task failed: " + task_id +
                (task_result.error_message ? ": " + *task_result.error_message : ""));
        }

        if (task_result.status == "cancelled") {
            throw McpError(McpErrorCode::InternalError,
                "Task cancelled: " + task_id);
        }

        GetPromptResult result;
        if (task_result.result) {
            from_json(*task_result.result, result);
        }
        return result;
    }

    return result_json.get<GetPromptResult>();
}

// ====================================================================
// ====================================================================
// Tasks
// ====================================================================
GetTaskResult McpClient::GetTask(std::string_view task_id) {
    GetTaskRequestParams params;
    params.task_id = std::string(task_id);
    return SendRequest<GetTaskRequestParams, GetTaskResult>(
        methods::kGetTask, params, std::chrono::seconds(600));
}

UpdateTaskResult McpClient::UpdateTask(
    std::string_view task_id,
    std::optional<nlohmann::json> result)
{
    UpdateTaskRequestParams params;
    params.task_id = std::string(task_id);
    params.result = std::move(result);
    return SendRequest<UpdateTaskRequestParams, UpdateTaskResult>(
        methods::kUpdateTask, params, std::chrono::seconds(600));
}

CancelTaskResult McpClient::CancelTask(
    std::string_view task_id,
    std::optional<std::string> reason)
{
    CancelTaskRequestParams params;
    params.task_id = std::string(task_id);
    params.reason = std::move(reason);
    return SendRequest<CancelTaskRequestParams, CancelTaskResult>(
        methods::kCancelTask, params, std::chrono::seconds(600));
}

// ====================================================================
// PollTaskToCompletion
// ====================================================================
GetTaskResult McpClient::PollTaskToCompletionAsync(
    const std::string& task_id,
    std::chrono::milliseconds poll_interval,
    std::chrono::seconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        auto task = GetTask(task_id);

        if (task.status == "completed" ||
            task.status == "failed" ||
            task.status == "cancelled")
        {
            return task;
        }

        // 检查剩余时间是否足够再睡一轮
        if (std::chrono::steady_clock::now() + poll_interval >= deadline) {
            break;
        }

        std::this_thread::sleep_for(poll_interval);
    }

    throw McpError(McpErrorCode::InternalError,
        "Task polling timed out for task: " + task_id);
}

// ====================================================================
// Completions / Ping / Discover
// ====================================================================
CompleteResult McpClient::Complete(const CompleteRequestParams& params) {
    return SendRequest<CompleteRequestParams, CompleteResult>(
        methods::kComplete, params);
}

EmptyResult McpClient::Ping() {
    nlohmann::json empty_params = nlohmann::json::object();
    auto future = handler_->SendRequest(methods::kPing, empty_params, {}, std::chrono::seconds(10));
    auto result_json = future.get();
    EmptyResult result;
    return result;
}

DiscoverResult McpClient::Discover() {
    auto future = handler_->SendRequest(
        methods::kDiscover, nlohmann::json::object(), {}, std::chrono::seconds(30));
    auto result_json = future.get();
    return result_json.get<DiscoverResult>();
}

// ====================================================================
// Subscriptions
// ====================================================================
void McpClient::SubscribeAsync(const SubscriptionsListenRequestParams& params) {
    nlohmann::json params_json;
    to_json(params_json, params);

    auto future = handler_->SendRequest(
        methods::kSubscribe, params_json, {}, std::chrono::seconds(30));
    auto result_json = future.get();

    if (result_json.contains("code") && result_json["code"].get<int32_t>() < 0) {
        throw McpError(
            static_cast<McpErrorCode>(result_json["code"].get<int32_t>()),
            result_json.value("message", "subscriptions/listen failed"));
    }

}

} // namespace mcp
