#pragma once

#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/server/McpServerTool.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerHandlers.hpp>
#include <mcp/server/Extension.hpp>


#include <future>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp {

// ── McpServer (对应 C# McpServer) ──
class McpServer {
public:
    // ── Factory ──
    // If io_ctx is provided, use it instead of creating an internal one.
    // This is required for transports (e.g., StdioServerTransport) that need
    // to share the same io_context with the protocol.
    static std::unique_ptr<McpServer> Create(
        std::shared_ptr<ITransport> transport,
        const ServerOptions& options = {},
        asio::io_context* io_ctx = nullptr);

    virtual ~McpServer() = default;

    // ── Lifecycle ──
    // Start processing messages. Blocks until Close() or transport closes.
    // Internally calls io_context.run().
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
            const std::optional<nlohmann::json>& args)> handler);

    // ── Extension registration ──
    void RegisterExtension(std::shared_ptr<Extension> extension);

    // ── Elicitation (server→client) ──
    std::future<ElicitResult> Elicit(const ElicitRequestParams& params);

    // ── Elicitation typed (generic) ──
    template <typename T>
    std::future<ElicitResultTyped<T>> Elicit(
        std::string_view message,
        std::optional<nlohmann::json> extra_meta = std::nullopt)
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

    // ── Internal access (for RequestContext) ──
    McpSessionHandler& GetSessionHandler() { return *handler_; }
    asio::io_context& IoContext() { return *io_ctx_ptr_; }

private:
    McpServer(
        std::shared_ptr<ITransport> transport,
        ServerOptions options,
        asio::io_context* external_io_ctx);

    // ── Auto-wire handlers from registered tools/resources/prompts ──
    void WireHandlers();
    void DeriveCapabilities();

    // ── Internal handler implementations ──
    void HandleListTools(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);
    void HandleCallTool(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);
    void HandleListResources(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);
    void HandleListResourceTemplates(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);
    void HandleReadResource(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);
    void HandleListPrompts(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);
    void HandleGetPrompt(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);
    void HandleComplete(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);
    void HandleDiscover(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);
    void HandleSubscriptionsListen(
        const JsonRpcRequest& req, std::promise<nlohmann::json> promise);

    // ── State ──
    // io_ctx_ptr_ owns the io_context if created internally; otherwise
    // it references an external io_context. io_ctx_ is always valid.
    std::unique_ptr<asio::io_context> io_ctx_owner_;
    asio::io_context* io_ctx_ptr_;
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
        std::function<GetPromptResult(const std::string&, const std::optional<nlohmann::json>&)> handler;
    };
    std::vector<PromptEntry> prompts_;

    // Client info (set on first request in 2026-era, or from initialize)
    std::optional<ClientCapabilities> client_capabilities_;
    std::optional<Implementation> client_info_;

    // Completion handler (optional user-registered)
    std::function<CompleteResult(const CompleteRequestParams&)> completion_handler_;

    // Extensions
    std::vector<std::shared_ptr<Extension>> extensions_;

    // Active subscriptions
    std::vector<Subscription> subscriptions_;

    // Notification flags
    bool tools_changed_flag_{false};
    bool resources_changed_flag_{false};
    bool prompts_changed_flag_{false};

    // Stateless mode (no session persistence, no MRTR)
    bool is_stateless_{false};
};

} // namespace mcp
