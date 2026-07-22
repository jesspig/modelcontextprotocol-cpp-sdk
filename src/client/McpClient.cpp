#include <mcp/client/McpClient.hpp>
#include <mcp/McpError.hpp>

#include <thread>

namespace mcp {

// ── Helper: build RequestMeta from ClientOptions and version ──
static RequestMeta BuildClientMeta(
    const ClientOptions& options, const std::string& version)
{
    RequestMeta meta;
    meta.protocol_version = version;
    meta.client_info = options.client_info;
    if (options.capabilities) {
        meta.client_capabilities = options.capabilities;
    }
    if (options.extensions) {
        if (!meta.client_capabilities)
            meta.client_capabilities = ClientCapabilities{};
        if (options.extensions->IsObject()) {
            std::map<std::string, JsonValue> exts;
            for (const auto& [k, v] : options.extensions->GetObject())
                exts[k] = v;
            meta.client_capabilities->extensions = std::move(exts);
        } else {
            meta.client_capabilities->extensions = std::nullopt;
        }
    }
    return meta;
}

// ── Helper: send request and check for protocol errors ──
static JsonValue DoSendRequest(
    McpSessionHandler& handler,
    std::string_view method,
    JsonValue params,
    const RequestMeta& meta,
    std::chrono::milliseconds timeout)
{
    auto future = handler.SendRequest(method, std::move(params), meta, timeout);
    auto result = future.get();

    if (result.Contains("code") && result["code"].GetInt() < 0) {
        std::string msg = "request failed";
        if (result.Contains("message"))
            msg = result["message"].GetString();
        throw McpError(
            static_cast<McpErrorCode>(result["code"].GetInt()),
            std::move(msg));
    }

    return result;
}

// ====================================================================
// VersionNegotiation implementation
// ====================================================================
NegotiationResult VersionNegotiation::Negotiate(
    McpSessionHandler& handler, const ClientOptions& options)
{
    if (options.connect_mode == ConnectMode::Legacy) {
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
        if (options.extensions->IsObject()) {
            std::map<std::string, JsonValue> exts;
            for (const auto& [k, v] : options.extensions->GetObject())
                exts[k] = v;
            meta.client_capabilities->extensions = std::move(exts);
        } else {
            meta.client_capabilities->extensions = std::nullopt;
        }
    }

    auto future = handler.SendRequest(
        methods::kDiscover, JsonValue(JsonValue::object_tag), meta, timeout);

    if (future.wait_for(timeout) == std::future_status::timeout) {
        return std::nullopt;
    }

    try {
        auto result = future.get();
        if (result.Contains("code") &&
            static_cast<int32_t>(result["code"].GetInt()) < 0) {
            return std::nullopt;
        }
        return DeserializeDiscoverResult(result);
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
        methods::kInitialize, SerializeInitializeRequestParams(params), {}, timeout);

    auto result = future.get();

    handler.SendNotification(notifications::kInitialized, JsonValue(JsonValue::object_tag));

    return DeserializeInitializeResult(result);
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
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (!sampling_handler_) {
                throw McpError(McpErrorCode::MethodNotFound, "sampling not supported");
            }
            CreateMessageRequestParams params;
            if (req.params) params = DeserializeCreateMessageRequestParams(*req.params);
            try {
                auto result = (*sampling_handler_)(params);
                p.set_value(SerializeCreateMessageResult(result));
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        });

    // Roots handler (deprecated)
    handler_->SetRequestHandler(methods::kListRoots,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            (void)req;
            if (!roots_handler_) {
                p.set_value(SerializeListRootsResult(ListRootsResult{}));
                return;
            }
            try {
                auto result = (*roots_handler_)(ListRootsRequestParams{});
                p.set_value(SerializeListRootsResult(result));
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        });

    // Elicitation handler
    handler_->SetRequestHandler(methods::kElicit,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (!elicitation_handler_) {
                throw McpError(McpErrorCode::MethodNotFound, "elicitation not supported");
            }
            ElicitRequestParams params;
            if (req.params) params = DeserializeElicitRequestParams(*req.params);
            try {
                auto result = (*elicitation_handler_)(params);
                p.set_value(SerializeElicitResult(result));
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
// MRTR helper: attempt to fulfill input_required responses via elicitation handler
// ====================================================================
static bool TryFulfillInputRequired(
    const JsonValue& result_json,
    const ClientOptions& options,
    const std::optional<std::function<ElicitResult(const ElicitRequestParams&)>>& elicitation_handler,
    JsonValue& out_input_responses,
    std::optional<std::string>& out_request_state)
{
    auto* rt = result_json.Find("resultType");
    if (!rt || rt->GetString() != "input_required") {
        return false;
    }

    if (!options.input_required_config || !options.input_required_config->auto_fulfill) {
        return false;
    }

    auto input_req = DeserializeInputRequiredResult(result_json);

    if (!input_req.input_requests.elicit && !input_req.input_requests.confirm) {
        return false;
    }

    JsonValue responses(JsonValue::object_tag);

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

JsonValue McpClient::SendRequestWithMrtr(
    std::string_view method,
    JsonValue params_json,
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
        if (result_json.Contains("code") && result_json["code"].GetInt() < 0) {
            throw McpError(
                static_cast<McpErrorCode>(result_json["code"].GetInt()),
                result_json.Contains("message")
                    ? result_json["message"].GetString()
                    : "request failed");
        }

        // Check for input_required (MRTR)
        JsonValue input_responses(JsonValue::object_tag);
        std::optional<std::string> request_state;
        if (TryFulfillInputRequired(result_json, options_, elicitation_handler_,
                input_responses, request_state)) {
            // Inject input_responses for next round
            params_json["inputResponses"] = std::move(input_responses);
            if (request_state) params_json["requestState"] = JsonValue(*request_state);
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
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kListTools,
        SerializePaginatedRequestParams(params), meta, std::chrono::seconds(30));
    return DeserializeListToolsResult(result);
}

CallToolResult McpClient::CallTool(
    std::string_view name,
    std::optional<JsonValue> arguments,
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
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    if (options.meta) meta.extensions = options.meta;

    for (int round = 0; round <= max_rounds; ++round) {
        // Build request params JSON manually so we can inject input_responses
        JsonValue req_json(JsonValue::object_tag);
        req_json["name"] = JsonValue(params.name);
        if (params.arguments) req_json["arguments"] = *params.arguments;
        if (params.input_responses) req_json["inputResponses"] = *params.input_responses;
        if (params.request_state)   req_json["requestState"] = JsonValue(*params.request_state);

        auto future = handler_->SendRequest(
            methods::kCallTool, req_json, meta, round_timeout);
        auto result_json = future.get();

        // Check for protocol errors
        if (result_json.Contains("code") && result_json["code"].GetInt() < 0) {
            throw McpError(
                static_cast<McpErrorCode>(result_json["code"].GetInt()),
                result_json.Contains("message")
                    ? result_json["message"].GetString()
                    : "request failed");
        }

        // Check for input_required (MRTR)
        if (auto* rt = result_json.Find("resultType");
            rt && rt->GetString() == "input_required" && auto_fulfill)
        {
            auto input_req = DeserializeInputRequiredResult(result_json);

            if (!input_req.input_requests.elicit && !input_req.input_requests.confirm) {
                // No actionable requests — stop the loop
                return DeserializeCallToolResult(result_json);
            }

            // Fulfill each request
            JsonValue responses(JsonValue::object_tag);

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

        // Check for task result type (extended task)
        if (auto* rt = result_json.Find("resultType");
            rt && rt->GetString() == "task")
        {
            auto task_id = result_json["taskId"].GetString();
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
                result = DeserializeCallToolResult(*task_result.result);
            }
            return result;
        }

        // Complete result — deserialize and return
        return DeserializeCallToolResult(result_json);
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
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kListResources,
        SerializePaginatedRequestParams(params), meta, std::chrono::seconds(30));
    return DeserializeListResourcesResult(result);
}

ListResourceTemplatesResult McpClient::ListResourceTemplates(
    const CacheableRequestOptions& options,
    std::optional<std::string> cursor)
{
    (void)options;
    ListResourceTemplatesRequestParams params;
    params.cursor = std::move(cursor);
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kListResourceTemplates,
        SerializePaginatedRequestParams(params), meta, std::chrono::seconds(30));
    return DeserializeListResourceTemplatesResult(result);
}

ReadResourceResult McpClient::ReadResource(
    std::string_view uri, const CacheableRequestOptions& options)
{
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    if (options.meta) meta.extensions = options.meta;

    JsonValue req_json(JsonValue::object_tag);
    req_json["uri"] = JsonValue(std::string(uri));

    auto timeout = options.read_timeout_ms
        ? std::chrono::milliseconds(*options.read_timeout_ms)
        : std::chrono::seconds(30);
    auto result_json = SendRequestWithMrtr(
        methods::kReadResource, std::move(req_json), meta, timeout);
    return DeserializeReadResourceResult(result_json);
}

EmptyResult McpClient::SubscribeResource(std::string_view uri) {
    SubscribeRequestParams params;
    params.uri = std::string(uri);
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kSubscribeResource,
        SerializeResourceRequestParams(params), meta, std::chrono::seconds(30));
    return DeserializeEmptyResult(result);
}

EmptyResult McpClient::UnsubscribeResource(std::string_view uri) {
    UnsubscribeRequestParams params;
    params.uri = std::string(uri);
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kUnsubscribeResource,
        SerializeResourceRequestParams(params), meta, std::chrono::seconds(30));
    return DeserializeEmptyResult(result);
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
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kListPrompts,
        SerializePaginatedRequestParams(params), meta, std::chrono::seconds(30));
    return DeserializeListPromptsResult(result);
}

GetPromptResult McpClient::GetPrompt(
    std::string_view name,
    std::optional<JsonValue> arguments,
    const RequestOptions& options)
{
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    if (options.meta) meta.extensions = options.meta;

    JsonValue req_json(JsonValue::object_tag);
    req_json["name"] = JsonValue(std::string(name));
    if (arguments) req_json["arguments"] = *arguments;

    auto timeout = options.read_timeout_ms
        ? std::chrono::milliseconds(*options.read_timeout_ms)
        : std::chrono::seconds(30);
    auto result_json = SendRequestWithMrtr(
        methods::kGetPrompt, std::move(req_json), meta, timeout);

    // Check for task result type
    if (auto* rt = result_json.Find("resultType");
        rt && rt->GetString() == "task")
    {
        auto task_id = result_json["taskId"].GetString();
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
            result = DeserializeGetPromptResult(*task_result.result);
        }
        return result;
    }

    return DeserializeGetPromptResult(result_json);
}

// ====================================================================
// Tasks
// ====================================================================
GetTaskResult McpClient::GetTask(std::string_view task_id) {
    GetTaskRequestParams params;
    params.task_id = std::string(task_id);
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kGetTask,
        SerializeGetTaskRequestParams(params), meta, std::chrono::seconds(600));
    return DeserializeGetTaskResult(result);
}

UpdateTaskResult McpClient::UpdateTask(
    std::string_view task_id,
    std::optional<JsonValue> result)
{
    UpdateTaskRequestParams params;
    params.task_id = std::string(task_id);
    params.result = std::move(result);
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto res = DoSendRequest(*handler_, methods::kUpdateTask,
        SerializeUpdateTaskRequestParams(params), meta, std::chrono::seconds(600));
    return DeserializeEmptyResult(res);
}

CancelTaskResult McpClient::CancelTask(
    std::string_view task_id,
    std::optional<std::string> reason)
{
    CancelTaskRequestParams params;
    params.task_id = std::string(task_id);
    params.reason = std::move(reason);
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kCancelTask,
        SerializeCancelTaskRequestParams(params), meta, std::chrono::seconds(600));
    return DeserializeEmptyResult(result);
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
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kComplete,
        SerializeCompleteRequestParams(params), meta, std::chrono::seconds(30));
    return DeserializeCompleteResult(result);
}

EmptyResult McpClient::Ping() {
    auto result = DoSendRequest(*handler_, methods::kPing,
        JsonValue(JsonValue::object_tag), RequestMeta{}, std::chrono::seconds(10));
    (void)result;
    return EmptyResult{};
}

DiscoverResult McpClient::Discover() {
    auto result = DoSendRequest(*handler_, methods::kDiscover,
        JsonValue(JsonValue::object_tag), RequestMeta{}, std::chrono::seconds(30));
    return DeserializeDiscoverResult(result);
}

// ====================================================================
// Subscriptions
// ====================================================================
void McpClient::SubscribeAsync(const SubscriptionsListenRequestParams& params) {
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto params_json = SerializeSubscriptionsListenRequestParams(params);

    auto result = DoSendRequest(*handler_,
        methods::kSubscribe, std::move(params_json), meta, std::chrono::seconds(30));

    if (result.Contains("code") && result["code"].GetInt() < 0) {
        throw McpError(
            static_cast<McpErrorCode>(result["code"].GetInt()),
            result.Contains("message")
                ? result["message"].GetString()
                : "subscriptions/listen failed");
    }
}

} // namespace mcp
