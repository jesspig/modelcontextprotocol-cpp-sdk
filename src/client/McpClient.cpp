// McpClient.cpp
// McpClient and VersionNegotiation implementation
#include <detail/JsonFields.hpp>
#include <detail/ResponseCache.hpp>
#include <mcp/client/McpClient.hpp>
#include <mcp/McpError.hpp>
#include <mcp/Log.hpp>
#include <mcp/Transport.hpp>

#include <cstring>
#include <thread>
#include <typeinfo>

namespace mcp {

namespace {
    // ── Timeouts ──
    // kDefaultRequestTimeout (30s) comes from mcp/protocol/McpSession.hpp
    constexpr std::chrono::seconds kTaskRequestTimeout(600);
    constexpr std::chrono::seconds kPingTimeout(10);

    // ── Helper: apply client extensions declaration to capabilities ──
    static void ApplyExtensions(
        ClientCapabilities& caps, const std::optional<JsonValue>& extensions)
    {
        if (!extensions) return;
        if (extensions->IsObject()) {
            std::map<std::string, JsonValue> exts;
            for (const auto& [k, v] : extensions->GetObject())
                exts[k] = v;
            caps.extensions = std::move(exts);
        } else {
            caps.extensions = std::nullopt;
        }
    }

    // ── Pagination ──
    // Defensive cap: never page more than this many times (cursor loop guard).
    constexpr size_t kMaxAutoPages = 64;

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

        if (result.Contains(detail::kCode) && result[detail::kCode].GetInt() < 0) {
            std::string msg = "request failed";
            if (result.Contains(detail::kMessage))
                msg = result[detail::kMessage].GetString();
            throw McpError(
                static_cast<McpErrorCode>(result[detail::kCode].GetInt()),
                std::move(msg));
        }

        return result;
    }

    // ── Helper: extract a cache hint from a wire result ──
    // The 2026 era flattens ttlMs/cacheScope onto the result top level; the
    // 2025 era nests them under cacheHint. Returns the canonical nested shape.
    static std::optional<JsonValue> ExtractCacheHint(const JsonValue& result) {
        auto* ttl = result.Find(detail::kTTLMs);
        auto* scope = result.Find(detail::kCacheScope);
        if (ttl || scope) {
            JsonValue hint(JsonValue::object_tag);
            if (ttl) hint[detail::kTTLMs] = *ttl;
            if (scope) hint[detail::kCacheScope] = *scope;
            return hint;
        }
        if (auto* hint = result.Find(detail::kCacheHint); hint && hint->IsObject())
            return *hint;
        return std::nullopt;
    }

    // Fetch a paginated list method. With an explicit cursor a single page is
    // returned (caller-driven pagination); without one all pages are merged
    // automatically until nextCursor is exhausted.
    JsonValue ListPages(
        McpSessionHandler& handler,
        std::string_view method,
        std::string_view result_key,
        const RequestMeta& meta,
        const std::optional<std::string>& cursor)
    {
        if (cursor) {
            PaginatedRequestParams params;
            params.cursor = cursor;
            return DoSendRequest(handler, method,
                SerializePaginatedRequestParams(params), meta, kDefaultRequestTimeout);
        }

        JsonValue::Array merged;
        std::optional<std::string> current;
        std::optional<JsonValue> first_hint;
        for (size_t page = 0; page < kMaxAutoPages; ++page) {
            PaginatedRequestParams params;
            params.cursor = current;
            auto result = DoSendRequest(handler, method,
                SerializePaginatedRequestParams(params), meta, kDefaultRequestTimeout);
            if (page == 0) {
                if (auto hint = ExtractCacheHint(result))
                    first_hint = std::move(hint);
            }
            if (auto* arr = result.Find(std::string(result_key)); arr && arr->IsArray()) {
                for (const auto& item : arr->GetArray()) merged.push_back(item);
            }
            auto* nc = result.Find(detail::kNextCursor);
            if (!nc || !nc->IsString() || nc->GetString().empty()) break;
            current = nc->GetString();
        }
        JsonValue out(JsonValue::object_tag);
        out[std::string(result_key)] = JsonValue(std::move(merged));
        if (first_hint) out[detail::kCacheHint] = std::move(*first_hint);
        return out;
    }

    // ── Helper: classify the active transport for probe-failure handling ──
    // stdio and in-memory transports have no network-failure concept: any
    // discover probe failure falls back to initialize. HTTP-like transports
    // (streamable-http, sse, websocket) surface timeouts and connection
    // errors as typed errors instead of falling back. ITransport exposes no
    // Name(); the concrete session transports are identified via RTTI.
    static bool IsStdioLikeTransport(const ITransport& transport)
    {
        const char* type_name = typeid(transport).name();
        return std::strstr(type_name, "InMemoryTransportImpl") != nullptr ||
               std::strstr(type_name, "StdioClientSessionTransport") != nullptr;
    }

    // ── Helper: extract result["data"]["supported"] as version strings ──
    // Returns nullopt when the field is missing or malformed; callers treat
    // that like any unrecognized error code (fall back to initialize).
    static std::optional<std::vector<std::string>> ExtractSupportedVersions(
        const JsonValue& result)
    {
        auto* data = result.Find("data");
        if (!data || !data->IsObject()) return std::nullopt;
        auto* supported = data->Find("supported");
        if (!supported || !supported->IsArray()) return std::nullopt;
        std::vector<std::string> versions;
        for (const auto& item : supported->GetArray()) {
            if (!item.IsString()) return std::nullopt;
            versions.push_back(item.GetString());
        }
        return versions;
    }

    // ── Helper: check for a JSON-RPC error response ──
    static bool IsErrorResponse(const JsonValue& result)
    {
        return result.Contains(detail::kCode) &&
               static_cast<int32_t>(result[detail::kCode].GetInt()) < 0;
    }
}

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
        ApplyExtensions(*meta.client_capabilities, options.extensions);
    }
    return meta;
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
    // stdio-like transports fall back to initialize on any probe failure;
    // HTTP-like transports surface timeouts and connection errors as typed
    // errors instead of falling back.
    const bool stdio_like = IsStdioLikeTransport(handler.GetTransport());

    auto send_probe = [&handler, &options, timeout](std::string_view version) {
        RequestMeta meta;
        meta.protocol_version = std::string(version);
        meta.client_info = options.client_info;
        meta.client_capabilities = options.capabilities.value_or(ClientCapabilities{});
        ApplyExtensions(*meta.client_capabilities, options.extensions);
        return handler.SendRequest(
            methods::kDiscover, JsonValue(JsonValue::object_tag), meta, timeout);
    };

    // Awaits a probe future. Returns nullopt for the fallback outcome
    // (stdio-like timeout); throws McpError for network-class failures.
    auto await_probe = [stdio_like, timeout](std::future<JsonValue>& future)
        -> std::optional<JsonValue>
    {
        if (future.wait_for(timeout) == std::future_status::timeout) {
            if (!stdio_like) {
                throw McpError(McpErrorCode::RequestTimeout,
                    "server/discover timed out");
            }
            return std::nullopt;
        }
        try {
            return future.get();
        } catch (const std::exception& e) {
            if (!stdio_like) {
                throw McpError(McpErrorCode::ConnectionClosed,
                    std::string("server/discover failed: ") + e.what());
            }
            return std::nullopt;
        } catch (...) {
            if (!stdio_like) {
                throw McpError(McpErrorCode::ConnectionClosed,
                    "server/discover failed");
            }
            return std::nullopt;
        }
    };

    auto first_probe = send_probe(preferred_version);
    auto first = await_probe(first_probe);
    if (!first) return std::nullopt;

    if (IsErrorResponse(*first)) {
        auto code = static_cast<int32_t>((*first)[detail::kCode].GetInt());

        if (code == static_cast<int32_t>(McpErrorCode::UnsupportedProtocolVersion)) {
            auto supported = ExtractSupportedVersions(*first);
            if (!supported) return std::nullopt;

            bool shares_latest = false;
            bool has_modern = false;
            for (const auto& v : *supported) {
                if (v == kLatestProtocolVersion) shares_latest = true;
                if (IsModernProtocolVersion(v)) has_modern = true;
            }

            if (shares_latest) {
                // Corrective: retry server/discover once with the shared
                // version; a second rejection is a hard error (no fallback).
                auto retry = send_probe(kLatestProtocolVersion);
                auto retried = await_probe(retry);
                if (!retried) {
                    throw McpError(McpErrorCode::UnsupportedProtocolVersion,
                        "server/discover retry timed out for " +
                        std::string(kLatestProtocolVersion));
                }
                if (IsErrorResponse(*retried)) {
                    throw McpError(McpErrorCode::UnsupportedProtocolVersion,
                        "server/discover rejected the shared protocol version " +
                        std::string(kLatestProtocolVersion));
                }
                try {
                    return DeserializeDiscoverResult(*retried);
                } catch (...) {
                    throw McpError(McpErrorCode::UnsupportedProtocolVersion,
                        "server/discover retry result could not be parsed for " +
                        std::string(kLatestProtocolVersion));
                }
            }

            if (!has_modern) return std::nullopt;  // only legacy versions → initialize

            throw McpError(McpErrorCode::UnsupportedProtocolVersion,
                "server does not support the client protocol version " +
                std::string(kLatestProtocolVersion));
        }

        // Legacy-era signals (-32001, -32020, -32021, -32601) and any other
        // error code fall back to initialize.
        return std::nullopt;
    }

    try {
        return DeserializeDiscoverResult(*first);
    } catch (...) {
        // Unrecognized result shape falls back to initialize.
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
    params.protocol_version = std::string(kLegacyProtocolVersion);
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
    , response_cache_(std::make_unique<detail::ResponseCache>())
{
    auto codec = MakeWireCodec(std::string(kLatestProtocolVersion));
    handler_ = std::make_shared<McpSessionHandler>(
        transport_, std::move(codec));
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
    // Elicitation handler
    handler_->SetRequestHandler(methods::kElicit,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (!elicitation_handler_) {
                p.set_exception(std::make_exception_ptr(
                    McpError(McpErrorCode::MethodNotFound, "elicitation not supported")));
                return;
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

    // ── Client-side notification handlers: listChanged invalidates the
    // response cache so subsequent calls observe the new listing ──
    handler_->SetNotificationHandler(notifications::kToolListChanged,
        [this](const JsonRpcNotification&) { response_cache_->Clear(); });
    handler_->SetNotificationHandler(notifications::kResourceListChanged,
        [this](const JsonRpcNotification&) { response_cache_->Clear(); });
    handler_->SetNotificationHandler(notifications::kPromptListChanged,
        [this](const JsonRpcNotification&) { response_cache_->Clear(); });
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
    handler_->SetRequestHandler(methods::kCreateMessage,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (!sampling_handler_) {
                p.set_exception(std::make_exception_ptr(
                    McpError(McpErrorCode::MethodNotFound, "sampling not supported")));
                return;
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
}

void McpClient::SetRootsHandler(RootsHandler handler) {
    roots_handler_ = std::move(handler);
    handler_->SetRequestHandler(methods::kListRoots,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            (void)req;
            if (!roots_handler_) {
                p.set_exception(std::make_exception_ptr(
                    McpError(McpErrorCode::MethodNotFound, "roots not supported")));
                return;
            }
            try {
                auto result = (*roots_handler_)(ListRootsRequestParams{});
                p.set_value(SerializeListRootsResult(result));
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        });
}

void McpClient::SetElicitationHandler(ElicitationHandler handler) {
    elicitation_handler_ = std::move(handler);
}

void McpClient::SetNotificationHandler(
    std::string_view method, ClientNotificationHandler handler)
{
    auto nh = std::move(handler);
    handler_->SetNotificationHandler(method,
        [nh](const JsonRpcNotification& notif) {
            nh(notif);
        });
}

void McpClient::SetLoggingHandler(
    std::function<void(const LoggingMessageNotificationParams&)> handler)
{
    logging_handler_ = std::move(handler);
    handler_->SetNotificationHandler(notifications::kMessage,
        [this](const JsonRpcNotification& notif) {
            if (!logging_handler_) return;
            if (!notif.params || !notif.params->IsObject()) return;
            try {
                (*logging_handler_)(
                    DeserializeLoggingMessageNotificationParams(*notif.params));
            } catch (...) {
                MCP_LOG(Error, "logging notification handler threw");
            }
        });
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
    auto round_timeout = cfg ? cfg->round_timeout : timeout;
    auto total_budget = cfg ? cfg->max_total_timeout : std::chrono::seconds(0);
    auto flow_start = std::chrono::steady_clock::now();

    for (int round = 0; round <= max_rounds; ++round) {
        auto effective_timeout = round_timeout;
        if (total_budget.count() > 0) {
            auto elapsed = std::chrono::steady_clock::now() - flow_start;
            auto remaining = total_budget - elapsed;
            if (remaining <= std::chrono::seconds(0)) {
                throw McpError(McpErrorCode::RequestTimeout,
                    "MRTR: exceeded max_total_timeout");
            }
            if (remaining < effective_timeout)
                effective_timeout = std::chrono::duration_cast<std::chrono::seconds>(remaining);
        }
        auto future = handler_->SendRequest(method, params_json, meta, effective_timeout);
        auto result_json = future.get();

        // Check for protocol errors
        if (result_json.Contains(detail::kCode) && result_json[detail::kCode].GetInt() < 0) {
            throw McpError(
                static_cast<McpErrorCode>(result_json[detail::kCode].GetInt()),
                result_json.Contains(detail::kMessage)
                    ? result_json[detail::kMessage].GetString()
                    : "request failed");
        }

        // Check for input_required (MRTR)
        JsonValue input_responses(JsonValue::object_tag);
        std::optional<std::string> request_state;
        if (TryFulfillInputRequired(result_json, options_, elicitation_handler_,
                input_responses, request_state)) {
            // Inject input_responses for next round
            params_json[detail::kInputResponses] = std::move(input_responses);
            if (request_state) params_json[detail::kRequestState] = JsonValue(*request_state);
            continue;
        }

        // Complete result or non-MRTR �?return raw JSON
        return result_json;
    }

    throw McpError(McpErrorCode::InternalError,
        "MRTR: exceeded max_rounds (" + std::to_string(max_rounds) + ")");
}

void McpClient::CacheIfHinted(std::string_view key, const JsonValue& result) {
    auto* ttl = result.Find(detail::kCacheHint);
    if (ttl && ttl->IsObject()) ttl = ttl->Find(detail::kTTLMs);
    if (!ttl) ttl = result.Find(detail::kTTLMs);
    if (ttl && ttl->IsInt() && ttl->GetInt() > 0) {
        response_cache_->Store(key, result, std::chrono::milliseconds(ttl->GetInt()));
    }
}

// ====================================================================
// Tools
// ====================================================================
ListToolsResult McpClient::ListTools(
    std::optional<std::string> cursor)
{
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    if (!cursor) {
        if (auto cached = response_cache_->Get("tools/list")) {
            return DeserializeListToolsResult(*cached);
        }
    }
    auto result = ListPages(*handler_, methods::kListTools, "tools", meta, cursor);
    if (!cursor) CacheIfHinted("tools/list", result);
    return DeserializeListToolsResult(result);
}

// ── Helper: complete a task-typed result by polling to completion ──
// Returns nullopt when the result is not a task; throws on failed/cancelled;
// returns a (possibly null) JsonValue payload for task results.
static std::optional<JsonValue> ResolveTaskResult(
    McpClient& client, const JsonValue& result_json)
{
    auto* rt = result_json.Find("resultType");
    if (!rt || rt->GetString() != "task") return std::nullopt;

    auto* tid = result_json.Find(detail::kTaskId);
    if (!tid || !tid->IsString()) {
        throw McpError(McpErrorCode::InvalidParams,
            "task result missing valid taskId");
    }
    auto task_id = tid->GetString();
    auto task_result = client.PollTaskToCompletion(task_id);

    if (task_result.status == "failed") {
        throw McpError(McpErrorCode::InternalError,
            "Task failed: " + task_id +
            (task_result.error_message ? ": " + *task_result.error_message : ""));
    }
    if (task_result.status == "cancelled") {
        throw McpError(McpErrorCode::InternalError,
            "Task cancelled: " + task_id);
    }
    return task_result.result;
}

CallToolResult McpClient::CallTool(
    std::string_view name,
    std::optional<JsonValue> arguments,
    const RequestOptions& options)
{
    CallToolRequestParams params;
    params.name = std::string(name);
    params.arguments = std::move(arguments);

    // Send with meta
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    if (options.meta) meta.extensions = options.meta;

    JsonValue req_json(JsonValue::object_tag);
    req_json[detail::kName] = JsonValue(params.name);
    if (params.arguments) req_json["arguments"] = *params.arguments;

    auto round_timeout = options_.input_required_config
        ? options_.input_required_config->round_timeout
        : kTaskRequestTimeout;
    auto result_json = SendRequestWithMrtr(
        methods::kCallTool, std::move(req_json), meta, round_timeout);

    if (auto task_payload = ResolveTaskResult(*this, result_json)) {
        return task_payload->IsNull()
            ? CallToolResult{}
            : DeserializeCallToolResult(*task_payload);
    }
    return DeserializeCallToolResult(result_json);
}

// ====================================================================
// Resources
// ====================================================================
ListResourcesResult McpClient::ListResources(
    std::optional<std::string> cursor)
{
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    if (!cursor) {
        if (auto cached = response_cache_->Get("resources/list")) {
            return DeserializeListResourcesResult(*cached);
        }
    }
    auto result = ListPages(*handler_, methods::kListResources, "resources", meta, cursor);
    if (!cursor) CacheIfHinted("resources/list", result);
    return DeserializeListResourcesResult(result);
}

ListResourceTemplatesResult McpClient::ListResourceTemplates(
    std::optional<std::string> cursor)
{
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    if (!cursor) {
        if (auto cached = response_cache_->Get("resources/templates/list")) {
            return DeserializeListResourceTemplatesResult(*cached);
        }
    }
    auto result = ListPages(*handler_, methods::kListResourceTemplates, "resourceTemplates", meta, cursor);
    if (!cursor) CacheIfHinted("resources/templates/list", result);
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
        : kDefaultRequestTimeout;
    auto result_json = SendRequestWithMrtr(
        methods::kReadResource, std::move(req_json), meta, timeout);
    return DeserializeReadResourceResult(result_json);
}

EmptyResult McpClient::SubscribeResource(std::string_view uri) {
    SubscribeRequestParams params;
    params.uri = std::string(uri);
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kSubscribeResource,
        SerializeResourceRequestParams(params), meta, kDefaultRequestTimeout);
    return DeserializeEmptyResult(result);
}

EmptyResult McpClient::UnsubscribeResource(std::string_view uri) {
    UnsubscribeRequestParams params;
    params.uri = std::string(uri);
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto result = DoSendRequest(*handler_, methods::kUnsubscribeResource,
        SerializeResourceRequestParams(params), meta, kDefaultRequestTimeout);
    return DeserializeEmptyResult(result);
}

// ====================================================================
// Prompts
// ====================================================================
ListPromptsResult McpClient::ListPrompts(
    std::optional<std::string> cursor)
{
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    if (!cursor) {
        if (auto cached = response_cache_->Get("prompts/list")) {
            return DeserializeListPromptsResult(*cached);
        }
    }
    auto result = ListPages(*handler_, methods::kListPrompts, "prompts", meta, cursor);
    if (!cursor) CacheIfHinted("prompts/list", result);
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
    req_json[detail::kName] = JsonValue(std::string(name));
    if (arguments) req_json["arguments"] = *arguments;

    auto timeout = options.read_timeout_ms
        ? std::chrono::milliseconds(*options.read_timeout_ms)
        : kDefaultRequestTimeout;
    auto result_json = SendRequestWithMrtr(
        methods::kGetPrompt, std::move(req_json), meta, timeout);

    if (auto task_payload = ResolveTaskResult(*this, result_json)) {
        return task_payload->IsNull()
            ? GetPromptResult{}
            : DeserializeGetPromptResult(*task_payload);
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
        SerializeGetTaskRequestParams(params), meta, kTaskRequestTimeout);
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
        SerializeUpdateTaskRequestParams(params), meta, kTaskRequestTimeout);
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
        SerializeCancelTaskRequestParams(params), meta, kTaskRequestTimeout);
    return DeserializeEmptyResult(result);
}

// ====================================================================
// PollTaskToCompletion
// ====================================================================
GetTaskResult McpClient::PollTaskToCompletion(
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
        SerializeCompleteRequestParams(params), meta, kDefaultRequestTimeout);
    return DeserializeCompleteResult(result);
}

EmptyResult McpClient::Ping() {
    auto result = DoSendRequest(*handler_, methods::kPing,
        JsonValue(JsonValue::object_tag), RequestMeta{}, kPingTimeout);
    (void)result;
    return EmptyResult{};
}

DiscoverResult McpClient::Discover() {
    auto result = DoSendRequest(*handler_, methods::kDiscover,
        JsonValue(JsonValue::object_tag), RequestMeta{}, kDefaultRequestTimeout);
    return DeserializeDiscoverResult(result);
}

// ====================================================================
// Subscriptions
// ====================================================================
void McpClient::SubscribeAsync(const SubscriptionsListenRequestParams& params) {
    auto meta = BuildClientMeta(options_, negotiation_.negotiated_version);
    auto params_json = SerializeSubscriptionsListenRequestParams(params);

    DoSendRequest(*handler_,
        methods::kSubscribe, std::move(params_json), meta, kDefaultRequestTimeout);
}

} // namespace mcp
