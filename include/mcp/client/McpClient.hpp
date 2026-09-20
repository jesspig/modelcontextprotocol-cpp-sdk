#pragma once
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

namespace detail { class ResponseCache; }

using SamplingHandler = std::function<CreateMessageResult(
    const CreateMessageRequestParams&)>;

using RootsHandler = std::function<ListRootsResult(
    const ListRootsRequestParams&)>;

using ElicitationHandler = std::function<ElicitResult(
    const ElicitRequestParams&)>;

using UrlElicitationHandler = std::function<void(const ElicitRequestParams&)>;

using ProgressCallback = std::function<void(const ProgressNotificationParams&)>;

using ClientNotificationHandler = std::function<void(
    const JsonRpcNotification&)>;

class MCP_API McpClient {
public:
    static std::unique_ptr<McpClient> Create(
        std::shared_ptr<ITransport> transport,
        const ClientOptions& options = {});

    ~McpClient();

    const ServerCapabilities& GetServerCapabilities() const;
    const Implementation& GetServerInfo() const;
    std::optional<std::string> GetInstructions() const;
    std::string_view GetNegotiatedProtocolVersion() const;
    bool IsModernProtocol() const;

    ListToolsResult ListTools(
        std::optional<std::string> cursor = std::nullopt);
    ListToolsResult ListToolsAll();
    CallToolResult CallTool(
        std::string_view name,
        std::optional<JsonValue> arguments = std::nullopt,
        const RequestOptions& options = {});

    ListResourcesResult ListResources(
        std::optional<std::string> cursor = std::nullopt);
    ListResourcesResult ListResourcesAll();
    ListResourceTemplatesResult ListResourceTemplates(
        std::optional<std::string> cursor = std::nullopt);
    ListResourceTemplatesResult ListResourceTemplatesAll();
    ReadResourceResult ReadResource(
        std::string_view uri,
        const CacheableRequestOptions& options = {});
    EmptyResult SubscribeResource(std::string_view uri);
    EmptyResult UnsubscribeResource(std::string_view uri);

    ListPromptsResult ListPrompts(
        std::optional<std::string> cursor = std::nullopt);
    ListPromptsResult ListPromptsAll();
    GetPromptResult GetPrompt(
        std::string_view name,
        std::optional<JsonValue> arguments = std::nullopt,
        const RequestOptions& options = {});

    CompleteResult Complete(const CompleteRequestParams& params);

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

    GetTaskResult PollTaskToCompletion(
        const std::string& task_id,
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500),
        std::chrono::seconds timeout = std::chrono::seconds(300));

    [[deprecated("Ping is deprecated in 2026-07-28 protocol version")]]
    EmptyResult Ping();

    DiscoverResult Discover();

    [[deprecated("Use SetElicitationHandler instead (SEP-2577)")]]
    void SetSamplingHandler(SamplingHandler handler);
    [[deprecated("Roots are deprecated in 2026-07-28 (SEP-2577)")]]
    void SetRootsHandler(RootsHandler handler);
    void SetElicitationHandler(ElicitationHandler handler);
    void SetUrlElicitationHandler(UrlElicitationHandler handler);
    void SetNotificationHandler(
        std::string_view method,
        ClientNotificationHandler handler);

    void SetLoggingHandler(std::function<void(const LoggingMessageNotificationParams&)> handler);

    void SubscribeAsync(const SubscriptionsListenRequestParams& params = {});

    void SendRootsListChanged();

    void Close();

private:
    McpClient(
        std::shared_ptr<ITransport> transport,
        ClientOptions options);

    void WireClientHandlers();
    NegotiationResult NegotiateProtocol();

    std::optional<std::string> AttachProgressCallback(
        const RequestOptions& options, RequestMeta& meta);
    void DetachProgressCallback(const std::string& key);

    JsonValue SendRequestWithMrtr(
        std::string_view method,
        JsonValue params_json,
        const RequestMeta& meta,
        std::chrono::milliseconds timeout);

    JsonValue SendRequestWithMrtrOnce(
        std::string_view method,
        JsonValue params_json,
        const RequestMeta& meta,
        std::chrono::milliseconds timeout);

    void RecoverExpiredSession();

    void CacheIfHinted(std::string_view key, const JsonValue& result);

    std::shared_ptr<ITransport> transport_;
    std::shared_ptr<McpSessionHandler> handler_;
    ClientOptions options_;

    NegotiationResult negotiation_;

    std::optional<SamplingHandler> sampling_handler_;
    std::optional<RootsHandler> roots_handler_;
    std::optional<ElicitationHandler> elicitation_handler_;
    std::optional<UrlElicitationHandler> url_elicitation_handler_;
    std::optional<std::function<void(const LoggingMessageNotificationParams&)>> logging_handler_;

    std::unique_ptr<detail::ResponseCache> response_cache_;

    std::mutex ack_mutex_;
    std::condition_variable ack_cv_;
    std::optional<std::string> pending_ack_id_;
    std::optional<ClientNotificationHandler> user_ack_notification_handler_;

    std::atomic<int64_t> next_progress_token_{1};
    std::unordered_map<std::string, ProgressCallback> progress_callbacks_;
    std::mutex progress_callbacks_mutex_;

    std::atomic<uint64_t> session_generation_{0};
    std::mutex reinit_mutex_;

};

} // namespace mcp
