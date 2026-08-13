// HttpServer.hpp - Minimal HTTP server with SSE streaming support (libhv PIMPL)

#pragma once

#include <mcp/JsonRpc.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mcp {

// ── HTTP request / response ──
struct HttpRequest {
    std::string method;      // GET, POST
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status_code{200};
    std::string status_text{"OK"};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool is_sse{false};  // if true, body is ignored and SSE stream is used
};

// ── HTTP handler callback ──
using HttpHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

// ── HttpServer options — event callbacks ──
using HttpRequestCallback = std::function<void(const HttpRequest&)>;
using HttpConnectCallback = std::function<void()>;
using HttpDisconnectCallback = std::function<void()>;

struct HttpServerOptions {
    HttpRequestCallback on_request;
    HttpConnectCallback on_connect;
    HttpDisconnectCallback on_disconnect;

    // DNS rebinding protection: when non-empty, requests whose Host header is
    // not in this list are rejected with 403. When empty, only localhost
    // hosts (localhost / 127.0.0.1 / ::1) are allowed.
    std::vector<std::string> allowed_hosts;
    // When non-empty, requests carrying an Origin header must match one of
    // these exact origins; otherwise the Origin header is ignored.
    std::vector<std::string> allowed_origins;
};

// ── HttpServer — minimal HTTP server ──
// Handles GET and POST. Supports SSE streaming via callback.
struct HttpServerImpl;
class HttpServer {
public:
    HttpServer(uint16_t port,
               const HttpServerOptions& options = {});
    ~HttpServer();

    // Start accepting connections
    void Start();

    // Stop the server
    void Stop();

    // Set handler for a specific path + method
    void SetHandler(std::string_view method, std::string_view path,
                    HttpHandler handler);

    // Send SSE event to a connected SSE client
    // (for server-initiated notifications)
    using SseClientId = uint64_t;
    SseClientId AddSseClient(std::function<void(std::string_view)> send_fn);
    void RemoveSseClient(SseClientId id);
    void BroadcastSse(std::string_view event);

private:
    uint16_t port_;
    std::atomic<bool> running_{false};
    HttpServerOptions options_;

    // DNS rebinding protection: validates Host (and Origin when configured).
    bool IsRequestAllowed(const HttpRequest& req) const;

    // Handlers: (method, path) → handler
    std::map<std::pair<std::string, std::string>, HttpHandler> handlers_;

    // shared_ptr so SSE onclose callbacks can capture the impl safely
    std::shared_ptr<HttpServerImpl> impl_;
};

} // namespace mcp
