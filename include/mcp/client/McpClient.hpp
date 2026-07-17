#pragma once

#include <mcp/Export.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/client/ClientOptions.hpp>
#include <mcp/client/ClientHandlers.hpp>
#include <mcp/client/McpClientTool.hpp>
#include <mcp/client/VersionNegotiation.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mcp {

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
        const CacheableRequestOptions& options = {},
        std::optional<std::string> cursor = std::nullopt);
    CallToolResult CallTool(
        std::string_view name,
        std::optional<nlohmann::json> arguments = std::nullopt,
        const RequestOptions& options = {});

    // ── Resources ──
    ListResourcesResult ListResources(
        const CacheableRequestOptions& options = {},
        std::optional<std::string> cursor = std::nullopt);
    ListResourceTemplatesResult ListResourceTemplates(
        const CacheableRequestOptions& options = {},
        std::optional<std::string> cursor = std::nullopt);
    ReadResourceResult ReadResource(
        std::string_view uri,
        const CacheableRequestOptions& options = {});
    EmptyResult SubscribeResource(std::string_view uri);
    EmptyResult UnsubscribeResource(std::string_view uri);

    // ── Prompts ──
    ListPromptsResult ListPrompts(
        const CacheableRequestOptions& options = {},
        std::optional<std::string> cursor = std::nullopt);
    GetPromptResult GetPrompt(
        std::string_view name,
        std::optional<nlohmann::json> arguments = std::nullopt,
        const RequestOptions& options = {});

    // ── Completions ──
    CompleteResult Complete(const CompleteRequestParams& params);

    // ── Tasks ──
    GetTaskResult GetTask(std::string_view task_id);
    UpdateTaskResult UpdateTask(
        std::string_view task_id,
        std::optional<nlohmann::json> result = std::nullopt);
    CancelTaskResult CancelTask(
        std::string_view task_id,
        std::optional<std::string> reason = std::nullopt);

    // 轮询任务直到完成
    GetTaskResult PollTaskToCompletionAsync(
        const std::string& task_id,
        std::chrono::milliseconds poll_interval = std::chrono::milliseconds(500),
        std::chrono::seconds timeout = std::chrono::seconds(300));

    // ── Ping ──
    EmptyResult Ping();

    // ── Discover (re-negotiate) ──
    DiscoverResult Discover();

    // ── Client handlers (server-to-client: sampling, roots, elicitation) ──
    // SetSamplingHandler is deprecated in 2026-07-28 (SEP-2577).
    // Use SetElicitationHandler instead.
    void SetSamplingHandler(SamplingHandler handler);
    void SetRootsHandler(RootsHandler handler);
    void SetElicitationHandler(ElicitationHandler handler);
    void SetNotificationHandler(
        std::string_view method,
        ClientNotificationHandler handler);

    // ── Subscriptions ──
    void SubscribeAsync(const SubscriptionsListenRequestParams& params = {});

    // ── Tool cache management (对应 C# AddKnownTools / RemoveKnownTools / ClearKnownTools) ──
    void AddKnownTools(const std::vector<Tool>& tools);
    void RemoveKnownTools(const std::vector<std::string>& names);
    void ClearKnownTools();

    // ── Close ──
    void Close();

private:
    McpClient(
        std::shared_ptr<ITransport> transport,
        ClientOptions options);

    // Internal helpers
    void WireClientHandlers();
    NegotiationResult NegotiateProtocol();

    // Request sending helpers
    template <typename TParams, typename TResult>
    TResult SendRequest(
        std::string_view method,
        TParams params,
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    // MRTR-aware request: handles input_required loop
    nlohmann::json SendRequestWithMrtr(
        std::string_view method,
        nlohmann::json params_json,
        const RequestMeta& meta,
        std::chrono::milliseconds timeout);

    // State
    std::shared_ptr<ITransport> transport_;
    std::shared_ptr<McpSessionHandler> handler_;
    ClientOptions options_;

    // Negotiation result
    NegotiationResult negotiation_;
    bool negotiated_{false};

    // Server-to-client handlers
    std::optional<SamplingHandler> sampling_handler_;
    std::optional<RootsHandler> roots_handler_;
    std::optional<ElicitationHandler> elicitation_handler_;
    std::vector<std::pair<std::string, ClientNotificationHandler>> notif_handlers_;

    // Known tool cache (for client-side tool metadata)
    std::unordered_map<std::string, McpClientTool> known_tools_;
};

} // namespace mcp
