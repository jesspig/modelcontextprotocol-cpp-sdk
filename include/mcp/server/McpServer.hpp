#pragma once

#include <mcp/Export.hpp>
#include <mcp/JsonValue.hpp>
#include <mcp/Meta.hpp>
#include <mcp/detail/McpParamAnnotations.hpp>
#include <mcp/protocol/McpSessionHandler.hpp>
#include <mcp/ext/Triggers.hpp>
#include <mcp/ext/ServerCard.hpp>
#include <mcp/server/McpServerTool.hpp>
#include <mcp/server/ServerOptions.hpp>
#include <mcp/server/RequestContext.hpp>

#include <condition_variable>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp {

class MCP_API McpServer {
public:
    static std::unique_ptr<McpServer> Create(
        std::shared_ptr<ITransport> transport,
        const ServerOptions& options = {});

    virtual ~McpServer() = default;

    void Run();
    void Close();

    void RegisterTool(std::shared_ptr<McpServerTool> tool);
    void RegisterTool(
        std::string_view name,
        const ToolOptions& options,
        std::function<CallToolResult(const RequestContext<CallToolRequestParams>&)> fn)
    {
        RegisterTool(McpServerTool::Create(name, std::move(fn), options));
    }

    std::vector<detail::McpParamAnnotation> ResolveToolParamAnnotations(
        const std::string&, const std::string& name) const;

    void RegisterResource(
        std::string_view name,
        std::string_view uri,
        const ResourceOptions&,
        std::function<ReadResourceResult(const std::string& uri)> handler);

    void RegisterResourceTemplate(
        std::string_view name,
        std::string_view uri_template,
        const ResourceOptions&,
        std::function<ReadResourceResult(
            const std::string& uri,
            const std::map<std::string, std::string>& vars)> handler);

    void RegisterPrompt(
        std::string_view name,
        const PromptOptions&,
        std::function<GetPromptResult(const std::string& name,
            const std::optional<JsonValue>& args)> handler);

    void RegisterSkill(Skill skill);
    void SetSkillsDirectoryRead(bool enabled);

    std::future<ElicitResult> Elicit(const ElicitRequestParams& params);
    std::future<ElicitResult> ElicitUrl(const std::string& url,
        const std::string& message,
        std::chrono::seconds timeout = std::chrono::seconds(600));


    using CompletionHandler = std::function<CompleteResult(const CompleteRequestParams&)>;
    void SetCompletionHandler(CompletionHandler handler);

    void SendToolListChanged();
    void SendResourceListChanged();
    void SendResourceUpdated(const std::string& uri);
    void SendPromptListChanged();
    void SendLoggingMessage(LoggingLevel level, std::string_view data);
    void SendLoggingMessage(LoggingLevel level, std::string_view data, std::optional<LoggingLevel> min_level);
    void SetCacheable(std::function<std::optional<CacheHint>(std::string_view method)> resolver);
    void SendTaskStatus(std::string_view task_id, TaskStatus status);
    void SendProgress(const ProgressToken& token, double progress,
                      std::optional<double> total = std::nullopt,
                      std::optional<std::string> message = std::nullopt);
    void RegisterTrigger(const std::string& id,
                         std::vector<std::string> types,
                         const std::string& webhook_url);
    bool UnregisterTrigger(const std::string& id);
    void EmitTriggerEvent(const std::string& type, JsonValue data = {});

    std::shared_ptr<const ClientCapabilities> GetClientCapabilities() const;
    std::shared_ptr<const Implementation> GetClientInfo() const;
    std::string GetNegotiatedProtocolVersion() const;
    ServerCapabilities GetCapabilities() const;
    ext::ServerCard GetServerCard() const;
    bool IsMrtrSupported() const;
    bool IsRunning() const;
    const std::string& SessionId() const { return session_id_; }

    McpSessionHandler& GetSessionHandler() { return *handler_; }

private:
    McpServer(
        std::shared_ptr<ITransport> transport,
        ServerOptions options);

    void WireHandlers();
    void WireToolHandlers();
    void WireResourceHandlers();
    void WirePromptHandlers();
    void WireCoreHandlers();
    void WireExtensionHandlers();
    void WireTaskHandlers();
    void WireSkillHandlers();
    void WireSubscriptionHandlers();
    void DeriveCapabilities();
    void SendListChangedNotification(std::string_view method);

    JsonValue BuildToolsJson();
    std::vector<std::string> EffectiveSupportedVersions() const;
    Implementation ResolveServerInfo() const;
    CacheHint ResolveCacheHint(std::string_view method) const;
    void EmitLoggingMessage(LoggingLevel level, std::string_view data);
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
    void HandleListSkills(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleGetSkill(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleReadDirectory(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleComplete(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleDiscover(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleInitialize(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void HandleSubscriptionsListen(
        const JsonRpcRequest& req, std::promise<JsonValue> promise);
    void SendSubscriptionsAcknowledged(
        const SubscriptionFilter& honored, std::string_view subscription_id);
    void AbandonPendingUrlElicitation(
        const std::string& elicitation_id, std::exception_ptr error);
    std::string MakeTaskId() const;
    void SweepExpiredTasks(const std::shared_ptr<IMcpTaskStore>& store);
    bool IsTaskOwnedBySession(const TaskState& task) const;

    std::shared_ptr<ITransport> transport_;
    std::shared_ptr<McpSessionHandler> handler_;
    ServerOptions options_;
    ServerCapabilities capabilities_;

    mutable std::shared_mutex registry_mutex_;
    std::map<std::string, std::shared_ptr<McpServerTool>, std::less<>> tools_;
    std::optional<JsonValue> cached_tools_json_;
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
        std::optional<std::vector<PromptArgument>> arguments;
        std::function<GetPromptResult(const std::string&, const std::optional<JsonValue>&)> handler;
    };
    std::vector<PromptEntry> prompts_;

    std::vector<Skill> skills_;
    bool skills_directory_read_{false};
    ext::TriggerRegistry triggers_;

    std::shared_ptr<const ClientCapabilities> client_capabilities_;
    std::shared_ptr<const Implementation> client_info_;
    mutable std::mutex client_info_mutex_;

    std::function<CompleteResult(const CompleteRequestParams&)> completion_handler_;
    mutable std::mutex completion_handler_mutex_;

    std::mutex pending_async_mutex_;
    std::vector<std::shared_future<void>> pending_async_futures_;

    std::mutex running_task_flags_mutex_;
    std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> running_task_cancel_flags_;

    std::atomic<bool> initialized_{false};

    mutable std::mutex log_level_mutex_;
    std::optional<LoggingLevel> current_log_level_;

    mutable std::mutex cache_hook_mutex_;
    std::function<std::optional<CacheHint>(std::string_view)> cache_hint_hook_;

    bool is_stateless_{false};

    std::atomic<uint64_t> next_subscription_id_{1};

    std::atomic<uint64_t> next_elicitation_id_{1};

    std::string session_id_;

    struct PendingUrlElicitation {
        std::promise<ElicitResult> promise;
        bool completed{false};
    };
    std::mutex pending_url_elicitations_mutex_;
    std::condition_variable pending_url_elicitations_cv_;
    std::unordered_map<std::string, std::shared_ptr<PendingUrlElicitation>>
        pending_url_elicitations_;

    std::mutex run_mutex_;
    std::condition_variable run_cv_;
    bool running_{false};
};

} // namespace mcp
