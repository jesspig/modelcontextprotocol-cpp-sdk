#pragma once

#include <mcp/Transport.hpp>
#include <mcp/http/HttpServer.hpp>
#include <mcp/http/EventStore.hpp>
#include <mcp/http/SessionStore.hpp>
#include <mcp/transport/detail/Limits.hpp>

#include <mcp/JsonValue.hpp>
#include <mcp/McpVersion.hpp>
#include <mcp/detail/McpParamAnnotations.hpp>
#include <mcp/ext/ServerCard.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mcp {

inline constexpr uint16_t kDefaultPort = 3001;

struct AuthResult {
    bool ok{false};
    std::vector<std::string> scopes;
    std::optional<std::string> resource;
};

using McpParamAnnotationInfo = detail::McpParamAnnotation;

struct StreamableHttpServerOptions {
    uint16_t port{kDefaultPort};
    std::string endpoint{"/mcp"};

    bool stateless{true};

    bool enable_legacy_sse{false};

    int sse_keep_alive_ms{15000};

    std::shared_ptr<EventStore> event_store;

    std::shared_ptr<SessionStore> session_store;

    std::string server_name{"mcp-server"};
    std::string server_version{std::string(kSdkVersion)};

    std::string host;

    std::optional<std::size_t> max_request_body_bytes = detail::kMaxHttpBodyBytes;
    struct BearerAuthConfig {
        std::function<AuthResult(const std::string& token)> verify;
        std::string resource_metadata_url;
        std::vector<std::string> scopes_supported;
        std::vector<std::string> required_scopes;
        std::vector<std::string> authorization_servers;
        bool serve_metadata_endpoint{true};
        std::string expected_resource;
        bool validate_token_resource{true};
    };
    std::optional<BearerAuthConfig> bearer_auth;

    std::function<std::vector<McpParamAnnotationInfo>(const std::string& method,
                                                      const std::string& name)>
        resolve_param_annotations;

    bool enable_server_card{false};
    std::optional<ext::ServerCard> server_card;
};

class StreamableHttpServerTransport : public TransportBase {
public:
    StreamableHttpServerTransport(
        StreamableHttpServerOptions options = {});

    ~StreamableHttpServerTransport() override;

    void Start() override;
    void Close() override;
    void SendMessageAsync(JsonRpcMessage message) override;
    bool BufferRequestStreamNotification(JsonRpcMessage message) override;
    bool IsStateless() const override { return options_.stateless; }

    static bool ValidateMcpHeaders(
        const std::string& method_header,
        const std::string& name_header,
        const JsonValue& body,
        std::string& error_out);

private:
    void HandlePost(const HttpRequest& req, HttpResponse& resp);
    void HandleGet(const HttpRequest& req, HttpResponse& resp);

    bool AuthorizeRequest(const HttpRequest& req, HttpResponse& resp);
    void HandleMetadataRequest(HttpResponse& resp);
    void HandleServerCardRequest(HttpResponse& resp);
    ext::ServerCard BuildDefaultServerCard() const;

    std::string BuildSseEvent(JsonRpcMessage msg);

    std::optional<std::string> GetMcpHeader(const HttpRequest& req,
                                            std::string_view header_name) const;

    bool ValidateMcpHeaders(const HttpRequest& req, const JsonRpcRequest& request,
                            std::string& error_out);

    bool ValidateParamHeaders(const HttpRequest& req, const JsonRpcRequest& request,
                              std::string& error_out);

    bool EnsureSession(const HttpRequest& req, HttpResponse& resp);
    std::string ActiveSessionId() const;
    void AdoptSession(const std::string& session_id);

    static std::string RequestIdToString(const RequestId& id);

    bool TryBufferRequestStreamNotification(const JsonRpcMessage& message);
    void EraseRequestStreamStateLocked(const std::string& id_str);
    static std::optional<std::string> RequestTokenKey(const JsonRpcRequest& req);
    static std::optional<std::string> NotificationTokenKey(const JsonRpcNotification& notif);

    StreamableHttpServerOptions options_;
    std::unique_ptr<HttpServer> http_server_;
    std::shared_ptr<EventStore> event_store_;
    std::shared_ptr<SessionStore> session_store_;
    mutable std::mutex session_state_mutex_;

    std::atomic<size_t> stateless_inflight_{0};
    std::unordered_map<std::string, std::shared_ptr<std::promise<JsonRpcMessage>>> pending_responses_;
    std::mutex pending_mutex_;
    std::unordered_map<std::string, std::vector<std::string>> request_stream_frames_;
    std::unordered_map<std::string, std::string> progress_token_routes_;
    std::unordered_set<std::string> hold_open_requests_;
};

} // namespace mcp
