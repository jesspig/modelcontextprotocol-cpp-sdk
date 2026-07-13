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
    Protocol& protocol, const ClientOptions& options)
{
    if (options.connect_mode == ConnectMode::Legacy) {
        // Force legacy initialize handshake
        auto init = HandshakeInitialize(
            protocol, options.client_info, options.capabilities,
            options.initialization_timeout);
        NegotiationResult result;
        result.is_modern = false;
        result.negotiated_version = init.protocol_version;
        result.initialize = std::move(init);
        result.capabilities = result.initialize->capabilities;
        result.server_info = result.initialize->server_info;
        result.instructions = result.initialize->instructions;
        protocol.SetNegotiatedProtocolVersion(result.negotiated_version);
        return result;
    }

    if (options.connect_mode == ConnectMode::Pin) {
        // Pin to specific version — skip negotiation, assume modern
        NegotiationResult result;
        result.is_modern = true;
        result.negotiated_version = options.pin_protocol_version.value_or(
            std::string(kLatestProtocolVersion));
        protocol.SetNegotiatedProtocolVersion(result.negotiated_version);

        DiscoverResult disc;
        disc.supported_versions = {result.negotiated_version};
        disc.capabilities = ServerCapabilities{};
        disc.server_info = options.client_info;
        result.discover = std::move(disc);
        return result;
    }

    // Auto mode: probe server/discover, fallback to initialize
    auto discover = ProbeDiscover(
        protocol, std::string(kLatestProtocolVersion),
        options.discover_probe_timeout);

    if (discover.has_value()) {
        NegotiationResult result;
        result.is_modern = true;
        result.discover = std::move(discover);
        result.negotiated_version = kLatestProtocolVersion.data();
        result.capabilities = result.discover->capabilities;
        result.server_info = result.discover->server_info;
        result.instructions = result.discover->instructions;
        protocol.SetNegotiatedProtocolVersion(result.negotiated_version);
        return result;
    }

    // Fallback to initialize
    auto init = HandshakeInitialize(
        protocol, options.client_info, options.capabilities,
        options.initialization_timeout);
    NegotiationResult result;
    result.is_modern = false;
    result.negotiated_version = init.protocol_version;
    result.initialize = std::move(init);
    result.capabilities = result.initialize->capabilities;
    result.server_info = result.initialize->server_info;
    result.instructions = result.initialize->instructions;
    protocol.SetNegotiatedProtocolVersion(result.negotiated_version);
    return result;
}

std::optional<DiscoverResult> VersionNegotiation::ProbeDiscover(
    Protocol& protocol,
    std::string_view preferred_version,
    std::chrono::seconds timeout)
{
    RequestMeta meta;
    meta.protocol_version = std::string(preferred_version);
    meta.client_info = Implementation{"mcp-cpp-client", "0.1.0"};
    meta.client_capabilities = ClientCapabilities{};

    auto future = protocol.SendRequest(
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
    Protocol& protocol,
    const Implementation& client_info,
    const std::optional<ClientCapabilities>& capabilities,
    std::chrono::seconds timeout)
{
    InitializeRequestParams params;
    params.protocol_version = "2025-11-25";
    params.client_info = client_info;
    params.capabilities = capabilities.value_or(ClientCapabilities{});

    auto future = protocol.SendRequest(
        methods::kInitialize, nlohmann::json(params), {}, timeout);

    auto result = future.get();
    return result.get<InitializeResult>();
}

// ====================================================================
// McpClient construction / destruction
// ====================================================================
McpClient::McpClient(
    std::unique_ptr<Transport> transport,
    ClientOptions options)
    : io_ctx_()
    , transport_(std::move(transport))
    , options_(std::move(options))
{
    protocol_ = std::make_shared<Protocol>(io_ctx_, std::move(transport_));
    protocol_->Start();
    WireClientHandlers();
}

McpClient::~McpClient() {
    Close();
}

std::unique_ptr<McpClient> McpClient::Create(
    std::unique_ptr<Transport> transport,
    const ClientOptions& options)
{
    auto client = std::unique_ptr<McpClient>(
        new McpClient(std::move(transport), options));
    client->negotiation_ = client->NegotiateProtocol();
    client->negotiated_ = true;
    return client;
}

// ====================================================================
// Negotiation
// ====================================================================
NegotiationResult McpClient::NegotiateProtocol() {
    return VersionNegotiation::Negotiate(*protocol_, options_);
}

// ====================================================================
// Close
// ====================================================================
void McpClient::Close() {
    if (protocol_) protocol_->Close();
    io_ctx_.stop();
}

// ====================================================================
// Wire client-side handlers
// ====================================================================
void McpClient::WireClientHandlers() {
    // Sampling handler (deprecated)
    protocol_->SetRequestHandler(methods::kCreateMessage,
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
    protocol_->SetRequestHandler(methods::kListRoots,
        [this](const JsonRpcRequest& req, std::promise<nlohmann::json> p) {
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
    protocol_->SetRequestHandler(methods::kElicit,
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
    for (auto& [method, handler] : notif_handlers_) {
        protocol_->SetNotificationHandler(method,
            [handler](const JsonRpcNotification& notif) {
                handler(notif);
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

    auto future = protocol_->SendRequest(method, params_json, meta, timeout);
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
// Tools
// ====================================================================
ListToolsResult McpClient::ListTools(const CacheableRequestOptions& options) {
    ListToolsRequestParams params;
    params.cursor = std::nullopt;
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
    return SendRequest<CallToolRequestParams, CallToolResult>(
        methods::kCallTool, params);
}

// ====================================================================
// Resources
// ====================================================================
ListResourcesResult McpClient::ListResources(const CacheableRequestOptions& options) {
    ListResourcesRequestParams params;
    return SendRequest<ListResourcesRequestParams, ListResourcesResult>(
        methods::kListResources, params);
}

ListResourceTemplatesResult McpClient::ListResourceTemplates(const CacheableRequestOptions& options) {
    ListResourceTemplatesRequestParams params;
    return SendRequest<ListResourceTemplatesRequestParams, ListResourceTemplatesResult>(
        methods::kListResourceTemplates, params);
}

ReadResourceResult McpClient::ReadResource(
    std::string_view uri, const CacheableRequestOptions& options)
{
    ReadResourceRequestParams params;
    params.uri = std::string(uri);
    return SendRequest<ReadResourceRequestParams, ReadResourceResult>(
        methods::kReadResource, params);
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
ListPromptsResult McpClient::ListPrompts(const CacheableRequestOptions& options) {
    ListPromptsRequestParams params;
    return SendRequest<ListPromptsRequestParams, ListPromptsResult>(
        methods::kListPrompts, params);
}

GetPromptResult McpClient::GetPrompt(
    std::string_view name,
    std::optional<nlohmann::json> arguments,
    const RequestOptions& options)
{
    GetPromptRequestParams params;
    params.name = std::string(name);
    params.arguments = std::move(arguments);
    return SendRequest<GetPromptRequestParams, GetPromptResult>(
        methods::kGetPrompt, params);
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
    auto future = protocol_->SendRequest(methods::kPing, empty_params, {}, std::chrono::seconds(10));
    auto result_json = future.get();
    EmptyResult result;
    return result;
}

DiscoverResult McpClient::Discover() {
    auto future = protocol_->SendRequest(
        methods::kDiscover, nlohmann::json::object(), {}, std::chrono::seconds(30));
    auto result_json = future.get();
    return result_json.get<DiscoverResult>();
}

} // namespace mcp
