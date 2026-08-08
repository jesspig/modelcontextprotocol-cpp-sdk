// McpServer.hpp - MCP Server class definition

#pragma once

#include <mcp/Export.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/server/McpServerTool.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/server/RequestContext.hpp>

#include <condition_variable>
#include <future>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp {

// ── McpServer (对应 C# McpServer) ──
class MCP_API McpServer {
public:
    // ── Factory ──
    static std::unique_ptr<McpServer> Create(
        std::shared_ptr<ITransport> transport,
        const ServerOptions& options = {});

    virtual ~McpServer() = default;

    // ── Lifecycle ──
    void Run();
    void Close();

    // ── Tool registration ──
    void RegisterTool(std::shared_ptr<McpServerTool> tool);
    void RegisterTool(
        std::string_view name,
        const ToolOptions& options,
        std::function<CallToolResult(const RequestContext<CallToolRequestParams>&)> fn)
    {
        RegisterTool(McpServerTool::Create(name, std::move(fn), options));
    }

    // ── Resource registration ──
    void RegisterResource(
        std::string_view name,
        std::string_view uri,
        const ResourceOptions& /*options*/,
        std::function<ReadResourceResult(const std::string& uri)> handler);

    void RegisterResourceTemplate(
        std::string_view name,
        std::string_view uri_template,
        const ResourceOptions& /*options*/,
        std::function<ReadResourceResult(
            const std::string& uri,
            const std::map<std::string, std::string>& vars)> handler);

    // ── Prompt registration ──
    void RegisterPrompt(
        std::string_view name,
        const PromptOptions& /*options*/,
        std::function<GetPromptResult(const std::string& name,
            const std::optional<JsonValue>& args)> handler);

    // ── Elicitation (server→client) ──
    std::future<ElicitResult> Elicit(const ElicitRequestParams& params);

    // Elicit (server→client) — typed convenience removed; use raw Elicit with explicit schema

    // ── Completion handler ──
    using CompletionHandler = std::function<CompleteResult(const CompleteRequestParams&)>;
    void SetCompletionHandler(CompletionHandler handler);

    // ── Notifications ──
    void SendToolListChanged();
    void SendResourceListChanged();
    void SendPromptListChanged();
    void SendLoggingMessage(LoggingLevel level, std::string_view data);
    void SendLoggingMessage(LoggingLevel level, std::string_view data, std::optional<LoggingLevel> min_level);

    // ── Properties ──
    std::shared_ptr<const ClientCapabilities> GetClientCapabilities() const;
    std::shared_ptr<const Implementation> GetClientInfo() const;
    std::string_view GetNegotiatedProtocolVersion() const;
    const ServerCapabilities& GetCapabilities() const;
    bool IsMrtrSupported() const;

    // ── Internal access ──
    McpSessionHandler& GetSessionHandler() { return *handler_; }

private:
    McpServer(
        std::shared_ptr<ITransport> transport,
        ServerOptions options);

    // ── Auto-wire handlers from registered tools/resources/prompts ──
    void WireHandlers();
    void DeriveCapabilities();

    // ── Internal handler implementations ──
    void HandleListTools(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleCallTool(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleListResources(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleListResourceTemplates(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleReadResource(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleListPrompts(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleGetPrompt(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleComplete(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleDiscover(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleInitialize(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleSubscriptionsListen(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);

    // ── State ──
    std::shared_ptr<ITransport> transport_;
    std::shared_ptr<McpSessionHandler> handler_;
    ServerOptions options_;
    ServerCapabilities capabilities_;

    // Registered primitives (guarded by registry_mutex_, which also guards capabilities_)
    mutable std::shared_mutex registry_mutex_;
    std::unordered_map<std::string, std::shared_ptr<McpServerTool>> tools_;
    struct ResourceEntry {
        std::string name;
        std::string uri_pattern;
        bool is_template;
        std::optional<std::string> description;
        std::optional<std::string> title;
        std::optional<std::string> mime_type;
        std::vector<Icon> icons;
        std::function<ReadResourceResult(const std::string&)> handler;
        std::function<ReadResourceResult(const std::string&, const std::map<std::string, std::string>&)> template_handler;
    };
    std::vector<ResourceEntry> resources_;
    struct PromptEntry {
        std::string name;
        std::optional<std::string> description;
        std::optional<std::string> title;
        std::vector<Icon> icons;
        std::function<GetPromptResult(const std::string&, const std::optional<JsonValue>&)> handler;
    };
    std::vector<PromptEntry> prompts_;

    // Client info (set on first request in 2026-era, or from initialize)
    std::shared_ptr<const ClientCapabilities> client_capabilities_;
    std::shared_ptr<const Implementation> client_info_;
    mutable std::mutex client_info_mutex_;

    // Completion handler (optional user-registered)
    std::function<CompleteResult(const CompleteRequestParams&)> completion_handler_;

    // Async tool call lifecycle management
    std::mutex pending_async_mutex_;
    std::vector<std::shared_future<void>> pending_async_futures_;

    // Initialization state (2025-era protocol)
    std::atomic<bool> initialized_{false};

    // Current logging level (set via logging/setLevel)
    mutable std::mutex log_level_mutex_;
    std::optional<LoggingLevel> current_log_level_;

    // Stateless mode (no session persistence, no MRTR)
    bool is_stateless_{false};

    // Subscription ID allocation (monotonic, process-local)
    std::atomic<uint64_t> next_subscription_id_{1};

    // Run loop synchronization
    std::mutex run_mutex_;
    std::condition_variable run_cv_;
    bool running_{false};
};

} // namespace mcp
