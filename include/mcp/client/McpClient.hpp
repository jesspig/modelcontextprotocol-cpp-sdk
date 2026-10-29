#pragma once
#include <mcp/Export.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/VersionNegotiation.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp {

namespace detail { class ResponseCache; }

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
    ListToolsResult SearchTools(
        std::optional<std::string> query = std::nullopt,
        std::optional<int64_t> page_size = std::nullopt);
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

    SkillsListResult ListSkills(
        std::optional<std::string> cursor = std::nullopt);
    SkillsGetResult GetSkill(
        std::string_view uri,
        const CacheableRequestOptions& options = {});
    ReadDirectoryResult ReadDirectory(
        std::string_view uri,
        std::optional<std::string> cursor = std::nullopt,
        const CacheableRequestOptions& options = {});

    CompleteResult Complete(const CompleteRequestParams& params);

    GetTaskResult CallToolAsTask(
        std::string_view name,
        std::optional<JsonValue> arguments = std::nullopt,
        const RequestOptions& options = {});

    GetTaskResult GetTask(std::string_view task_id);
    UpdateTaskResult UpdateTask(
        std::string_view task_id,
        std::optional<JsonValue> result = std::nullopt,
        std::optional<JsonValue> input_responses = std::nullopt);
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
    [[deprecated("use ElicitResult-returning handler")]]
    void SetLegacyUrlElicitationHandler(
        std::function<void(const ElicitRequestParams&)> handler);
    void SetNotificationHandler(
        std::string_view method,
        ClientNotificationHandler handler);

    void SetLoggingHandler(std::function<void(const LoggingMessageNotificationParams&)> handler);

    void SubscribeAsync(const SubscriptionsListenRequestParams& params = {});

    std::future<JsonValue> SendRequest(
        std::string_view method,
        JsonValue params,
        const RequestMeta& meta = {},
        std::chrono::milliseconds timeout = kDefaultRequestTimeout);
    void SendNotification(std::string_view method, JsonValue params = {});
    void CancelRequest(const RequestId& id, std::string_view reason = {});
    void SetLoggingLevel(LoggingLevel level);

    void SendRootsListChanged();

    void Close();

private:
    McpClient(
        std::shared_ptr<ITransport> transport,
        ClientOptions options);

    void WireClientHandlers();
    void WireTransportCallbacks();
    void DetachTransportCallbacks();
    void InstallSamplingHandler();
    void InstallRootsHandler();
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

    struct EtagEntry {
        std::string etag;
        JsonValue body;
    };
    std::optional<std::string> GetStoredEtag(const std::string& key);
    std::optional<JsonValue> GetStoredEtagBody(const std::string& key);
    void StoreEtagBody(const std::string& key, const JsonValue& result);
    void MaybeAttachEtag(const std::string& key, RequestMeta& meta);
    std::unordered_map<std::string, EtagEntry> etag_entries_;
    std::deque<std::string> etag_order_;
    std::mutex etag_mutex_;

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
