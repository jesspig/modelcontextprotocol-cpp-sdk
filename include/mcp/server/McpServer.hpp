#pragma once

#include <mcp/protocol/Protocol.hpp>
#include <mcp/server/McpServerTool.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/server/RequestContext.hpp>
#include <mcp/server/ServerHandlers.hpp>
#include <mcp/server/Extension.hpp>


#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp {

// ── McpServer (对应 C# McpServer) ──
class McpServer {
public:
    // ── Factory ──
    static std::unique_ptr<McpServer> Create(
        std::unique_ptr<Transport> transport,
        const ServerOptions& options = {});

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

    // ── Notifications ──
    void SendToolListChanged();
    void SendResourceListChanged();
    void SendPromptListChanged();
    void SendLoggingMessage(LoggingLevel level, std::string_view data);

    // ── Properties ──
    const ClientCapabilities* GetClientCapabilities() const;
    const Implementation* GetClientInfo() const;
    std::string_view GetNegotiatedProtocolVersion() const;
    const ServerCapabilities& GetCapabilities() const;
    bool IsMrtrSupported() const;

    // ── Internal access (for RequestContext) ──
    Protocol& GetProtocol() { return *protocol_; }
    asio::io_context& IoContext() { return io_ctx_; }

private:
    McpServer(
        std::unique_ptr<Transport> transport,
        ServerOptions options);

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

    // ── State ──
    asio::io_context io_ctx_;
    std::unique_ptr<Transport> transport_;
    std::shared_ptr<Protocol> protocol_;
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

    // Extensions
    std::vector<std::shared_ptr<Extension>> extensions_;

    // Notification flags
    bool tools_changed_flag_{false};
    bool resources_changed_flag_{false};
    bool prompts_changed_flag_{false};
};

} // namespace mcp
