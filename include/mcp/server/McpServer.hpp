#pragma once

#include <mcp/Export.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/server/McpServerTool.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerHandlers.hpp>

#include <future>
#include <memory>
#include <atomic>
#include <mutex>
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

    // ── Elicitation typed (generic) ──
    template <typename T>
    std::future<ElicitResultTyped<T>> Elicit(
        std::string_view message,
        std::optional<JsonValue> extra_meta = std::nullopt)
    {
        ElicitRequestParams params;
        params.message = std::string(message);
        params.requested_schema = detail::build_json_schema<T>();

        auto raw_future = Elicit(params);
        return std::async(std::launch::deferred,
            [raw_future = std::move(raw_future)]() mutable {
                auto raw = raw_future.get();
                ElicitResultTyped<T> typed;
                if (raw.values) {
                    typed.content = raw.values->get<T>();
                    typed.action = "accept";
                }
                return typed;
            });
    }

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
    const ClientCapabilities* GetClientCapabilities() const;
    const Implementation* GetClientInfo() const;
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

    // Registered primitives
    std::unordered_map<std::string, std::shared_ptr<McpServerTool>> tools_;
    struct ResourceEntry {
        std::string name;
        std::string uri_pattern;
        bool is_template;
        std::function<ReadResourceResult(const std::string&)> handler;
        std::function<ReadResourceResult(const std::string&, const std::map<std::string, std::string>&)> template_handler;
    };
    std::vector<ResourceEntry> resources_;
    struct PromptEntry {
        std::string name;
        std::function<GetPromptResult(const std::string&, const std::optional<JsonValue>&)> handler;
    };
    std::vector<PromptEntry> prompts_;

    // Client info (set on first request in 2026-era, or from initialize)
    std::optional<ClientCapabilities> client_capabilities_;
    std::optional<Implementation> client_info_;

    // Completion handler (optional user-registered)
    std::function<CompleteResult(const CompleteRequestParams&)> completion_handler_;

    // Active subscriptions
    std::vector<Subscription> subscriptions_;

    // Async tool call lifecycle management
    std::mutex pending_async_mutex_;
    std::vector<std::shared_future<void>> pending_async_futures_;

    // Initialization state (2025-era protocol)
    std::atomic<bool> initialized_{false};

    // Stateless mode (no session persistence, no MRTR)
    bool is_stateless_{false};
};

} // namespace mcp
