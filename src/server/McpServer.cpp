// McpServer.cpp - MCP server implementation: lifecycle, handler wiring, and primitive registration

#include <mcp/JsonValue.hpp>
#include <mcp/server/McpServer.hpp>
#include <mcp/server/RequestState.hpp>
#include <mcp/Content.hpp>
#include <mcp/McpError.hpp>
#include <mcp/McpVersion.hpp>
#include <mcp/Transport.hpp>
#include <mcp/Log.hpp>
#include <detail/JsonFields.hpp>
#include <detail/JsonSchemaValidator.hpp>
#include <detail/UriTemplate.hpp>

#include <condition_variable>
#include <thread>
#include <set>
#include <algorithm>
#include <ctime>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <type_traits>

namespace mcp {

namespace {
    // ── Pagination helper ──
    constexpr size_t kDefaultPageSize = 100;

    // ── Timeouts ──
    constexpr std::chrono::seconds kElicitTimeout(600);
    constexpr std::chrono::seconds kNoWait(0);

    // ── Logging ──
    constexpr std::string_view kDefaultLoggerName = "mcp-server";

    size_t ParseCursor(const std::optional<std::string>& cursor) {
        if (!cursor || cursor->empty()) return 0;
        try {
            return std::stoul(*cursor);
        } catch (const std::exception&) {
            throw McpError(McpErrorCode::InvalidParams, "invalid cursor: " + *cursor);
        }
    }

    std::string MakeNextCursor(size_t next_index) {
        return std::to_string(next_index);
    }

    template <typename Entry, typename Fn, typename IncludeFn = std::nullptr_t>
    bool PaginateEntries(
        const std::vector<Entry>& entries,
        size_t cursor_val,
        size_t page_size,
        size_t& next_index,
        Fn&& emit,
        IncludeFn&& should_include = nullptr)
    {
        size_t index = 0;
        size_t sent = 0;
        for (const auto& entry : entries) {
            if constexpr (std::is_invocable_v<IncludeFn, const Entry&>) {
                if (!should_include(entry)) continue;
            }
            if (index++ < cursor_val) continue;
            if (sent >= page_size) {
                next_index = index;
                return true;
            }
            emit(entry);
            sent++;
        }
        return false;
    }

    std::string TaskStatusToWireString(TaskStatus status) {
        switch (status) {
            case TaskStatus::Working:
            case TaskStatus::Pending:
                return "working";
            case TaskStatus::InputRequired:
                return "input_required";
            case TaskStatus::Completed:
                return "completed";
            case TaskStatus::Failed:
                return "failed";
            case TaskStatus::Cancelled:
                return "cancelled";
        }
        return "working";
    }

    bool IsValidToolName(std::string_view name) {
        if (name.empty() || name.size() > 128) return false;
        return std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') ||
                   (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-';
        });
    }

    void SendTaskNotification(
        McpSessionHandler& handler,
        std::string_view method,
        std::string_view task_id,
        TaskStatus status)
    {
        TaskStatusNotificationParams params;
        params.task_id = std::string(task_id);
        params.status = TaskStatusToWireString(status);
        handler.SendNotification(method, SerializeTaskStatusNotificationParams(params));
    }

    std::string MakeIso8601Now() {
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
#ifdef _WIN32
        gmtime_s(&tm_buf, &now);
#else
        gmtime_r(&now, &tm_buf);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        return std::string(buf);
    }

    JsonValue MakeGetTaskResultJson(const TaskState& task, bool include_optional_fields) {
        GetTaskResult r;
        r.task_id = task.task_id;
        r.status = TaskStatusToWireString(task.status);
        r.result = task.result;
        if (include_optional_fields) {
            r.error_message = task.error_message;
            r.input_required = task.input_required;
        }
        return SerializeGetTaskResult(r);
    }
}

namespace {

CacheHint GetCacheHint(const std::optional<std::map<std::string, CacheHint, std::less<>>>& hints, const std::string_view method) {
    if (hints) {
        auto it = hints->find(method);
        if (it != hints->end()) return it->second;
    }
    return {};
}

}

static bool RequireInitialized(bool initialized, bool modern_era, std::promise<JsonValue>& p) {
    if (modern_era || initialized) return true;
    p.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::InvalidRequest, "Server not initialized")));
    return false;
}

// ====================================================================
// Factory
// ====================================================================
std::unique_ptr<McpServer> McpServer::Create(
    std::shared_ptr<ITransport> transport,
    const ServerOptions& options)
{
    return std::unique_ptr<McpServer>(
        new McpServer(std::move(transport), options));
}

McpServer::McpServer(
    std::shared_ptr<ITransport> transport,
    ServerOptions options)
    : transport_(std::move(transport))
    , options_(std::move(options))
{
    auto codec = MakeWireCodec(
        options_.protocol_version.value_or(std::string(kLatestProtocolVersion)));
    handler_ = std::make_shared<McpSessionHandler>(
        transport_, std::move(codec),
        options_.incoming_filters,
        options_.outgoing_filters);

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
    } else if (options_.request_state_key) {
        auto key = *options_.request_state_key;
        auto ttl = options_.request_state_ttl;
        handler_->SetRequestStateVerifier(
            [key, ttl](std::string_view state) {
                JsonValue payload;
                return detail::VerifyRequestState(key, state, payload, ttl);
            });
    }

    // Wire event callbacks — chain new full-message callbacks with existing shorthands
    if (options_.on_request || options_.on_method_called) {
        handler_->SetOnRequestCallback(
            [this](std::string_view method, const JsonRpcRequest& req) {
                if (options_.on_request) options_.on_request(method, req);
                if (options_.on_method_called) options_.on_method_called(method);
            });
    }
    if (options_.on_response) {
        handler_->SetOnResponseCallback(options_.on_response);
    }
    if (options_.on_error || options_.on_protocol_error) {
        handler_->SetOnErrorCallback(
            [this](const JsonRpcErrorResponse& err) {
                if (options_.on_error) options_.on_error(err);
                if (options_.on_protocol_error) options_.on_protocol_error(err.error.message);
            });
    }
    if (options_.on_notification) {
        handler_->SetOnNotificationCallback(options_.on_notification);
    }
    if (options_.on_transport_close || options_.on_transport_error) {
        if (auto* tb = dynamic_cast<TransportBase*>(transport_.get())) {
            if (options_.on_transport_close)
                tb->SetOnClose(options_.on_transport_close);
            if (options_.on_transport_error)
                tb->SetOnError(options_.on_transport_error);
        }
    }

    // Start the transport's IO threads before the session handler's message loop
    transport_->Start();

    // Start the session handler
    handler_->Start();

    // Mark as running for Run() loop
    running_ = true;
}

// ====================================================================
// Lifecycle
// ====================================================================
void McpServer::Run() {
    std::unique_lock<std::mutex> lock(run_mutex_);
    run_cv_.wait(lock, [this] { return !running_; });
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
    {
        std::lock_guard<std::mutex> lock(run_mutex_);
        running_ = false;
    }
    run_cv_.notify_one();
}

// ====================================================================
// Tool registration
// ====================================================================
void McpServer::RegisterTool(std::shared_ptr<McpServerTool> tool) {
    const auto& t = tool->ProtocolTool();
    if (!IsValidToolName(t.name)) {
        throw McpError(McpErrorCode::InvalidParams,
            "invalid tool name: " + t.name);
    }
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        tools_[t.name] = std::move(tool);
        cached_tools_json_ = std::nullopt;
    }
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
    const ResourceOptions& opts,
    std::function<ReadResourceResult(const std::string&)> handler)
{
    ResourceEntry entry;
    entry.name = std::string(name);
    entry.uri_pattern = std::string(uri);
    entry.is_template = false;
    entry.description = opts.description;
    entry.title = opts.title;
    entry.mime_type = opts.mime_type;
    entry.icons = opts.icons;
    entry.handler = std::move(handler);
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        resources_.push_back(std::move(entry));
    }
    WireHandlers();
    DeriveCapabilities();
}

void McpServer::RegisterResourceTemplate(
    std::string_view name,
    std::string_view uri_template,
    const ResourceOptions& opts,
    std::function<ReadResourceResult(
        const std::string&,
        const std::map<std::string, std::string>&)> handler)
{
    std::string error;
    if (!detail::UriTemplate::Parse(uri_template, error)) {
        throw McpError(McpErrorCode::InvalidParams,
                       "invalid resource template '" + std::string(uri_template) +
                           "': " + error);
    }

    ResourceEntry entry;
    entry.name = std::string(name);
    entry.uri_pattern = std::string(uri_template);
    entry.is_template = true;
    entry.description = opts.description;
    entry.title = opts.title;
    entry.mime_type = opts.mime_type;
    entry.icons = opts.icons;
    entry.template_handler = std::move(handler);
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        resources_.push_back(std::move(entry));
    }
    WireHandlers();
    DeriveCapabilities();
}

// ====================================================================
// Prompt registration
// ====================================================================
void McpServer::RegisterPrompt(
    std::string_view name,
    const PromptOptions& opts,
    std::function<GetPromptResult(
        const std::string&,
        const std::optional<JsonValue>&)> handler)
{
    PromptEntry entry;
    entry.name = std::string(name);
    entry.description = opts.description;
    entry.title = opts.title;
    entry.icons = opts.icons;
    entry.arguments = opts.arguments;
    entry.handler = std::move(handler);
    {
        std::unique_lock<std::shared_mutex> lock(registry_mutex_);
        prompts_.push_back(std::move(entry));
    }
    WireHandlers();
    DeriveCapabilities();
}

// ====================================================================
// Notifications
// ====================================================================
void McpServer::SendToolListChanged() {
    handler_->SendNotification(
        notifications::kToolListChanged, JsonValue(JsonValue::object_tag));
}

void McpServer::SendResourceListChanged() {
    handler_->SendNotification(
        notifications::kResourceListChanged, JsonValue(JsonValue::object_tag));
}

void McpServer::SendPromptListChanged() {
    handler_->SendNotification(
        notifications::kPromptListChanged, JsonValue(JsonValue::object_tag));
}

void McpServer::SendLoggingMessage(LoggingLevel level, std::string_view data) {
    std::optional<LoggingLevel> current_level;
    {
        std::lock_guard<std::mutex> lock(log_level_mutex_);
        current_level = current_log_level_;
    }
    if (current_level && static_cast<int>(level) < static_cast<int>(*current_level))
        return;
    JsonValue params(JsonValue::object_tag);
    params[detail::kLevel] = SerializeLoggingLevel(level);
    params[detail::kData] = JsonValue(std::string(data));
    params["logger"] = JsonValue(std::string(kDefaultLoggerName));
    handler_->SendNotification(notifications::kMessage, std::move(params));
}

void McpServer::SendLoggingMessage(LoggingLevel level, std::string_view data, std::optional<LoggingLevel> min_level) {
    if (min_level && static_cast<int>(level) < static_cast<int>(*min_level))
        return;
    SendLoggingMessage(level, data);
}

void McpServer::SendTaskStatus(std::string_view task_id, TaskStatus status) {
    SendTaskNotification(*handler_, notifications::kTaskStatus, task_id, status);
}

void McpServer::SendProgress(const ProgressToken& token, double progress,
                             std::optional<double> total,
                             std::optional<std::string> message) {
    ProgressNotificationParams params;
    params.progress_token = token;
    params.progress = progress;
    params.total = total;
    params.message = std::move(message);
    handler_->SendNotification(notifications::kProgress,
        SerializeProgressNotificationParams(params));
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
        methods::kElicit, SerializeElicitRequestParams(params), meta,
        options_.input_required_config
            ? options_.input_required_config->round_timeout
            : kElicitTimeout);

    auto result_future = std::async(std::launch::async, [future = std::move(future)]() mutable {
        auto jv = future.get();
        if (auto* c = jv.Find("code"); c && c->IsInt() && c->GetInt() < 0) {
            auto msg = jv.Find("message");
            throw McpError(
                static_cast<McpErrorCode>(c->GetInt()),
                msg ? msg->GetString() : std::string("elicitation failed"));
        }
        return DeserializeElicitResult(jv);
    });

    return result_future;
}

std::future<ElicitResult> McpServer::ElicitUrl(
    const std::string& url,
    const std::string& message,
    std::chrono::seconds timeout) {
    const std::string elicitation_id =
        "elicit-" + std::to_string(next_elicitation_id_.fetch_add(1));

    ElicitRequestParams params;
    params.message = message;
    params.mode = "url";
    params.url = url;
    params.elicitation_id = elicitation_id;

    RequestMeta meta;
    auto vers = handler_->NegotiatedProtocolVersion();
    meta.protocol_version = vers.empty()
        ? std::string(kLatestProtocolVersion) : std::string(vers);

    auto pending = std::make_shared<PendingUrlElicitation>();
    auto result_future = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(pending_url_elicitations_mutex_);
        pending_url_elicitations_.emplace(elicitation_id, pending);
    }

    std::future<JsonValue> request_future;
    try {
        request_future = handler_->SendRequest(
            methods::kElicit, SerializeElicitRequestParams(params), meta, timeout);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(pending_url_elicitations_mutex_);
            pending_url_elicitations_.erase(elicitation_id);
        }
        throw;
    }

    auto watchdog = std::async(std::launch::async,
        [this, elicitation_id, pending, timeout,
         request_future = std::move(request_future)]() mutable {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

            std::exception_ptr error;
            if (request_future.wait_until(deadline) != std::future_status::ready) {
                error = std::make_exception_ptr(McpError(
                    McpErrorCode::RequestTimeout,
                    "url elicitation timed out (no response): " + elicitation_id));
            } else {
                auto jv = request_future.get();
                if (auto* c = jv.Find("code"); c && c->IsInt() && c->GetInt() < 0) {
                    auto msg = jv.Find("message");
                    error = std::make_exception_ptr(McpError(
                        static_cast<McpErrorCode>(c->GetInt()),
                        msg ? msg->GetString() : std::string("elicitation failed")));
                }
            }
            if (error) {
                AbandonPendingUrlElicitation(elicitation_id, std::move(error));
                return;
            }

            std::unique_lock<std::mutex> lock(pending_url_elicitations_mutex_);
            if (!pending_url_elicitations_cv_.wait_until(lock, deadline,
                    [&] { return pending->completed; })) {
                lock.unlock();
                AbandonPendingUrlElicitation(elicitation_id,
                    std::make_exception_ptr(McpError(
                        McpErrorCode::RequestTimeout,
                        "url elicitation timed out (no complete notification): " + elicitation_id)));
            }
        });

    {
        std::lock_guard<std::mutex> lock(pending_async_mutex_);
        pending_async_futures_.push_back(watchdog.share());
        pending_async_futures_.erase(
            std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
                [](const auto& f) { return f.wait_for(kNoWait) == std::future_status::ready; }),
            pending_async_futures_.end());
    }

    return result_future;
}

void McpServer::AbandonPendingUrlElicitation(
    const std::string& elicitation_id, std::exception_ptr error) {
    std::shared_ptr<PendingUrlElicitation> pending;
    {
        std::lock_guard<std::mutex> lock(pending_url_elicitations_mutex_);
        auto it = pending_url_elicitations_.find(elicitation_id);
        if (it == pending_url_elicitations_.end()) return;
        pending = std::move(it->second);
        pending_url_elicitations_.erase(it);
    }
    pending->promise.set_exception(std::move(error));
}

// ====================================================================
// Handlers auto-wiring
// ====================================================================
void McpServer::WireHandlers() {
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);

    WireToolHandlers();
    WireResourceHandlers();
    WirePromptHandlers();
    WireCoreHandlers();
    WireExtensionHandlers();
    WireTaskHandlers();
    WireSubscriptionHandlers();
}

void McpServer::WireToolHandlers() {
    // ── tools/list ──
    if (!tools_.empty()) {
        handler_->SetRequestHandler(methods::kListTools,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleListTools(req, std::move(p));
            });
    }

    // ── tools/call ──
    handler_->SetRequestHandler(methods::kCallTool,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleCallTool(req, std::move(p));
        });
}

void McpServer::WireResourceHandlers() {
    // ── resources/list ──
    if (!resources_.empty() && std::any_of(resources_.begin(), resources_.end(),
            [](const auto& r) { return !r.is_template; })) {
        handler_->SetRequestHandler(methods::kListResources,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleListResources(req, std::move(p));
            });
    }

    // ── resources/templates/list ──
    if (!resources_.empty() && std::any_of(resources_.begin(), resources_.end(),
            [](const auto& r) { return r.is_template; })) {
        handler_->SetRequestHandler(methods::kListResourceTemplates,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleListResourceTemplates(req, std::move(p));
            });
    }

    // ── resources/read ──
    if (!resources_.empty()) {
        handler_->SetRequestHandler(methods::kReadResource,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleReadResource(req, std::move(p));
            });
    }

    // ── resources/subscribe / unsubscribe (2025-era) ──
    if (!resources_.empty()) {
        handler_->SetRequestHandler(methods::kSubscribeResource,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
                SubscribeRequestParams params;
                if (req.params) params = DeserializeResourceRequestParams(*req.params);
                Subscription sub{params.uri, {}};
                handler_->AddSubscription(sub);
                EmptyResult r;
                p.set_value(SerializeEmptyResult(r));
            });

        handler_->SetRequestHandler(methods::kUnsubscribeResource,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
                UnsubscribeRequestParams params;
                if (req.params) params = DeserializeResourceRequestParams(*req.params);
                handler_->RemoveSubscription(params.uri);
                EmptyResult r;
                p.set_value(SerializeEmptyResult(r));
            });
    }
}

void McpServer::WirePromptHandlers() {
    // ── prompts/list ──
    if (!prompts_.empty()) {
        handler_->SetRequestHandler(methods::kListPrompts,
            [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                HandleListPrompts(req, std::move(p));
            });
    }

    // ── prompts/get ──
    handler_->SetRequestHandler(methods::kGetPrompt,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleGetPrompt(req, std::move(p));
        });
}

void McpServer::WireCoreHandlers() {
    // ── initialize ──
    handler_->SetRequestHandler(methods::kInitialize,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleInitialize(req, std::move(p));
        });

    // ── server/discover ──
    handler_->SetRequestHandler(methods::kDiscover,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleDiscover(req, std::move(p));
        });

    // ── ping ──
    handler_->SetRequestHandler(methods::kPing,
        [this](const JsonRpcRequest&, std::promise<JsonValue> p) {
            if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
            PingResult r;
            p.set_value(SerializeEmptyResult(r));
        });

    // ── notifications/initialized ──
    handler_->SetNotificationHandler(notifications::kInitialized,
        [this](const JsonRpcNotification&) {
            initialized_ = true;
            if (options_.on_initialized) {
                try { options_.on_initialized(); } catch (...) {
                    MCP_LOG(Error, "on_initialized callback threw an exception");
                }
            }
        });

    // ── logging/setLevel ──
    handler_->SetRequestHandler(methods::kSetLoggingLevel,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
            SetLevelRequestParams params;
            if (req.params) params = DeserializeSetLevelRequestParams(*req.params);
            {
                std::lock_guard<std::mutex> lock(log_level_mutex_);
                current_log_level_ = params.level;
            }
            EmptyResult r;
            p.set_value(SerializeEmptyResult(r));
        });

    // ── notifications/progress ──
    handler_->SetNotificationHandler(notifications::kProgress,
        [this](const JsonRpcNotification& notif) {
            if (notif.params && notif.params->IsObject()) {
                auto* pt = notif.params->Find(detail::kProgressToken);
                if (pt) {
                    ProgressToken token;
                    if (pt->IsString())
                        token = pt->GetString();
                    else if (pt->IsInt())
                        token = pt->GetInt();
                    auto pt_key = std::visit([](const auto& v) -> std::string {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, std::string>) return v;
                        else return std::to_string(v);
                    }, token);
                    handler_->ResetTimeoutByProgressToken(pt_key);
                }
            }
        });

    // ── completion/complete ──
    handler_->SetRequestHandler(methods::kComplete,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleComplete(req, std::move(p));
        });

    // ── notifications/elicitation/complete ──
    handler_->SetNotificationHandler(notifications::kElicitationComplete,
        [this](const JsonRpcNotification& notif) {
            if (!notif.params || !notif.params->IsObject()) return;
            auto* id = notif.params->Find("elicitationId");
            if (!id || !id->IsString()) return;
            std::shared_ptr<PendingUrlElicitation> pending;
            {
                std::lock_guard<std::mutex> lock(pending_url_elicitations_mutex_);
                auto it = pending_url_elicitations_.find(id->GetString());
                if (it == pending_url_elicitations_.end()) {
                    MCP_LOG(Warning,
                        "dropping elicitation complete for unknown id: " + id->GetString());
                    return;
                }
                pending = std::move(it->second);
                pending_url_elicitations_.erase(it);
                pending->completed = true;
            }
            pending_url_elicitations_cv_.notify_all();
            ElicitResult result;
            result.action = "accept";
            pending->promise.set_value(std::move(result));
        });
}

void McpServer::WireExtensionHandlers() {
    // ── server/extensions/list ──
    handler_->SetRequestHandler(methods::kListExtensions,
        [this](const JsonRpcRequest&, std::promise<JsonValue> p) {
            if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), p)) return;
            JsonValue j(JsonValue::object_tag);
            JsonValue ext_list(JsonValue::array_tag);
            if (capabilities_.extensions) {
                for (const auto& [key, val] : *capabilities_.extensions) {
                    JsonValue entry(JsonValue::object_tag);
                    entry[detail::kName] = key;
                    entry["settings"] = val;
                    ext_list.PushBack(std::move(entry));
                }
            }
            j[detail::kExtensions] = std::move(ext_list);
            p.set_value(std::move(j));
        });
}

void McpServer::WireTaskHandlers() {
    // ── tasks/get, tasks/update, tasks/cancel (2025 era only) ──
    auto& store = options_.task_store;
    if (store) {
        handler_->SetRequestHandler(methods::kGetTask,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                            "tasks/get is only available in 2025 and earlier protocol versions")));
                    return;
                }
                GetTaskRequestParams params;
                if (req.params) params = DeserializeGetTaskRequestParams(*req.params);
                auto task = store->GetTask(params.task_id);
                if (!task) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                p.set_value(MakeGetTaskResultJson(*task, true));
            });

        handler_->SetRequestHandler(methods::kUpdateTask,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                            "tasks/update is only available in 2025 and earlier protocol versions")));
                    return;
                }
                UpdateTaskRequestParams params;
                if (req.params) params = DeserializeUpdateTaskRequestParams(*req.params);
                try {
                    if (!store->UpdateTask(params.task_id, params.result)) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                     "task not found: " + params.task_id)));
                        return;
                    }
                } catch (const std::exception& e) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InternalError,
                                 std::string("task persist failed: ") + e.what())));
                    return;
                }
                SendTaskNotification(*handler_,
                    params.result ? notifications::kTaskCompleted : notifications::kTaskWorking,
                    params.task_id,
                    params.result ? TaskStatus::Completed : TaskStatus::Working);
                UpdateTaskResult r;
                p.set_value(SerializeEmptyResult(r));
            });

        handler_->SetRequestHandler(methods::kCancelTask,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                            "tasks/cancel is only available in 2025 and earlier protocol versions")));
                    return;
                }
                CancelTaskRequestParams params;
                if (req.params) params = DeserializeCancelTaskRequestParams(*req.params);
                {
                    std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                    auto flag_it = running_task_cancel_flags_.find(params.task_id);
                    if (flag_it != running_task_cancel_flags_.end()) {
                        flag_it->second->store(true);
                    }
                }
                auto existing = store->GetTask(params.task_id);
                if (!existing) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                const bool already_terminal =
                    existing->status == TaskStatus::Completed
                    || existing->status == TaskStatus::Failed
                    || existing->status == TaskStatus::Cancelled;
                if (already_terminal) {
                    CancelTaskResult r;
                    p.set_value(SerializeEmptyResult(r));
                    return;
                }
                try {
                    if (!store->CancelTask(params.task_id, params.reason)) {
                        p.set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                     "task not found: " + params.task_id)));
                        return;
                    }
                } catch (const std::exception& e) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InternalError,
                                 std::string("task persist failed: ") + e.what())));
                    return;
                }
                SendTaskNotification(*handler_, notifications::kTaskCancelled,
                    params.task_id, TaskStatus::Cancelled);
                CancelTaskResult r;
                p.set_value(SerializeEmptyResult(r));
            });

        handler_->SetRequestHandler(methods::kGetTaskPayload,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                            "tasks/result is only available in 2025 and earlier protocol versions")));
                    return;
                }
                GetTaskRequestParams params;
                if (req.params) params = DeserializeGetTaskRequestParams(*req.params);
                auto task = store->GetTask(params.task_id);
                if (!task) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::InvalidParams,
                                 "task not found: " + params.task_id)));
                    return;
                }
                p.set_value(MakeGetTaskResultJson(*task, false));
            });

        handler_->SetRequestHandler(methods::kListTasks,
            [this, store](const JsonRpcRequest& req, std::promise<JsonValue> p) {
                if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
                    p.set_exception(std::make_exception_ptr(
                        McpError(McpErrorCode::MethodNotFound,
                            "tasks/list is only available in 2025 and earlier protocol versions")));
                    return;
                }
                (void)req;
                JsonValue result(JsonValue::object_tag);
                JsonValue tasks_arr(JsonValue::array_tag);
                auto all_tasks = store->GetAllTasks();
                for (const auto& t : all_tasks) {
                    JsonValue entry(JsonValue::object_tag);
                    entry[detail::kTaskId] = t.task_id;
                    entry["status"] = TaskStatusToWireString(t.status);
                    if (t.result) entry[detail::kResult] = *t.result;
                    tasks_arr.PushBack(std::move(entry));
                }
                result["tasks"] = std::move(tasks_arr);
                p.set_value(std::move(result));
            });
    }
}

void McpServer::WireSubscriptionHandlers() {
    // ── subscriptions/listen (2026 era only) ──
    handler_->SetRequestHandler(methods::kSubscribe,
        [this](const JsonRpcRequest& req, std::promise<JsonValue> p) {
            HandleSubscriptionsListen(req, std::move(p));
        });
}

// ====================================================================
// Capability derivation
// ====================================================================
void McpServer::DeriveCapabilities() {
    std::unique_lock<std::shared_mutex> registry_lock(registry_mutex_);
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
    if (options_.declare_logging) {
        capabilities_.logging = LoggingCapability{};
    }
    if (options_.declare_completions) {
        capabilities_.completions = CompletionsCapability{};
    }
    if (options_.task_store) {
        capabilities_.extensions = std::map<std::string, JsonValue>{};
    }
}

// ====================================================================
// Handler implementations
// ====================================================================
JsonValue McpServer::BuildToolsJson() {
    ListToolsResult result;
    for (const auto& [name, tool_ptr] : tools_) {
        result.tools.push_back(tool_ptr->ProtocolTool());
    }
    auto hint = GetCacheHint(options_.cache_hints, "tools/list");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    return SerializeListToolsResult(result);
}

void McpServer::HandleListTools(
    const JsonRpcRequest& /*req*/, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    {
        std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
        if (cached_tools_json_) {
            promise.set_value(*cached_tools_json_);
            return;
        }
    }
    {
        std::unique_lock<std::shared_mutex> registry_lock(registry_mutex_);
        if (!cached_tools_json_) {
            cached_tools_json_ = BuildToolsJson();
        }
        promise.set_value(*cached_tools_json_);
    }
}

void McpServer::HandleCallTool(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;

    // Parse params
    CallToolRequestParams params;
    if (req.params) {
        params = DeserializeCallToolRequestParams(*req.params);
    }

    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
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
    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
    auto ctx = RequestContext<CallToolRequestParams>(
        *this, req, std::move(params), std::move(log_fn), cancel_flag);

    auto tool = it->second;

    const auto& tool_def = tool->ProtocolTool();
    const bool task_declared = tool_def.execution
        && tool_def.execution->mode == ToolExecutionMode::Task;

    if (task_declared && options_.task_store) {
        if (IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
            promise.set_exception(std::make_exception_ptr(
                McpError(McpErrorCode::MethodNotFound,
                    "tools/call with task execution is only available in 2025 and earlier protocol versions")));
            return;
        }

        auto store = options_.task_store;
        const std::string task_id = "task-" + std::to_string(next_task_id_++);
        {
            std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
            running_task_cancel_flags_[task_id] = cancel_flag;
        }
        store->CreateTask(task_id);
        store->SetTaskStatus(task_id, TaskStatus::Working);
        SendTaskNotification(*handler_, notifications::kTaskStatus,
            task_id, TaskStatus::Working);

        CreateTaskResult created;
        created.task_id = task_id;
        created.status = "working";
        created.created_at = MakeIso8601Now();
        promise.set_value(SerializeCreateTaskResult(created));

        auto fut = std::async(std::launch::async,
            [this, tool, ctx = std::move(ctx), store, task_id, cancel_flag]() mutable {
                auto retire_task = [this, &task_id]() {
                    std::lock_guard<std::mutex> lock(running_task_flags_mutex_);
                    running_task_cancel_flags_.erase(task_id);
                };
                auto finish = [&](TaskStatus status, bool notify) {
                    store->SetTaskStatus(task_id, status);
                    if (notify) {
                        SendTaskNotification(*handler_, notifications::kTaskStatus,
                            task_id, status);
                    }
                    retire_task();
                };
                auto result_promise = std::make_shared<std::promise<CallToolResult>>();
                auto result_future = result_promise->get_future();
                tool->InvokeAsync(ctx, std::move(*result_promise));
                try {
                    auto result = result_future.get();
                    if (cancel_flag->load()) {
                        finish(TaskStatus::Cancelled, false);
                        return;
                    }
                    if (result.is_error) {
                        std::string error_text = "tool execution failed";
                        for (const auto& c : result.content) {
                            if (auto* t = std::get_if<TextContent>(&c)) {
                                error_text = t->text;
                                break;
                            }
                        }
                        JsonValue error_payload(JsonValue::object_tag);
                        error_payload["error"] = JsonValue(error_text);
                        store->UpdateTask(task_id, error_payload);
                        finish(TaskStatus::Failed, true);
                        return;
                    }
                    store->UpdateTask(task_id, SerializeCallToolResult(result));
                    finish(TaskStatus::Completed, true);
                } catch (...) {
                    if (cancel_flag->load()) {
                        finish(TaskStatus::Cancelled, false);
                    } else {
                        finish(TaskStatus::Failed, true);
                    }
                }
            });

        // Store future for lifecycle management; clean up completed futures
        std::lock_guard<std::mutex> lock(pending_async_mutex_);
        pending_async_futures_.push_back(fut.share());
        pending_async_futures_.erase(
            std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
                [](const auto& f) { return f.wait_for(kNoWait) == std::future_status::ready; }),
            pending_async_futures_.end());
        return;
    }

    auto captured_promise = std::make_shared<std::promise<JsonValue>>(std::move(promise));
    auto request_state_key = options_.request_state_key;
    auto fut = std::async(std::launch::async,
        [tool, ctx = std::move(ctx), captured_promise,
         request_state_key = std::move(request_state_key)]() mutable {
            auto result_promise = std::make_shared<std::promise<CallToolResult>>();
            auto result_future = result_promise->get_future();
            tool->InvokeAsync(ctx, std::move(*result_promise));
            try {
                auto result = result_future.get();
                if (result.input_required) {
                    if (request_state_key) {
                        JsonValue payload(JsonValue::object_tag);
                        if (result.input_required->request_state) {
                            auto parsed =
                                JsonValue::Parse(*result.input_required->request_state);
                            if (parsed.IsObject()) payload = parsed;
                        }
                        payload["iat"] = JsonValue(
                            static_cast<int64_t>(std::time(nullptr)));
                        result.input_required->request_state =
                            detail::MintRequestState(*request_state_key, payload);
                    }
                    captured_promise->set_value(SerializeCallToolResult(result));
                    return;
                }
                const auto& tool_def = tool->ProtocolTool();
                if (tool_def.output_schema && result.structured_content) {
                    std::string schema_error;
                    if (!detail::ValidateJsonSchema(*result.structured_content,
                            *tool_def.output_schema, schema_error)) {
                        captured_promise->set_exception(std::make_exception_ptr(
                            McpError(McpErrorCode::InvalidParams,
                                "tool output does not match outputSchema: " + schema_error)));
                        return;
                    }
                }
                captured_promise->set_value(SerializeCallToolResult(result));
            } catch (...) {
                captured_promise->set_exception(std::current_exception());
            }
        });

    // Store future for lifecycle management; clean up completed futures
    std::lock_guard<std::mutex> lock(pending_async_mutex_);
    pending_async_futures_.push_back(fut.share());
    pending_async_futures_.erase(
        std::remove_if(pending_async_futures_.begin(), pending_async_futures_.end(),
            [](const auto& f) { return f.wait_for(kNoWait) == std::future_status::ready; }),
        pending_async_futures_.end());
}

void McpServer::HandleListResources(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    ListResourcesResult result;
    size_t cursor_val = 0;
    if (req.params) {
        cursor_val = ParseCursor(DeserializePaginatedRequestParams(*req.params).cursor);
    }
    size_t next_index = 0;
    if (PaginateEntries(resources_, cursor_val, kDefaultPageSize, next_index,
            [&result](const ResourceEntry& entry) {
                Resource r;
                r.uri = entry.uri_pattern;
                r.name = entry.name;
                r.description = entry.description;
                r.title = entry.title;
                r.mime_type = entry.mime_type;
                if (!entry.icons.empty()) r.icons = entry.icons;
                result.resources.push_back(std::move(r));
            },
            [](const ResourceEntry& entry) { return !entry.is_template; })) {
        result.next_cursor = MakeNextCursor(next_index);
    }
    auto hint = GetCacheHint(options_.cache_hints, "resources/list");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    promise.set_value(SerializeListResourcesResult(result));
}

void McpServer::HandleListResourceTemplates(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    ListResourceTemplatesResult result;
    size_t cursor_val = 0;
    if (req.params) {
        cursor_val = ParseCursor(DeserializePaginatedRequestParams(*req.params).cursor);
    }
    size_t next_index = 0;
    if (PaginateEntries(resources_, cursor_val, kDefaultPageSize, next_index,
            [&result](const ResourceEntry& entry) {
                ResourceTemplate rt;
                rt.uri_template = entry.uri_pattern;
                rt.name = entry.name;
                rt.description = entry.description;
                rt.title = entry.title;
                rt.mime_type = entry.mime_type;
                if (!entry.icons.empty()) rt.icons = entry.icons;
                result.resource_templates.push_back(std::move(rt));
            },
            [](const ResourceEntry& entry) { return entry.is_template; })) {
        result.next_cursor = MakeNextCursor(next_index);
    }
    auto hint = GetCacheHint(options_.cache_hints, "resources/templates/list");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    promise.set_value(SerializeListResourceTemplatesResult(result));
}

void McpServer::HandleReadResource(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    ReadResourceRequestParams params;
    if (req.params) {
        params = DeserializeResourceRequestParams(*req.params);
    }

    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    for (const auto& entry : resources_) {
        if (entry.is_template) continue;
        if (entry.uri_pattern == params.uri) {
            try {
                auto result = entry.handler(params.uri);
                auto hint = GetCacheHint(options_.cache_hints, "resources/read");
                if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
                promise.set_value(SerializeReadResourceResult(result));
                return;
            } catch (...) {
                promise.set_exception(std::current_exception());
                return;
            }
        }
    }

    for (const auto& entry : resources_) {
        if (!entry.is_template) continue;
        std::string error;
        auto tmpl = detail::UriTemplate::Parse(entry.uri_pattern, error);
        if (!tmpl) continue;
        auto variables = tmpl->Match(params.uri);
        if (!variables) continue;
        try {
            auto result = entry.template_handler(params.uri, *variables);
            auto hint = GetCacheHint(options_.cache_hints, "resources/read");
            if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
            promise.set_value(SerializeReadResourceResult(result));
            return;
        } catch (...) {
            promise.set_exception(std::current_exception());
            return;
        }
    }

    promise.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::InvalidParams,
                 "resource not found: " + params.uri)));
}

void McpServer::HandleListPrompts(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    ListPromptsResult result;
    size_t cursor_val = 0;
    if (req.params) {
        cursor_val = ParseCursor(DeserializePaginatedRequestParams(*req.params).cursor);
    }
    size_t next_index = 0;
    if (PaginateEntries(prompts_, cursor_val, kDefaultPageSize, next_index,
            [&result](const PromptEntry& entry) {
                Prompt p;
                p.name = entry.name;
                p.description = entry.description;
                p.title = entry.title;
                p.arguments = entry.arguments;
                if (!entry.icons.empty()) p.icons = entry.icons;
                result.prompts.push_back(std::move(p));
            })) {
        result.next_cursor = MakeNextCursor(next_index);
    }
    auto hint = GetCacheHint(options_.cache_hints, "prompts/list");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    promise.set_value(SerializeListPromptsResult(result));
}

void McpServer::HandleGetPrompt(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    GetPromptRequestParams params;
    if (req.params) {
        params = DeserializeGetPromptRequestParams(*req.params);
    }

    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    for (const auto& entry : prompts_) {
        if (entry.name == params.name) {
            try {
                auto result = entry.handler(params.name, params.arguments);
                promise.set_value(SerializeGetPromptResult(result));
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
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!RequireInitialized(initialized_, handler_->IsJuly2026OrLater(), promise)) return;
    CompleteRequestParams params;
    if (req.params) params = DeserializeCompleteRequestParams(*req.params);

    if (completion_handler_) {
        try {
            auto result = completion_handler_(params);
            promise.set_value(SerializeCompleteResult(result));
            return;
        } catch (...) {
            promise.set_exception(std::current_exception());
            return;
        }
    }

    promise.set_exception(std::make_exception_ptr(
        McpError(McpErrorCode::MethodNotFound, "No completion handler registered")));
}

void McpServer::HandleDiscover(
    const JsonRpcRequest& /*req*/, std::promise<JsonValue> promise)
{
    initialized_ = true;
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (options_.protocol_version) {
        handler_->SetNegotiatedProtocolVersion(*options_.protocol_version);
    } else {
        handler_->SetNegotiatedProtocolVersion(kLatestProtocolVersion);
    }
    DiscoverResult result;
    result.supported_versions.reserve(std::size(kProtocolVersions));
    for (auto v : kProtocolVersions) {
        result.supported_versions.emplace_back(v);
    }
    result.capabilities = capabilities_;
    if (options_.server_info) {
        result.server_info = Implementation{
            options_.server_info->name,
            options_.server_info->version,
            {}
        };
    } else {
        result.server_info = Implementation{"mcp-server", std::string(kSdkVersion), {}};
    }
    if (options_.server_instructions) {
        result.instructions = options_.server_instructions;
    }
    auto hint = GetCacheHint(options_.cache_hints, "server/discover");
    if (hint.ttl_ms || hint.cache_scope) result.cache_hint = hint;
    promise.set_value(SerializeDiscoverResult(result));
}

void McpServer::HandleInitialize(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    InitializeRequestParams params;
    if (req.params) {
        params = DeserializeInitializeRequestParams(*req.params);
    }

    // Store client info
    {
        std::lock_guard<std::mutex> lock(client_info_mutex_);
        client_capabilities_ = std::make_shared<const ClientCapabilities>(params.capabilities);
        client_info_ = std::make_shared<const Implementation>(params.client_info);
    }

    std::shared_ptr<const Implementation> client_info;
    {
        std::lock_guard<std::mutex> lock(client_info_mutex_);
        client_info = client_info_;
    }
    if (options_.on_client_connected && client_info) {
        options_.on_client_connected(*client_info);
    }

    // Negotiate protocol version
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    if (options_.protocol_version) {
        handler_->SetNegotiatedProtocolVersion(*options_.protocol_version);
    } else {
        // Find a common legacy version with the client.
        // Modern versions (2026-07-28+) are NEVER negotiated via
        // initialize — only through server/discover.
        // Missing declaration falls back to the default version;
        // an unknown non-empty version falls back to the legacy version.
        std::string_view selected = kDefaultNegotiatedProtocolVersion;
        if (!params.protocol_version.empty()) {
            selected = kLegacyProtocolVersion;
            for (auto v : kProtocolVersions) {
                if (v == params.protocol_version && !IsModernProtocolVersion(v)) {
                    selected = v;
                    break;
                }
            }
        }
        handler_->SetNegotiatedProtocolVersion(selected);
    }

    InitializeResult result;
    result.protocol_version = std::string(handler_->NegotiatedProtocolVersion());
    result.capabilities = capabilities_;
    if (options_.server_info) {
        result.server_info = Implementation{
            options_.server_info->name,
            options_.server_info->version,
            {}
        };
    } else {
        result.server_info = Implementation{"mcp-server", std::string(kSdkVersion), {}};
    }
    if (options_.server_instructions) {
        result.instructions = options_.server_instructions;
    }
    promise.set_value(SerializeInitializeResult(result));
}

void McpServer::HandleSubscriptionsListen(
    const JsonRpcRequest& req, std::promise<JsonValue> promise)
{
    if (!IsModernProtocolVersion(handler_->NegotiatedProtocolVersion())) {
        promise.set_exception(std::make_exception_ptr(
            McpError(McpErrorCode::MethodNotFound,
                     "subscriptions/listen not available in this protocol version")));
        return;
    }

    SubscriptionsListenRequestParams params;
    if (req.params) params = DeserializeSubscriptionsListenRequestParams(*req.params);

    auto meta = handler_->ExtractIncomingMeta(req);

    SubscriptionEntry entry;
    entry.id = std::to_string(next_subscription_id_++);
    entry.filter = params.notifications;
    entry.created_at = std::chrono::steady_clock::now();

    if (meta.subscription_id) {
        entry.session_id = *meta.subscription_id;
    }

    std::string ack_subscription_id =
        entry.session_id.empty() ? entry.id : entry.session_id;
    handler_->AddSubscriptionEntry(std::move(entry));

    SendSubscriptionsAcknowledged(params.notifications, ack_subscription_id);

    JsonValue result = JsonValue(JsonValue::object_tag);
    promise.set_value(std::move(result));
}

void McpServer::SendSubscriptionsAcknowledged(
    const SubscriptionFilter& honored, std::string_view subscription_id)
{
    JsonRpcNotification notif;
    notif.method = std::string(notifications::kSubscriptionsAcknowledged);
    notif.params = SerializeSubscriptionsAcknowledgedNotificationParams(
        SubscriptionsAcknowledgedNotificationParams{honored});

    JsonValue meta(JsonValue::object_tag);
    meta[detail::kMetaProtocolVersionKey] =
        JsonValue(handler_->NegotiatedProtocolVersion());
    meta[detail::kMetaSubscriptionIdKey] = JsonValue(std::string(subscription_id));
    notif.meta = std::move(meta);

    handler_->SendMessage(JsonRpcMessage{std::move(notif)});
}

// ====================================================================
// Properties
// ====================================================================
std::shared_ptr<const ClientCapabilities> McpServer::GetClientCapabilities() const {
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    return client_capabilities_;
}

std::shared_ptr<const Implementation> McpServer::GetClientInfo() const {
    std::lock_guard<std::mutex> lock(client_info_mutex_);
    return client_info_;
}

std::string McpServer::GetNegotiatedProtocolVersion() const {
    return handler_->NegotiatedProtocolVersion();
}

const ServerCapabilities& McpServer::GetCapabilities() const {
    std::shared_lock<std::shared_mutex> registry_lock(registry_mutex_);
    return capabilities_;
}

bool McpServer::IsMrtrSupported() const {
    // MRTR requires stateful transport
    if (is_stateless_) return false;
    auto caps = GetClientCapabilities();
    return caps && caps->elicitation.has_value();
}

} // namespace mcp
