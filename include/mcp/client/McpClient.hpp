#pragma once
// McpClient.hpp
// MCP client implementation for connecting to MCP servers
#include <mcp/Export.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/VersionNegotiation.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mcp {

// Client-side response cache (SEP-2549); defined in src/detail/ResponseCache.hpp.
namespace detail { class ResponseCache; }

// ── Server-to-client request handlers ──

// SamplingHandler is deprecated in 2026-07-28 (SEP-2577).
// Use ElicitationHandler instead.
using SamplingHandler = std::function<CreateMessageResult(
    const CreateMessageRequestParams&)>;

// Roots (deprecated): server requests root directory list
using RootsHandler = std::function<ListRootsResult(
    const ListRootsRequestParams&)>;

// Elicitation: server requests user input
using ElicitationHandler = std::function<ElicitResult(
    const ElicitRequestParams&)>;

// URL 模式 elicitation 处理器：收到 mode=="url" 的 elicitation/create 时调用，
// 返回后 SDK 自动发送 notifications/elicitation/complete。
using UrlElicitationHandler = std::function<void(const ElicitRequestParams&)>;

// Progress: server pushes notifications/progress for an in-flight request.
// Invoked synchronously on the session message loop thread; must return fast.
using ProgressCallback = std::function<void(const ProgressNotificationParams&)>;

// Notification handler: server sends notification
using ClientNotificationHandler = std::function<void(
    const JsonRpcNotification&)>;

// ── McpClient (对应 C# McpClient) ──
class MCP_API McpClient {
public:
    // ── Factory ──
    // Create and connect. Blocks until negotiation completes.
    static std::unique_ptr<McpClient> Create(
        std::shared_ptr<ITransport> transport,
        const ClientOptions& options = {});

    ~McpClient();

    // ── Properties (对应 C# ServerCapabilities / ServerInfo / ServerInstructions) ──
    const ServerCapabilities& GetServerCapabilities() const;
    const Implementation& GetServerInfo() const;
    std::optional<std::string> GetInstructions() const;
    std::string_view GetNegotiatedProtocolVersion() const;
    bool IsModernProtocol() const;

    // ── Tools ──
    ListToolsResult ListTools(
        std::optional<std::string> cursor = std::nullopt);
    // Aggregate every page into one result; next_cursor is empty on return.
    // Throws McpError if pagination does not converge within 64 pages.
    ListToolsResult ListToolsAll();
    CallToolResult CallTool(
        std::string_view name,
        std::optional<JsonValue> arguments = std::nullopt,
        const RequestOptions& options = {});

    // ── Resources ──
    ListResourcesResult ListResources(
        std::optional<std::string> cursor = std::nullopt);
    // Aggregate every page into one result; next_cursor is empty on return.
    ListResourcesResult ListResourcesAll();
    ListResourceTemplatesResult ListResourceTemplates(
        std::optional<std::string> cursor = std::nullopt);
    // Aggregate every page into one result; next_cursor is empty on return.
    ListResourceTemplatesResult ListResourceTemplatesAll();
    ReadResourceResult ReadResource(
        std::string_view uri,
        const CacheableRequestOptions& options = {});
    EmptyResult SubscribeResource(std::string_view uri);
    EmptyResult UnsubscribeResource(std::string_view uri);

    // ── Prompts ──
    ListPromptsResult ListPrompts(
        std::optional<std::string> cursor = std::nullopt);
    // Aggregate every page into one result; next_cursor is empty on return.
    ListPromptsResult ListPromptsAll();
    GetPromptResult GetPrompt(
        std::string_view name,
        std::optional<JsonValue> arguments = std::nullopt,
        const RequestOptions& options = {});

    // ── Completions ──
    CompleteResult Complete(const CompleteRequestParams& params);

    // ── Tasks ──
    // 发起任务化工具调用；对端返回 task 句柄时立即返回（不等待执行完成）。
    // 之后可用 GetTask / PollTaskToCompletion / CancelTask 跟进。
    GetTaskResult CallToolAsTask(
        std::string_view name,
        std::optional<JsonValue> arguments = std::nullopt,
        const RequestOptions& options = {});

    GetTaskResult GetTask(std::string_view task_id);
    UpdateTaskResult UpdateTask(
        std::string_view task_id,
        std::optional<JsonValue> result = std::nullopt);
    CancelTaskResult CancelTask(
        std::string_view task_id,
        std::optional<std::string> reason = std::nullopt);

    // Poll task until completion
    GetTaskResult PollTaskToCompletion(
        const std::string& task_id,
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500),
        std::chrono::seconds timeout = std::chrono::seconds(300));

    // ── Ping ──
    [[deprecated("Ping is deprecated in 2026-07-28 protocol version")]]
    EmptyResult Ping();

    // ── Discover (re-negotiate) ──
    DiscoverResult Discover();

    // ── Client handlers (server-to-client: sampling, roots, elicitation) ──
    // SetSamplingHandler is deprecated in 2026-07-28 (SEP-2577).
    // Use SetElicitationHandler instead.
    [[deprecated("Use SetElicitationHandler instead (SEP-2577)")]]
    void SetSamplingHandler(SamplingHandler handler);
    [[deprecated("Roots are deprecated in 2026-07-28 (SEP-2577)")]]
    void SetRootsHandler(RootsHandler handler);
    void SetElicitationHandler(ElicitationHandler handler);
    void SetUrlElicitationHandler(UrlElicitationHandler handler);
    void SetNotificationHandler(
        std::string_view method,
        ClientNotificationHandler handler);

    // ── Logging handler (server→client logging notifications) ──
    void SetLoggingHandler(std::function<void(const LoggingMessageNotificationParams&)> handler);

    // ── Subscriptions ──
    void SubscribeAsync(const SubscriptionsListenRequestParams& params = {});

    // ── Notifications (client → server) ──
    // notifications/roots/list_changed requires negotiated version >= 2025-06-18.
    void SendRootsListChanged();

    // ── Close ──
    void Close();

private:
    McpClient(
        std::shared_ptr<ITransport> transport,
        ClientOptions options);

    // Internal helpers
    void WireClientHandlers();
    NegotiationResult NegotiateProtocol();

    // Register options.on_progress under a progress token key and stamp the
    // token onto meta; returns the key, or nullopt when no callback is set.
    std::optional<std::string> AttachProgressCallback(
        const RequestOptions& options, RequestMeta& meta);
    void DetachProgressCallback(const std::string& key);

    // MRTR-aware request: handles input_required loop
    JsonValue SendRequestWithMrtr(
        std::string_view method,
        JsonValue params_json,
        const RequestMeta& meta,
        std::chrono::milliseconds timeout);

    // Single MRTR pass without session-expiry recovery
    JsonValue SendRequestWithMrtrOnce(
        std::string_view method,
        JsonValue params_json,
        const RequestMeta& meta,
        std::chrono::milliseconds timeout);

    // Re-run negotiation exactly once per expired session generation
    void RecoverExpiredSession();

    // Store the result in the cache when it carries a ttlMs cache hint.
    void CacheIfHinted(std::string_view key, const JsonValue& result);

    // State
    std::shared_ptr<ITransport> transport_;
    std::shared_ptr<McpSessionHandler> handler_;
    ClientOptions options_;

    // Negotiation result
    NegotiationResult negotiation_;

    // Server-to-client handlers
    std::optional<SamplingHandler> sampling_handler_;
    std::optional<RootsHandler> roots_handler_;
    std::optional<ElicitationHandler> elicitation_handler_;
    std::optional<UrlElicitationHandler> url_elicitation_handler_;
    std::optional<std::function<void(const LoggingMessageNotificationParams&)>> logging_handler_;

    // Client-side response cache (SEP-2549); invalidated on listChanged
    // notifications. Defined in src/detail/ResponseCache.hpp.
    std::unique_ptr<detail::ResponseCache> response_cache_;

    std::mutex ack_mutex_;
    std::condition_variable ack_cv_;
    std::optional<std::string> pending_ack_id_;
    std::optional<ClientNotificationHandler> user_ack_notification_handler_;

    // Progress callbacks for in-flight requests, keyed by progress token
    std::atomic<int64_t> next_progress_token_{1};
    std::unordered_map<std::string, ProgressCallback> progress_callbacks_;
    std::mutex progress_callbacks_mutex_;

    // 404 session-expiry recovery: generation counter plus mutex serializing
    // re-initialization; waiters observe a changed generation and skip it.
    std::atomic<uint64_t> session_generation_{0};
    std::mutex reinit_mutex_;

};

} // namespace mcp
